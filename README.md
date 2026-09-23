# hyprland-macos-zoom

[![CI](https://github.com/aastrand/hyprland-macos-zoom/actions/workflows/ci.yml/badge.svg)](https://github.com/aastrand/hyprland-macos-zoom/actions/workflows/ci.yml)
[![License: BSD-3-Clause](https://img.shields.io/badge/License-BSD--3--Clause-blue.svg)](LICENSE)

A native Hyprland plugin for macOS-like full-screen accessibility zoom. It uses Hyprland’s compositor-native zoom camera and leaves bindings in your configuration.

![Continuous modifier-and-scroll zoom demo](assets/demo.webp)

## Why a plugin?

Hyprland already renders full-screen zoom, but ordinary wheel bindings reduce
scrolling to discrete `mouse_up` and `mouse_down` events. This plugin listens to
the compositor's high-resolution axis events directly, maps their full delta to
the native per-monitor zoom camera, and optionally consumes the modified scroll
so the focused application does not move at the same time. No daemon or
per-scroll subprocess is involved.

## Requirements

- Hyprland 0.56.2 or a source-compatible build with matching installed headers.
- CMake 3.27+, a C++23 compiler, `pkg-config`, and Hyprland build dependencies.
- Hyprland’s Lua configuration for the provided examples.

Hyprland plugins are ABI-coupled to the compositor build. Rebuild after every Hyprland update.

## Install with HyprPM

```sh
hyprpm add https://github.com/aastrand/hyprland-macos-zoom.git
hyprpm enable macos-zoom
hyprpm reload -n
```

To load enabled plugins automatically when Hyprland starts, add this to your
Hyprland Lua configuration:

```lua
hl.on("hyprland.start", function()
  hl.exec_cmd("hyprpm reload -n")
end)
```

Update and rebuild against the current Hyprland version with:

```sh
hyprpm update
hyprpm reload -n
```

Remove the plugin with:

```sh
hyprpm disable macos-zoom
hyprpm remove hyprland-macos-zoom
```

## Build and test

```sh
make test
```

The plugin is written to `build/macos-zoom.so`.

For a local development load:

```sh
hyprctl plugin load "$PWD/build/macos-zoom.so"
hyprctl eval "hl.plugin.macos_zoom.adjust('in')"
hyprctl eval "hl.plugin.macos_zoom.adjust('out')"
hyprctl eval "hl.plugin.macos_zoom.adjust('toggle')"
hyprctl eval "hl.plugin.macos_zoom.adjust('reset')"
hyprctl eval "hl.plugin.macos_zoom.adjust('set 3.5')"
```

The native `macos-zoom` dispatcher is also registered for legacy Hyprland
configurations. Lua configurations should use the Lua API shown above.

Unload with an absolute path; unloading resets every output to 1×.

```sh
hyprctl plugin unload "$PWD/build/macos-zoom.so"
```

## Hyprland configuration

Copy the relevant parts of [examples/config.lua](examples/config.lua) into your Hyprland configuration. With `raw_scroll = true`, the plugin reads high-resolution wheel/trackpad axis deltas directly while the configured modifier is held. This is the path that gives the responsive, linear macOS-like feel.

[examples/bindings.lua](examples/bindings.lua) contains optional discrete fallbacks plus toggle and reset bindings for `~/.config/hypr/bindings.lua`. You can replace those with any keys you prefer. The raw modifier is separately configurable as `CTRL`, `ALT`, `META`, `SHIFT`, or a `+`-separated combination.

After editing your Hyprland config:

```sh
hyprctl reload
hyprctl configerrors
```

## Tracking modes

Use exactly one of these pairs in `cursor` configuration:

- macOS “Continuously with Pointer”: `zoom_detached_camera = false`, `zoom_rigid = false`.
- macOS “When Pointer Reaches Edge”: `zoom_detached_camera = true`, `zoom_rigid = false`.
- macOS “To Keep Pointer Centered”: `zoom_detached_camera = false`, `zoom_rigid = true`.

## Plugin options

All options live below `plugin.macos_zoom` in Lua config:

- `raw_scroll`: use continuous axis deltas; default `true`.
- `modifier`: modifier for raw scrolling; default `CTRL`.
- `sensitivity`: linear factor change per raw axis unit; default `0.01`.
- `invert_scroll`: reverse only the modifier-scroll zoom direction, leaving normal scrolling unchanged; default `false`.
- `momentum`: continue zooming after a touchpad scroll ends; default `true` (wheel scrolling stays discrete).
- `momentum_strength`: multiply the initial coast speed; default `1.0`, set to `0` for no coast. Increase for a longer flick.
- `momentum_decay_ms`: exponential coast time constant in milliseconds; default `240`. Increase for a longer-lasting coast.
- `momentum_max_speed`: cap the coast speed in zoom factors per second; default `12.0`.
- `consume_scroll`: prevent the modified gesture from also scrolling the app; default `true`.
- `step`: multiplicative change used by the optional discrete `in`/`out` actions; default `1.20`.
- `min_factor`: lower bound; default `1.0`.
- `max_factor`: upper bound; default `40.0`.
- `snap_threshold`: values below this snap to the minimum when zooming out; default `1.05`.
- `toggle_factor`: initial toggle target before a previous factor exists; default `2.0`.
- `independent_displays`: affect only the output under the pointer; default `true`.

## Current scope

Version 0.1 targets macOS full-screen modifier-plus-wheel behavior with continuous raw input and conventional bindable actions. Split-screen and picture-in-picture lenses, keyboard-focus following, persistent zoom across login, temporary detachment, and capture policy are planned separately because they require different compositor integration.

## License

[BSD-3-Clause](LICENSE)
