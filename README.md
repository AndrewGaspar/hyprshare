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
- **`sharewindow [selector]`** redirects every *active* capture (by the window XDPH originally
  asked for) to the target, and retargets all existing redirects. Shares started later are left
  alone until the next `sharewindow`. **`shareclear`** drops all redirects.
- **Target closes** → shares fall back to their picked windows; redirects are dropped.
- **Picked window closes while redirected** → the share keeps showing the target. (XDPH keeps
  capturing the closed foreign-toplevel handle, which then resolves to no window at all; the
  plugin recognises that client's dead, redirected capture and keeps serving the target.)
- The window actually being served to an active capture carries the tag **`screenshared`**; it
  moves with redirects and is removed ~1 s after the window stops being served, on clear, and on
  unload.

Everything runs on the compositor main thread (Wayland requests, dispatchers, hyprctl, Lua,
event-loop timers). Nothing touches the XR frame thread.

## Build

The plugin must be compiled against the **exact Hyprland tree the running compositor was built
from** — not `/usr/include/hyprland`, which belongs to stock hyprland with different layouts.

```sh
# 1. Headers tree: detached worktree at the running commit, configured like the hypxrland
#    PKGBUILD (Release, WITH_OPENXR, the package's version stamp), protocol headers generated.
#    (For the r468 package, pass its cosmetic stamp too so version.h is byte-identical.)
GIT_TAG=v0.56.2-468-g9648898995 GIT_COMMITS=8129 GIT_COMMIT_DATE=2026-11-05 scripts/prepare-headers.sh

# 2. Plugin and the test probe
make              # -> build/hyprshare.so
make -C tests     # -> tests/build/capture-probe
make hash         # shows the tree's commit next to the running compositor's
```

`HYPRLAND_SRC=` overrides the headers tree (default
`~/.cache/hypxrland-worktrees/hyprshare-headers`). The Makefile mirrors the defines `hyprland_lib`
is built with in the package (`NDEBUG`, `HAVE_OPENXR=1`, `HAVE_XR_VULKAN_PROBE=1`, …; taken from
the tree's `build-pkg/compile_commands.json`). **Rebuild after every compositor update**: on a
version mismatch the plugin refuses to load (it compares `__hyprland_api_get_hash()` with its own
client hash in `pluginInit`, before hooking anything).

## Load / unload (live session)

```sh
hyprctl plugin load   /home/ajg/code/hyprshare/build/hyprshare.so
hyprctl plugin unload /home/ajg/code/hyprshare/build/hyprshare.so
hyprctl plugin list
```

Unloading removes the hook, the hyprctl commands, the Lua functions, the frame listeners, the timer
and the `screenshared` tags; running shares just go back to the picked windows.

## Use

### hyprctl (works with either config front end)

```sh
hyprctl sharewindow                     # redirect active shares to the focused window
hyprctl sharewindow address:0x55d1...   # ... or to a selected window
hyprctl sharewindow class:firefox
hyprctl shareclear                      # back to the windows picked in the portal
hyprctl sharestatus                     # JSON, see below
```

Selectors are the compositor's own window selectors (`Desktop::CViewQuery::selector`):
`active`, `address:0x…`, `class:<regex>`, `title:<regex>`, `initialclass:`, `initialtitle:`,
`tag:<regex>`, `pid:<n>`, `stableid:<hex>`, `floating`, `tiled`; a bare string is a class regex.

Under the **Lua** config, `hyprctl dispatch X` is evaluated as `hl.dispatch(X)`, so plugin
dispatchers registered by name are *not* reachable through it — use the `hyprctl` commands above,
or `hyprctl eval 'hl.plugin.hyprshare.share("address:0x…")'`. Under a hyprlang `.conf` the
dispatchers `sharewindow` / `shareclear` exist too (`hyprctl dispatch sharewindow …`,
`bind = SUPER CTRL ALT, S, sharewindow`) — that path is registered but was not tested.

### Lua binds

The plugin registers `hl.plugin.hyprshare.share([selector])`, `.clear()` and `.status()`. They act
immediately and return `true` or `false, err` (they never raise). Wrap them in a function so the
config still parses before the plugin is loaded:

```lua
-- Share the focused window instead (SUPER+ALT+S is taken: Omarchy scratchpad + XR stereo toggle).
hl.bind("SUPER + CTRL + ALT + S", function() hl.plugin.hyprshare.share() end)
hl.bind("SUPER + CTRL + ALT + SHIFT + S", function() hl.plugin.hyprshare.clear() end)
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

### `sharestatus`

```json
{
  "target": {"address": "0x55df55a73ca0", "alive": true, "mapped": true, "class": "hsC", "title": "sh"},
  "redirectedOrigins": [
    {"address": "0x55df55847710", "alive": false}
  ],
  "captures": [
    {"pid": 246, "active": true, "redirected": true, "lastSeenMsAgo": 25, "outstandingFrames": 0,
     "origin": {"address": "0x55df55847710", "alive": false},
     "served": {"address": "0x55df55a73ca0", "alive": true, "mapped": true, "class": "hsC", "title": "sh"}}
  ]
}
```

`pid` is the capturing client (XDPH in real use, not the app). Inactive captures are forgotten
after 10 s.

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

`tests/capture-probe` stands in for XDPH: a fresh capture request per frame, printing the
advertised buffer size; `--app-id` uses XDPH's exact request
(`capture_toplevel_with_wlr_toplevel_handle`, keeping the handle after the toplevel closes),
`--handle` the uint variant, `--hold` keeps a frame pending, `--copy` really copies into shm.

Covered (44 checks): load + hash check against the live binary; per-frame sizes before/after
`sharewindow`; two sessions on one origin following; uint-handle path; real pixel copies into a
B-sized buffer after the redirect; tag + border rule moving and expiring; `sharestatus` fields
(also a layout sanity check: class and pid read through the plugin's view of the structs); Lua
functions via `hyprctl eval`, error returns, surviving `hyprctl reload`; `shareclear`; target
closing (fallback); picked window closing (keeps serving the target, both handle paths); a held
frame keeping a capture active; unload (tags gone, commands gone, hook really out).

## Known limitations (spike)

- **Every swap is a resize to the consumer.** XDPH renegotiates PipeWire on size change; XDPH 1.4.1
  has freeze-on-resize bugs (#451/#445) — expect stalls until a patched XDPH is in use.
- **One global target.** All redirected shares show the same window; no per-share targeting.
- **Only shares active at `sharewindow` time** are redirected; a share started later needs another
  `sharewindow`.
- **Picked + target both closed** → the stream stalls exactly as it would without the plugin
  (the compositor rejects captures of a dead window and sends nothing).
- **Screencast indicators may flicker or lie.** Each (client, window) is its own compositor
  screenshare session: on a swap the target's session starts (`screencast>>1,…` IPC event) and
  the picked window's session stops ~500 ms later (`screencast>>0,…`). Anything counting those
  events may show "not sharing" while a redirected share is live. Not verified with a real bar.
- **Only buffer negotiation and copies were tested**, against a probe — not against real XDPH,
  PipeWire or any consuming app (that is what the spike is for).
- XDPH restore tokens still name the picked window.
- The tag is a plain static tag: tagging a window `screenshared` by hand gets undone by the plugin.
- The hook targets a private, non-API compositor function; the plugin is pinned to one exact commit.
