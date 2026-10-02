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
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/Probe.h"

namespace Orca::UX
{
void Init()
{
  Orca::Events::SetLocalPadSource([] { return LocalPad(); });
  InitOverlay();
  // The name tags write emulated memory: the compatibility key carries their version.
  Orca::Events::SetUXCompatVersion(kCompatVersion);
  // At every frame boundary, first runs and re-runs, before the session saves anything there, with
  // the ports in effect at that frame (the session's synced plan; ORCA_UX_TEST_NAMES in harness
  // runs).
  Orca::Events::SetFrameCallback([](const Core::CPUThreadGuard& guard, int frame,
                                    bool resimulating,
                                    const std::vector<Orca::Events::PortInfo>& ports) {
    static bool s_logged = false;
    if (!std::exchange(s_logged, true))
      NOTICE_LOG_FMT(ROLLBACK, "Orca UX: frame hook live at frame {}", frame);
    ProbeFrame(guard, frame);
    // Test only, ORCA_UX_TEST_LATE_NAME=<frame>: a friend's name that reached this machine late.
    // First runs before that frame see it empty, re-runs see it, as when the roster arrives
    // between a frame's first run and its re-run (the known gap in NameTags.h).
    static const int s_late = [] {
      const char* v = std::getenv("ORCA_UX_TEST_LATE_NAME");
      return v && TestKnobsAllowed() ? std::atoi(v) : -1;
    }();
    if (s_late >= 0 && !resimulating && frame < s_late)
    {
      std::vector<Orca::Events::PortInfo> early = ports;
      for (Orca::Events::PortInfo& p : early)
      {
        if (p.remote)
          p.name.clear();
      }
      WriteNameTags(guard, early);
      return;
    }
    WriteNameTags(guard, ports);
  });
  // Nothing is presented while the game waits for a friend: show the last frame again, with the
  // overlay's "Waiting for <name>..." on it.
  Orca::Events::SetStallCallback([](int stalled_ms) { RepresentDuringStall(stalled_ms); });
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
  Orca::Events::SetStallCallback({});
  Orca::Events::SetFrameCallback({});
  Orca::Events::SetPlugInCallback({});
  Orca::Events::SetLocalPadSource({});
  ShutdownOverlay();
  StopControllerStream();
}
}  // namespace Orca::UX
