#pragma once

#include <hyprland/src/render/pass/PassElement.hpp>

class CGlassDecoration;

class CGlassPassElement final : public IPassElement {
public:
  struct SData {
    CGlassDecoration *decoration = nullptr;
    float alpha = 1.F;
  };

  explicit CGlassPassElement(const SData &data);

  std::vector<UP<IPassElement>> draw() override;
  bool needsLiveBlur() override;
  bool needsPrecomputeBlur() override;
  const char *passName() override;
  ePassElementType type() override;
  std::optional<CBox> boundingBox() override;

private:
  SData m_data;
};
