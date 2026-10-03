// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/UX.h"

#include <chrono>
#include <cstdlib>
#include <utility>
#include <vector>

#include "Common/Logging/Log.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "Core/Orca/UX/GamePatches.h"
#include "Core/Orca/UX/MenuText.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/OnlineRules.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Orca/UX/Results.h"

namespace Orca::UX
{
void Init()
{
  StartControllerStreamFromEnvironment();
  Orca::Events::SetLocalPadSource([](bool for_frame) { return LocalPad(for_frame); });
  Orca::Events::SetInputAgeSource([] { return GetInputAge(); });
  InitOverlay();
  // The online rules' input masks (a queue match's locks), evaluated at every boundary.
  Rules::Install();
  // The name tags write emulated memory: the compatibility key carries their version.
  Orca::Events::SetUXCompatVersion(kCompatVersion);
  // At every frame boundary, first runs and re-runs, before the session saves anything there, with
  // the ports in effect at that frame (the session's synced plan; ORCA_UX_TEST_NAMES in harness
  // runs).
  Orca::Events::SetFrameCallback([](const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                                    const std::vector<Orca::Events::PortInfo>& ports, bool alone) {
    static bool s_logged = false;
    if (!std::exchange(s_logged, true))
      NOTICE_LOG_FMT(ROLLBACK, "Orca UX: frame hook live at frame {}", frame);
    ApplyGamePatches(guard);
    // The main menu's words about going online (a pure function of memory, like the patches).
    WriteMenuText(guard);
    // Reads only: the app hears what the player picked in the game's Online menu.
    ReadOnlineMenu(guard, resimulating, alone);
    // Reads only: each game's result in a matchmade room, once its frames are final (Results.h).
    ReadResultsFrame(guard, frame);
    ProbeFrame(guard, frame);
    // This player's own controls in its own game, for the session to carry into a game it joins or
    // hosts (reads only; only while this game is its player's alone, so never a friend's game).
    if (alone)
      ReadOwnControlsFrame(guard, ports);
    // Test only, ORCA_UX_TEST_LATE_NAME=<frame>: a friend's name and controls that reached this
    // machine late. First runs before that frame see them empty, re-runs see them, as when a port's
    // values arrive between a frame's first run and its re-run: what the session's value channel
    // rules out (NameTags.h), shown desyncing.
    static const int s_late = [] {
      const char* v = std::getenv("ORCA_UX_TEST_LATE_NAME");
      return v ? std::atoi(v) : -1;
    }();
    // Checked every frame: a host that boots solo can take a real friend's drop-in later.
    if (s_late >= 0 && !resimulating && frame < s_late && TestKnobsAllowed())
    {
      std::vector<Orca::Events::PortInfo> early = ports;
      // PortInfo.remote differs per machine: the friend is whoever is not on port 1.
      for (Orca::Events::PortInfo& p : early)
      {
        if (p.port != 0)
        {
          p.name.clear();
          p.controls.clear();
        }
      }
      WriteNameTags(guard, early);
    }
    else
    {
      WriteNameTags(guard, ports);
    }
    // The online rules (OnlineRules.h): a queue room's header at an alone boundary, then the
    // locks it calls for (rules, stages, the ready timer), on first runs and re-runs alike.
    Rules::OnFrame(guard, frame, resimulating, alone);
    // Last, after every other writer here: a toggled patch group follows its condition as this
    // boundary's writers left it, so the hook run again at this boundary (a re-run from the
    // snapshot saved here) decides the same and writes nothing new (GamePatches.h).
    ApplyToggledGamePatches(guard);
  });
  // Nothing is presented while the game waits for a friend: show the last frame again, with the
  // overlay's "Waiting for <name>..." on it.
  Orca::Events::SetStallCallback([](int stalled_ms) { RepresentDuringStall(stalled_ms); });
  // The frame meter times the game's shown frames.
  Orca::Events::SetBoundaryCallback([](bool shown) { FrameBoundary(shown); });
  // The session's plug-in callback, for the overlay's joined/left toasts only (local UI; never a
  // source for memory writes: those take the frame callback's ports).
  Orca::Events::SetPlugInCallback([](int frame, const std::vector<Orca::Events::PortInfo>& ports) {
    const double now = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    Overlay().OnPorts(frame, ports, now);
  });
}

void Shutdown()
{
  Rules::Uninstall();
  Orca::Events::SetStallCallback({});
  Orca::Events::SetBoundaryCallback({});
  Orca::Events::SetFrameCallback({});
  Orca::Events::SetPlugInCallback({});
  Orca::Events::SetLocalPadSource({});
  Orca::Events::SetInputAgeSource({});
  ShutdownOverlay();
  StopControllerStream();
}
}  // namespace Orca::UX
