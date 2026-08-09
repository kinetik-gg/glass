#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

class CLayerGlassPassElement final : public IPassElement {
public:
  struct SData {
    PHLLSREF layer;
    PHLMONITORREF monitor;
    float rounding = 0.F; // logical px, or Glass::PILL_ROUNDING
    bool frameOnly = false;
  };

  explicit CLayerGlassPassElement(const SData &data);

  std::vector<UP<IPassElement>> draw() override;
  bool needsLiveBlur() override;
  bool needsPrecomputeBlur() override;
  const char *passName() override;
  ePassElementType type() override;
  std::optional<CBox> boundingBox() override;

private:
  SData m_data;
  CBox m_monitorLogicalBox;
  CBox m_monitorBox;
};
