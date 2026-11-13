# Bar suite: sourced by run-isolated.sh (SUITE=bar) after the compositor is up, the plugin is loaded
# and windows hsA / hsB are mapped. Covers the status-bar API: `capturing` / `sessions` /
# `sharedWindow` in sharestatus, the `hyprshare>>changed` socket2 event, `sharefocus` and `sharestop`
# (with its grace window). Uses run-isolated.sh's helpers (ctl, CLIENT_ENV, start_probe, status,
# expect_eq, expect_match, PIDS, T, HERE, HIS, GLYPH_CONF, addr_of, sharetag_of, tags_of).
#
# Capture clients: tests/build/output-probe stands in for wf-recorder / a portal's monitor share
# (wlr-screencopy, or ext-image-copy-capture with --ext); tests/build/capture-probe for XDPH's
# window share. Events: socat reads socket2 into a log.

OUTPUT_PROBE="$HERE/build/output-probe"
[[ -x "$OUTPUT_PROBE" ]] || { fail "build the probes first: make -C $HERE"; return; }

start_out() { # name args...
    local name="$1"
    shift
    "${CLIENT_ENV[@]}" "$OUTPUT_PROBE" "$@" >"$T/logs/$name.out" 2>"$T/logs/$name.err" &
    PIDS+=($!)
    eval "PID_$name=$!"
}

EVENTS="$T/logs/events.log"
: >"$EVENTS"
socat -u "UNIX-CONNECT:$T/run/hypr/$HIS/.socket2.sock" "OPEN:$EVENTS,creat,append" 2>"$T/logs/socat.err" &
PIDS+=($!)
sleep 0.3

changed_n() { grep -c '^hyprshare>>changed$' "$EVENTS"; }
wait_changed() { # since-count -> 0 if a new `changed` arrived within 3 s
    for _ in $(seq 30); do
        (($(changed_n) > $1)) && return 0
        sleep 0.1
    done
    return 1
}
expect_changed() { # what since-count
    expect_eq "$1: hyprshare>>changed posted" "yes" "$(wait_changed "$2" && echo yes || echo no)"
}
lines_after() { tail -n "+$(($2 + 1))" "$T/logs/$1.out"; } # probe mark
count_after() { lines_after "$1" "$2" | grep -c -- "$3"; }  # probe mark pattern
nlines() { wc -l <"$T/logs/$1.out"; }
lua_eval() { ctl eval "$1" | tail -1; }
dsp() { ctl eval "hl.dispatch($1)" >/dev/null; }
active_ws() { ctl activeworkspace -j | jq -r '.id'; }
active_win() { ctl activewindow -j | jq -r '.address'; }
sessions_where() { status | jq -r "[.sessions[] | select($1)] | length"; }
MON="$(ctl monitors -j | jq -r '.[0].name')"

echo "--- bar suite (monitor $MON)"

# --- idle ------------------------------------------------------------------------------------------
expect_eq "idle: capturing false" "false" "$(status | jq -r '.capturing')"
expect_eq "idle: anyShare false" "false" "$(status | jq -r '.anyShare')"
expect_eq "idle: sharedWindow null" "null" "$(status | jq -r '.sharedWindow')"
expect_eq "idle: no sessions" "0" "$(status | jq '.sessions | length')"
expect_eq "idle: sharestatus is valid JSON with the old fields" "true" "$(status | jq 'has("shareMode") and has("target") and has("paused") and has("captures") and has("glyph")')"
expect_eq "idle: sharefocus with no share" "no window share" "$(ctl sharefocus)"
expect_eq "idle: Lua focus() returns false, 'no window share'" "ok" "$(lua_eval "local ok, err = hl.plugin.hyprshare.focus(); assert(ok == false and err == 'no window share')")"
N0="$(changed_n)"
sleep 1
expect_eq "idle: no changed events while nothing happens" "$N0" "$(changed_n)"

# --- monitor capture (wlr-screencopy, like wf-recorder) --------------------------------------------
N0="$(changed_n)"
start_out m1 --interval-ms 100
sleep 1
expect_match "m1 copies the monitor" "frame * ready" "$(tail -1 "$T/logs/m1.out")"
expect_eq "monitor capture: capturing true" "true" "$(status | jq -r '.capturing')"
expect_eq "monitor capture: anyShare true, shareMode false" "true|false" "$(status | jq -r '"\(.anyShare)|\(.shareMode)"')"
expect_eq "monitor capture: session listed (type, sharing, pid, monitor, window)" "monitor|true|$PID_m1|$MON|" \
    "$(status | jq -r --argjson p "$PID_m1" '[.sessions[] | select(.pid == $p)][0] | "\(.type)|\(.sharing)|\(.pid)|\(.monitor)|\(.window)"')"
expect_changed "monitor capture started" "$N0"
expect_eq "built-in screencast event seen too" "1" "$(grep -c '^screencast>>1,monitor$' "$EVENTS")"

start_out r1 --region 10 10 200 100 --interval-ms 100
sleep 1
expect_match "r1 copies a region" "frame * 200x100 ready" "$(tail -1 "$T/logs/r1.out")"
expect_eq "region capture: a sharing region session from r1" "1" "$(sessions_where ".pid == $PID_r1 and .type == \"region\" and .sharing")"
kill "$PID_r1" 2>/dev/null

N0="$(changed_n)"
kill "$PID_m1" 2>/dev/null
sleep 1.2
expect_eq "monitor capture stopped: capturing false (after the 500ms stop timer)" "false" "$(status | jq -r '.capturing')"
expect_changed "monitor capture stopped" "$N0"
expect_eq "built-in screencast stop event seen" "true" "$(grep -q '^screencast>>0,monitor$' "$EVENTS" && echo true || echo false)"
# The compositor never stops a disconnected client's screencopy session; it stays listed, not sharing.
expect_eq "dead client's leftover sessions are listed as clientAlive false, pid -1, not sharing" "0" "$(sessions_where '.clientAlive == false and (.pid != -1 or .sharing)')"

# --- ext-image-copy-capture monitor capture ----------------------------------------------------------
start_out x1 --ext --interval-ms 100
sleep 1
expect_match "x1 copies the monitor (ext-image-copy-capture)" "frame * ready" "$(tail -1 "$T/logs/x1.out")"
# (A leftover session of the dead m1 may now report x1's pid: a new client can reuse a dead one's
# wl_client address. So look for x1's sharing session rather than its first one.)
expect_eq "ext capture: a sharing monitor session from x1" "1" "$(sessions_where ".pid == $PID_x1 and .type == \"monitor\" and .sharing")"
kill "$PID_x1" 2>/dev/null
sleep 1.2
expect_eq "ext client gone: its session stopped (not listed)" "0" "$(sessions_where ".pid == $PID_x1")"

# --- window share ------------------------------------------------------------------------------------
N0="$(changed_n)"
start_probe w1 --xdph --app-id hsA --copy --interval-ms 50 # copies: a session only "shares" once a frame is copied
sleep 1
expect_eq "window share: shareMode + capturing" "true|true" "$(status | jq -r '"\(.shareMode)|\(.capturing)"')"
expect_eq "window share: sharedWindow live on A" "live|$A|true|hsA" "$(status | jq -r '.sharedWindow | "\(.state)|\(.address)|\(.alive)|\(.class)"')"
expect_eq "window share: session is a window session on A, from w1" "window|true|$A" \
    "$(status | jq -r --argjson p "$PID_w1" '[.sessions[] | select(.pid == $p and .sharing)][0] | "\(.type)|\(.sharing)|\(.window)"')"
expect_changed "window share started" "$N0"
# w1 may have reused the dead m1/r1/x1's wl_client address; their leftover sessions stay orphaned.
expect_eq "leftover sessions of dead clients are not attributed to w1" "0" "$(sessions_where ".pid == $PID_w1 and .type != \"window\"")"

N0="$(changed_n)"
expect_eq "sharewindow address:B" "ok" "$(ctl sharewindow "address:$B")"
expect_changed "retarget" "$N0"
expect_eq "retarget: sharedWindow is B immediately" "live|$B|hsB" "$(status | jq -r '.sharedWindow | "\(.state)|\(.address)|\(.class)"')"
sleep 1
expect_eq "retarget: B's session sharing, A's stopped sharing" "$B" "$(status | jq -r --argjson p "$PID_w1" '[.sessions[] | select(.pid == $p and .sharing) | .window] | join(",")')"
expect_eq "retarget: capturing stays true" "true" "$(status | jq -r '.capturing')"
start_out m2 --interval-ms 200 # a monitor capture alongside, for the README sample
sleep 0.8
status >"$T/logs/bar-status-sharing.json"
kill "$PID_m2" 2>/dev/null
sleep 0.8

# Steady share: frames keep flowing, nothing changes -> no events (fingerprint dedup).
N0="$(changed_n)"
sleep 1.5
expect_eq "steady share: no changed events" "$N0" "$(changed_n)"

N0="$(changed_n)"
expect_eq "sharepause" "ok" "$(ctl sharepause)"
expect_changed "pause" "$N0"
expect_eq "paused: sharedWindow state paused, still names B" "paused|$B" "$(status | jq -r '.sharedWindow | "\(.state)|\(.address)"')"
sleep 1
expect_eq "paused: capturing false (placeholder only), anyShare true" "false|true" "$(status | jq -r '"\(.capturing)|\(.anyShare)"')"

N0="$(changed_n)"
expect_eq "resume via sharewindow" "ok" "$(ctl sharewindow "address:$B")"
expect_changed "resume" "$N0"
sleep 0.6
expect_eq "resumed: live on B, capturing" "live|$B|true" "$(status | jq -r '"\(.sharedWindow.state)|\(.sharedWindow.address)|\(.capturing)"')"

# Coalescing: state changes within one event-loop iteration (one hyprctl batch) are checked once,
# against the last posted state: a round trip B -> A -> B posts nothing.
N0="$(changed_n)"
expect_eq "batch round trip" "ok


ok" "$(ctl --batch "sharewindow address:$A ; sharewindow address:$B")"
sleep 0.8
expect_eq "B -> A -> B in one iteration: no changed event" "$N0" "$(changed_n)"
expect_eq "batch landed back on B" "live|$B" "$(status | jq -r '.sharedWindow | "\(.state)|\(.address)"')"

# --- sharefocus ----------------------------------------------------------------------------------------
dsp "hl.dsp.window.move({ workspace = \"2\", follow = false, window = \"address:$B\" })"
dsp "hl.dsp.focus({ window = \"address:$A\" })"
sleep 0.4
expect_eq "B moved away: workspace 1 active, A focused" "1|$A" "$(active_ws)|$(active_win)"
expect_eq "sharefocus" "ok" "$(ctl sharefocus)"
sleep 0.3
expect_eq "sharefocus switched to B's workspace and focused B" "2|$B" "$(active_ws)|$(active_win)"
expect_eq "sharedWindow reports B's workspace" "2" "$(status | jq -r '.sharedWindow.workspace')"

dsp "hl.dsp.window.move({ workspace = \"special:hs\", follow = false, window = \"address:$B\" })"
dsp "hl.dsp.focus({ workspace = \"1\" })"
dsp "hl.dsp.focus({ window = \"address:$A\" })"
sleep 0.4
expect_eq "B on a hidden special workspace: special not shown" "" "$(ctl monitors -j | jq -r '.[0].specialWorkspace.name')"
expect_eq "Lua focus() on a special-workspace window" "ok" "$(lua_eval "assert(hl.plugin.hyprshare.focus() == true)")"
sleep 0.3
expect_eq "focus() showed the special workspace and focused B" "special:hs|$B" "$(ctl monitors -j | jq -r '.[0].specialWorkspace.name')|$(active_win)"
dsp "hl.dsp.window.move({ workspace = \"1\", follow = false, window = \"address:$B\" })"
dsp "hl.dsp.focus({ workspace = \"1\" })"
sleep 0.4

# Paused: focus goes to the window a resume would return to.
ctl sharepause >/dev/null
dsp "hl.dsp.focus({ window = \"address:$A\" })"
sleep 0.2
expect_eq "sharefocus while paused focuses the paused window" "ok|$B" "$(ctl sharefocus)|$(sleep 0.2; active_win)"
ctl sharewindow "address:$B" >/dev/null

# Dead target: the served window closes.
spawn_window hsC
for _ in $(seq 100); do [[ -n "$(addr_of hsC)" ]] && break; sleep 0.1; done
sleep 1
C="$(addr_of hsC)"
ctl sharewindow "address:$C" >/dev/null
sleep 0.6
expect_eq "shared to C" "live|$C" "$(status | jq -r '.sharedWindow | "\(.state)|\(.address)"')"
N0="$(changed_n)"
kill "$PID_hsC"
expect_changed "served window closed" "$N0"
sleep 1
expect_eq "served window closed: sharedWindow closed, not alive, no address" "closed|false|" "$(status | jq -r '.sharedWindow | "\(.state)|\(.alive)|\(.address)"')"
expect_eq "sharefocus with the shared window gone" "shared window is gone" "$(ctl sharefocus)"
expect_eq "Lua focus() with the shared window gone" "ok" "$(lua_eval "local ok, err = hl.plugin.hyprshare.focus(); assert(ok == false and err == 'shared window is gone')")"
expect_eq "compositor alive" "0" "$(kill -0 "$COMPOSITOR_PID" 2>/dev/null; echo $?)"

# --- sharestop ---------------------------------------------------------------------------------------------
kill "$PID_w1" 2>/dev/null
sleep 0.5
start_out sm --interval-ms 100                    # portal-style monitor share: retries on failed
start_out sx --ext --interval-ms 100              # ext-image-copy-capture: re-creates its session
start_out sw --exit-on-failed --interval-ms 100   # wf-recorder-style: exits on the first failure
start_probe st --xdph --app-id hsA --copy --interval-ms 60 # XDPH window share: re-requests on failed
sleep 1.5
expect_eq "before sharestop: capturing" "true" "$(status | jq -r '.capturing')"
expect_eq "before sharestop: all four clients have a sharing session" "4" \
    "$(status | jq --argjson a "$PID_sm" --argjson b "$PID_sx" --argjson c "$PID_sw" --argjson d "$PID_st" '[.sessions[] | select(.sharing and (.pid == $a or .pid == $b or .pid == $c or .pid == $d)) | .pid] | unique | length')"
expect_eq "before sharestop: st shares A (screenshared tag)" "screenshared" "$(sharetag_of hsA)"
LIVE_BEFORE="$(status | jq '.sessions | length')"
SM0="$(nlines sm)"; SX0="$(nlines sx)"; ST0="$(nlines st)"
N0="$(changed_n)"
STOP_REPLY="$(ctl sharestop)"
expect_eq "sharestop returns ok <every live session>" "ok $LIVE_BEFORE" "$STOP_REPLY"
expect_changed "sharestop" "$N0"
sleep 0.3
expect_eq "after sharestop: capturing false" "false" "$(status | jq -r '.capturing')"
expect_eq "after sharestop: no session sharing" "0" "$(sessions_where '.sharing')"
expect_eq "after sharestop: redirect forgot everything" "false|null|null|0" "$(status | jq -r '"\(.shareMode)|\(.target)|\(.paused)|\(.captures | length)"')"
expect_eq "after sharestop: no screenshared / screenshare-mode tags" "" "$(tags_of hsA)"
expect_eq "after sharestop: grace window active, names the stopped clients" "true|true" \
    "$(status | jq -r --argjson p "$PID_sm" '"\(.stopGrace.active)|\(.stopGrace.pids | index($p) != null)"')"
sleep 1.2 # ~1.5 s into the 2 s grace window
expect_eq "grace: monitor client's retries all refused (no frame copied)" "0" "$(count_after sm "$SM0" ready)"
expect_eq "grace: monitor client saw failed frames" "true" "$( (($(count_after sm "$SM0" failed) > 0)) && echo true || echo false)"
expect_eq "grace: ext client told stopped, no frame copied" "true|0" "$( (($(count_after sx "$SX0" 'session stopped') > 0)) && echo true || echo false)|$(count_after sx "$SX0" ready)"
expect_eq "grace: window-share client's re-requests all failed (no window served)" "0" "$(count_after st "$ST0" 'x[0-9]')"
expect_eq "grace: window-share client got failed frames" "true" "$( (($(count_after st "$ST0" failed) > 0)) && echo true || echo false)"
expect_eq "grace: window-share retries are paced (<= ~10/s, not a tight loop)" "true" "$( (($(count_after st "$ST0" failed) <= 20)) && echo true || echo false)"
expect_eq "grace: wf-recorder-style client exited on failure" "1|failed" "$(kill -0 "$PID_sw" 2>/dev/null; echo $?)|$(tail -1 "$T/logs/sw.out" | awk '{print $3}')"
expect_eq "grace: still not capturing, no capture record" "false|0" "$(status | jq -r '"\(.capturing)|\(.captures | length)"')"
sleep 1.6 # past the grace window
expect_eq "after grace: monitor client captures again" "true" "$( (($(count_after sm "$SM0" ready) > 0)) && echo true || echo false)"
expect_eq "after grace: ext client captures again" "true" "$( (($(count_after sx "$SX0" ready) > 0)) && echo true || echo false)"
expect_eq "after grace: window-share client served again (its pick, A)" "$SIZE_A" "$(lines_after st "$ST0" | grep 'x[0-9]' | tail -1 | awk '{print $3}')"
expect_eq "after grace: capturing again, grace over" "true|false" "$(status | jq -r '"\(.capturing)|\(.stopGrace.active)"')"
expect_eq "compositor alive after sharestop" "0" "$(kill -0 "$COMPOSITOR_PID" 2>/dev/null; echo $?)"
expect_eq "no protocol errors in the stopped clients" "0" "$(cat "$T"/logs/{sm,sx,sw,st}.{out,err} 2>/dev/null | grep -ci 'display-error\|invalid object\|protocol error')"

# --- Lua stop() + a configured (short) grace -----------------------------------------------------------
printf '%s\n' 'if hl.plugin.hyprshare then hl.config({ plugin = { hyprshare = { stop_grace_ms = 500 } } }) end' >"$GLYPH_CONF"
ctl reload >/dev/null
sleep 1
SM0="$(nlines sm)"
expect_eq "Lua stop() returns true, <number>" "ok" "$(lua_eval "local ok, n = hl.plugin.hyprshare.stop(); assert(ok == true and type(n) == 'number' and n >= 2)")"
sleep 0.3
expect_eq "Lua stop(): capturing false" "false" "$(status | jq -r '.capturing')"
sleep 1
expect_eq "stop_grace_ms = 500: monitor client captures again within ~1.3 s" "true" "$( (($(count_after sm "$SM0" ready) > 0)) && echo true || echo false)"
expect_eq "stop_grace_ms = 500: refused at first" "true" "$( (($(count_after sm "$SM0" failed) > 0)) && echo true || echo false)"

kill "$PID_sm" "$PID_sx" "$PID_st" 2>/dev/null
sleep 0.5
status >"$T/logs/bar-status.json"
