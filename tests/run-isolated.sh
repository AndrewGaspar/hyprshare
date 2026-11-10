#!/usr/bin/env bash
# End-to-end test of hyprshare in an ISOLATED headless compositor.
#
#   tests/run-isolated.sh [path/to/hyprshare.so]
#
# Isolation (this must never touch the live session):
#   * clean environment (env -i): private XDG_RUNTIME_DIR, HOME and XDG dirs under a mktemp dir, no
#     WAYLAND_DISPLAY / DISPLAY / HYPRLAND_INSTANCE_SIGNATURE / DBUS_SESSION_BUS_ADDRESS / NOTIFY_SOCKET;
#   * HYPRLAND_HEADLESS_ONLY=1 (no DRM, no seat), HYPRLAND_NO_SD_VARS=1 + HYPRLAND_NO_SD_NOTIFY=1
#     (never push env into, or notify, the user's systemd manager);
#   * the real hyprctl binary (not the ~/.local/bin ticker shim), pointed at our instance only;
#   * every process is killed by the PID we captured when starting it — never by name.
#
# This Hyprland ignores HYPRLAND_HEADLESS_ONLY ("TODO: headless only" in CCompositor::initServer):
# with no seat it can only start NESTED, under NEST_WAYLAND (absolute path of a Wayland socket).
# tests/run-in-container.sh provides one (a headless labwc in a throwaway container), so nothing
# is drawn on, or connected to, the live session.
#
# HYPRLAND_BIN defaults to the installed hypxrland compositor: the exact binary the live session
# runs, which is the point (plugin ABI is checked against it).

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
PLUGIN="$(realpath "${1:-$ROOT/build/hyprshare.so}")"
HYPRLAND_BIN="${HYPRLAND_BIN:-/usr/lib/hypxrland/Hyprland}"
HYPRCTL_BIN="${HYPRCTL_BIN:-/usr/lib/hypxrland/hyprctl}"
PROBE="$HERE/build/capture-probe"
CONFIG="$HERE/hyprshare-test.lua"
TERM_BIN="${TERM_BIN:-/usr/bin/foot}"
NEST_WAYLAND="${NEST_WAYLAND:-}"
# Extra library dir for the compositor only (e.g. the host's exact libhyprutils inside a container).
HYPRLAND_LIBRARY_PATH="${HYPRLAND_LIBRARY_PATH:-}"

[[ -x "$PROBE" ]] || { echo "build the probe first: make -C $HERE"; exit 2; }
[[ -f "$PLUGIN" ]] || { echo "no plugin at $PLUGIN"; exit 2; }

[[ -S "$NEST_WAYLAND" ]] || { echo "NEST_WAYLAND must be the absolute path of a nesting compositor's socket (see tests/run-in-container.sh)"; exit 2; }

# Short on purpose: $XDG_RUNTIME_DIR/hypr/<64-char signature>/.socket2.sock must fit in sun_path.
T="$(mktemp -d /tmp/hs.XXXXXX)"
mkdir -p "$T/run" "$T/home" "$T/logs"
chmod 0700 "$T/run"

PIDS=()
COMPOSITOR_PID=""
FAILS=0
PASSES=0

cleanup() {
    local pid
    for pid in "${PIDS[@]}"; do
        [[ "$pid" =~ ^[0-9]+$ ]] && ((pid > 1)) && kill "$pid" 2>/dev/null
    done
    if [[ "$COMPOSITOR_PID" =~ ^[0-9]+$ ]] && ((COMPOSITOR_PID > 1)); then
        kill "$COMPOSITOR_PID" 2>/dev/null
        for _ in $(seq 50); do kill -0 "$COMPOSITOR_PID" 2>/dev/null || break; sleep 0.1; done
        kill -9 "$COMPOSITOR_PID" 2>/dev/null
    fi
    cp "$T"/run/hypr/*/hyprland.log "$T/logs/" 2>/dev/null
    if [[ -n "${LOG_EXPORT_DIR:-}" ]]; then
        cp -r "$T/logs/." "$LOG_EXPORT_DIR/" 2>/dev/null
        echo "logs exported to $LOG_EXPORT_DIR"
    else
        echo "logs kept in $T/logs"
    fi
}
trap cleanup EXIT

ISOLATED_ENV=(
    env -i
    PATH=/usr/bin
    HOME="$T/home"
    XDG_RUNTIME_DIR="$T/run"
    XDG_CONFIG_HOME="$T/home/.config"
    XDG_CACHE_HOME="$T/home/.cache"
    XDG_DATA_HOME="$T/home/.local/share"
    XDG_STATE_HOME="$T/home/.local/state"
    LANG=C.UTF-8
)

pass() { PASSES=$((PASSES + 1)); echo "PASS: $*"; }
fail() { FAILS=$((FAILS + 1)); echo "FAIL: $*"; }
expect_eq() { # what expected actual
    if [[ "$2" == "$3" ]]; then pass "$1 ($3)"; else fail "$1: expected '$2', got '$3'"; fi
}

# --- compositor -------------------------------------------------------------------------------
"${ISOLATED_ENV[@]}" HYPRLAND_HEADLESS_ONLY=1 HYPRLAND_NO_SD_VARS=1 HYPRLAND_NO_SD_NOTIFY=1 \
    WAYLAND_DISPLAY="$NEST_WAYLAND" ${HYPRLAND_LIBRARY_PATH:+LD_LIBRARY_PATH="$HYPRLAND_LIBRARY_PATH"} \
    "$HYPRLAND_BIN" --config "$CONFIG" >"$T/logs/hyprland.out" 2>&1 &
COMPOSITOR_PID=$!

HIS=""
WL=""
for _ in $(seq 100); do
    for lock in "$T"/run/hypr/*/hyprland.lock; do
        [[ -f "$lock" ]] || continue
        if [[ "$(sed -n 1p "$lock")" == "$COMPOSITOR_PID" ]]; then
            HIS="$(basename "$(dirname "$lock")")"
            WL="$(sed -n 2p "$lock")"
        fi
    done
    [[ -n "$HIS" && -n "$WL" ]] && break
    kill -0 "$COMPOSITOR_PID" 2>/dev/null || break
    sleep 0.1
done
[[ -n "$HIS" && -n "$WL" ]] || { echo "compositor (pid $COMPOSITOR_PID) did not come up"; tail -30 "$T/logs/hyprland.out"; exit 1; }
echo "isolated compositor pid=$COMPOSITOR_PID instance=$HIS socket=$T/run/$WL"

CLIENT_ENV=("${ISOLATED_ENV[@]}" HYPRLAND_INSTANCE_SIGNATURE="$HIS" WAYLAND_DISPLAY="$WL")
ctl() { "${CLIENT_ENV[@]}" "$HYPRCTL_BIN" "$@"; }

sleep 1
ctl version | head -1
echo "compositor ABI: $(ctl version | sed -n 's/^Version ABI string: //p')"
if [[ -z "$(ctl monitors -j | jq -r '.[0].name // empty')" ]]; then
    ctl output create headless HEADLESS-1 >/dev/null
    sleep 0.5
fi
echo "monitors: $(ctl monitors -j | jq -r '[.[].name] | join(",")')"

# --- plugin -----------------------------------------------------------------------------------
expect_eq "plugin load" "ok" "$(ctl plugin load "$PLUGIN" | tail -1)"
sleep 0.5
ctl plugin list | head -3

# --- windows ----------------------------------------------------------------------------------
spawn_window() { # class
    local classFlag="--app-id=$1"
    [[ "$(basename "$TERM_BIN")" == kitty ]] && classFlag="--class=$1"
    "${CLIENT_ENV[@]}" "$TERM_BIN" "$classFlag" sh -c 'sleep 600' >"$T/logs/$1.out" 2>&1 &
    PIDS+=($!)
    eval "PID_$1=$!"
}
addr_of() { ctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | .address' | head -1; }
size_of() { ctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.size[0])x\(.size[1])"' | head -1; }
tags_of() { ctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | .tags | join(",")' | head -1; }
border_of() { ctl getprop "address:$(addr_of "$1")" active_border_color 2>/dev/null; }

spawn_window hsA
spawn_window hsB
for _ in $(seq 100); do
    [[ -n "$(addr_of hsA)" && -n "$(addr_of hsB)" ]] && break
    sleep 0.1
done
sleep 0.5
A="$(addr_of hsA)"; B="$(addr_of hsB)"
SIZE_A="$(size_of hsA)"; SIZE_B="$(size_of hsB)"
echo "A=$A ($SIZE_A)  B=$B ($SIZE_B)"
[[ -n "$A" && -n "$B" && "$SIZE_A" != "$SIZE_B" ]] || { echo "test windows did not map with distinct sizes"; exit 1; }

# --- probes -----------------------------------------------------------------------------------
# p1, p2: two sessions on A through the wlr-handle path (XDPH's request; Electron opens several).
# p3: one on A through the uint-handle path.
start_probe() { # name args...
    local name="$1"
    shift
    "${CLIENT_ENV[@]}" "$PROBE" "$@" >"$T/logs/$name.out" 2>"$T/logs/$name.err" &
    PIDS+=($!)
    eval "PID_$name=$!"
}
last_size() { tail -1 "$T/logs/$1.out" | awk '{print $3}'; }
status() { ctl sharestatus; }

start_probe p1 --app-id hsA --interval-ms 50
start_probe p2 --app-id hsA --interval-ms 80
start_probe p3 --handle "$((A & 0xFFFFFFFF))" --interval-ms 50
# cp: really copies pixels (shm, ignore_damage) so the render path runs for redirected frames too.
start_probe cp --app-id hsA --copy --interval-ms 100
sleep 1

expect_eq "p1 captures A before any redirect" "$SIZE_A" "$(last_size p1)"
expect_eq "p3 (uint handle) captures A" "$SIZE_A" "$(last_size p3)"
expect_eq "cp copies A" "$SIZE_A ready" "$(tail -1 "$T/logs/cp.out" | cut -d' ' -f3-)"
expect_eq "sharestatus sees one active capture per client" "4" "$(status | jq '[.captures[] | select(.active)] | length')"
expect_eq "A tagged while shared" "screenshared" "$(tags_of hsA)"
expect_eq "border rule colors A" "ffff3030 0deg" "$(border_of hsA)"

# --- redirect A -> B via hyprctl command --------------------------------------------------------
expect_eq "hyprctl sharewindow address:B" "ok" "$(ctl sharewindow "address:$B")"
sleep 0.6
expect_eq "p1 now sees B" "$SIZE_B" "$(last_size p1)"
expect_eq "p2 (second session, same origin) follows" "$SIZE_B" "$(last_size p2)"
expect_eq "p3 follows" "$SIZE_B" "$(last_size p3)"
sleep 0.3
expect_eq "cp copies B's pixels into a B-sized buffer" "$SIZE_B ready" "$(tail -1 "$T/logs/cp.out" | cut -d' ' -f3-)"
kill "$PID_cp" 2>/dev/null
expect_eq "tag moved to B" "screenshared" "$(tags_of hsB)"
sleep 1.2
expect_eq "tag removed from A ~1s after it stopped being served" "" "$(tags_of hsA)"
expect_eq "sharestatus target is B" "$B" "$(status | jq -r '.target.address')"
# Fields read through the plugin's view of CWindow / wl_client: garbage here would mean an ABI mismatch.
expect_eq "sharestatus reads CWindow::m_class (layout sanity)" "hsB" "$(status | jq -r '.target.class')"
expect_eq "sharestatus reports the probe's pid" "$PID_p1" "$(status | jq -r --argjson pid "$PID_p1" '.captures[] | select(.pid == $pid) | .pid')"
expect_eq "sharestatus served is B" "$B" "$(status | jq -r '[.captures[].served.address] | unique | join(",")')"

# --- Lua function, via the same Lua state binds use ----------------------------------------------
expect_eq "hl.plugin.hyprshare.share(address:A) via eval" "ok" "$(ctl eval "assert(hl.plugin.hyprshare.share('address:$A'))" | tail -1)"
sleep 0.6
expect_eq "retargeted back to A" "$SIZE_A" "$(last_size p1)"
expect_eq "bad selector returns false, err (no raise)" "ok" "$(ctl eval "local ok, err = hl.plugin.hyprshare.share('address:0xdead'); assert(ok == false and err:find('no window matches'))" | tail -1)"
ctl reload >/dev/null
sleep 1
expect_eq "Lua functions survive a config reload" "ok" "$(ctl eval "assert(hl.plugin.hyprshare and hl.plugin.hyprshare.status():find('captures'))" | tail -1)"
expect_eq "status() returns JSON" "ok" "$(ctl eval "assert(hl.plugin.hyprshare.status():find('captures'))" | tail -1)"

# --- clear ------------------------------------------------------------------------------------
ctl sharewindow "address:$B" >/dev/null
sleep 0.6
expect_eq "shareclear" "ok" "$(ctl shareclear)"
sleep 0.6
expect_eq "after clear p1 is back on A" "$SIZE_A" "$(last_size p1)"
expect_eq "after clear: no redirects" "0" "$(status | jq '.redirectedOrigins | length')"
sleep 1.2
expect_eq "after clear: tag back on A only" "screenshared|" "$(tags_of hsA)|$(tags_of hsB)"

# --- target closes -> fall back to the origin ------------------------------------------------------
ctl sharewindow "address:$B" >/dev/null
sleep 0.6
expect_eq "redirected to B again" "$SIZE_B" "$(last_size p1)"
kill "$PID_hsB"
sleep 1.5
expect_eq "target B closed: p1 falls back to A" "$SIZE_A" "$(last_size p1)"
expect_eq "target B closed: redirects dropped" "0" "$(status | jq '.redirectedOrigins | length')"
expect_eq "compositor alive" "0" "$(kill -0 "$COMPOSITOR_PID" 2>/dev/null; echo $?)"

# --- origin closes while redirected -> keep serving the target -------------------------------------
spawn_window hsC
for _ in $(seq 100); do [[ -n "$(addr_of hsC)" ]] && break; sleep 0.1; done
sleep 0.3
C="$(addr_of hsC)"; SIZE_C="$(size_of hsC)"
expect_eq "sharewindow class:hsC (regex selector)" "ok" "$(ctl sharewindow class:hsC)"
sleep 0.6
expect_eq "redirected to C" "$SIZE_C" "$(last_size p1)"
kill "$PID_hsA"
sleep 1.5
expect_eq "origin A closed: p1 (wlr handle) still sees C" "$SIZE_C" "$(last_size p1)"
expect_eq "origin A closed: p2 still sees C" "$SIZE_C" "$(last_size p2)"
expect_eq "origin A closed: p3 (uint handle, resolves to nothing now) still sees C" "$SIZE_C" "$(last_size p3)"
expect_eq "C tagged" "screenshared" "$(tags_of hsC)"

# --- a frame held open (waiting for damage) keeps the capture active -------------------------------
start_probe hold --app-id hsC --hold
sleep 2
expect_eq "held frame: capture still active after 2s" "true" "$(status | jq -r --argjson pid "$PID_hold" '[.captures[] | select(.pid == $pid)][0] | "\(.active)"')"
expect_eq "held frame: outstandingFrames" "1" "$(status | jq -r --argjson pid "$PID_hold" '[.captures[] | select(.pid == $pid)][0].outstandingFrames')"

status >"$T/logs/status.json"

# --- unload -----------------------------------------------------------------------------------
expect_eq "plugin unload" "ok" "$(ctl plugin unload "$PLUGIN" | tail -1)"
sleep 0.5
expect_eq "tag removed on unload" "" "$(tags_of hsC)"
expect_eq "sharestatus gone after unload" "unknown request" "$(ctl sharestatus)"
sleep 0.5
expect_eq "compositor alive after unload" "0" "$(kill -0 "$COMPOSITOR_PID" 2>/dev/null; echo $?)"

# Without the plugin, p1's request for the dead origin is served by the stock path again (the
# compositor rejects it, so the probe reports "none"): proves the hook really came out.
sleep 2.5
P1_AFTER="$(last_size p1)"
if [[ "$P1_AFTER" != "$SIZE_C" ]]; then pass "after unload p1 no longer redirected ($P1_AFTER)"; else fail "after unload p1 still sees C"; fi

grep -i "hyprshare\|captureToplevel\|Couldn't capture" "$T/run/hypr/$HIS/hyprland.log" 2>/dev/null | tail -10 > "$T/logs/plugin-lines.log"

echo "--- final sharestatus sample (pre-unload) is in $T/logs/status.json"
echo
echo "$PASSES passed, $FAILS failed"
((FAILS == 0))
