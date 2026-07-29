// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/PerformanceManager.h"

#include <switch.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <ranges>
#include <string>
#include <utility>

#include "Common/FileUtil.h"
#include "Common/Config/Config.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/PowerPC/PowerPC.h"
#include "VideoCommon/PerformanceMetrics.h"

namespace DolphinSwitch::Performance
{
namespace
{
// sys-clk IPC command numbers and wire layouts are compatible with retronx-team/sys-clk API v4.
// The original IPC definitions use the Beer-Ware License (Revision 42), by
// <p-sam@d3vs.net>, <natinusala@gmail.com> and <m4x@m4xw.net>.
constexpr const char* SYSCLK_SERVICE_NAME = "sys:clk";
constexpr std::uint32_t SYSCLK_MIN_API_VERSION = 4;
constexpr std::string_view CONFIG_PATH = "sdmc:/switch/dolphin/launcher.ini";
constexpr std::string_view METRICS_DIRECTORY =
    "sdmc:/switch/dolphin/Logs/Performance";
constexpr std::uint32_t THERMAL_GUARD_ENTER_MILLIC = 75000;
constexpr std::uint32_t THERMAL_GUARD_EXIT_MILLIC = 70000;

enum class SysClkProfile : std::uint32_t
{
  Handheld = 0,
  HandheldCharging,
  HandheldChargingUSB,
  HandheldChargingOfficial,
  Docked,
};

enum class SysClkModule : std::uint32_t
{
  CPU = 0,
  GPU,
  MEM,
};

enum class SysClkCommand : std::uint32_t
{
  GetApiVersion = 0,
  GetCurrentContext = 2,
  SetEnabled = 7,
  SetOverride = 8,
};

struct SysClkContext
{
  std::uint8_t enabled;
  std::uint8_t padding[7];
  std::uint64_t application_id;
  SysClkProfile profile;
  std::uint32_t frequencies[3];
  std::uint32_t real_frequencies[3];
  std::uint32_t override_frequencies[3];
  std::uint32_t temperatures[3];
  std::int32_t power[2];
  std::uint32_t ram_load[2];
};
static_assert(sizeof(SysClkContext) == 88);

struct SysClkOverrideArgs
{
  SysClkModule module;
  std::uint32_t hz;
};

struct ClockTargets
{
  std::uint32_t cpu_mhz;
  std::uint32_t gpu_mhz;
  std::uint32_t mem_mhz;
};

struct State
{
  Service service{};
  bool service_open = false;
  bool session_active = false;
  bool original_enabled = false;
  bool changed_enabled = false;
  bool thermal_guard = false;
  bool benchmark_mode = false;
  Hardware hardware = Hardware::Unknown;
  Profile profile = Profile::Stock;
  SysClkProfile operating_mode = SysClkProfile::Handheld;
  std::array<std::uint32_t, 3> original_overrides_hz{};
  std::array<std::uint32_t, 3> applied_targets_hz{};
  std::chrono::steady_clock::time_point started_at{};
  std::chrono::steady_clock::time_point next_update{};
  std::string game_id;
  FILE* metrics_file = nullptr;
  Snapshot snapshot;
};

std::mutex s_mutex;
State s_state;

std::string Trim(std::string value)
{
  const std::size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const std::size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::map<std::string, std::string> ReadLauncherConfig()
{
  std::map<std::string, std::string> values;
  std::ifstream file{std::string(CONFIG_PATH)};
  std::string line;
  while (std::getline(file, line))
  {
    line = Trim(std::move(line));
    if (line.empty() || line.front() == '#' || line.front() == ';' || line.front() == '[')
      continue;
    const std::size_t separator = line.find('=');
    if (separator == std::string::npos)
      continue;
    std::string key = Trim(line.substr(0, separator));
    if (!key.empty())
      values[std::move(key)] = Trim(line.substr(separator + 1));
  }
  return values;
}

int ParseInt(const std::map<std::string, std::string>& values, const std::string& key,
             int fallback)
{
  const auto iterator = values.find(key);
  if (iterator == values.end())
    return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(iterator->second.c_str(), &end, 10);
  return end != iterator->second.c_str() && *end == '\0' ? static_cast<int>(parsed) :
                                                           fallback;
}

bool ParseBool(const std::map<std::string, std::string>& values, const std::string& key,
               bool fallback)
{
  const auto iterator = values.find(key);
  if (iterator == values.end())
    return fallback;
  std::string value = iterator->second;
  std::ranges::transform(value, value.begin(),
                         [](unsigned char character) { return std::tolower(character); });
  if (value == "true" || value == "1" || value == "yes")
    return true;
  if (value == "false" || value == "0" || value == "no")
    return false;
  return fallback;
}

Result GetApiVersion(std::uint32_t* version)
{
  return serviceDispatchOut(&s_state.service,
                            static_cast<std::uint32_t>(SysClkCommand::GetApiVersion), *version);
}

Result GetContext(SysClkContext* context)
{
  return serviceDispatchOut(&s_state.service,
                            static_cast<std::uint32_t>(SysClkCommand::GetCurrentContext),
                            *context);
}

Result SetEnabled(bool enabled)
{
  const std::uint8_t raw = enabled ? 1 : 0;
  return serviceDispatchIn(&s_state.service,
                           static_cast<std::uint32_t>(SysClkCommand::SetEnabled), raw);
}

Result SetOverride(SysClkModule module, std::uint32_t hz)
{
  const SysClkOverrideArgs args{module, hz};
  return serviceDispatchIn(&s_state.service,
                           static_cast<std::uint32_t>(SysClkCommand::SetOverride), args);
}

Hardware DetectHardware()
{
  if (R_FAILED(splInitialize()))
    return Hardware::Unknown;
  std::uint64_t hardware_type = 0;
  const Result result = splGetConfig(SplConfigItem_HardwareType, &hardware_type);
  splExit();
  if (R_FAILED(result))
    return Hardware::Unknown;
  return hardware_type >= 2 && hardware_type <= 5 ? Hardware::Mariko : Hardware::Erista;
}

std::string_view OperatingModeName(SysClkProfile mode)
{
  switch (mode)
  {
  case SysClkProfile::Handheld:
    return "Handheld";
  case SysClkProfile::HandheldCharging:
    return "Handheld charging";
  case SysClkProfile::HandheldChargingUSB:
    return "Handheld USB charging";
  case SysClkProfile::HandheldChargingOfficial:
    return "Handheld official charger";
  case SysClkProfile::Docked:
    return "Docked";
  }
  return "Unknown";
}

constexpr ClockTargets StockTargets(SysClkProfile mode)
{
  if (mode == SysClkProfile::Docked)
    return {1020, 768, 1600};
  return {1020, 384, 1331};
}

constexpr ClockTargets TargetsFor(Profile profile, SysClkProfile mode, Hardware hardware)
{
  const bool mariko = hardware == Hardware::Mariko;
  const bool docked = mode == SysClkProfile::Docked;
  const bool official_charger = mode == SysClkProfile::HandheldChargingOfficial;
  const bool charging = mode == SysClkProfile::HandheldCharging ||
                        mode == SysClkProfile::HandheldChargingUSB || official_charger;

  if (profile == Profile::Stock)
    return StockTargets(mode);
  if (profile == Profile::Balanced)
  {
    if (docked)
      return {1428, 691, 1600};
    if (charging)
      return {1428, mariko ? 614u : 460u, 1600};
    return {1224, mariko ? 614u : 460u, 1600};
  }
  if (profile == Profile::Performance)
  {
    if (docked || official_charger)
      return {1581, 768, 1600};
    if (charging)
      return {1581, mariko ? 614u : 460u, 1600};
    return {1428, mariko ? 614u : 460u, 1600};
  }

  if (docked || official_charger)
    return {1785, 768, 1600};
  if (charging)
    return {1785, mariko ? 614u : 460u, 1600};
  return {1785, mariko ? 614u : 460u, 1600};
}

consteval bool ValidateBundledTargets()
{
  constexpr std::array profiles = {Profile::Stock, Profile::Balanced, Profile::Performance,
                                   Profile::Maximum};
  constexpr std::array modes = {
      SysClkProfile::Handheld,        SysClkProfile::HandheldCharging,
      SysClkProfile::HandheldChargingUSB, SysClkProfile::HandheldChargingOfficial,
      SysClkProfile::Docked,
  };
  constexpr std::array hardware = {Hardware::Unknown, Hardware::Erista, Hardware::Mariko};
  for (const Profile profile : profiles)
  {
    for (const SysClkProfile mode : modes)
    {
      for (const Hardware model : hardware)
      {
        const ClockTargets target = TargetsFor(profile, mode, model);
        const bool unrestricted_gpu =
            mode == SysClkProfile::Docked ||
            mode == SysClkProfile::HandheldChargingOfficial;
        const std::uint32_t gpu_limit =
            unrestricted_gpu ? 768u : model == Hardware::Mariko ? 614u : 460u;
        if (target.cpu_mhz > 1785 || target.gpu_mhz > gpu_limit || target.mem_mhz > 1600)
          return false;
      }
    }
  }
  return true;
}
static_assert(ValidateBundledTargets(),
              "A bundled profile exceeds the conservative standard sys-clk table");

constexpr std::array<std::uint32_t, 3> ToHz(const ClockTargets& targets)
{
  return {targets.cpu_mhz * 1000000, targets.gpu_mhz * 1000000,
          targets.mem_mhz * 1000000};
}

std::array<std::uint32_t, 3> ToMHz(const std::uint32_t values[3])
{
  return {values[0] / 1000000, values[1] / 1000000, values[2] / 1000000};
}

void UpdateSnapshot(const SysClkContext& context, const ClockTargets& targets)
{
  s_state.snapshot.session_active = s_state.session_active;
  s_state.snapshot.sysclk_available = s_state.service_open;
  s_state.snapshot.sysclk_enabled = context.enabled != 0;
  s_state.snapshot.thermal_guard = s_state.thermal_guard;
  s_state.snapshot.benchmark_mode = s_state.benchmark_mode;
  s_state.snapshot.profile = s_state.profile;
  s_state.snapshot.hardware = s_state.hardware;
  s_state.snapshot.operating_mode = OperatingModeName(context.profile);
  s_state.snapshot.requested_mhz = {targets.cpu_mhz, targets.gpu_mhz, targets.mem_mhz};
  s_state.snapshot.actual_mhz = ToMHz(context.real_frequencies);
  std::copy(std::begin(context.temperatures), std::end(context.temperatures),
            s_state.snapshot.temperatures_millic.begin());
  s_state.snapshot.power_mw = context.power[0];
}

bool ApplyTargets(const ClockTargets& targets)
{
  const std::array<std::uint32_t, 3> target_hz = ToHz(targets);
  bool success = true;
  for (std::size_t index = 0; index < target_hz.size(); ++index)
  {
    if (s_state.applied_targets_hz[index] == target_hz[index])
      continue;
    if (R_FAILED(SetOverride(static_cast<SysClkModule>(index), target_hz[index])))
    {
      success = false;
      continue;
    }
    s_state.applied_targets_hz[index] = target_hz[index];
  }
  return success;
}

void OpenMetricsLog()
{
  if (!File::CreateFullPath(std::string(METRICS_DIRECTORY) + "/"))
    return;
  std::time_t raw_time = std::time(nullptr);
  std::tm local_time{};
  localtime_r(&raw_time, &local_time);
  char timestamp[32]{};
  std::strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &local_time);
  const std::string game = s_state.game_id.empty() ? "unknown" : s_state.game_id;
  const std::string path =
      std::string(METRICS_DIRECTORY) + "/" + game + "-" + timestamp + ".csv";
  s_state.metrics_file = std::fopen(path.c_str(), "wb");
  if (!s_state.metrics_file)
    return;
  std::fputs(
      "elapsed_ms,game_id,profile,mode,hardware,fps,vps,speed_pct,max_speed_pct,"
      "requested_cpu_mhz,requested_gpu_mhz,requested_mem_mhz,actual_cpu_mhz,"
      "actual_gpu_mhz,actual_mem_mhz,soc_temp_c,pcb_temp_c,skin_temp_c,power_mw,"
      "thermal_guard,benchmark_mode\n",
      s_state.metrics_file);
}

void WriteMetrics(const PerformanceMetrics& metrics)
{
  if (!s_state.metrics_file)
    return;
  const Snapshot& snapshot = s_state.snapshot;
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - s_state.started_at)
                           .count();
  std::fprintf(
      s_state.metrics_file,
      "%lld,%s,%s,%s,%s,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,%u,%.3f,%.3f,"
      "%.3f,%d,%d,%d\n",
      static_cast<long long>(elapsed), s_state.game_id.c_str(),
      ProfileName(snapshot.profile).data(), snapshot.operating_mode.c_str(),
      HardwareName(snapshot.hardware).data(), metrics.GetFPS(), metrics.GetVPS(),
      metrics.GetSpeed() * 100.0, metrics.GetMaxSpeed() * 100.0, snapshot.requested_mhz[0],
      snapshot.requested_mhz[1], snapshot.requested_mhz[2], snapshot.actual_mhz[0],
      snapshot.actual_mhz[1], snapshot.actual_mhz[2],
      snapshot.temperatures_millic[0] / 1000.0, snapshot.temperatures_millic[1] / 1000.0,
      snapshot.temperatures_millic[2] / 1000.0, snapshot.power_mw,
      snapshot.thermal_guard ? 1 : 0, snapshot.benchmark_mode ? 1 : 0);
  std::fflush(s_state.metrics_file);
}
}  // namespace

Profile ProfileFromInt(int value)
{
  return static_cast<Profile>(
      std::clamp(value, static_cast<int>(Profile::Stock), static_cast<int>(Profile::Maximum)));
}

std::string_view ProfileName(Profile profile)
{
  switch (profile)
  {
  case Profile::Stock:
    return "Stock baseline";
  case Profile::Balanced:
    return "Balanced";
  case Profile::Performance:
    return "Performance";
  case Profile::Maximum:
    return "Maximum (opt-in)";
  }
  return "Stock baseline";
}

std::string_view ProfileDescription(Profile profile)
{
  switch (profile)
  {
  case Profile::Stock:
    return "Official clocks. Use this to prove optimization and compatibility without an overclock.";
  case Profile::Balanced:
    return "Conservative CPU, GPU and memory clocks with separate handheld and docked targets.";
  case Profile::Performance:
    return "Higher stock-table clocks intended for demanding games, with Erista and Mariko caps.";
  case Profile::Maximum:
    return "Maximum stock-table CPU clock. Higher heat and battery use; explicitly opt in.";
  }
  return {};
}

std::string_view HardwareName(Hardware hardware)
{
  switch (hardware)
  {
  case Hardware::Erista:
    return "Erista";
  case Hardware::Mariko:
    return "Mariko";
  case Hardware::Unknown:
    return "Unknown (Erista limits)";
  }
  return "Unknown";
}

Settings LoadSettings(std::string_view game_id)
{
  const auto values = ReadLauncherConfig();
  Settings settings;
  settings.profile = ProfileFromInt(ParseInt(values, "Performance/Profile", 0));
  settings.metrics_logging = ParseBool(values, "Performance/MetricsLogging", false);
  settings.benchmark_mode = ParseBool(values, "Performance/BenchmarkMode", false);
  if (!game_id.empty())
  {
    const std::string prefix = "Performance/Game/" + std::string(game_id) + "/";
    const int local_profile = ParseInt(values, prefix + "Profile", -1);
    if (local_profile >= 0)
      settings.profile = ProfileFromInt(local_profile);
    const int local_logging = ParseInt(values, prefix + "MetricsLogging", -1);
    if (local_logging >= 0)
      settings.metrics_logging = local_logging != 0;
  }
  if (settings.benchmark_mode)
  {
    settings.profile = Profile::Stock;
    settings.metrics_logging = true;
  }
  return settings;
}

void ApplyBenchmarkConfigOverrides(const Settings& settings)
{
  if (!settings.benchmark_mode)
    return;

  Config::ConfigChangeCallbackGuard config_guard;

  Config::SetCurrent(Config::MAIN_CPU_CORE, PowerPC::CPUCore::JITARM64);
  Config::SetCurrent(Config::MAIN_CPU_THREAD, true);
  Config::SetCurrent(Config::MAIN_FASTMEM, true);
  Config::SetCurrent(Config::MAIN_PAGE_TABLE_FASTMEM, true);
  Config::SetCurrent(Config::MAIN_FASTMEM_ARENA, true);
  Config::SetCurrent(Config::MAIN_LARGE_ENTRY_POINTS_MAP, true);
  Config::SetCurrent(Config::MAIN_ACCURATE_CPU_CACHE, false);
  Config::SetCurrent(Config::MAIN_DSP_HLE, true);
  Config::SetCurrent(Config::MAIN_ENABLE_CHEATS, false);
  Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 1.0f);
  Config::SetCurrent(Config::MAIN_OVERCLOCK_ENABLE, false);
  Config::SetCurrent(Config::MAIN_VI_OVERCLOCK_ENABLE, false);

  Config::SetCurrent(Config::GFX_EFB_SCALE, 1);
  Config::SetCurrent(Config::GFX_MSAA, 1u);
  Config::SetCurrent(Config::GFX_SSAA, false);
  Config::SetCurrent(Config::GFX_WIDESCREEN_HACK, false);
  Config::SetCurrent(Config::GFX_HIRES_TEXTURES, false);
  Config::SetCurrent(Config::GFX_CACHE_HIRES_TEXTURES, false);
  Config::SetCurrent(Config::GFX_ENABLE_PIXEL_LIGHTING, false);
  Config::SetCurrent(Config::GFX_ENHANCE_POST_SHADER, std::string{});
  Config::SetCurrent(Config::GFX_LSFG_ENABLED, false);
  Config::SetCurrent(Config::GFX_VSYNC, false);
  Config::SetCurrent(Config::GFX_LOG_RENDER_TIME_TO_FILE, true);
}

void BeginSession(std::string game_id, Settings settings)
{
  std::lock_guard lock{s_mutex};
  if (s_state.session_active)
    return;

  s_state = {};
  s_state.session_active = true;
  s_state.profile = settings.profile;
  s_state.benchmark_mode = settings.benchmark_mode;
  s_state.game_id = std::move(game_id);
  s_state.hardware = DetectHardware();
  s_state.started_at = std::chrono::steady_clock::now();
  s_state.next_update = s_state.started_at;
  s_state.snapshot.session_active = true;
  s_state.snapshot.profile = settings.profile;
  s_state.snapshot.hardware = s_state.hardware;
  s_state.snapshot.benchmark_mode = settings.benchmark_mode;

  if (R_FAILED(smGetService(&s_state.service, SYSCLK_SERVICE_NAME)))
  {
    s_state.snapshot.status =
        "sys-clk is not available; Dolphin left host clocks unchanged";
    if (settings.metrics_logging)
      OpenMetricsLog();
    return;
  }
  s_state.service_open = true;
  s_state.snapshot.sysclk_available = true;

  std::uint32_t api_version = 0;
  SysClkContext context{};
  if (R_FAILED(GetApiVersion(&api_version)) || api_version < SYSCLK_MIN_API_VERSION ||
      R_FAILED(GetContext(&context)))
  {
    s_state.snapshot.status = "The installed sys-clk IPC version is incompatible";
    serviceClose(&s_state.service);
    s_state.service_open = false;
    s_state.snapshot.sysclk_available = false;
    if (settings.metrics_logging)
      OpenMetricsLog();
    return;
  }

  s_state.original_enabled = context.enabled != 0;
  std::copy(std::begin(context.override_frequencies), std::end(context.override_frequencies),
            s_state.original_overrides_hz.begin());
  s_state.operating_mode = context.profile;
  if (!s_state.original_enabled)
  {
    if (R_FAILED(SetEnabled(true)))
    {
      s_state.snapshot.status = "sys-clk is installed but could not be enabled";
      UpdateSnapshot(context, StockTargets(context.profile));
      if (settings.metrics_logging)
        OpenMetricsLog();
      return;
    }
    s_state.changed_enabled = true;
    context.enabled = 1;
  }

  const ClockTargets targets = TargetsFor(s_state.profile, context.profile, s_state.hardware);
  if (ApplyTargets(targets))
    s_state.snapshot.status =
        settings.benchmark_mode ?
            "Benchmark mode active: clean stock baseline and logging enabled" :
            "Session clock profile active; prior overrides will be restored";
  else
    s_state.snapshot.status = "One or more sys-clk overrides could not be applied";
  UpdateSnapshot(context, targets);
  if (settings.metrics_logging)
    OpenMetricsLog();
}

void Tick(const PerformanceMetrics& metrics)
{
  std::lock_guard lock{s_mutex};
  if (!s_state.session_active)
    return;
  const auto now = std::chrono::steady_clock::now();
  if (now < s_state.next_update)
    return;
  s_state.next_update = now + std::chrono::milliseconds(500);
  s_state.snapshot.fps = metrics.GetFPS();
  s_state.snapshot.speed_percent = metrics.GetSpeed() * 100.0;
  s_state.snapshot.max_speed_percent = metrics.GetMaxSpeed() * 100.0;

  if (!s_state.service_open)
  {
    WriteMetrics(metrics);
    return;
  }

  SysClkContext context{};
  if (R_FAILED(GetContext(&context)))
  {
    s_state.snapshot.status = "Lost connection to sys-clk";
    WriteMetrics(metrics);
    return;
  }
  s_state.operating_mode = context.profile;
  const std::uint32_t hottest =
      *std::max_element(std::begin(context.temperatures), std::end(context.temperatures));
  if (!s_state.thermal_guard && hottest >= THERMAL_GUARD_ENTER_MILLIC)
  {
    s_state.thermal_guard = true;
    s_state.applied_targets_hz = {};
    s_state.snapshot.status = "Thermal guard active: reduced to official clocks";
  }
  else if (s_state.thermal_guard && hottest <= THERMAL_GUARD_EXIT_MILLIC)
  {
    s_state.thermal_guard = false;
    s_state.applied_targets_hz = {};
    s_state.snapshot.status = "Temperature recovered: selected profile restored";
  }

  const ClockTargets targets =
      s_state.thermal_guard ? StockTargets(context.profile) :
                              TargetsFor(s_state.profile, context.profile, s_state.hardware);
  if (!ApplyTargets(targets))
    s_state.snapshot.status = "One or more sys-clk overrides could not be applied";
  UpdateSnapshot(context, targets);
  WriteMetrics(metrics);
}

void EndSession()
{
  std::lock_guard lock{s_mutex};
  if (!s_state.session_active)
    return;

  if (s_state.metrics_file)
  {
    std::fflush(s_state.metrics_file);
    std::fclose(s_state.metrics_file);
    s_state.metrics_file = nullptr;
  }
  if (s_state.service_open)
  {
    for (std::size_t index = 0; index < s_state.original_overrides_hz.size(); ++index)
      (void)SetOverride(static_cast<SysClkModule>(index),
                        s_state.original_overrides_hz[index]);
    if (s_state.changed_enabled)
      (void)SetEnabled(s_state.original_enabled);
    serviceClose(&s_state.service);
  }

  const std::string final_status =
      s_state.service_open ? "Previous sys-clk state restored" : s_state.snapshot.status;
  const Profile profile = s_state.profile;
  const Hardware hardware = s_state.hardware;
  s_state = {};
  s_state.snapshot.profile = profile;
  s_state.snapshot.hardware = hardware;
  s_state.snapshot.status = final_status;
}

Snapshot GetSnapshot()
{
  std::lock_guard lock{s_mutex};
  return s_state.snapshot;
}
}  // namespace DolphinSwitch::Performance
