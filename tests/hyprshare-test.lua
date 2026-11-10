-- Config for the isolated test compositor (tests/run-isolated.sh). Not for the live session.

hl.monitor({ output = "HEADLESS-1", mode = "1920x1080@60", position = "0x0", scale = "1" })
hl.monitor({ output = "", mode = "1920x1080@60", position = "auto-right", scale = "1" })

hl.config({
    general = { border_size = 2, gaps_in = 0, gaps_out = 0 },
    animations = { enabled = false },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    debug = { disable_logs = false },
})

-- Three floating test windows with distinct sizes, so a redirect is visible as a size change.
hl.window_rule({ name = "hs-a", match = { class = "hsA" }, float = true, size = "400 300", move = "0 0" })
hl.window_rule({ name = "hs-b", match = { class = "hsB" }, float = true, size = "640 480", move = "500 0" })
hl.window_rule({ name = "hs-c", match = { class = "hsC" }, float = true, size = "800 200", move = "0 600" })
hl.window_rule({ name = "hs-d", match = { class = "hsD" }, float = true, size = "300 200", move = "1000 0" })
hl.window_rule({ name = "hs-e", match = { class = "hsE" }, float = true, size = "700 350", move = "1000 300" })
hl.window_rule({ name = "hs-f", match = { class = "hsF" }, float = true, size = "500 500", move = "1200 500" })

-- The README's border rule, verified through `hyprctl getprop ... active_border_color`.
-- The README's share-mode rule (every window while sharing), declared first so the shared window's
-- own rule below wins for it.
hl.window_rule({ name = "share-mode-borders", match = { tag = "screenshare-mode" }, border_color = "rgba(80808080)" })
hl.window_rule({ name = "hyprshare-border", match = { tag = "screenshared" }, border_color = "rgb(ff3030)" })

-- The README's binds: must parse with the plugin not yet loaded (the body only runs on press).
hl.bind("SUPER + CTRL + ALT + S", function() hl.plugin.hyprshare.share() end)
hl.bind("SUPER + CTRL + ALT + SHIFT + S", function() hl.plugin.hyprshare.clear() end)

-- Glyph suite (SUITE=glyph): two solid-colour click-logger windows at known places.
hl.window_rule({ name = "g-a", match = { class = "gA" }, float = true, size = "400 300", move = "100 100" })
hl.window_rule({ name = "g-b", match = { class = "gB" }, float = true, size = "500 350", move = "700 100" })

-- Plugin styling lives in a separate file the glyph suite rewrites + reloads. The guard keeps the
-- config valid while the plugin isn't loaded (unknown plugin keys would be config errors).
local glyphConf = os.getenv("HYPRSHARE_GLYPH_CONF")
if glyphConf then
    dofile(glyphConf)
end
