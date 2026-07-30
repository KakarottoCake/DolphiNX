// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace DolphinSwitch
{
void InitializeStartupLog();
void LogStartupStage(const char* stage);
void ShutdownStartupLog();
}  // namespace DolphinSwitch
