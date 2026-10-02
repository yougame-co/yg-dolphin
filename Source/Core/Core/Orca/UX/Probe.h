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
void ProbeFrame(const Core::CPUThreadGuard& guard);
}  // namespace Orca::UX
