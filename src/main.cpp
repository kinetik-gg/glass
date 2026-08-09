#include <array>
#include <cmath>
#include <ranges>
#include <sstream>
#include <optional>
#include <stdexcept>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include "glass_decoration.hpp"
#include "glass_shader.hpp"
#include "glass_blur.hpp"
#include "globals.hpp"
#include "layer_glass_pass.hpp"

inline CFunctionHook *g_renderLayerHook = nullptr;
inline CFunctionHook *g_surfacePassHook = nullptr;
inline CHyprSignalListener g_windowOpened;
inline CHyprSignalListener g_configReloaded;
inline CHyprSignalListener g_renderStage;

using OrigRenderLayer = void (*)(void *, PHLLS, PHLMONITOR,
                                 const Time::steady_tp &, bool, bool);

// Radius a matching layer_namespaces entry asks for. Entries are a bare name,
// or name=pill, or name=<logical px>; PILL_ROUNDING resolves later against the
// surface, since a pill is half the short side and that is not known here.
using Glass::PILL_ROUNDING;

static std::optional<float> glassRoundingForLayer(const PHLLS &layer) {
  if (!layer || !g_config.enabled->value())
    return std::nullopt;

  std::stringstream configured{g_config.layerNamespaces->value()};
  std::string candidate;
  while (std::getline(configured, candidate, ',')) {
    const auto first = candidate.find_first_not_of(" \t");
    if (first == std::string::npos)
      continue;
    const auto last = candidate.find_last_not_of(" \t");
    auto entry = candidate.substr(first, last - first + 1);

    std::string rounding;
    if (const auto split = entry.find('='); split != std::string::npos) {
      rounding = entry.substr(split + 1);
      entry = entry.substr(0, split);
    }
    if (entry != layer->m_namespace)
      continue;

    if (rounding.empty())
      return static_cast<float>(g_config.layerRounding->value());
    if (rounding == "pill")
      return PILL_ROUNDING;
    try {
      return std::stof(rounding);
    } catch (const std::exception &) {
      return static_cast<float>(g_config.layerRounding->value());
    }
  }
  return std::nullopt;
}

static bool glassEnabledForLayer(const PHLLS &layer) {
  return glassRoundingForLayer(layer).has_value();
}

static void damageGlassOutputs() {
  if (!g_pHyprRenderer || !State::monitorState())
    return;

  for (const auto &monitor : State::monitorState()->monitors()) {
    if (monitor)
      g_pHyprRenderer->damageMonitor(monitor);
  }
}

using OrigSurfacePassConstructor = void (*)(
    CSurfacePassElement *, const CSurfacePassElement::SRenderData &);

static void hookSurfacePassConstructor(
    CSurfacePassElement *element,
    const CSurfacePassElement::SRenderData &data) {
  auto adapted = data;
  const auto windowHasGlass = adapted.pWindow && adapted.decorate &&
      std::ranges::any_of(adapted.pWindow->m_windowDecorations,
                          [](const auto &decoration) {
                            return decoration->getDisplayName() ==
                                   "Glass";
                          });
  if (windowHasGlass && !adapted.popup && g_config.enabled->value()) {
    // Glass already draws the blurred/refracted background below the
    // client. Hyprland's native surface blur would reconstruct another opaque
    // frosted background here and cover the lens before client alpha is
    // composited, with especially visible cache differences during movement.
    adapted.blur = false;
  }

  if (adapted.pLS && adapted.mainSurface && !adapted.popup &&
      glassEnabledForLayer(adapted.pLS)) {
    adapted.blur = false;
    adapted.dontRound = false;
    // Match CWindow::rounding(): Hyprland scales configured rounding by
    // roundingPower / 2 to preserve perceived roundness, then by output scale.
    const auto requested = *glassRoundingForLayer(adapted.pLS);
    const auto scaled = requested == PILL_ROUNDING
        ? std::min(adapted.w, adapted.h) * 0.5
        : requested * (g_config.cornerPower->value() / 2.F) *
              adapted.pMonitor->m_scale;
    adapted.rounding = static_cast<int>(scaled);
    adapted.roundingPower =
        requested == PILL_ROUNDING ? 2.F : g_config.cornerPower->value();
  }

  reinterpret_cast<OrigSurfacePassConstructor>(g_surfacePassHook->m_original)(
      element, adapted);
}

static void hookRenderLayer(void *renderer, PHLLS layer, PHLMONITOR monitor,
                            const Time::steady_tp &now, bool popups,
                            bool lockscreen) {
  const auto selected = !popups && !lockscreen && glassEnabledForLayer(layer);
  if (selected)
    g_pHyprRenderer->m_renderPass.add(makeUnique<CLayerGlassPassElement>(
        CLayerGlassPassElement::SData{.layer = layer, .monitor = monitor,
                                      .rounding = *glassRoundingForLayer(layer)}));

  reinterpret_cast<OrigRenderLayer>(g_renderLayerHook->m_original)(
      renderer, layer, monitor, now, popups, lockscreen);

  if (selected)
    g_pHyprRenderer->m_renderPass.add(makeUnique<CLayerGlassPassElement>(
        CLayerGlassPassElement::SData{
            .layer = layer, .monitor = monitor,
            .rounding = *glassRoundingForLayer(layer), .frameOnly = true}));
}

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

static void attachGlass(PHLWINDOW window) {
  if (!window)
    return;

  const auto hasDecoration = [&](const std::string_view name) {
    return std::ranges::any_of(window->m_windowDecorations,
                               [&](const auto &decoration) {
                                 return decoration->getDisplayName() == name;
                               });
  };

  if (!hasDecoration("Glass"))
    HyprlandAPI::addWindowDecoration(PHANDLE, window,
                                     makeUnique<CGlassDecoration>(window));
  if (!hasDecoration("Glass Frame"))
    HyprlandAPI::addWindowDecoration(
        PHANDLE, window, makeUnique<CGlassDecoration>(window, true));
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
  PHANDLE = handle;

  // Hyprpm and a manually loaded development build can otherwise coexist
  // under different paths and install duplicate hooks into the same renderer.
  // Reject the second copy before it registers config values or mutates state.
  if (g_pPluginSystem) {
    const auto loadedPlugins = g_pPluginSystem->getAllPlugins();
    const auto duplicate = std::ranges::find_if(
        loadedPlugins, [](const CPlugin *plugin) {
          return plugin && plugin->m_name == "kinetik-glass";
        });
    if (duplicate != loadedPlugins.end())
      throw std::runtime_error(
          "[kinetik-glass] Another Glass instance is already loaded");
  }

  const std::string serverHash = __hyprland_api_get_hash();
  const std::string clientHash = __hyprland_api_get_client_hash();

  if (serverHash != clientHash) {
    HyprlandAPI::addNotification(PHANDLE,
                                 "[kinetik-glass] Plugin/header version mismatch",
                                 CHyprColor{1.F, 0.2F, 0.2F, 1.F}, 5000);
    throw std::runtime_error("[kinetik-glass] Hyprland ABI mismatch: server=" +
                             serverHash + " client=" + clientHash);
  }

  g_config.enabled = makeShared<Config::Values::CBoolValue>(
      "plugin:kinetik_glass:enabled", "Enable glass surfaces", false);
  g_config.tint = makeShared<Config::Values::CColorValue>(
      "plugin:kinetik_glass:tint", "Glass tint", 0xff050505);
  g_config.tintOpacity = makeShared<Config::Values::CFloatValue>(
      "plugin:kinetik_glass:tint_opacity", "Glass tint opacity", 0.38F,
      Config::Values::SFloatValueOptions{.min = 0.F, .max = 1.F});
  g_config.blurOpacity = makeShared<Config::Values::CFloatValue>(
      "plugin:kinetik_glass:blur_opacity", "Background blur opacity", 0.88F,
      Config::Values::SFloatValueOptions{.min = 0.F, .max = 1.F});
  // Body-wide directional lighting read as a vignette on large windows and is
  // gone. Specular definition comes from the frame rings instead.
  g_config.highlightOpacity = makeShared<Config::Values::CFloatValue>(
      "plugin:kinetik_glass:highlight_opacity", "Additive rim light on the bevel",
      0.12F, Config::Values::SFloatValueOptions{.min = 0.F, .max = 1.F});
  g_config.frameHighlightOpacity = makeShared<Config::Values::CFloatValue>(
      "plugin:kinetik_glass:frame_highlight_opacity",
      "One-physical-pixel inner frame highlight opacity", 0.07F,
      Config::Values::SFloatValueOptions{.min = 0.F, .max = 1.F});
  g_config.frameBorderOpacity = makeShared<Config::Values::CFloatValue>(
      "plugin:kinetik_glass:frame_border_opacity",
      "One-physical-pixel dark outer frame opacity", 0.30F,
      Config::Values::SFloatValueOptions{.min = 0.F, .max = 1.F});
  g_config.refraction = makeShared<Config::Values::CFloatValue>(
      "plugin:kinetik_glass:refraction",
      "Bevel steepness; 0 is flat glass, 1 is the sharpest edge", 0.7F,
      Config::Values::SFloatValueOptions{.min = 0.F, .max = 1.F});
  g_config.refractionPower = makeShared<Config::Values::CFloatValue>(
      "plugin:kinetik_glass:refraction_power",
      "Bevel profile exponent; lower widens the bend", 1.F,
      Config::Values::SFloatValueOptions{.min = 0.1F, .max = 8.F});
  g_config.dispersion = makeShared<Config::Values::CFloatValue>(
      "plugin:kinetik_glass:dispersion", "Chromatic dispersion in pixels", 1.2F,
      Config::Values::SFloatValueOptions{.min = 0.F, .max = 8.F});
  g_config.edgeClarity = makeShared<Config::Values::CFloatValue>(
      "plugin:kinetik_glass:edge_clarity",
      "How much detail survives in the refracting rim", 0.75F,
      Config::Values::SFloatValueOptions{.min = 0.F, .max = 1.F});
  g_config.blurPasses = makeShared<Config::Values::CIntValue>(
      "plugin:kinetik_glass:blur_passes",
      "Separable blur iterations; more dissolves finer detail", 4,
      Config::Values::SIntValueOptions{.min = 1, .max = 8});
  g_config.blurDownscale = makeShared<Config::Values::CFloatValue>(
      "plugin:kinetik_glass:blur_downscale",
      "Scene buffer scale; lower widens the blur and costs less", 0.25F,
      Config::Values::SFloatValueOptions{.min = 0.125F, .max = 1.F});
  g_config.saturation = makeShared<Config::Values::CFloatValue>(
      "plugin:kinetik_glass:saturation",
      "How much of the backdrop's colour survives in the material", 1.F,
      Config::Values::SFloatValueOptions{.min = 0.F, .max = 1.F});
  g_config.noise = makeShared<Config::Values::CFloatValue>(
      "plugin:kinetik_glass:noise", "Screen-space glass grain", 0.015F,
      Config::Values::SFloatValueOptions{.min = 0.F, .max = 0.2F});
  g_config.cornerPower = makeShared<Config::Values::CFloatValue>(
      "plugin:kinetik_glass:corner_power",
      "Corner exponent for layer surfaces; windows use their own", 3.37F,
      Config::Values::SFloatValueOptions{.min = 2.F, .max = 10.F});
  g_config.edgeWidth = makeShared<Config::Values::CIntValue>(
      "plugin:kinetik_glass:edge_width",
      "Bevel width in logical px; near the corner radius reads best", 24,
      Config::Values::SIntValueOptions{.min = 4, .max = 512});
  g_config.layerNamespaces = makeShared<Config::Values::CStringValue>(
      "plugin:kinetik_glass:layer_namespaces",
      "Comma-separated layer-shell namespaces that receive glass", "");
  g_config.layerRounding = makeShared<Config::Values::CIntValue>(
      "plugin:kinetik_glass:layer_rounding",
      "Logical corner radius for glass layer surfaces", 10,
      Config::Values::SIntValueOptions{.min = 0, .max = 64});

  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.enabled);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.tint);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.tintOpacity);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.blurOpacity);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.highlightOpacity);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.frameHighlightOpacity);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.frameBorderOpacity);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.refraction);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.refractionPower);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.dispersion);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.edgeClarity);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.blurPasses);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.blurDownscale);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.saturation);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.noise);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.cornerPower);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.edgeWidth);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.layerNamespaces);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_config.layerRounding);

  const auto renderLayerFunctions =
      HyprlandAPI::findFunctionsByName(PHANDLE, "renderLayer");
  if (renderLayerFunctions.empty())
    throw std::runtime_error("[kinetik-glass] Unable to find renderLayer hook");

  g_renderLayerHook = HyprlandAPI::createFunctionHook(
      PHANDLE, renderLayerFunctions[0].address,
      reinterpret_cast<void *>(hookRenderLayer));
  if (!g_renderLayerHook || !g_renderLayerHook->hook())
    throw std::runtime_error("[kinetik-glass] Unable to hook renderLayer");

  // Constructor lookup needs the exported Itanium ABI symbol. Hyprland's
  // function matcher does not return constructors for the demangled spelling.
  const auto surfacePassConstructors = HyprlandAPI::findFunctionsByName(
      PHANDLE, "_ZN19CSurfacePassElementC1ERKNS_11SRenderDataE");
  if (surfacePassConstructors.empty())
    throw std::runtime_error(
        "[kinetik-glass] Unable to find surface pass constructor");

  g_surfacePassHook = HyprlandAPI::createFunctionHook(
      PHANDLE, surfacePassConstructors[0].address,
      reinterpret_cast<void *>(hookSurfacePassConstructor));
  if (!g_surfacePassHook || !g_surfacePassHook->hook())
    throw std::runtime_error("[kinetik-glass] Unable to hook surface pass");

  HyprlandAPI::reloadConfig();

  g_windowOpened = Event::bus()->m_events.window.open.listen(
      [](PHLWINDOW window) { attachGlass(window); });
  g_configReloaded = Event::bus()->m_events.config.reloaded.listen(
      []() { damageGlassOutputs(); });
  // The scene is captured once per monitor per frame; this is what makes the
  // next frame capture again instead of reusing the previous one.
  g_renderStage = Event::bus()->m_events.render.stage.listen(
      [](eRenderStage stage) {
        if (stage == RENDER_BEGIN)
          Glass::Blur::beginFrame();
      });

  for (const auto &window : Desktop::windowState()->windows()) {
    if (window->m_isMapped && !window->isHidden())
      attachGlass(window);
  }
  damageGlassOutputs();

  HyprlandAPI::addNotification(PHANDLE,
                               "[kinetik-glass] Loaded in disabled-safe mode",
                               CHyprColor{0.4F, 0.8F, 1.F, 1.F}, 3500);
  return {"kinetik-glass",
          "Kinetik Glass — refractive glass surfaces for Hyprland",
          "kinetik-gg", "0.8.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
  g_renderStage.reset();
  g_configReloaded.reset();
  g_windowOpened.reset();
  g_pHyprRenderer->m_renderPass.removeAllOfType("CGlassPassElement");
  g_pHyprRenderer->m_renderPass.removeAllOfType("CLayerGlassPassElement");
  Glass::Blur::destroy();

  if (g_renderLayerHook) {
    HyprlandAPI::removeFunctionHook(PHANDLE, g_renderLayerHook);
    g_renderLayerHook = nullptr;
  }

  if (g_surfacePassHook) {
    HyprlandAPI::removeFunctionHook(PHANDLE, g_surfacePassHook);
    g_surfacePassHook = nullptr;
  }

  std::vector<IHyprWindowDecoration *> decorations;
  for (const auto &window : Desktop::windowState()->windows()) {
    for (const auto &decoration : window->m_windowDecorations) {
      const auto name = decoration->getDisplayName();
      if (name == "Glass" || name == "Glass Frame")
        decorations.push_back(decoration.get());
    }
  }
  for (const auto decoration : decorations)
    HyprlandAPI::removeWindowDecoration(PHANDLE, decoration);

  // Removing a decoration damages windows and may schedule another frame.
  g_pHyprRenderer->m_renderPass.removeAllOfType("CGlassPassElement");
  Glass::Shader::destroy();
}
