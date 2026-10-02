// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/UX.h"

#include <chrono>
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
  // At every frame boundary, first runs and re-runs, before the session saves anything there.
  Orca::Events::SetFrameCallback([](const Core::CPUThreadGuard& guard, int frame, bool) {
    static bool s_logged = false;
    if (!std::exchange(s_logged, true))
      NOTICE_LOG_FMT(ROLLBACK, "Orca UX: frame hook live at frame {}", frame);
    ProbeFrame(guard);
    NameTags().OnFrame(guard, frame);
  });
  // Nothing is presented while the game waits for a friend: show the last frame again, with the
  // overlay's "Waiting for <name>..." on it.
  Orca::Events::SetStallCallback([](int) { RepresentDuringStall(); });
  // The session's one plug-in callback: the overlay's joined/left toasts (local UI only; a re-run
  // frame repeats the same list, which shows nothing new) and the name tags' port lists.
  Orca::Events::SetPlugInCallback([](int frame, const std::vector<Orca::Events::PortInfo>& ports) {
    const double now = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    Overlay().OnPorts(frame, ports, now);
    // The names, for the character select's tags (written at frame boundaries; NameTags.h).
    NameTags().OnPlugIn(frame, ports);
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
