// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "Core/Orca/Engine/MatchTypes.h"

#include "Core/Orca/Session/Events.h"
#include "Core/Rollback/InputGate.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
}

// The seam between the engine and a game module (the Orca split, modules.md section 4). The engine
// owns the three hooks into emulation (the frame callback, the input gate's source and its latch)
// and calls the active module from each; the module owns its game's steps and their order, which
// is part of what makes two builds compatible (UX::kCompatVersion).
//
// S2 commit 4: one module, the builtin Brawl/Project+ one (Games/Brawl/BrawlModule.h), whose three
// entry points are UX.cpp's three hook bodies moved verbatim. Later commits narrow the interface
// toward API v1 (Read, Css, Display, OnHeader...) and move what the session needs out of the module.
namespace Orca::Engine
{
// Verdicts on confirmed frames (S2 commit 5, group C): the module keeps a reading per frame and
// turns frames into game results once they are final, identically on every machine. CPU thread.
class Verdicts
{
public:
  virtual ~Verdicts() = default;
  // Frames through `confirmed` are final. Returns the games they finish, in order; `starts` gets
  // the ranked set games they begin (the reports' ids). Only games begun after `plug_frame` count.
  virtual std::vector<GameResult> Confirm(int confirmed, int plug_frame,
                                          std::vector<int>* starts = nullptr) = 0;
  // Memory was replaced (a keyframe or snapshot load): everything stored is stale.
  virtual void Reset() = 0;
  // The scene of the last final reading, and of the newest (possibly predicted) one.
  virtual std::optional<SceneKind> FinalScene() const = 0;
  virtual std::optional<SceneKind> LatestScene() const = 0;
  // The ranked no-show port (0-based), once frames through it are `confirmed` final.
  virtual std::optional<int> ConfirmNoShow(int confirmed) = 0;
};

class Module
{
public:
  virtual ~Module() = default;
  // A stable id ("brawl-builtin"), for logs.
  virtual std::string_view Id() const = 0;
  // Every frame boundary, first runs and rollback re-runs, before the session saves state there.
  // Every memory write must be a pure function of emulated memory, the frame number and the synced
  // ports, so a re-run writes exactly what the first run did. Runs after the engine's own
  // ApplyGamePatches and before its ApplyToggledGamePatches.
  virtual void OnBoundary(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                          const std::vector<Events::PortInfo>& ports, bool alone) = 0;
  // The input gate's masks for the frame about to run, from memory alone.
  virtual Rollback::InputGate::Masks Masks(const Core::CPUThreadGuard& guard) = 0;
  // The frame's raw buttons, before it runs (writes the module's latch regions only).
  virtual void Latch(const Core::CPUThreadGuard& guard,
                     const std::array<std::optional<GCPadStatus>, Rollback::InputGate::PORTS>& raw) = 0;

  // What the session asks of the module (S2 commit 5 routes Rollback/OnlineMatch.cpp through these).
  virtual Verdicts& Results() = 0;
};

// The module the engine's hooks call. Set by UX::Init before the hooks are installed and cleared by
// UX::Shutdown after they are removed; nullptr: the hooks do the engine's part only.
void SetModule(Module* module);
Module* ActiveModule();

// ORs `more` into `masks`: an input any rule takes is taken.
void MergeMasks(Rollback::InputGate::Masks& masks, const Rollback::InputGate::Masks& more);
}  // namespace Orca::Engine
