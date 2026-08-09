#pragma once

#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

inline HANDLE PHANDLE = nullptr;

namespace Glass {
// Sentinel for layer_namespaces entries written as name=pill: the radius is
// half the surface's short side, which is only known once it is laid out.
inline constexpr float PILL_ROUNDING = -1.F;
} // namespace Glass

struct SGlassConfig {
  SP<Config::Values::CBoolValue> enabled;
  SP<Config::Values::CColorValue> tint;
  SP<Config::Values::CFloatValue> tintOpacity;
  SP<Config::Values::CFloatValue> blurOpacity;
  SP<Config::Values::CFloatValue> highlightOpacity;
  SP<Config::Values::CFloatValue> frameHighlightOpacity;
  SP<Config::Values::CFloatValue> frameBorderOpacity;
  SP<Config::Values::CFloatValue> refraction;
  SP<Config::Values::CFloatValue> refractionPower;
  SP<Config::Values::CFloatValue> dispersion;
  SP<Config::Values::CFloatValue> edgeClarity;
  SP<Config::Values::CFloatValue> saturation;
  SP<Config::Values::CIntValue> blurPasses;
  SP<Config::Values::CFloatValue> blurDownscale;
  SP<Config::Values::CFloatValue> noise;
  SP<Config::Values::CFloatValue> cornerPower;
  SP<Config::Values::CIntValue> edgeWidth;
  SP<Config::Values::CStringValue> layerNamespaces;
  SP<Config::Values::CIntValue> layerRounding;
};

inline SGlassConfig g_config = {};
