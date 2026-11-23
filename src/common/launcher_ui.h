// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace suffering {

// Runs the self-contained Pre-Game Graphical Launcher before FindGame / PrepareGameCode / kernel::Boot.
// Returns true to proceed launching the game, or false if the user closed the launcher / clicked Exit
// (or --launcher-exit was passed for automated testing).
bool RunLauncher(void* hinstance);

}  // namespace suffering
