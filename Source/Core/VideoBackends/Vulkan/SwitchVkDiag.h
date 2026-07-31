// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Switch-only diagnostics for Vulkan init failures.
//
// Dolphin's own LOG_VULKAN_ERROR output goes nowhere on this port: LogManager file logging is
// never initialized, so a backend that fails to start on a remote device leaves no evidence
// beyond the panic-alert text. This writes to its own file rather than reusing the startup log,
// because that log holds an open FILE* for the whole session and interleaving two handles onto
// one file through libnx's fatfs is asking for trouble.

#ifdef __SWITCH__

#include <cstdarg>
#include <cstdio>

namespace Vulkan
{
inline void SwitchVkDiagLog(const char* format, ...)
{
  std::FILE* log = std::fopen("sdmc:/dolphinx-vulkan.log", "a");
  if (!log)
    return;

  std::va_list args;
  va_start(args, format);
  std::vfprintf(log, format, args);
  va_end(args);

  std::fputc('\n', log);
  std::fflush(log);
  std::fclose(log);
}
}  // namespace Vulkan

#define SWITCH_VK_DIAG(...) ::Vulkan::SwitchVkDiagLog(__VA_ARGS__)

#else

#define SWITCH_VK_DIAG(...) ((void)0)

#endif
