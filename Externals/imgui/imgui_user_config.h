#pragma once

#include <cstddef>

#include "Common/Assert.h"

// A debug build stops on an ImGui assert (Dolphin's ASSERT: a message box asks whether to carry
// on). A release build writes the first failure at each place to stderr and the log
// (ImGuiAssertFailed) and carries on, as ImGui's own default (assert()) does under NDEBUG: ImGui
// only draws over the picture (Dolphin's OSD, Orca's overlay), and nogui answers ASSERT's question
// with no, which traps. Orca 0.3.13 ended a player's game four times on 2026-10-04 so (SIGTRAP, a
// font size of 0 in ImFontAtlasBakedGetOrAdd), with nothing wrong in the game.
#if defined(_DEBUG) || defined(DEBUGFAST)
#define IM_ASSERT(_EXPR) ASSERT(_EXPR)
#else
#define IM_ASSERT(_EXPR)                                                                           \
  do                                                                                               \
  {                                                                                                \
    if (!(_EXPR)) [[unlikely]]                                                                     \
      ::ImGuiAssertFailed(#_EXPR, __FILE__, __LINE__);                                             \
  } while (0)
#endif

// A release build's failed IM_ASSERT (imgui_user_config.cpp): counts it, writes the first one at
// each place (file and line) to stderr and the log, and returns. Any thread.
void ImGuiAssertFailed(const char* expression, const char* file, int line);
// The ImGui asserts that failed in this process so far, in a release build (tests).
std::size_t ImGuiAssertFailures();

#define IMGUI_DISABLE_DEMO_WINDOWS
