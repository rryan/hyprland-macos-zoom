-- Put these values in an hl.config({...}) call in your Hyprland Lua
-- configuration. The tracking pair below selects macOS's
-- "Continuously with Pointer" behavior.
hl.config({
  cursor = {
    zoom_detached_camera = false,
    zoom_rigid = false,
  },
  plugin = {
    macos_zoom = {
      raw_scroll = true,
      modifier = "CTRL",
      sensitivity = 0.01,
      invert_scroll = false, -- true: reverse zoom without changing normal scrolling
      momentum = true,
      momentum_strength = 1.0,
      momentum_decay_ms = 240.0,
      momentum_max_speed = 12.0,
      consume_scroll = true,
      step = 1.20,
      min_factor = 1.0,
      max_factor = 40.0,
      snap_threshold = 1.05,
      toggle_factor = 2.0,
      independent_displays = true,
    },
  },
})

-- Other macOS tracking modes:
--
-- "When Pointer Reaches Edge"
--   cursor.zoom_detached_camera = true
--   cursor.zoom_rigid = false
--
-- "To Keep Pointer Centered"
--   cursor.zoom_detached_camera = false
--   cursor.zoom_rigid = true
