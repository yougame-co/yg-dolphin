// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Engine/Module.h"

#include <atomic>

namespace Orca::Engine
{
namespace
{
std::atomic<Module*> s_module{nullptr};
}  // namespace

void SetModule(Module* module)
{
  // Its writes are recorded against its regions from now on (record-only, Engine/Regions.h).
  if (module)
    SetWriteRegions(module->Id(), module->Regions());
  else
    SetWriteRegions({}, {});
  s_module = module;
}

Module* ActiveModule()
{
  return s_module.load();
}

void MergeMasks(Rollback::InputGate::Masks& masks, const Rollback::InputGate::Masks& more)
{
  for (int port = 0; port < Rollback::InputGate::PORTS; ++port)
  {
    masks[port].buttons |= more[port].buttons;
    masks[port].press |= more[port].press;
    masks[port].a_as |= more[port].a_as;
    masks[port].main_stick = masks[port].main_stick || more[port].main_stick;
    masks[port].c_stick = masks[port].c_stick || more[port].c_stick;
    masks[port].a_centres_stick = masks[port].a_centres_stick || more[port].a_centres_stick;
    masks[port].drop_with_lr |= more[port].drop_with_lr;
    if (more[port].steer)
    {
      masks[port].steer = true;
      masks[port].steer_x = more[port].steer_x;
      masks[port].steer_y = more[port].steer_y;
    }
  }
}
}  // namespace Orca::Engine
