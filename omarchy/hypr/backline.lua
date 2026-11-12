-- Backline's hotkeys, on Omarchy's planes: Super + Ctrl opens a panel, and
-- Super + Ctrl + Alt with the same letter is its quick action. omarchy-backline-setup
-- adds the lines between the backline markers to ~/.config/hypr/bindings.lua,
-- after checking each key is free on this system (a taken key isn't bound).
-- Lines tagged "panels" need the bar panels (Omarchy's Quattro shell and the
-- bar plugin); without them the "no-panels" lines stand in, so no key points
-- at a panel that can't open. The tags are not copied.
--
-- Super + Ctrl + I is Omarchy's "Toggle locking on idle", so the Instruments
-- panel is on U.
-- backline:begin
o.bind("SUPER + CTRL + U", "Backline instruments", "omarchy-shell -q backline.instruments toggle") -- panels
o.bind("SUPER + CTRL + M", "Backline record panel", "omarchy-shell -q backline.record toggle") -- panels
o.bind("SUPER + CTRL + G", "Backline transport", "omarchy-shell -q backline.transport toggle") -- panels
o.bind("SUPER + CTRL + M", "Backline Mixer", "omarchy-backline-mixer") -- no-panels
o.bind("SUPER + CTRL + ALT + M", "Backline: record or stop", "omarchy-backline-record toggle")
o.bind("SUPER + CTRL + ALT + G", "Backline: play or stop", "omarchy-backline-transport toggle")
-- backline:end
