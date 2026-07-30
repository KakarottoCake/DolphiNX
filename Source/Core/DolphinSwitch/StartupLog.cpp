// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/StartupLog.h"

#include <cstdio>

namespace DolphinSwitch
{
namespace
{
std::FILE* s_startup_log = nullptr;
}

void InitializeStartupLog()
{
  if (!s_startup_log)
    s_startup_log = std::fopen("sdmc:/dolphinx-startup.log", "w");
}

void LogStartupStage(const char* stage)
{
  if (!s_startup_log)
    return;

  std::fputs(stage, s_startup_log);
  std::fputc('\n', s_startup_log);
  std::fflush(s_startup_log);
}

void ShutdownStartupLog()
{
  LogStartupStage("shutdown: clean exit");
  if (s_startup_log)
  {
    std::fclose(s_startup_log);
    s_startup_log = nullptr;
  }
}
}  // namespace DolphinSwitch
