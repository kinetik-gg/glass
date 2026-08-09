#pragma once

#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>

class CGlassDecoration final : public IHyprWindowDecoration {
public:
  explicit CGlassDecoration(PHLWINDOW window, bool frameOnly = false);
  ~CGlassDecoration() override;

  SDecorationPositioningInfo getPositioningInfo() override;
  void onPositioningReply(const SDecorationPositioningReply &reply) override;
  void draw(PHLMONITOR monitor, const float &alpha) override;
  eDecorationType getDecorationType() override;
  void updateWindow(PHLWINDOW window) override;
  void damageEntire() override;
  uint64_t getDecorationFlags() override;
  eDecorationLayer getDecorationLayer() override;
  std::string getDisplayName() override;

  void drawPass(PHLMONITOR monitor, float alpha);
  CBox monitorBox() const;
  CBox monitorLogicalBox() const;
  bool frameOnly() const;

private:
  void updateGeometry(PHLMONITOR monitor);
  static double opticalMargin(const PHLMONITOR &monitor);

  PHLWINDOWREF m_window;
  CBox m_lastGlobalBox;
  CBox m_monitorLogicalBox;
  CBox m_monitorBox;
  bool m_frameOnly = false;
};
