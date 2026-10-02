// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/UX.h"

#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/ControllerSource.h"

namespace Orca::UX
{
void Init()
{
  Orca::Events::SetLocalPadSource([] { return LocalPad(); });
}
}  // namespace Orca::UX
