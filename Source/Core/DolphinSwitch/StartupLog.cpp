// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/StartupLog.h"

#include <cstdio>
#include <mutex>

namespace DolphinSwitch
{
namespace
{
std::FILE* s_startup_log = nullptr;
// Boot-time stages come from the host thread, but alerts and core state changes are raised from
// emulation threads, so serialize writes to keep lines from interleaving.
std::mutex s_startup_log_mutex;
}

void InitializeStartupLog()
{
  if (!s_startup_log)
    s_startup_log = std::fopen("sdmc:/dolphinx-startup.log", "w");
}

void LogStartupStage(const char* stage)
{
  std::lock_guard lock{s_startup_log_mutex};
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
