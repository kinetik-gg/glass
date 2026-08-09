#pragma once

#include <hyprland/src/defines.hpp>

#include "glass_blur.hpp"

namespace Glass::Shader {

// Geometry shared by both passes. One shape drives coverage, refraction and the
// frame, so the plugin silhouette cannot drift from the client's own clip.
struct SGeometry {
  CBox box;            // monitor framebuffer pixels
  float radius = 0.F;  // already scaled to the output
  float roundingPower = 2.F;
};

struct SGlass {
  CHyprColor tint;
  float tintOpacity = 0.F;
  float opacity = 1.F;
  float refraction = 0.7F;     // bevel steepness, 0..1
  float refractionPower = 1.F; // bevel profile exponent
  float glow = 0.F;            // directional rim light on the bevel
  float dispersion = 0.F;      // px of red/blue separation
  float clarity = 0.F;         // how sharp the refracting rim reads
  float saturation = 1.F;      // 0 = greyscale backdrop, 1 = untouched
  float noise = 0.F;
  float band = 0.F;            // px the lens reaches inward, 0 = reference
};

struct SFrame {
  float opacity = 1.F;
  float highlightOpacity = 0.F;
  float borderOpacity = 0.F;
  float width = 1.F; // px per ring
};

// Optical underlay. Samples the monitor-space scene; must run below the client.
bool drawGlass(const Blur::SScene &scene, const SGeometry &geometry,
               const SGlass &glass, const CRegion &damage);

// Decorative border rings. Never samples the scene, so it is safe above the
// client and cannot duplicate its content.
bool drawFrame(const SGeometry &geometry, const SFrame &frame,
               const CRegion &damage);

void destroy();

} // namespace Glass::Shader
