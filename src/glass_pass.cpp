#include "glass_pass.hpp"

#include <cmath>

#include <hyprland/src/render/Renderer.hpp>

#include "glass_blur.hpp"
#include "glass_decoration.hpp"
#include "globals.hpp"

CGlassPassElement::CGlassPassElement(const SData &data) : m_data(data) {}

std::vector<UP<IPassElement>> CGlassPassElement::draw() {
  if (m_data.decoration)
    m_data.decoration->drawPass(g_pHyprRenderer->m_renderData.pMonitor.lock(),
                                m_data.alpha);

  return {};
}

bool CGlassPassElement::needsLiveBlur() { return false; }

bool CGlassPassElement::needsPrecomputeBlur() { return false; }

const char *CGlassPassElement::passName() { return "CGlassPassElement"; }

ePassElementType CGlassPassElement::type() { return EK_CUSTOM; }

std::optional<CBox> CGlassPassElement::boundingBox() {
  if (!m_data.decoration)
    return std::nullopt;

  const auto box = m_data.decoration->monitorLogicalBox();
  if (box.width <= 0 || box.height <= 0)
    return std::nullopt;

  // Report the optical footprint, not just the surface, so damage that lands
  // beside the lens still schedules this element.
  const auto monitor = g_pHyprRenderer->m_renderData.pMonitor;
  const auto scale = monitor && monitor->m_scale > 0 ? monitor->m_scale : 1.0;
  return box.copy().expand(
      std::ceil(Glass::Blur::kernelExtent() / scale));
}
