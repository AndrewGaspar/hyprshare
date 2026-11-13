# hyprshare (spike)

A Hyprland plugin that swaps **which window a running window-share shows**, without stopping the
share. Pick window A in Discord / Meet / Zoom / OBS, then press a key: the stream now shows window B.

This is a **spike** to answer one question before building the real thing: *do consuming apps
tolerate the shared window changing mid-stream?* To them it looks like the shared window resized
(and changed content).

## How it works

`xdg-desktop-portal-hyprland` (XDPH) sends a fresh `hyprland_toplevel_export_manager_v1`
capture request for **every frame**, and the compositor answers each one from the managed
screenshare session for *(client, window)*, re-advertising that window's buffer size. The plugin
hooks `CToplevelExportClient::captureToplevel` (`src/protocols/ToplevelExport.cpp`) and hands the
original a different window. XDPH sees a size change and renegotiates its PipeWire stream.

- **Captures** are tracked per *(wl_client, picked window)*. One is *active* if it requested a
  frame in the last ~1 s, or still holds a frame object (a static window keeps one frame pending,
  waiting for damage). XDPH multiplexes every portal session over one connection, so the 2–3
  sessions an Electron app opens for one share collapse into one capture and all follow together.
- **`sharewindow [selector]`** redirects every *active* capture to the target. Captures sit in one
  of three states: **origin** (serving the window XDPH picked), **target** (following the global
  redirect target), or **nothing** (see below). XDPH multiplexes every portal session over one
  connection, so the 2–3 sessions an Electron app opens for one share collapse into one capture and
  all follow together. Shares started later are left alone until the next `sharewindow`.
- **`shareclear`** sends every capture back to the window its own portal picked (state → origin).
- **Closing the shared window never reveals another window** (privacy). Whenever the window a
  capture is serving dies — the picked window, or a redirect target — the capture enters the sticky
  **nothing** state and is served a **black placeholder** at the last advertised size (so nothing
  renegotiates) until you explicitly `sharewindow` again. A target closing does **not** fall back to
  the picked window; a new window (even one that reuses a dead window's address) does not leave the
  state. State is keyed on the capture, never on an address.
- **Every capture request always gets a real frame and an answer.** 0.56's `captureToplevel`
  returns early for a null window *without creating the frame resource*; libwayland then leaves that
  new-id a NULL object, and the client's next destroy on it posts `wl_display error 0 (invalid
  object)` and kills the portal. The plugin creates the resource itself for every request (a
  placeholder, or `failed` for an unknown/untracked null) so this can't happen.
- The window actually being served to an active capture carries the tag **`screenshared`** (nobody
  in the nothing state); it moves with redirects and is removed ~1 s after the window stops being
  served, on clear, and on unload.

Everything runs on the compositor main thread (Wayland requests, dispatchers, hyprctl, Lua,
event-loop timers). Nothing touches the XR frame thread.

## Build

The plugin must be compiled against the **exact Hyprland tree the running compositor was built
from** — not `/usr/include/hyprland`, which belongs to stock hyprland with different layouts.

```sh
# 1. Headers tree: detached worktree at the running commit, configured like the hypxrland
#    PKGBUILD (Release, WITH_OPENXR, the package's version stamp), protocol headers generated.
#    (For the r469 package, 7ac272db9, pass its cosmetic stamp too so version.h is byte-identical.)
GIT_TAG=v0.56.2-469-g7ac272db9a GIT_COMMITS=8130 GIT_COMMIT_DATE=2026-11-11 scripts/prepare-headers.sh

# 2. Plugin and the test probe
make              # -> build/hyprshare-v5.so (versioned: never overwrite a .so a session has loaded)
make -C tests     # -> tests/build/{capture-probe,output-probe,click-logger,vpointer}
make hash         # shows the tree's commit next to the running compositor's
```

`HYPRLAND_SRC=` overrides the headers tree (default
`~/.cache/hypxrland-worktrees/hyprshare-headers`). The Makefile mirrors the defines `hyprland_lib`
is built with in the package (`NDEBUG`, `HAVE_OPENXR=1`, `HAVE_XR_VULKAN_PROBE=1`, …; taken from
the tree's `build-pkg/compile_commands.json`). **Rebuild after every compositor update**: on a
version mismatch the plugin refuses to load (it compares `__hyprland_api_get_hash()` with its own
client hash in `pluginInit`, before hooking anything).

For packaging, `make HYPRLAND_PC_DIR=/usr/lib/hypxrland/pkgconfig` takes the include paths from a
hypxrland `hyprland.pc` instead of the source tree (untested until that headers package exists).
The .so is linked `-z nodelete`: the compositor keeps pointers into it after unload (config value
names, a decoration a hidden window only frees once shown again, pass-element vtables), and keeping
the image mapped makes those inert instead of dangling.

## Load / unload (live session)

```sh
hyprctl plugin load   /home/ajg/code/hyprshare/build/hyprshare-v5.so
hyprctl plugin unload /home/ajg/code/hyprshare/build/hyprshare-v5.so
hyprctl plugin list
```

Unloading removes the hooks, the hyprctl commands, the Lua functions, the glyph decorations, the
frame/client/input listeners and the timer, removes the `screenshared` / `screenshare-mode` tags,
and answers any pending placeholder frame; it **disowns**
(does not destroy) the placeholder frame resources, so the portal client sees no protocol error.

Hyprland `dlopen`s by path, so each build has its own file name. Upgrade by unloading the old path
and loading the new one, e.g.:

```sh
hyprctl plugin unload /home/ajg/code/hyprshare/build/hyprshare-v4.so
hyprctl plugin load   /home/ajg/code/hyprshare/build/hyprshare-v5.so
```

## Use

### hyprctl (works with either config front end)

```sh
hyprctl sharewindow                     # redirect active shares to the focused window
hyprctl sharewindow address:0x55d1...   # ... or to a selected window
hyprctl sharewindow class:firefox
hyprctl shareclear                      # back to the windows picked in the portal
hyprctl sharepause                      # pause: every active share shows the black placeholder
hyprctl sharefocus                      # focus the window the share is showing   -> ok
hyprctl sharestop                       # end ALL screen capturing now            -> ok <n>
hyprctl sharestatus                     # JSON, see below
```

Replies are `ok` or an error string; `sharestop` replies `ok <n>`, the number of compositor capture
sessions it stopped. `sharefocus` errors: `no window share` (no active window share at all),
`shared window is gone` (the shared window closed — the share is showing the black placeholder).

Selectors are the compositor's own window selectors (`Desktop::CViewQuery::selector`):
`active`, `address:0x…`, `class:<regex>`, `title:<regex>`, `initialclass:`, `initialtitle:`,
`tag:<regex>`, `pid:<n>`, `stableid:<hex>`, `floating`, `tiled`; a bare string is a class regex.

Under the **Lua** config, `hyprctl dispatch X` is evaluated as `hl.dispatch(X)`, so plugin
dispatchers registered by name are *not* reachable through it — use the `hyprctl` commands above,
or `hyprctl eval 'hl.plugin.hyprshare.share("address:0x…")'`. Under a hyprlang `.conf` the
dispatchers `sharewindow` / `shareclear` / `sharepause` exist too (`hyprctl dispatch sharewindow …`,
`bind = SUPER CTRL ALT, S, sharewindow`), as do `sharefocus` and `sharestop` — that path is registered
but was not tested.

### Lua binds

The plugin registers `hl.plugin.hyprshare.share([selector])`, `.clear()`, `.pause()`, `.focus()`,
`.stop()` and `.status()`. They act immediately and return `true` or `false, err` (they never raise);
`.stop()` returns `true, n` (sessions stopped) and `.status()` the `sharestatus` JSON string. Wrap them in a function so the
config still parses before the plugin is loaded:

```lua
-- Share the focused window instead (SUPER+ALT+S is taken: Omarchy scratchpad + XR stereo toggle).
hl.bind("SUPER + CTRL + ALT + S", function() hl.plugin.hyprshare.share() end)
hl.bind("SUPER + CTRL + ALT + SHIFT + S", function() hl.plugin.hyprshare.clear() end)
hl.bind("SUPER + CTRL + ALT + P", function() hl.plugin.hyprshare.pause() end)
hl.bind("SUPER + CTRL + ALT + F", function() hl.plugin.hyprshare.focus() end)
hl.bind("SUPER + CTRL + ALT + X", function() hl.plugin.hyprshare.stop() end)
```

Pressing them without the plugin loaded is a harmless Lua error (`hl.plugin.hyprshare` is nil).

### Border on the shared window

```lua
hl.window_rule({
    name = "hyprshare-border",
    match = { tag = "screenshared" },
    border_color = "rgb(ff3030)",
})
```

### The glyph (click to share / pause / resume)

While any window share is active (in any state), every eligible window shows a small dot in a
corner — top right by default:

| state  | which window                         | default look             | click        |
|--------|--------------------------------------|--------------------------|--------------|
| LIVE   | the window currently being shared    | filled red dot           | pause        |
| IDLE   | every other window                   | hollow white ring        | share it     |
| PAUSED | the window whose share you paused    | filled amber, pause bars | resume it    |

Pause is exactly the closed-window state: every active share shows the black placeholder, no
window carries `screenshared`, nothing auto-resumes. `hyprctl sharepause` / `hl.plugin.hyprshare.pause()`
do the same from a keybind; `sharewindow` also resumes.

- Left click only, on release; a press that moves more than a few px (a drag) does nothing.
  Glyph clicks never reach the window's client; clicks anywhere else are untouched. Clicks with a
  modifier held (e.g. SUPER+drag binds) are left alone.
- No share active → nothing drawn, no input taken. Fullscreen windows hide their glyph (configurable),
  windows too small to hold it are skipped. Monitor scale and window opacity are respected.
- The glyph never appears in the shared stream: window capture renders without decorations.
- It is a plain window decoration drawn in the normal render pass (also on XR virtual monitors);
  input comes from the cancellable `input.mouse.button` event, because Hyprland only routes
  decoration clicks to decorations with a positioner box, which in-window (absolute) decorations
  don't get.

#### Styling (plugin config, from your Lua config)

Hyprland itself knows nothing about this chrome; it's all plugin config, set with `hl.config`:

```lua
-- Guarded: the keys only exist once the plugin is loaded (loading it triggers a reload, which then
-- applies them). Without the guard, an unloaded plugin makes these unknown-key config errors.
if hl.plugin.hyprshare then
    hl.config({
        plugin = {
            hyprshare = {
                corner     = "top_right", -- your setup; this is also the default
                offset_x   = 10,
                offset_y   = 10,
                size       = 14,
                visibility = "all",       -- or "shared_only"
            },
        },
    })
end
```

| key (`plugin.hyprshare.*`) | default          | meaning                                                        |
|----------------------------|------------------|----------------------------------------------------------------|
| `corner`                   | `"top_right"`    | `top_right`, `top_left`, `bottom_right`, `bottom_left`         |
| `offset_x`, `offset_y`     | `10`, `10`       | inset from the window's side / top-or-bottom edge, logical px  |
| `size`                     | `14`             | glyph diameter, logical px                                     |
| `outline`                  | `2`              | outline width, logical px                                      |
| `opacity`                  | `1.0`            | multiplier, on top of the window's own opacity                 |
| `visibility`               | `"all"`          | `all` windows, or `shared_only` (LIVE/PAUSED glyphs only)      |
| `hide_fullscreen`          | `1`              | hide on fullscreen windows                                     |
| `live_fill`, `live_outline`     | `rgba(ff3030ff)`, `rgba(ffffffe6)` | LIVE colours                    |
| `idle_fill`, `idle_outline`     | `rgba(00000040)`, `rgba(ffffffd9)` | IDLE colours                    |
| `paused_fill`, `paused_outline` | `rgba(ffb000ff)`, `rgba(ffffffe6)` | PAUSED colours (bars use outline) |

Colours take the usual colour strings (`"rgba(rrggbbaa)"`, `"rgb(rrggbb)"`, …). Changes apply on
config reload (`hyprctl reload`, or saving the config).

#### `screenshare-mode` tag

While any share is active every window carries the tag `screenshare-mode` (removed when the last
share stops), so the whole desktop can look different during a share:

```lua
-- dim borders of everything while sharing; the shared window keeps its own rule below
hl.window_rule({ name = "share-mode-borders", match = { tag = "screenshare-mode" }, border_color = "rgba(80808080)" })
hl.window_rule({ name = "hyprshare-border",   match = { tag = "screenshared" },     border_color = "rgb(ff3030)" })
```

### `sharestatus`

```json
{
  "shareMode": true,
  "capturing": true,
  "anyShare": true,
  "sharedWindow": {"state": "live", "address": "0x5643af87d880", "alive": true, "class": "hsB", "title": "sh", "workspace": "1"},
  "target": {"address": "0x5643af87d880", "alive": true, "mapped": true, "class": "hsB", "title": "sh"},
  "paused": null,
  "captures": [
    {"pid": 334, "active": true, "state": "target", "nothingReason": "", "lastSeenMsAgo": 43, "outstandingFrames": 0, "placeholderFrames": 0,
     "origin": {"address": "0x5643afaad1a0", "alive": true, "mapped": true, "class": "hsA", "title": "sh"},
     "served": {"address": "0x5643af87d880", "alive": true, "mapped": true, "class": "hsB", "title": "sh"}}
  ],
  "sessions": [
    {"type": "region", "sharing": false, "pid": -1, "clientAlive": false, "monitor": "HEADLESS-1", "window": ""},
    {"type": "window", "sharing": false, "pid": 334, "clientAlive": true, "monitor": "HEADLESS-1", "window": "0x5643afaad1a0"},
    {"type": "window", "sharing": true, "pid": 334, "clientAlive": true, "monitor": "HEADLESS-1", "window": "0x5643af87d880"},
    {"type": "monitor", "sharing": true, "pid": 369, "clientAlive": true, "monitor": "HEADLESS-1", "window": ""}
  ],
  "stopGrace": {"active": false, "msLeft": 0, "pids": []},
  "glyph": {"corner": "top_right", "visibility": "all",
            "glyphs": [{"address": "0x5643af87d880", "state": "live", "box": [1116, 10, 14, 14]}]}
}
```

- `shareMode`: any hyprshare window capture is active (in any state, including paused / placeholder).
- `capturing`: any compositor capture session is sharing right now — monitor, region or window, over
  any protocol (wlr-screencopy clients such as wf-recorder or a portal's monitor share,
  ext-image-copy-capture, toplevel export), whether or not hyprshare redirects it. It is the
  session's own sharing flag, the exact thing the built-in `screencast` / `screencastv2` events
  report: on with the first copied frame, off ~500 ms after the last one or on stop.
- `anyShare` = `capturing || shareMode`. A paused (or closed-window) window share only sends black
  placeholder frames, which the compositor does not count as sharing, so `capturing` goes false
  ~500 ms into a pause while the stream is still live; `anyShare` stays true.
- `sharedWindow`: `null` when no window share is active. Otherwise the window the most recently
  started / retargeted active capture shows (after a `sharewindow` all active captures show the same
  window), with `state`:
  - `live`: being shared; `address` / `class` / `title` / `workspace` describe it, `alive` true;
  - `paused`: paused; the fields describe the window a resume returns to (`alive` false if it closed);
  - `closed`: the shared window closed, the share shows the placeholder; `address` is `""`.

  Every key is always present (`""` / `false` when there is no window). `sharefocus` succeeds exactly
  when `sharedWindow.alive` is true (when paused it focuses the paused window).
- `sessions`: every live (not stopped) compositor capture session. `pid` is the capturing client
  (XDPH, wf-recorder, …), `monitor` the output it captures (for a window, the window's monitor),
  `window` the captured window's address (window sessions only). The compositor keeps a
  wlr-screencopy / toplevel-export session after its client disconnects (it never stops it); such a
  leftover shows `clientAlive: false`, `pid: -1` and `sharing: false` until something stops it
  (`sharestop` does). A redirected window share has one session per window it has served.
- `stopGrace`: the post-`sharestop` refusal window: `pids` of the refused clients, `msLeft`.
- `target`, `paused`, `captures`, `glyph`: unchanged from v4. `state` is `origin`, `target` or
  `nothing` (serving a placeholder); `served` is null in `nothing`, and `nothingReason` says why
  (`paused` or `closed`). `paused` is the window a pause will resume. `glyph.glyphs` lists the glyphs
  currently drawn, with their hit boxes in global layout coordinates. `pid` is the capturing client
  (XDPH in real use, not the app). Inactive captures are forgotten after 10 s.

### For a status bar

- **Listen** on socket2 for `hyprshare>>changed` and re-query `hyprctl sharestatus` (`hyprctl -j`
  is not needed: it is always JSON). The plugin posts it whenever anything above that a bar cares
  about changed: `capturing` on/off, a session added / removed / sharing flip, the share target
  changing, pause/resume, a capture entering or leaving the placeholder state, the served window
  closing or retitling, `sharestop`. It is coalesced: one check per event-loop iteration against the
  last state posted, so a burst (or a change and its undo in one iteration) posts at most one event,
  and a re-query always sees the change. The built-in `screencast` / `screencastv2` events still
  fire too; listening to `hyprshare>>changed` alone is enough.
- **Red bar** while anything captures: `.anyShare` (or `.capturing` for the compositor's strict view).
- **Focus button** while window-sharing: show it when `.sharedWindow != null`; enable it when
  `.sharedWindow.alive`; label it with `.sharedWindow.title` / `.class`; on click run
  `hyprctl sharefocus` (or `hl.plugin.hyprshare.focus()`).
- **Stop button** while `.anyShare`: first close the portal's sessions (XDPH `CloseAll`, so the
  apps see the share end), then `hyprctl sharestop` for whatever is left (direct screencopy clients).

### `sharestop`

Compositor-side "end all capturing now":

1. every frame that was given a buffer and is waiting for an output commit is answered `failed`
   (otherwise a stopped session's frame is never answered and a client like wf-recorder hangs);
2. every live capture session of every type is stopped (`screencast>>0` fires for the ones that
   were sharing; ext-image-copy-capture clients are sent `stopped`; wlr-screencopy and toplevel
   export sessions are torn down);
3. the redirect forgets everything: captures, target, pause, placeholder state, `screenshared` /
   `screenshare-mode` tags, glyphs;
4. for `plugin:hyprshare:stop_grace_ms` (default `2000`, `0` disables, max `60000`) every client it
   stopped is **refused**: its window-share requests get `failed` (after 100 ms, so a portal that
   re-requests on `failed` is not spun in a tight loop), and any copy it requests over
   wlr-screencopy, ext-image-copy-capture or toplevel export gets `failed` (the session it belongs to
   is stopped too). Nothing is copied for a refused client. After the window a retry captures again
   (that is why the portal's own sessions must be closed first).

```lua
if hl.plugin.hyprshare then
    hl.config({ plugin = { hyprshare = { stop_grace_ms = 2000 } } })
end
```

## Tests

```sh
make && make -C tests && tests/run-in-container.sh
```

`tests/run-in-container.sh` starts a throwaway podman container (the `hypxrland-ctr` session image,
for its labwc/kitty/jq — **not** its Hyprland), brings up a headless labwc there and nests the
**host's installed `/usr/lib/hypxrland/Hyprland`** under it (bind-mounted read-only, with the host's
exact `libhyprutils`). This Hyprland ignores `HYPRLAND_HEADLESS_ONLY`, so a seatless compositor
can only run nested; the container keeps that off the live display. `tests/run-isolated.sh`
(the actual test) also runs on the host given `NEST_WAYLAND=<socket>`, with a clean `env -i`
environment, private `XDG_RUNTIME_DIR`/`HOME`, `HYPRLAND_NO_SD_VARS=1`, the real `hyprctl` binary
and PID-tracked cleanup.

Three suites run, each against a fresh nested compositor: `share` (the redirect/placeholder suite),
`glyph` and `bar` (the status-bar API). `HYPRSHARE_SUITES=bar tests/run-in-container.sh` runs one.

`tests/capture-probe` stands in for XDPH: a fresh capture request per frame, printing the
advertised buffer size; `--app-id` uses XDPH's exact request
(`capture_toplevel_with_wlr_toplevel_handle`, keeping the handle after the toplevel closes),
`--handle` the uint variant, `--copy` copies into shm and `--copy-dmabuf` into a gbm dmabuf (the
real XDPH/PipeWire path), both reading the first pixel back (`ready-black`/`ready-nonblack`);
`--xdph` waits for frame events forever and re-requests after `failed` (patched XDPH). Every
iteration it destroys the frame proxy and reports `display-ok` / `display-error` at the end;
`--sample X Y` also prints the copied buffer's pixel there. `tests/output-probe` captures the monitor
like wf-recorder or a portal's monitor share: wlr-screencopy by default (`--region X Y W H` for a
region), ext-image-copy-capture with `--ext`; it re-requests after `failed` (re-creating an ext
session after `stopped`) unless `--exit-on-failed`. `tests/click-logger` is a solid-colour
xdg-shell window that prints every pointer button it receives; `tests/vpointer` injects moves,
clicks and drags through `zwlr_virtual_pointer_v1` (a real pointer device to the compositor);
on-screen pixels are read with `grim`.

Share suite (57 checks): load + hash check against the live binary; per-frame sizes before/after
`sharewindow`; two Electron-style sessions on one origin following; uint-handle path; real shm +
dmabuf pixel copies; tag + border rule moving and expiring; `sharestatus` fields (also a layout
sanity check: class and pid read through the plugin's view of the structs); Lua functions via
`hyprctl eval`, error returns, surviving `hyprctl reload`; `shareclear` = back to the picked window.
Privacy/placeholder: a target closing goes to **nothing** (not back to the origin) and serves a
black placeholder at the target's last size — verified black via both shm and dmabuf readback — with
the tag on nobody; a new window appearing in **nothing** does not reveal it; `sharewindow` resumes;
an origin closing while unredirected does the same. Crash-proofing: destroying frame proxies every
iteration and the untracked null-handle → destroy sequence both post **no protocol error**; unload
with a placeholder pending leaves the client unharmed; unload (tags gone, commands gone, hook out).

Bar suite (87 checks): idle status (`capturing`/`anyShare` false, `sharedWindow` null, no sessions,
`sharefocus` = `no window share`, no events while idle); a wlr-screencopy monitor capture and a
region capture (`capturing`, `sessions` type/sharing/pid/monitor, `changed` and the built-in
`screencast` events on start and ~500 ms after stop, a dead client's leftover session reported as
`clientAlive: false` and not attributed to a later client reusing its address); an
ext-image-copy-capture capture; a window share (`sharedWindow` live, window session, `changed` on
start / retarget / pause / resume / served window closing, no events while a share is steady, a
change and its undo in one hyprctl batch post none); `sharefocus` switching to the shared window's
workspace and a special workspace, focusing the paused window, failing with `shared window is gone`
after it closes (also via Lua `focus()`); `sharestop` with four clients at once (portal-style
monitor share, ext-image-copy-capture, wf-recorder-style, XDPH-style window share): `ok <n>`, nothing
sharing, the redirect cleared and untagged, every retry refused during the grace window (no frame
copied, ext told `stopped`, window re-requests failed at a bounded rate, the wf-recorder-style client
exits), every client captures again after it, no protocol errors; Lua `stop()` and a configured
500 ms grace.

Glyph suite (66 checks): no glyph without a share (and the screen shows window pixels there); LIVE
and IDLE glyphs appear with a share, at the configured box, drawn on screen; the glyph is **not** in
the stream (shm readback at the glyph spot = window colour); `screenshare-mode` on every window;
clicking an IDLE glyph switches the share, the LIVE glyph pauses (black placeholder, `nothing` /
`paused`, no `screenshared` tag), the PAUSED glyph resumes; glyph clicks never reach the client,
clicks on content do; a drag from a glyph does nothing and leaks nothing; `sharepause` and Lua
`pause()`; corner/offset change moves the hit box on reload (old spot passes clicks through),
`shared_only` hides IDLE glyphs, colours apply; fullscreen hides the glyph; glyphs and the mode tag
vanish when the share stops; a new client reusing a dead client's address starts clean; unload with
glyphs visible → no crash, decorations gone, clicks reach the client, new windows get none.

## Known limitations (spike)

- **Every swap is a resize to the consumer.** XDPH renegotiates PipeWire on size change; XDPH 1.4.1
  has freeze-on-resize bugs (#451/#445) — expect stalls until a patched XDPH is in use.
- **One global target.** All redirected shares show the same window; no per-share targeting.
- **Only shares active at `sharewindow` time** are redirected; a share started later needs another
  `sharewindow`.
- **In the nothing state the consumer sees a black placeholder**, at ~10 fps, until the next
  `sharewindow`. It never auto-recovers to any window — that is the intended privacy behaviour.
  Resuming onto a new target needs the patched XDPH (its `failed` → re-request recovery); stock XDPH
  keeps showing black after a swap-away.
- **Pause can let one in-flight frame through.** A real frame the compositor already accepted before
  the pause may still complete with window content; every later frame is the placeholder.
- **Pause/resume is global.** Pausing pauses every active share and remembers one window to resume
  (the first live one); resuming sends every share there.
- **Glyph clicks are taken from the raw button event**, so they beat everything except an active
  session lock / layer-surface focus and modified clicks. A glyph covered by another window is
  covered for input too (the topmost window under the cursor owns the click).
- **The placeholder renders off the output-commit cycle** (from a throttle timer). This is a plugin
  reaching into the compositor's renderer; validated on a real GPU in the container suite, but it is
  not a path the compositor itself exercises.
- **`sharestop` grace gaps.** Refusal is per *client*: a newly started capture client (a fresh
  wf-recorder process, a new portal connection) is not refused. ext-image-copy-capture *cursor*
  sessions (cursor image only) are neither stopped nor refused. A share the user starts through the
  portal within the grace window fails until the window ends. A copy that was already in flight (a
  dmabuf blit waiting on its fence) at `sharestop` still completes.
- **Leftover sessions.** The compositor keeps a wlr-screencopy / toplevel-export session after its
  client disconnects; it stays in `sessions` (`clientAlive: false`) until `sharestop` or its
  monitor/window goes away. A session that was already orphaned when the plugin loaded, and whose
  address a live client now reuses, is attributed to that client.
- **Screencast indicators may flicker or lie.** Each (client, window) is its own compositor
  screenshare session: on a swap the target's session starts (`screencast>>1,…` IPC event) and
  the picked window's session stops ~500 ms later (`screencast>>0,…`). Anything counting those
  events may show "not sharing" while a redirected share is live. A bar should use `sharestatus`
  (`.anyShare`) and the `hyprshare>>changed` event instead (see "For a status bar").
- **Only buffer negotiation and copies were tested**, against a probe — not against real XDPH,
  PipeWire or any consuming app (that is what the spike is for).
- XDPH restore tokens still name the picked window.
- The tag is a plain static tag: tagging a window `screenshared` by hand gets undone by the plugin.
- The hooks target private, non-API compositor functions (`CToplevelExportClient::captureToplevel`,
  `Screenshare::CScreenshareFrame::share`) and it reads private screenshare state; the plugin is
  pinned to one exact commit.
