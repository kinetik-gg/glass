# Glass

Glass is a [Kinetik](https://github.com/kinetik-gg) project.

[![Hyprland](https://img.shields.io/badge/Hyprland-0.56.x-58E1FF?logo=hyprland&logoColor=white)](https://hypr.land/)
[![Status](https://img.shields.io/badge/status-experimental-f5a623)](#safety)
[![License: MIT](https://img.shields.io/badge/license-MIT-2ea44f)](LICENSE)

Experimental framebuffer-sampled liquid glass surfaces for Hyprland 0.56.x.

Glass installs a window underlay that samples Hyprland's pre-blurred
monitor framebuffer. A custom OpenGL ES shader adds localized lens refraction,
subtle chromatic dispersion, a dark tint, directional edge lighting, and an
antialiased continuous-corner mask. It loads disabled by default.

## Highlights

- Samples Hyprland's already blurred framebuffer instead of faking a static
  translucent surface.
- Adds tunable refraction, chromatic dispersion, tint, edge lighting, and
  material opacity.
- Uses a measured continuous-corner approximation with an antialiased shader
  mask.
- Validates Hyprland's plugin ABI before initialization.
- Keeps the effect disabled until explicitly enabled in configuration.

## Compatibility

| Component | Supported |
| --- | --- |
| Hyprland | `0.56.x` |
| Configuration | Lua |
| Renderer | OpenGL ES |
| Build system | CMake |

Hyprland plugins are ABI-sensitive. Rebuild Glass after every Hyprland
upgrade, even when the source revision has not changed.

Distribution package rebuilds can also change the ABI without changing the
Hyprland version number, for example when Aquamarine changes. Compare the full
`Version ABI string` from `Hyprland --version` (installed binary) and
`hyprctl version` (running compositor). Restart the session when they differ,
then rebuild and reload Glass. Garage installations use
`garage-rebuild-plugins` to rebuild and deploy their pinned plugins.

Glass rejects an ABI mismatch before inspecting compositor state or registering
settings. If `plugin.kinetik_glass.*` settings are reported as unknown, check
`hyprctl plugin list` first: the plugin may not have loaded. In Lua,
`hl.plugin.load(path)` queues a load for the end of config parsing; a successful
call does not mean Glass is available yet. Guard plugin settings using
`hl.get_loaded_plugins()` until Hyprland reloads the config with Glass loaded.

## Build

Install Hyprland development headers, CMake, pkg-config, and a C++ compiler,
then run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

## Safe loading

Test in a nested Hyprland session before loading into a daily session:

```bash
hyprctl plugin load "$PWD/build/libkinetik-glass.so"
```

The plugin validates the installed Hyprland ABI and starts with rendering
disabled. Enable and tune it in Hyprland's Lua configuration:

```lua
hl.config({
    plugin = {
        kinetik_glass = {
            enabled = true,
            tint = "rgba(050505ff)",
            tint_opacity = 0.38,
            blur_opacity = 0.88,
            refraction = 8.0,
            refraction_power = 1.0,
            dispersion = 1.2,
            noise = 0.015,
            highlight_opacity = 0.10,
            frame_highlight_opacity = 0.07,
            frame_border_opacity = 0.30,
            edge_width = 8,
            corner_power = 3.37,
        },
    },
    decoration = {
        rounding = 18,
        rounding_power = 3.37,
    },
})
```

Windows take their corner radius and exponent straight from
`decoration.rounding` and `decoration.rounding_power`, the same values Hyprland
uses to clip the client surface. The glass silhouette therefore cannot drift
from the client's own clip, and there is no separate corner curve to keep in
sync. `corner_power` only applies to layer surfaces, which have no rounding
power of their own.

One signed rounded-rectangle field drives coverage, refraction, and the frame,
so the three cannot disagree about where the surface is.

The surface is modelled as a thick slab with a rounded bevel around its edge.
The bevel's tilt decays exponentially inward from the border, and the sample
position is displaced by refracting the view ray through that tilt with Snell's
law, at a fixed index of 1.52. The lens only ever samples from inside its own
silhouette, so corners bend hardest with no corner mask, and refraction
contributes nothing to the damage footprint.

The index is fixed because the bend barely responds to it -- across 1.5 to 3.0
the displacement changes by under a fifth -- so `refraction` controls the bevel
steepness instead, which is what actually shapes the result.

The shader controls:

- `refraction`: bevel steepness (`0`-`1`); `0` is flat glass, which is what
  makes the surface read as plain frost.
- `refraction_power`: exponent shaping the bevel profile (`0.1`-`8`).
- `edge_width`: logical pixels the bevel reaches inward (`4`-`512`).
- `edge_clarity`: how much unblurred detail survives in the refracting rim
  (`0`-`1`). Without it the bend displaces an already-flat blur and is
  invisible.
- `blur_passes` (`1`-`8`) and `blur_downscale` (`0.125`-`1`): blur strength.
  Fewer passes over a larger buffer is a tighter blur.
- `saturation`: colour surviving in the backdrop before the tint (`0`-`1`).
- `highlight_opacity`: additive rim light living only on the bevel (`0`-`1`).
- `dispersion`: red/blue separation along the bend direction, in px (`0`-`8`).
- `noise`: screen-space material grain (`0`-`0.2`).
- `tint_opacity`: amount of the configured tint mixed into sampled pixels.
- `blur_opacity`: overall material opacity.
- `frame_highlight_opacity`: opacity of the one-physical-pixel white inner ring.
- `frame_border_opacity`: opacity of the one-physical-pixel dark outer ring.

The frame rings are intentionally fixed at one framebuffer pixel each. Their
width is not scaled from logical coordinates, so fractional and integer output
scales retain the same crisp physical-pixel treatment. Both rings use the same
continuous-corner mask as the glass surface.

Client surfaces must contain transparency for an underlay material to remain
visible. For example, Kitty needs `background_opacity` below `1.0`.

## Hyprpm

```bash
hyprpm add https://github.com/kinetik-gg/glass
hyprpm enable kinetik-glass
```

Reload Hyprland's managed plugins after installation:

```bash
hyprpm reload
```

## Safety

Hyprland plugins execute inside the compositor. A plugin fault can terminate
the session. Keep another TTY available and do not autoload experimental builds.

Unload a manually loaded build with:

```bash
hyprctl plugin unload "$PWD/build/libkinetik-glass.so"
```

## License

Glass is available under the [MIT License](LICENSE).

## Acknowledgements

The normalized superellipse refraction model is inspired by Sepehr
Kalanaki's MIT-licensed [LiquidGlass](https://github.com/OverShifted/LiquidGlass)
shader. Glass reimplements the technique for Hyprland's OpenGL ES render
pipeline with safe curve evaluation, continuous-corner masking, and compositor
damage tracking. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

For corner-model validation methodology and how to regenerate the reference
artifacts, see [tools/README.md](tools/README.md).
