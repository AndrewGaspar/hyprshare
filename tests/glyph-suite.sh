# Glyph suite: sourced by run-isolated.sh (SUITE=glyph) after the compositor is up and the plugin
# is loaded. Uses its helpers: ctl, CLIENT_ENV, start_probe, status, expect_eq, expect_match, PIDS,
# T, HERE, COMPOSITOR_PID, PLUGIN, GLYPH_CONF, tags_of, addr_of.
#
# Input injection: tests/build/vpointer (zwlr_virtual_pointer_v1 — a real pointer device to the
# compositor, so clicks go through CInputManager::onMouseButton exactly like a mouse).
# On-screen pixels: grim (screencopy of the nested output). Stream pixels: capture-probe --sample.

LOGGER="$HERE/build/click-logger"
VPOINTER="$HERE/build/vpointer"

spawn_logger() { # app-id rrggbb
    "${CLIENT_ENV[@]}" "$LOGGER" "$1" "$2" >"$T/logs/$1.out" 2>"$T/logs/$1.err" &
    PIDS+=($!)
    eval "PID_$1=$!"
}
win_xywh() { ctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.at[0]) \(.at[1]) \(.size[0]) \(.size[1])"' | head -1; }
glyph_of() { # address jq-expr-on-glyph
    status | jq -r --arg a "$1" "[.glyph.glyphs[] | select(.address == \$a)][0] | $2"
}
glyph_center() { # address -> "x y"
    status | jq -r --arg a "$1" '[.glyph.glyphs[] | select(.address == $a)][0].box | "\((.[0] + .[2] / 2) | floor) \((.[1] + .[3] / 2) | floor)"'
}
glyph_count() { status | jq '.glyph.glyphs | length'; }
MON_W="$(ctl monitors -j | jq '.[0].width')"
MON_H="$(ctl monitors -j | jq '.[0].height')"
vp() { "${CLIENT_ENV[@]}" VP_EXTENT="${MON_W}x${MON_H}" "$VPOINTER" "$@"; }
pixel() { # x y -> rrggbb on screen (the cursor is parked first: grim would capture it)
    vp move 3 3 >/dev/null 2>&1
    sleep 0.2
    "${CLIENT_ENV[@]}" grim -g "$1,$2 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tx1 | tr -d ' \n'
}
is_reddish() { local p="$1"; ((16#${p:0:2} > 0xc0 && 16#${p:2:2} < 0x70 && 16#${p:4:2} < 0x70)); }
buttons_of() { grep -c '^button' "$T/logs/$1.out"; }
capture_of() { # probe-name jq-expr-on-capture
    local pid
    pid="$(eval echo "\$PID_$1")"
    status | jq -r --argjson pid "$pid" "[.captures[] | select(.pid == \$pid)][0] | $2"
}
wait_class() { for _ in $(seq 100); do [[ -n "$(addr_of "$1")" ]] && break; sleep 0.1; done; sleep 1; }

echo "--- glyph suite (monitor ${MON_W}x${MON_H})"

spawn_logger gA 00ff00
spawn_logger gB 0000ff
wait_class gA
wait_class gB
GA="$(addr_of gA)"; GB="$(addr_of gB)"
read -r AX AY AW AH <<<"$(win_xywh gA)"
read -r BX BY BW BH <<<"$(win_xywh gB)"
echo "gA=$GA at $AX,$AY ${AW}x$AH   gB=$GB at $BX,$BY ${BW}x$BH"
expect_eq "logger windows have distinct sizes" "true" "$([[ "${AW}x$AH" != "${BW}x$BH" ]] && echo true || echo false)"

# --- no share: no glyphs at all -----------------------------------------------------------------
expect_eq "no share: shareMode false" "false" "$(status | jq -r '.shareMode')"
expect_eq "no share: no glyphs" "0" "$(glyph_count)"
expect_eq "decoration attached even when idle (draws nothing)" "1" "$(ctl decorations "address:$GA" | grep -c hyprshare-glyph)"
GX=$((AX + AW - 10 - 7)); GY=$((AY + 10 + 7)) # default top_right, offset 10, size 14: centre
expect_eq "no share: screen at gA's glyph spot is the window colour" "00ff00" "$(pixel "$GX" "$GY")"
expect_eq "no share: not tagged screenshare-mode" "0" "$(tags_of gA | grep -c screenshare-mode)"

# --- share gA: glyphs appear ---------------------------------------------------------------------
# gs copies via shm and samples the pixel where the glyph sits on screen (window-relative).
start_probe gs --xdph --app-id gA --copy --sample $((AW - 10 - 7)) $((10 + 7)) --interval-ms 100
sleep 1.5
expect_eq "share active: shareMode true" "true" "$(status | jq -r '.shareMode')"
expect_eq "gA glyph LIVE" "live" "$(glyph_of "$GA" .state)"
expect_eq "gB glyph IDLE" "idle" "$(glyph_of "$GB" .state)"
expect_eq "gA glyph box: default top_right, inset 10, size 14" "$((AX + AW - 24)) $((AY + 10)) 14 14" "$(glyph_of "$GA" '.box | map(floor) | join(" ")')"
P="$(pixel "$GX" "$GY")"
expect_eq "LIVE glyph drawn on screen (reddish, got $P)" "true" "$(is_reddish "$P" && echo true || echo false)"
expect_match "stream readback at the glyph spot is window colour, not glyph" "${AW}x$AH ready-nonblack px=00ff00" "$(grep -E '^frame ' "$T/logs/gs.out" | tail -1 | cut -d' ' -f3-)"
expect_eq "share mode tags every window: gA" "1" "$(tags_of gA | grep -c screenshare-mode)"
expect_eq "share mode tags every window: gB" "1" "$(tags_of gB | grep -c screenshare-mode)"
expect_eq "README rules: share-mode border on the idle window" "80808080 0deg" "$(ctl getprop "address:$GB" active_border_color)"
expect_eq "README rules: shared window keeps the screenshared border" "ffff3030 0deg" "$(ctl getprop "address:$GA" active_border_color)"

# --- click gB's IDLE glyph: share switches, click consumed --------------------------------------
read -r CX CY <<<"$(glyph_center "$GB")"
vp click "$CX" "$CY"
sleep 0.8
expect_eq "IDLE glyph click switched the share to gB" "${BW}x$BH" "$(grep -E '^frame ' "$T/logs/gs.out" | tail -1 | awk '{print $3}')"
expect_eq "gB glyph now LIVE" "live" "$(glyph_of "$GB" .state)"
expect_eq "gA glyph now IDLE" "idle" "$(glyph_of "$GA" .state)"
expect_eq "glyph click never reached gB's client" "0" "$(buttons_of gB)"

# --- a click elsewhere in the window is untouched -----------------------------------------------
vp click $((BX + BW / 2)) $((BY + BH / 2))
sleep 0.4
expect_eq "click on gB's content reached the client (press+release)" "2" "$(buttons_of gB)"

# --- click the LIVE glyph: pause (placeholder, sticky NOTHING, remembered window) ----------------
read -r CX CY <<<"$(glyph_center "$GB")"
vp click "$CX" "$CY"
sleep 0.8
expect_eq "LIVE glyph click paused: capture state nothing" "nothing" "$(capture_of gs .state)"
expect_eq "pause reason reported" "paused" "$(capture_of gs .nothingReason)"
expect_eq "sharestatus paused window is gB" "$GB" "$(status | jq -r '.paused.address')"
expect_eq "gB glyph PAUSED" "paused" "$(glyph_of "$GB" .state)"
expect_match "paused stream is the black placeholder" "${BW}x$BH ready-black*" "$(grep -E '^frame ' "$T/logs/gs.out" | tail -1 | cut -d' ' -f3-)"
expect_eq "paused: screenshared tag on nobody" "0" "$( (tags_of gA; tags_of gB) | grep -c 'screenshared')"
expect_eq "paused: still in share mode (screenshare-mode stays)" "1" "$(tags_of gB | grep -c screenshare-mode)"
expect_eq "pause click consumed too" "2" "$(buttons_of gB)"

# --- click the PAUSED glyph: resume -------------------------------------------------------------
read -r CX CY <<<"$(glyph_center "$GB")"
vp click "$CX" "$CY"
sleep 0.8
expect_eq "PAUSED glyph click resumed: state target" "target" "$(capture_of gs .state)"
expect_eq "resumed onto gB" "$GB" "$(capture_of gs .served.address)"
expect_match "resumed stream shows gB again" "${BW}x$BH ready-nonblack*" "$(grep -E '^frame ' "$T/logs/gs.out" | tail -1 | cut -d' ' -f3-)"
expect_eq "paused cleared" "null" "$(status | jq -r '.paused')"

# --- a drag that starts on the glyph does nothing (and is still not leaked to the client) --------
read -r CX CY <<<"$(glyph_center "$GB")"
vp drag "$CX" "$CY" $((CX - 60)) $((CY + 60))
sleep 0.6
expect_eq "drag from the glyph: no state change" "target|live" "$(capture_of gs .state)|$(glyph_of "$GB" .state)"
expect_eq "drag from the glyph: press/release not leaked" "2" "$(buttons_of gB)"

# --- keybind parity: sharepause / Lua pause, then sharewindow resumes -----------------------------
expect_eq "hyprctl sharepause" "ok" "$(ctl sharepause)"
sleep 0.5
expect_eq "sharepause: state nothing (paused)" "nothing|paused" "$(capture_of gs .state)|$(capture_of gs .nothingReason)"
expect_eq "Lua hl.plugin.hyprshare.pause() while already paused" "ok" "$(ctl eval "assert(hl.plugin.hyprshare.pause())" | tail -1)"
expect_eq "sharewindow gA resumes" "ok" "$(ctl sharewindow class:gA)"
sleep 0.8
expect_eq "gA LIVE again" "live" "$(glyph_of "$GA" .state)"

# --- config: corner moves the hit box (applied on reload); shared_only hides IDLE ----------------
cat >"$GLYPH_CONF" <<'LUA'
if hl.plugin.hyprshare then
    hl.config({ plugin = { hyprshare = { corner = "top_left", offset_x = 12, offset_y = 8 } } })
end
LUA
ctl reload >/dev/null
sleep 1
expect_eq "corner=top_left applied on reload (guarded plugin config)" "top_left" "$(status | jq -r '.glyph.corner')"
expect_eq "gB IDLE glyph box moved to top_left with new offsets" "$((BX + 12)) $((BY + 8))" "$(glyph_of "$GB" '.box[0:2] | map(floor) | join(" ")')"
read -r CX CY <<<"$(glyph_center "$GB")"
vp click "$CX" "$CY"
sleep 0.8
expect_eq "clicking the moved hit box shares gB" "live" "$(glyph_of "$GB" .state)"
vp click $((BX + BW - 17)) $((BY + 17)) # the OLD top_right spot is plain window content now
sleep 0.4
expect_eq "old glyph spot now passes clicks to the client" "4" "$(buttons_of gB)"

cat >"$GLYPH_CONF" <<'LUA'
if hl.plugin.hyprshare then
    hl.config({ plugin = { hyprshare = { visibility = "shared_only", live_fill = "rgba(00ffffff)" } } })
end
LUA
ctl reload >/dev/null
sleep 1
expect_eq "shared_only: only the LIVE glyph remains" "1" "$(glyph_count)"
expect_eq "shared_only: it is gB's" "live" "$(glyph_of "$GB" .state)"
read -r CX CY <<<"$(glyph_center "$GB")"
P="$(pixel "$CX" "$CY")"
expect_eq "live_fill colour applied on reload (cyan-ish, got $P)" "true" "$(((16#${P:0:2} < 0x60 && 16#${P:2:2} > 0xc0 && 16#${P:4:2} > 0xc0)) && echo true || echo false)"
: >"$GLYPH_CONF"
ctl reload >/dev/null
sleep 1
expect_eq "config reverted to defaults on reload" "top_right|all" "$(status | jq -r '"\(.glyph.corner)|\(.glyph.visibility)"')"

# --- fullscreen windows hide their glyph (hide_fullscreen = 1 by default) -------------------------
ctl dispatch 'hl.dsp.window.fullscreen({ action = "set", window = "class:gA" })' >/dev/null
sleep 0.8
expect_eq "fullscreen gA: no glyph" "null" "$(glyph_of "$GA" .state)"
ctl dispatch 'hl.dsp.window.fullscreen({ action = "unset", window = "class:gA" })' >/dev/null
sleep 0.8
expect_eq "gA back from fullscreen: glyph again" "idle" "$(glyph_of "$GA" .state)"
read -r AX AY AW AH <<<"$(win_xywh gA)" # restore may land it elsewhere
GX=$((AX + AW - 10 - 7)); GY=$((AY + 10 + 7))

# --- share stops: glyphs and mode tags go away ---------------------------------------------------
kill "$PID_gs" 2>/dev/null
wait "$PID_gs" 2>/dev/null
sleep 2
expect_eq "share stopped: shareMode false" "false" "$(status | jq -r '.shareMode')"
expect_eq "share stopped: no glyphs" "0" "$(glyph_count)"
expect_eq "share stopped: screen at gA's glyph spot is window colour again" "00ff00" "$(pixel "$GX" "$GY")"
expect_eq "share stopped: screenshare-mode removed" "0" "$( (tags_of gA; tags_of gB) | grep -c screenshare-mode)"
B_BEFORE="$(buttons_of gA)"
vp click "$GX" "$GY"
sleep 0.4
expect_eq "no share: a click at the glyph spot reaches the client" "$((B_BEFORE + 2))" "$(buttons_of gA)"

# --- unload with glyphs visible ----------------------------------------------------------------
# A fresh client may get the dead gs client's wl_client address; it must start clean (its own pick,
# not gs's old redirect) — captures die with their client.
start_probe gu --xdph --app-id gA --interval-ms 100
sleep 1.5
expect_eq "new client starts on its own pick (no inherited redirect): gA LIVE" "live" "$(glyph_of "$GA" .state)"
expect_eq "plugin unload with glyphs visible" "ok" "$(ctl plugin unload "$PLUGIN" | tail -1)"
sleep 1
expect_eq "compositor alive after unload" "0" "$(kill -0 "$COMPOSITOR_PID" 2>/dev/null; echo $?)"
expect_eq "decorations gone from gA" "0" "$(ctl decorations "address:$GA" | grep -c hyprshare-glyph)"
expect_eq "decorations gone from gB" "0" "$(ctl decorations "address:$GB" | grep -c hyprshare-glyph)"
expect_eq "screenshare-mode tag removed on unload" "0" "$( (tags_of gA; tags_of gB) | grep -c screenshare-mode)"
expect_eq "screen at the glyph spot is window colour after unload" "00ff00" "$(pixel "$GX" "$GY")"
B_BEFORE="$(buttons_of gA)"
vp click "$GX" "$GY"
sleep 0.4
expect_eq "after unload a click at the old glyph spot reaches the client" "$((B_BEFORE + 2))" "$(buttons_of gA)"
spawn_logger gC ff00ff
wait_class gC
expect_eq "a window opened after unload gets no glyph" "0" "$(ctl decorations "address:$(addr_of gC)" | grep -c hyprshare-glyph)"
expect_eq "compositor still alive (rendered frames after unload)" "0" "$(kill -0 "$COMPOSITOR_PID" 2>/dev/null; echo $?)"
