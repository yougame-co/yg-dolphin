// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Orca rollback core: exact whole-machine snapshots taken and restored at one point in the frame.
//
// A snapshot is RAM (MEM1, MEM2, the locked L1 cache) copied by the ring, plus everything else
// through Dolphin's own DoState with RAM skipped. Saves and loads happen at the same instruction
// (the frame boundary), so CPU registers, the stack, CoreTiming, the DSP, IOS and the devices are
// all restored as they were. Subsystems check InSnapshotDoState() to leave out what a rollback
// must not touch: RAM inside DoState, the JIT clears on load (JitInterface and unchanged BATs), and
// NAND file contents.

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "Common/Buffer.h"
#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace Rollback
{
// True while a rollback snapshot is being saved or loaded (CPU thread; single core only).
bool InSnapshotDoState();

// True while frames are being re-run after a load: their host rendering is skipped. (Their audio
// still plays; muting it is not done yet.)
bool IsResimulating();
void SetResimulating(bool resimulating);

// Holds InSnapshotDoState() true for its lifetime.
class SnapshotScope
{
public:
  SnapshotScope();
  ~SnapshotScope();
  SnapshotScope(const SnapshotScope&) = delete;
  SnapshotScope& operator=(const SnapshotScope&) = delete;
};

// A ring of snapshots keyed by frame number. Saving overwrites the oldest slot.
class SnapshotRing
{
public:
  explicit SnapshotRing(std::size_t slots);

  void Save(Core::System& system, s64 frame);
  // Restores the snapshot taken at `frame`. Returns false if it is not in the ring, the state does
  // not load, or the NAND could not be put back. Only call this from the frame-boundary hook, where
  // every snapshot was taken: loading anywhere else would resume at the hook's pinned pc, run the
  // boundary twice and count the hooked instruction's cycles twice.
  bool Load(Core::System& system, s64 frame);
  bool Has(s64 frame) const;
  // Forgets every snapshot and stops the NAND journal (call while emulation is still running).
  void Reset(Core::System& system);

  // The non-RAM state written by the most recent Save (for hashing and diagnostics).
  std::span<const u8> LastState() const;

private:
  struct Slot
  {
    s64 frame = -1;
    std::vector<u8> mem1;
    std::vector<u8> mem2;
    std::vector<u8> l1_cache;
    Common::UniqueBuffer<u8> state;
    std::size_t state_size = 0;
    u64 nand_journal_mark = 0;  // the NAND journal mark this snapshot's state recorded
  };

  Slot* Find(s64 frame);
  const Slot* Find(s64 frame) const;

  std::vector<Slot> m_slots;
  std::size_t m_next = 0;
  std::size_t m_last = 0;
};

// The complete status a GameCube controller port (0-based) reports for the current frame, when the
// rollback session or the test harness supplies it. It replaces the host controller's status
// entirely (analog A/B included), so nothing local leaks into the emulated machine.
std::optional<GCPadStatus> InputOverride(int port);

// Called by the game profile's frame-boundary hook once per game frame.
void OnFrameBoundary(const Core::CPUThreadGuard& guard);
}  // namespace Rollback
