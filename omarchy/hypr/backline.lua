-- Backline's hotkeys, on the planes manual/07-hotkeys.md gives them, using
-- keys that are free in Omarchy's defaults. omarchy-backline-setup offers to add
-- these lines to ~/.config/hypr/bindings.lua between backline markers.
-- backline:begin
o.bind("SUPER + CTRL + I", "Instruments", "omarchy-shell -q backline.instruments toggle")
o.bind("SUPER + CTRL + M", "Record", "omarchy-shell -q backline.record toggle")
o.bind("SUPER + CTRL + G", "Transport", "omarchy-shell -q backline.transport toggle")
o.bind("SUPER + CTRL + ALT + M", "Record or stop", "omarchy-backline-record toggle")
o.bind("SUPER + CTRL + ALT + G", "Play or stop", "omarchy-backline-transport toggle")
-- backline:end
