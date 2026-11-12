-- Backline's hotkeys, on the planes manual/07-hotkeys.md gives them, using
-- keys that are free in Omarchy's defaults. omarchy/install offers to add
-- these lines to ~/.config/hypr/bindings.lua between backline markers.
-- backline:begin
o.bind("SUPER + CTRL + I", "Instruments", { panel = "backline.instruments" })
o.bind("SUPER + CTRL + M", "Record", { panel = "backline.record" })
o.bind("SUPER + CTRL + G", "Transport", { panel = "backline.transport" })
o.bind("SUPER + CTRL + ALT + M", "Record or stop", "omarchy-backline-record toggle")
o.bind("SUPER + CTRL + ALT + G", "Play or stop", "omarchy-backline-transport toggle")
-- backline:end
