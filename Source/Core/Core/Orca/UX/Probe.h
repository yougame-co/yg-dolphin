// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
// ORCA_UX_PROBE=<script>: scripted RAM writes and dumps at scene-relative frames, for research
// under the harness only (Probe.cpp). Inert without the variable.
// `frame` is the frame hook's (scene-relative frames count from it, so re-runs repeat them).
void ProbeFrame(const Core::CPUThreadGuard& guard, int frame);
// Test knobs (the probe, ORCA_UX_TEST_LATE_NAME) act only where no real player can be met: no
// room, or a dev game's room (ORCA_TEST_DEV_GAME).
bool TestKnobsAllowed();
}  // namespace Orca::UX
