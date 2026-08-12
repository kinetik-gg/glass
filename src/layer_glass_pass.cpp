#include "layer_glass_pass.hpp"

#include <algorithm>

#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include "glass_blur.hpp"
#include "glass_shader.hpp"
#include "globals.hpp"

using namespace Render::GL;

CLayerGlassPassElement::CLayerGlassPassElement(const SData &data)
    : m_data(data) {
  const auto layer = m_data.layer.lock();
  const auto monitor = m_data.monitor.lock();
  if (!layer || !monitor)
    return;

  const auto logical = layer->logicalBox();
  if (!logical)
    return;

  m_monitorLogicalBox = CBox{logical->x - monitor->m_position.x,
                             logical->y - monitor->m_position.y,
                             logical->width, logical->height};
  m_monitorBox =
      m_monitorLogicalBox.copy().scale(monitor->m_scale).round();

  // Layer surfaces need the same pre-pass damage expansion as window glass.
  // A hover often damages only one button or row; without expanding that to
  // the lens's optical footprint, refresh() samples last frame's glass from
  // the untouched part of currentFB and recursively blurs it back into the
  // new frame. The result is the flicker/ghost geometry visible on interactive
  // palettes. This constructor runs while the render pass is still being
  // assembled, which is the point at which claimDamage() can widen the pass.
  if (!m_data.frameOnly)
    Glass::Blur::claimDamage(m_monitorBox);
}

std::vector<UP<IPassElement>> CLayerGlassPassElement::draw() {
  const auto layer = m_data.layer.lock();
  const auto monitor = m_data.monitor.lock();
  if (!g_config.enabled->value() || !validMapped(layer) || !monitor ||
      m_monitorBox.width < 2 || m_monitorBox.height < 2)
    return {};

  // Layer surfaces have no roundingPower() of their own, so the plugin's corner
  // exponent is authoritative here. Mirror CWindow::rounding(), which scales the
  // configured radius by power / 2 to keep perceived roundness consistent.
  // A pill is half the short side of this surface, so it can only be resolved
  // here where the box is known.
  const auto requested = m_data.rounding;
  const auto pill = requested == Glass::PILL_ROUNDING;
  const auto power = pill ? 2.F : g_config.cornerPower->value();
  const auto radius = pill
      ? std::min(m_monitorBox.width, m_monitorBox.height) * 0.5
      : requested * (power / 2.F) * monitor->m_scale;
  const Glass::Shader::SGeometry geometry{
      .box = m_monitorBox,
      .radius = static_cast<float>(radius),
      .roundingPower = power};

  if (m_data.frameOnly) {
    Glass::Shader::drawFrame(
        geometry,
        {.opacity = 1.F,
         .highlightOpacity = g_config.frameHighlightOpacity->value(),
         .borderOpacity = g_config.frameBorderOpacity->value(),
         .width = static_cast<float>(std::round(monitor->m_scale))},
        g_pHyprRenderer->m_renderData.damage);
    return {};
  }

  CRegion visible{g_pHyprRenderer->m_renderData.damage};
  visible.intersect(m_monitorBox);
  if (visible.empty())
    return {};

  const auto scene = Glass::Blur::refresh(monitor, Glass::Blur::EStage::Overlay);
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
    return {};

  g_pHyprOpenGL->renderRect(
      m_monitorBox,
      CHyprColor{static_cast<float>(tint.r), static_cast<float>(tint.g),
                 static_cast<float>(tint.b),
                 std::clamp<float>(g_config.tintOpacity->value(), 0.F, 1.F)},
      {.damage = &g_pHyprRenderer->m_renderData.damage,
       .round = static_cast<int>(geometry.radius),
       .roundingPower = geometry.roundingPower});

  return {};
}

bool CLayerGlassPassElement::needsLiveBlur() { return false; }

bool CLayerGlassPassElement::needsPrecomputeBlur() { return false; }

const char *CLayerGlassPassElement::passName() {
  return "CLayerGlassPassElement";
}

ePassElementType CLayerGlassPassElement::type() { return EK_CUSTOM; }

std::optional<CBox> CLayerGlassPassElement::boundingBox() {
  if (m_monitorLogicalBox.width <= 0 || m_monitorLogicalBox.height <= 0)
    return std::nullopt;

  // Report the optical footprint, not just the surface, so damage that lands
  // beside the lens still schedules this element.
  const auto monitor = m_data.monitor.lock();
  const auto scale = monitor && monitor->m_scale > 0 ? monitor->m_scale : 1.0;
  return m_monitorLogicalBox.copy().expand(
      std::ceil(Glass::Blur::kernelExtent() / scale));
}
