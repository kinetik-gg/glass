#pragma once

#include <cstdint>

#include <hyprland/src/defines.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>

namespace Render {
class ITexture;
}

namespace Glass::Blur {

// A blurred copy of the scene in monitor framebuffer space. Sampling coordinate
// for a monitor pixel p is simply p / size, so a lens can reach any part of the
// scene without running into the edge of a crop.
struct SScene {
  SP<Render::ITexture> blurred;
  // The same scene downscaled but not blurred. Refraction is only visible if the
  // content it displaces still has detail at the scale of the displacement; a
  // fully blurred scene is locally flat, so bending it changes nothing and the
  // surface reads as plain frost.
  SP<Render::ITexture> sharp;
  Vector2D size; // full-resolution monitor framebuffer size, not texture size
};

// How far, in full-resolution monitor pixels, a scene pixel can influence the
// blurred result. This is the only optical margin the plugin needs: the lens
// itself never samples outside its own silhouette.
float kernelExtent();

// Marks the whole lens as damaged when anything within a kernel of it changed.
// Call while the render pass is still being built: the pass fixes its damage
// afterwards, and without this the composite would be scissored to a sliver
// while the rest of the glass kept showing a stale scene.
void claimDamage(const CBox &lensBox);

// Invalidates the per-frame capture. Call once at the start of every frame.
void beginFrame();

// Draw order within a frame. Windows composite before overlay layer surfaces,
// so a single capture per frame would hand an overlay the scene as it looked
// before any window was drawn -- a dialog would refract only the wallpaper.
enum class EStage : uint8_t {
  Windows = 0,
  Overlay = 1,
};

// Returns the blurred scene for this monitor, capturing once per stage per frame
// and reusing it within that stage. Sharing a capture across every lens in a
// stage keeps the result deterministic: a per-surface refresh would blur over
// regions of the framebuffer still holding the previous frame's composite,
// which differs from frame to frame.
SScene refresh(const PHLMONITOR &monitor, EStage stage);

void destroy();

} // namespace Glass::Blur
