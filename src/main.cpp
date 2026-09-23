#include "zoom_math.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/state/MonitorState.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {

HANDLE pluginHandle = nullptr;

struct SConfig {
  SP<Config::Values::CFloatValue> step;
  SP<Config::Values::CFloatValue> minimum;
  SP<Config::Values::CFloatValue> maximum;
  SP<Config::Values::CFloatValue> snapThreshold;
  SP<Config::Values::CFloatValue> toggleFactor;
  SP<Config::Values::CFloatValue> sensitivity;
  SP<Config::Values::CFloatValue> momentumStrength;
  SP<Config::Values::CFloatValue> momentumDecayMs;
  SP<Config::Values::CFloatValue> momentumMaxSpeed;
  SP<Config::Values::CBoolValue> independentDisplays;
  SP<Config::Values::CBoolValue> rawScroll;
  SP<Config::Values::CBoolValue> invertScroll;
  SP<Config::Values::CBoolValue> consumeScroll;
  SP<Config::Values::CBoolValue> momentum;
  SP<Config::Values::CStringValue> modifier;
} config;

std::unordered_map<MONITORID, float> previousZoom;

struct SMomentum {
  PHLMONITORREF monitor;
  Time::steady_tp lastInput;
  Time::steady_tp lastTick;
  float velocity = 0.F;
  bool hasInput = false;
  bool coasting = false;
  bool fingerEnded = false;
} momentum;

SP<CEventLoopTimer> momentumTimer;

void setZoom(const PHLMONITOR &monitor, float target, bool remember,
             bool immediate);

void stopMomentum() {
  momentum = {};
  if (momentumTimer)
    momentumTimer->updateTimeout(std::nullopt);
}

void tickMomentum(SP<CEventLoopTimer> timer, void*) {
  using namespace std::chrono_literals;
  const auto monitor = momentum.monitor.lock();
  if (!config.momentum->value() || !monitor || !monitor->m_cursorZoom) {
    stopMomentum();
    return;
  }

  const auto now = Time::steadyNow();
  if (!momentum.coasting) {
    if (!momentum.fingerEnded && now - momentum.lastInput < 50ms) {
      timer->updateTimeout(50ms - (now - momentum.lastInput));
      return;
    }
    momentum.coasting = true;
    momentum.velocity = std::clamp(
        momentum.velocity * config.momentumStrength->value(),
        -config.momentumMaxSpeed->value(), config.momentumMaxSpeed->value());
    momentum.lastTick = now;
  } else {
    const auto seconds = std::min(
        std::chrono::duration<float>(now - momentum.lastTick).count(), 0.05F);
    momentum.lastTick = now;
    const auto minimum = config.minimum->value();
    const auto maximum = std::max(minimum, config.maximum->value());
    const auto step = MacOSZoom::momentumStep(
        monitor->m_cursorZoom->value(), momentum.velocity, seconds,
        config.momentumDecayMs->value(), minimum, maximum,
        config.snapThreshold->value());
    momentum.velocity = step.velocity;
    setZoom(monitor, step.zoom, false, true);
  }

  if (std::abs(momentum.velocity) < 0.05F) {
    // At rest, settle a nearly unzoomed view exactly at 1x.
    const auto minimum = config.minimum->value();
    if (momentum.velocity < 0.F &&
        monitor->m_cursorZoom->value() < config.snapThreshold->value())
      setZoom(monitor, minimum, false, true);
    stopMomentum();
  } else {
    timer->updateTimeout(16ms);
  }
}

SDispatchResult failure(std::string message) {
  return {.success = false, .error = std::move(message)};
}

PHLMONITOR monitorUnderPointer() {
  if (!g_pInputManager || !State::monitorState())
    return nullptr;

  return State::monitorState()
      ->query()
      .vec(g_pInputManager->getMouseCoordsInternal())
      .run();
}

std::vector<PHLMONITOR> targetMonitors() {
  if (!State::monitorState())
    return {};

  if (!config.independentDisplays->value())
    return State::monitorState()->monitors();

  const auto monitor = monitorUnderPointer();
  return monitor ? std::vector<PHLMONITOR>{monitor} : std::vector<PHLMONITOR>{};
}

void setZoom(const PHLMONITOR &monitor, float target, bool remember = true,
             bool immediate = false) {
  if (!monitor || !monitor->m_cursorZoom)
    return;

  const auto minimum = config.minimum->value();
  const auto maximum = std::max(minimum, config.maximum->value());
  const auto current = monitor->m_cursorZoom->goal();
  const auto bounded = std::clamp(target, minimum, maximum);

  if (remember && current > minimum)
    previousZoom[monitor->m_id] = current;

  if (immediate)
    monitor->m_cursorZoom->setValueAndWarp(bounded);
  else
    *monitor->m_cursorZoom = bounded;
}

uint32_t modifierMask(std::string value) {
  std::ranges::transform(value, value.begin(),
                         [](unsigned char c) { return std::toupper(c); });

  uint32_t mask = 0;
  std::string token;
  std::istringstream stream(value);
  while (std::getline(stream, token, '+')) {
    token.erase(std::remove_if(token.begin(), token.end(),
                               [](unsigned char c) { return std::isspace(c); }),
                token.end());
    if (token == "CTRL" || token == "CONTROL")
      mask |= HL_MODIFIER_CTRL;
    else if (token == "ALT" || token == "OPTION")
      mask |= HL_MODIFIER_ALT;
    else if (token == "META" || token == "SUPER" || token == "COMMAND" ||
             token == "CMD")
      mask |= HL_MODIFIER_META;
    else if (token == "SHIFT")
      mask |= HL_MODIFIER_SHIFT;
  }
  return mask;
}

void onRawAxis(IPointer::SAxisEvent event, Event::SCallbackInfo &info) {
  if (!config.rawScroll->value() ||
      event.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
    return;

  constexpr uint32_t PRIMARY_MODIFIERS =
      HL_MODIFIER_SHIFT | HL_MODIFIER_CTRL | HL_MODIFIER_ALT | HL_MODIFIER_META;
  const auto required = modifierMask(config.modifier->value());
  const auto active = g_pInputManager->getModsFromAllKBs() & PRIMARY_MODIFIERS;
  if (required == 0 || active != required)
    return;

  const auto monitor = monitorUnderPointer();
  if (!monitor || !monitor->m_cursorZoom)
    return;

  if (event.delta == 0.0) {
    // Finger-scroll stop events let the coast start without waiting for the
    // inactivity timeout; wheels do not get synthetic momentum.
    if (event.source == WL_POINTER_AXIS_SOURCE_FINGER &&
        momentum.monitor.lock() == monitor && momentum.hasInput &&
        !momentum.coasting) {
      momentum.fingerEnded = true;
      momentumTimer->updateTimeout(std::chrono::milliseconds(1));
    }
    info.cancelled = config.consumeScroll->value();
    return;
  }

  const auto minimum = config.minimum->value();
  const auto maximum = std::max(minimum, config.maximum->value());
  const auto sensitivity = config.sensitivity->value();
  const auto next = MacOSZoom::scrollZoom(
      monitor->m_cursorZoom->value(), static_cast<float>(event.delta),
      sensitivity, minimum, maximum, config.snapThreshold->value(),
      config.invertScroll->value());

  setZoom(monitor, next, false, true);
  info.cancelled = config.consumeScroll->value();

  if (!config.momentum->value() ||
      event.source != WL_POINTER_AXIS_SOURCE_FINGER) {
    stopMomentum();
    return;
  }

  const auto now = Time::steadyNow();
  const auto previous = momentum.monitor.lock();
  const auto seconds = std::chrono::duration<float>(now - momentum.lastInput).count();
  const auto zoomDelta = (config.invertScroll->value() ? 1.F : -1.F) *
                         static_cast<float>(event.delta) * sensitivity;
  if (previous == monitor && momentum.hasInput && !momentum.coasting &&
      !momentum.fingerEnded && seconds > 0.F && seconds < 0.06F) {
    const auto sample = zoomDelta / seconds;
    momentum.velocity = sample * momentum.velocity > 0.F
                            ? 0.5F * (momentum.velocity + sample)
                            : sample;
  } else {
    momentum.velocity = 0.F;
  }
  momentum.monitor = monitor;
  momentum.lastInput = now;
  momentum.hasInput = true;
  momentum.coasting = false;
  momentum.fingerEnded = false;
  momentumTimer->updateTimeout(std::chrono::milliseconds(50));
}

std::optional<float> parseFloat(std::string_view input) {
  float value = 0.F;
  const auto [ptr, error] =
      std::from_chars(input.data(), input.data() + input.size(), value);
  if (error != std::errc{} || ptr != input.data() + input.size() ||
      !std::isfinite(value))
    return std::nullopt;
  return value;
}

SDispatchResult adjustZoom(std::string arguments) {
  std::istringstream stream(arguments);
  std::string action;
  stream >> action;

  if (action.empty())
    return failure(
        "macos-zoom requires one of: in, out, reset, toggle, set <factor>");

  auto monitors = targetMonitors();
  if (monitors.empty())
    return failure("macos-zoom could not find a monitor under the pointer");

  const auto minimum = config.minimum->value();
  const auto maximum = std::max(minimum, config.maximum->value());
  const auto step = std::max(1.001F, config.step->value());
  const auto snap = std::clamp(config.snapThreshold->value(), minimum, maximum);

  if (action == "in" || action == "out") {
    const auto multiplier = action == "in" ? step : 1.F / step;
    for (const auto &monitor : monitors) {
      const auto current = monitor->m_cursorZoom->goal();
      setZoom(
          monitor,
          MacOSZoom::boundedZoom(current, multiplier, minimum, maximum, snap),
          false);
    }
    return {};
  }

  if (action == "reset") {
    for (const auto &monitor : monitors)
      setZoom(monitor, minimum);
    return {};
  }

  if (action == "toggle") {
    for (const auto &monitor : monitors) {
      const auto current = monitor->m_cursorZoom->goal();
      if (current > snap) {
        previousZoom[monitor->m_id] = current;
        setZoom(monitor, minimum, false);
      } else {
        const auto remembered = previousZoom.contains(monitor->m_id)
                                    ? previousZoom.at(monitor->m_id)
                                    : config.toggleFactor->value();
        setZoom(monitor, remembered, false);
      }
    }
    return {};
  }

  if (action == "set") {
    std::string factor;
    stream >> factor;
    const auto parsed = parseFloat(factor);
    if (!parsed)
      return failure("macos-zoom set requires a numeric factor");
    for (const auto &monitor : monitors)
      setZoom(monitor, *parsed);
    return {};
  }

  return failure("unknown macos-zoom action: " + action);
}

int luaAdjust(lua_State *state) {
  const char *arguments = luaL_checkstring(state, 1);
  const auto result = adjustZoom(arguments ? arguments : "");

  lua_newtable(state);
  lua_pushboolean(state, result.success);
  lua_setfield(state, -2, "ok");
  if (!result.success) {
    lua_pushstring(state, result.error.c_str());
    lua_setfield(state, -2, "error");
  }
  return 1;
}

void resetAllMonitors() {
  if (!State::monitorState())
    return;
  for (const auto &monitor : State::monitorState()->monitors()) {
    if (monitor && monitor->m_cursorZoom)
      monitor->m_cursorZoom->setValueAndWarp(1.F);
  }
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
  pluginHandle = handle;

  const std::string runtimeHash = __hyprland_api_get_hash();
  const std::string headerHash = __hyprland_api_get_client_hash();
  if (runtimeHash != headerHash) {
    const auto mismatch = "macos-zoom: runtime ABI " + runtimeHash +
                          " does not match build ABI " + headerHash;
    HyprlandAPI::addNotification(
        handle, "[macos-zoom] Hyprland/header version mismatch",
        CHyprColor{1.F, 0.2F, 0.2F, 1.F}, 5000);
    throw std::runtime_error(mismatch);
  }

  config.step = makeShared<Config::Values::CFloatValue>(
      "plugin:macos_zoom:step", "multiplicative zoom per discrete input", 1.20F,
      Config::Values::SFloatValueOptions{.min = 1.001F, .max = 4.F});
  config.minimum = makeShared<Config::Values::CFloatValue>(
      "plugin:macos_zoom:min_factor", "minimum magnification", 1.F,
      Config::Values::SFloatValueOptions{.min = 1.F, .max = 40.F});
  config.maximum = makeShared<Config::Values::CFloatValue>(
      "plugin:macos_zoom:max_factor", "maximum magnification", 40.F,
      Config::Values::SFloatValueOptions{.min = 1.F, .max = 100.F});
  config.snapThreshold = makeShared<Config::Values::CFloatValue>(
      "plugin:macos_zoom:snap_threshold",
      "outward zoom below this snaps to minimum", 1.05F,
      Config::Values::SFloatValueOptions{.min = 1.F, .max = 4.F});
  config.toggleFactor = makeShared<Config::Values::CFloatValue>(
      "plugin:macos_zoom:toggle_factor", "initial factor restored by toggle",
      2.F, Config::Values::SFloatValueOptions{.min = 1.F, .max = 100.F});
  config.sensitivity = makeShared<Config::Values::CFloatValue>(
      "plugin:macos_zoom:sensitivity", "zoom factor change per raw axis unit",
      0.01F, Config::Values::SFloatValueOptions{.min = 0.0001F, .max = 1.F});
  config.momentum = makeShared<Config::Values::CBoolValue>(
      "plugin:macos_zoom:momentum", "coast after touchpad scrolling stops",
      true);
  config.momentumStrength = makeShared<Config::Values::CFloatValue>(
      "plugin:macos_zoom:momentum_strength", "initial coast velocity multiplier",
      1.F, Config::Values::SFloatValueOptions{.min = 0.F, .max = 4.F});
  config.momentumDecayMs = makeShared<Config::Values::CFloatValue>(
      "plugin:macos_zoom:momentum_decay_ms", "coast decay time constant in ms",
      240.F, Config::Values::SFloatValueOptions{.min = 20.F, .max = 2000.F});
  config.momentumMaxSpeed = makeShared<Config::Values::CFloatValue>(
      "plugin:macos_zoom:momentum_max_speed", "maximum coast zoom factors per second",
      12.F, Config::Values::SFloatValueOptions{.min = 0.1F, .max = 100.F});
  config.independentDisplays = makeShared<Config::Values::CBoolValue>(
      "plugin:macos_zoom:independent_displays",
      "zoom only the display under the pointer", true);
  config.rawScroll = makeShared<Config::Values::CBoolValue>(
      "plugin:macos_zoom:raw_scroll", "consume continuous raw scroll deltas",
      true);
  config.invertScroll = makeShared<Config::Values::CBoolValue>(
      "plugin:macos_zoom:invert_scroll", "reverse zoom scroll direction",
      false);
  config.consumeScroll = makeShared<Config::Values::CBoolValue>(
      "plugin:macos_zoom:consume_scroll",
      "prevent matching zoom scrolls from reaching applications", true);
  config.modifier = makeShared<Config::Values::CStringValue>(
      "plugin:macos_zoom:modifier", "modifier for continuous raw scroll",
      "CTRL");

  HyprlandAPI::addConfigValueV2(handle, config.step);
  HyprlandAPI::addConfigValueV2(handle, config.minimum);
  HyprlandAPI::addConfigValueV2(handle, config.maximum);
  HyprlandAPI::addConfigValueV2(handle, config.snapThreshold);
  HyprlandAPI::addConfigValueV2(handle, config.toggleFactor);
  HyprlandAPI::addConfigValueV2(handle, config.sensitivity);
  HyprlandAPI::addConfigValueV2(handle, config.momentum);
  HyprlandAPI::addConfigValueV2(handle, config.momentumStrength);
  HyprlandAPI::addConfigValueV2(handle, config.momentumDecayMs);
  HyprlandAPI::addConfigValueV2(handle, config.momentumMaxSpeed);
  HyprlandAPI::addConfigValueV2(handle, config.independentDisplays);
  HyprlandAPI::addConfigValueV2(handle, config.rawScroll);
  HyprlandAPI::addConfigValueV2(handle, config.invertScroll);
  HyprlandAPI::addConfigValueV2(handle, config.consumeScroll);
  HyprlandAPI::addConfigValueV2(handle, config.modifier);

  if (!HyprlandAPI::addDispatcherV2(handle, "macos-zoom", adjustZoom))
    throw std::runtime_error("macos-zoom: failed to register dispatcher");
  if (!HyprlandAPI::addLuaFunction(handle, "macos_zoom", "adjust", luaAdjust))
    throw std::runtime_error("macos-zoom: failed to register Lua function");

  momentumTimer = makeShared<CEventLoopTimer>(std::nullopt, tickMomentum, nullptr);
  g_pEventLoopManager->addTimer(momentumTimer);

  static auto axisListener = Event::bus()->m_events.input.mouse.axis.listen(
      [](IPointer::SAxisEvent event, Event::SCallbackInfo &info) {
        onRawAxis(event, info);
      });

  HyprlandAPI::reloadConfig();
  return {"macos-zoom", "macOS-like full-screen zoom controls for Hyprland",
          "Anders Åstrand", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
  stopMomentum();
  g_pEventLoopManager->removeTimer(momentumTimer);
  momentumTimer.reset();
  resetAllMonitors();
  previousZoom.clear();
  pluginHandle = nullptr;
}
