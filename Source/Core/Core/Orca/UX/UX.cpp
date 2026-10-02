// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/UX.h"

#include <chrono>
#include <vector>

#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/Overlay.h"

namespace Orca::UX
{
void Init()
{
  Orca::Events::SetLocalPadSource([] { return LocalPad(); });
  InitOverlay();
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
  Orca::Events::SetPlugInCallback({});
  Orca::Events::SetLocalPadSource({});
  ShutdownOverlay();
  StopControllerStream();
}
}  // namespace Orca::UX
