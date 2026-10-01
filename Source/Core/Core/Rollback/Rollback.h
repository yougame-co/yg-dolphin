// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Orca rollback core: exact whole-machine snapshots taken and restored at one point in the frame.
//
// A snapshot is RAM (MEM1, MEM2, the locked L1 cache) kept by the ring, plus everything else
// through Dolphin's own DoState with RAM skipped. In a session, MEM1 and MEM2 are kept copy-on-write
// (Rollback/Cow.h): a snapshot holds only the pages written after it; ORCA_TEST_FULL_SNAPSHOTS=1
// and anything outside a session copy them whole. Saves and loads happen at the same instruction
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

// True while frames are being re-run after a load: their host rendering is skipped and the audio
// they produce is dropped (it already played the first time).
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
  ~SnapshotRing();
  SnapshotRing(const SnapshotRing&) = delete;
  SnapshotRing& operator=(const SnapshotRing&) = delete;

  // Returns true when the save had to allocate (a slot's first use, or a bigger state).
  bool Save(Core::System& system, s64 frame);
  // Restores the snapshot taken at `frame`. Returns false if it is not in the ring, the state does
  // not load, or the NAND could not be put back. Only call this from the frame-boundary hook, where
  // every snapshot was taken: loading anywhere else would resume at the hook's pinned pc, run the
  // boundary twice and count the hooked instruction's cycles twice.
  bool Load(Core::System& system, s64 frame);
  bool Has(s64 frame) const;
  // Forgets every snapshot and stops the NAND journal (call while emulation is still running).
  void Reset(Core::System& system);
  // RamChecksum of the snapshot taken at `frame`, or nullopt if it is not in the ring.
  std::optional<u64> RamChecksum(s64 frame) const;

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
    // The copy-on-write snapshot of MEM1 and MEM2 (Cow.h), or 0 when they are copied in mem1/mem2.
    u64 cow_id = 0;
  };

  Slot* Find(s64 frame);
  const Slot* Find(s64 frame) const;
  // Empties a slot (its copy-on-write snapshot merges into the one before it).
  void Forget(Slot* slot);
  // Whether this ring's saves are copy-on-write, decided (and tracking armed) at its first save.
  bool UseCow(Core::System& system, bool* armed_now);

  std::vector<Slot> m_slots;
  std::size_t m_next = 0;
  std::size_t m_last = 0;
  std::optional<bool> m_cow;
};

// XXH3 over MEM1 then MEM2: the state checksum players compare to detect a desync.
u64 RamChecksum(std::span<const u8> mem1, std::span<const u8> mem2);

// The complete status a GameCube controller port (0-based) reports for the current frame: the
// session's pads, the test harness's, or, in a session that has not supplied pads yet (the boot), a
// neutral pad. It replaces the host controller's status entirely (analog A/B, origin), so nothing
// local leaks into the emulated machine.
std::optional<GCPadStatus> InputOverride(int port);

// Called by the game profile's frame-boundary hook once per game frame.
void OnFrameBoundary(const Core::CPUThreadGuard& guard);
}  // namespace Rollback
