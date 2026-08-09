#include "glass_blur.hpp"

#include "globals.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include <drm_fourcc.h>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Shader.hpp>
#include <hyprland/src/render/Texture.hpp>

using namespace Render;
using namespace Render::GL;

namespace {

// OverShifted/LiquidGlass defaults. The kernel below is its blur13 verbatim.
constexpr float BLUR_RADIUS = 2.348F;

// Working at a lower scale widens the effective kernel and costs less, which is
// the cheapest way to dissolve fine detail like text; the blur itself hides the
// magnification that would otherwise show.
float downscale() {
  return g_config.blurDownscale
      ? std::clamp(g_config.blurDownscale->value(), 0.125F, 1.F) : 0.25F;
}
constexpr float FURTHEST_TAP = 5.176470588235294F;

// One horizontal and one vertical draw per iteration.
int blurDraws() {
  const auto passes = g_config.blurPasses
      ? static_cast<int>(g_config.blurPasses->value()) : 4;
  return std::clamp(passes, 1, 8) * 2;
}

// Reach of a single blur draw, in downscaled pixels.
int passExtent() {
  return static_cast<int>(std::ceil(FURTHEST_TAP * BLUR_RADIUS));
}

struct SMonitorBuffers {
  PHLMONITORREF monitor;
  SP<IFramebuffer> ping;
  SP<IFramebuffer> pong;
  SP<IFramebuffer> sharp;
  SP<ITexture> captured;
  SP<ITexture> capturedSharp;
  Vector2D capturedSize;
  uint64_t frame = 0;
  Glass::Blur::EStage stage = Glass::Blur::EStage::Windows;
};

std::unordered_map<uintptr_t, SMonitorBuffers> g_buffers;
uint64_t g_frame = 1;
SP<CShader> g_downscale;
SP<CShader> g_blur;

// pos and texcoord are both supplied by Hyprland's shared VAO and are identical
// there, spanning [0,1]. Remapping texcoord lets a draw refresh a sub-rectangle
// of the target by pairing it with a matching viewport, which avoids touching
// the scissor state Hyprland caches.
constexpr const char *VERTEX = R"GLSL(#version 300 es
precision highp float;
in vec2 pos;
in vec2 texcoord;
uniform vec2 uvOffset;
uniform vec2 uvSize;
out vec2 v_texcoord;
void main() {
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    v_texcoord  = uvOffset + texcoord * uvSize;
}
)GLSL";

// A single bilinear tap at the destination texel center lands exactly between
// four source texels, so this is an exact 2x2 box downsample.
constexpr const char *DOWNSCALE_FRAGMENT = R"GLSL(#version 300 es
precision highp float;
in vec2 v_texcoord;
out vec4 fragColor;
uniform sampler2D tex;
void main() {
    fragColor = texture(tex, v_texcoord);
}
)GLSL";

constexpr const char *BLUR_FRAGMENT = R"GLSL(#version 300 es
precision highp float;
in vec2 v_texcoord;
out vec4 fragColor;
uniform sampler2D tex;
uniform vec2 fullSize;
uniform vec2 topLeft;
uniform float radius;
void main() {
    vec2 direction = topLeft * radius;
    vec2 off1 = 1.411764705882353 * direction / fullSize;
    vec2 off2 = 3.2941176470588234 * direction / fullSize;
    vec2 off3 = 5.176470588235294 * direction / fullSize;
    vec4 color = texture(tex, v_texcoord) * 0.1964825501511404;
    color += texture(tex, v_texcoord + off1) * 0.2969069646728344;
    color += texture(tex, v_texcoord - off1) * 0.2969069646728344;
    color += texture(tex, v_texcoord + off2) * 0.09447039785044732;
    color += texture(tex, v_texcoord - off2) * 0.09447039785044732;
    color += texture(tex, v_texcoord + off3) * 0.010381362401148057;
    color += texture(tex, v_texcoord - off3) * 0.010381362401148057;
    fragColor = color;
}
)GLSL";

bool ensureShaders() {
  const auto build = [](SP<CShader> &shader, const char *fragment) {
    if (shader)
      return true;
    shader = makeShared<CShader>();
    if (shader->createProgram(VERTEX, fragment, true, false))
      return true;
    shader.reset();
    return false;
  };

  return build(g_downscale, DOWNSCALE_FRAGMENT) && build(g_blur, BLUR_FRAGMENT);
}

// Drops buffers for monitors that no longer exist. Without this the pool would
// retain a full-size framebuffer pair per hotplug cycle.
void pruneDeadMonitors() {
  std::erase_if(g_buffers, [](const auto &entry) {
    return !entry.second.monitor;
  });
}

SMonitorBuffers *ensureBuffers(const PHLMONITOR &monitor, const Vector2D &size,
                              DRMFormat format) {
  const auto width = std::max(2, static_cast<int>(std::ceil(size.x * downscale())));
  const auto height = std::max(2, static_cast<int>(std::ceil(size.y * downscale())));

  auto &buffers = g_buffers[reinterpret_cast<uintptr_t>(monitor.get())];
  buffers.monitor = monitor;

  const auto ensureOne = [&](SP<IFramebuffer> &fb, const char *name) {
    if (!fb)
      fb = g_pHyprRenderer->createFB(name);
    if (!fb)
      return false;
    if (fb->isAllocated() && fb->m_size == Vector2D{width, height} &&
        fb->m_drmFormat == format)
      return true;
    if (fb->isAllocated())
      fb->release();
    return fb->alloc(width, height, format);
  };

  if (!ensureOne(buffers.ping, "kinetik-glass-scene-ping") ||
      !ensureOne(buffers.pong, "kinetik-glass-scene-pong") ||
      !ensureOne(buffers.sharp, "kinetik-glass-scene-sharp")) {
    g_buffers.erase(reinterpret_cast<uintptr_t>(monitor.get()));
    return nullptr;
  }

  return &g_buffers.at(reinterpret_cast<uintptr_t>(monitor.get()));
}

// Renders one full-viewport quad into target, restricted to region by way of the
// viewport rather than the scissor, sampling source over the matching uv range.
// direction is the separable blur axis; a zero direction means the plain
// downscale program.
void drawRegion(const SP<IFramebuffer> &target, const SP<ITexture> &source,
                const CBox &region, const Vector2D &direction) {
  g_pHyprRenderer->bindFB(target);
  g_pHyprOpenGL->setViewport(static_cast<GLint>(region.x),
                             static_cast<GLint>(region.y),
                             static_cast<GLsizei>(region.width),
                             static_cast<GLsizei>(region.height));

  glActiveTexture(GL_TEXTURE0);
  source->bind();
  source->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  source->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  source->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  source->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  const auto blurring = direction != Vector2D{};
  const auto bound = g_pHyprOpenGL->useShader(blurring ? g_blur : g_downscale);
  bound->setUniformInt(SHADER_TEX, 0);
  bound->setUniformFloat2(SHADER_UV_OFFSET,
                          static_cast<float>(region.x / target->m_size.x),
                          static_cast<float>(region.y / target->m_size.y));
  bound->setUniformFloat2(SHADER_UV_SIZE,
                          static_cast<float>(region.width / target->m_size.x),
                          static_cast<float>(region.height / target->m_size.y));

  if (blurring) {
    bound->setUniformFloat2(SHADER_FULL_SIZE,
                            static_cast<float>(target->m_size.x),
                            static_cast<float>(target->m_size.y));
    bound->setUniformFloat2(SHADER_TOP_LEFT, static_cast<float>(direction.x),
                            static_cast<float>(direction.y));
    bound->setUniformFloat(SHADER_RADIUS, BLUR_RADIUS);
  }

  glBindVertexArray(bound->getUniformLocation(SHADER_SHADER_VAO));
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  source->unbind();
}

} // namespace

// Iterated separable draws do not stack their reach linearly. Each draw
// convolves the running image with a small fixed-width kernel (passExtent()
// wide); convolving independent kernels adds their variances, not their
// widths, so N draws spread like sqrt(N) single draws, not N of them -- the
// same random-walk scaling that governs any sum of N iid steps. At the
// shipped defaults (blur_passes = 4, so blurDraws() = 8; passExtent() = 13;
// downscale() = 0.25) the old linear model claimed
// 8 * 13 / 0.25 = 416px, a ~2.8x overclaim versus the sqrt(8) = 2.828 factor
// that actually governs it: 13 * sqrt(8) / 0.25 ~= 147px. No extra fudge
// factor is layered on top of that: 147px already lands within the audited
// ~150px target, and callers still std::ceil() the result while claimDamage()
// only commits the margin when it actually intersects existing damage, so the
// tails of the distribution stay covered without re-inflating the common
// case.
float Glass::Blur::kernelExtent() {
  return passExtent() * std::sqrt(static_cast<float>(blurDraws())) /
      downscale();
}

void Glass::Blur::claimDamage(const CBox &lensBox) {
  auto &damage = g_pHyprRenderer->m_renderData.damage;
  if (damage.empty() || lensBox.width < 2 || lensBox.height < 2)
    return;

  // Claim everything the blur reads, not just the lens. The kernel reaches
  // kernelExtent() beyond the box, and outside this frame's damage the current
  // framebuffer still holds the previous frame's composite -- including the
  // previous frame's glass. Blurring that back in is a feedback loop, which is
  // what the flicker is. Claiming the margin forces it to be recomposited first.
  const CBox footprint = lensBox.copy().expand(kernelExtent());
  CRegion probe{footprint};
  probe.intersect(damage);
  if (probe.empty())
    return;

  damage.add(footprint);
}

void Glass::Blur::beginFrame() { ++g_frame; }

Glass::Blur::SScene
Glass::Blur::refresh(const PHLMONITOR &monitor, EStage stage) {
  const auto &renderData = g_pHyprRenderer->m_renderData;
  if (!monitor || !renderData.currentFB || !renderData.currentFB->getTexture())
    return {};

  const auto sceneSize = renderData.currentFB->m_size;
  if (sceneSize.x < 4 || sceneSize.y < 4 || !ensureShaders())
    return {};

  pruneDeadMonitors();

  auto format = renderData.currentFB->m_drmFormat;
  if (format == DRM_FORMAT_INVALID)
    format = DRM_FORMAT_ARGB8888;

  const auto buffers = ensureBuffers(monitor, sceneSize, format);
  if (!buffers)
    return {};

  // Overlay surfaces all composite after every window, so they can share one
  // capture per frame. Windows cannot: they draw bottom to top, and a shared
  // capture would be the scene as it looked beneath the lowest of them, leaving
  // every window above refracting the desktop instead of what it actually
  // covers. Each window therefore captures at its own point in the stack.
  const auto reusable = stage == EStage::Overlay;
  if (reusable && buffers->frame == g_frame && buffers->stage == stage &&
      buffers->captured && buffers->capturedSize == sceneSize)
    return {.blurred = buffers->captured,
            .sharp = buffers->capturedSharp,
            .size = sceneSize};

  const auto bufferSize = buffers->ping->m_size;
  const CBox whole{{}, bufferSize};

  // Resolve the scene before rebinding: bindTempFB reassigns currentFB, so
  // reading it afterwards would sample the buffer we are about to draw into.
  const auto sceneTexture = renderData.currentFB->getTexture();

  // CGLFramebuffer::bind() only re-applies the viewport for buffers it allocated
  // itself, so restoring the binding is not enough to restore the viewport.
  GLint previousViewport[4] = {};
  glGetIntegerv(GL_VIEWPORT, previousViewport);

  const auto guard = g_pHyprRenderer->bindTempFB(buffers->ping);
  const auto blendWasEnabled = glIsEnabled(GL_BLEND) == GL_TRUE;
  g_pHyprOpenGL->blend(false);
  g_pHyprOpenGL->scissor(nullptr);

  // Downscale once into its own buffer and keep it: the blur ping-pongs from
  // here and would otherwise overwrite the only detailed copy.
  drawRegion(buffers->sharp, sceneTexture, whole, {});
  drawRegion(buffers->ping, buffers->sharp->getTexture(), whole, {});

  auto *from = &buffers->ping;
  auto *to = &buffers->pong;
  for (int draw = 0; draw < blurDraws(); ++draw) {
    const Vector2D direction = draw % 2 == 0 ? Vector2D{1, 0} : Vector2D{0, 1};
    drawRegion(*to, (*from)->getTexture(), whole, direction);
    // After the swap, from points at what was just written.
    std::swap(from, to);
  }

  g_pHyprOpenGL->blend(blendWasEnabled);
  g_pHyprOpenGL->setViewport(previousViewport[0], previousViewport[1],
                             previousViewport[2], previousViewport[3]);

  buffers->captured = (*from)->getTexture();
  buffers->capturedSharp = buffers->sharp->getTexture();
  buffers->capturedSize = sceneSize;
  buffers->frame = g_frame;
  buffers->stage = stage;
  return {.blurred = buffers->captured,
          .sharp = buffers->capturedSharp,
          .size = sceneSize};
}

void Glass::Blur::destroy() {
  g_buffers.clear();
  g_downscale.reset();
  g_blur.reset();
}
