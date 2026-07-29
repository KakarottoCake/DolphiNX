// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

class PerformanceMetrics;

namespace DolphinSwitch::Performance
{
enum class Profile
{
  Stock = 0,
  Balanced,
  Performance,
  Maximum,
};

enum class Hardware
{
  Unknown,
  Erista,
  Mariko,
};

struct Settings
{
  Profile profile = Profile::Stock;
  bool metrics_logging = false;
};

struct Snapshot
{
  bool session_active = false;
  bool sysclk_available = false;
  bool sysclk_enabled = false;
  bool thermal_guard = false;
  Profile profile = Profile::Stock;
  Hardware hardware = Hardware::Unknown;
  std::string operating_mode = "Unknown";
  std::array<std::uint32_t, 3> requested_mhz{};
  std::array<std::uint32_t, 3> actual_mhz{};
  std::array<std::uint32_t, 3> temperatures_millic{};
  std::int32_t power_mw = 0;
  double fps = 0.0;
  double speed_percent = 0.0;
  double max_speed_percent = 0.0;
  std::string status;
};

Profile ProfileFromInt(int value);
std::string_view ProfileName(Profile profile);
std::string_view ProfileDescription(Profile profile);
std::string_view HardwareName(Hardware hardware);

// Reads the global settings and any game-ID override from launcher.ini.
Settings LoadSettings(std::string_view game_id);

// Host clock overrides exist only for the active emulation session. Any overrides and enabled
// state that existed before the session are restored by EndSession().
void BeginSession(std::string game_id, Settings settings);
void Tick(const PerformanceMetrics& metrics);
void EndSession();

Snapshot GetSnapshot();
}  // namespace DolphinSwitch::Performance
