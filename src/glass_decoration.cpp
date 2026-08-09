#include "glass_decoration.hpp"

#include <algorithm>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include "glass_blur.hpp"
#include "glass_pass.hpp"
#include "glass_shader.hpp"
#include "globals.hpp"

using namespace Render::GL;

CGlassDecoration::CGlassDecoration(PHLWINDOW window, bool frameOnly)
    : IHyprWindowDecoration(window), m_window(window), m_frameOnly(frameOnly) {
  updateWindow(window);
}

CGlassDecoration::~CGlassDecoration() { damageEntire(); }

SDecorationPositioningInfo CGlassDecoration::getPositioningInfo() {
  SDecorationPositioningInfo info;
  info.policy = DECORATION_POSITION_STICKY;
  info.reserved = false;
  info.priority = 100;
  info.edges = DECORATION_EDGE_BOTTOM | DECORATION_EDGE_LEFT |
               DECORATION_EDGE_RIGHT | DECORATION_EDGE_TOP;
  info.desiredExtents = {{0, 0}, {0, 0}};
  return info;
}

void CGlassDecoration::onPositioningReply(const SDecorationPositioningReply &) {
}

void CGlassDecoration::draw(PHLMONITOR monitor, const float &alpha) {
  if (!g_config.enabled->value() || !validMapped(m_window))
    return;

  // Pass metadata is queried before drawPass(). Freeze geometry now so damage,
  // the blur refresh and the shader all use the same box.
  updateGeometry(monitor);
  if (!m_frameOnly)
    Glass::Blur::claimDamage(m_monitorBox);
  g_pHyprRenderer->m_renderPass.add(makeUnique<CGlassPassElement>(
      CGlassPassElement::SData{.decoration = this, .alpha = alpha}));
}

void CGlassDecoration::updateGeometry(PHLMONITOR monitor) {
  if (!monitor || !validMapped(m_window))
    return;

  const auto window = m_window.lock();
  const auto workspaceOffset =
      window->m_workspace && !window->m_pinned
          ? window->m_workspace->m_renderOffset->value()
          : Vector2D{};
  const auto position =
      window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) +
      workspaceOffset + window->m_floatingOffset;
  const auto size = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
  const CBox newGlobalBox{position, size};

  const bool moved = m_lastGlobalBox.width > 0 && m_lastGlobalBox.height > 0 &&
                     newGlobalBox != m_lastGlobalBox;
  if (moved) {
    // Damage the old and the new optical footprint. The lens never samples
    // outside its own silhouette, so the blur kernel is the whole margin, and
    // the moving and stationary paths use the same one.
    const auto margin = opticalMargin(monitor);
    g_pHyprRenderer->damageBox(m_lastGlobalBox.copy().expand(margin));
    g_pHyprRenderer->damageBox(newGlobalBox.copy().expand(margin));
  }

  m_lastGlobalBox = newGlobalBox;
  m_monitorLogicalBox = newGlobalBox.copy().translate(-monitor->m_position);
  m_monitorBox =
      m_monitorLogicalBox.copy().scale(monitor->m_scale).round();
}

double CGlassDecoration::opticalMargin(const PHLMONITOR &monitor) {
  const auto scale = monitor && monitor->m_scale > 0 ? monitor->m_scale : 1.0;
  return std::ceil(Glass::Blur::kernelExtent() / scale);
}

void CGlassDecoration::drawPass(PHLMONITOR monitor, float alpha) {
  if (!monitor || !validMapped(m_window))
    return;

  const auto window = m_window.lock();
  if (m_monitorBox.width < 2 || m_monitorBox.height < 2)
    return;

  // Take radius and power from the window itself rather than a plugin option,
  // so the glass silhouette is the client's clip by construction.
  const Glass::Shader::SGeometry geometry{
      .box = m_monitorBox,
      .radius = static_cast<float>(window->rounding() * monitor->m_scale),
      .roundingPower = window->roundingPower()};

  if (m_frameOnly) {
    Glass::Shader::drawFrame(
        geometry,
        {.opacity = std::clamp(alpha, 0.F, 1.F),
         .highlightOpacity = g_config.frameHighlightOpacity->value(),
         .borderOpacity = g_config.frameBorderOpacity->value(),
         .width = static_cast<float>(std::round(monitor->m_scale))},
        g_pHyprRenderer->m_renderData.damage);
    return;
  }

  CRegion visible{g_pHyprRenderer->m_renderData.damage};
  visible.intersect(m_monitorBox);
  if (visible.empty())
    return;

  const auto scene = Glass::Blur::refresh(monitor, Glass::Blur::EStage::Windows);
  const auto tint = CHyprColor{static_cast<uint64_t>(g_config.tint->value())};

  const auto drawn = Glass::Shader::drawGlass(
      scene, geometry,
      {.tint = tint,
       .tintOpacity = g_config.tintOpacity->value(),
       .opacity = std::clamp<float>(g_config.blurOpacity->value(), 0.F, 1.F),
       .refraction = g_config.refraction->value(),
       .refractionPower = g_config.refractionPower->value(),
       .glow = g_config.highlightOpacity->value(),
       .dispersion = static_cast<float>(g_config.dispersion->value() *
                                        monitor->m_scale),
       .clarity = g_config.edgeClarity->value(),
       .saturation = g_config.saturation->value(),
       .noise = g_config.noise->value(),
       .band = static_cast<float>(g_config.edgeWidth->value() *
                                  monitor->m_scale)},
      g_pHyprRenderer->m_renderData.damage);

  if (drawn)
    return;

  // A shader failure may retain the tint, but must never route this surface
  // back through Hyprland's native blur pipeline.
  g_pHyprOpenGL->renderRect(
      m_monitorBox,
      CHyprColor{static_cast<float>(tint.r), static_cast<float>(tint.g),
                 static_cast<float>(tint.b),
                 std::clamp<float>(g_config.tintOpacity->value(), 0.F, 1.F)},
      {.damage = &g_pHyprRenderer->m_renderData.damage,
       .round = static_cast<int>(geometry.radius),
       .roundingPower = geometry.roundingPower});
}

eDecorationType CGlassDecoration::getDecorationType() {
  return DECORATION_CUSTOM;
}

void CGlassDecoration::updateWindow(PHLWINDOW) {
  damageEntire();
  m_lastGlobalBox = {};
  m_monitorLogicalBox = {};
  m_monitorBox = {};
}

void CGlassDecoration::damageEntire() {
  if (m_lastGlobalBox.width <= 0 || m_lastGlobalBox.height <= 0)
    return;

  const auto window = m_window.lock();
  g_pHyprRenderer->damageBox(m_lastGlobalBox.copy().expand(
      opticalMargin(window ? window->m_monitor.lock() : nullptr)));
}

uint64_t CGlassDecoration::getDecorationFlags() {
  return DECORATION_PART_OF_MAIN_WINDOW;
}

eDecorationLayer CGlassDecoration::getDecorationLayer() {
  return m_frameOnly ? DECORATION_LAYER_OVER : DECORATION_LAYER_UNDER;
}

std::string CGlassDecoration::getDisplayName() {
  return m_frameOnly ? "Glass Frame" : "Glass";
}

CBox CGlassDecoration::monitorBox() const { return m_monitorBox; }

CBox CGlassDecoration::monitorLogicalBox() const {
  return m_monitorLogicalBox;
}

bool CGlassDecoration::frameOnly() const { return m_frameOnly; }
