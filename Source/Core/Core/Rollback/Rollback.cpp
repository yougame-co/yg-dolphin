// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Rollback.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <span>
#include <unordered_set>

#include <xxh3.h>

#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/HW/GCPad.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/Harness.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/Rollback/SessionPort.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/VideoState.h"

namespace Rollback
{
namespace
{
std::atomic<bool> s_in_snapshot{false};
std::atomic<bool> s_resimulating{false};

constexpr std::size_t RESTORE_PAGE = 4096;
constexpr u32 MEM1_VIRTUAL = 0x80000000u;
constexpr u32 MEM2_VIRTUAL = 0x90000000u;
// Effective to physical for MEM1/MEM2 and their uncached mirrors (0x8/0xC -> 0x0, 0x9/0xD -> 0x1).
constexpr u32 PHYSICAL_MASK = 0x1FFFFFFFu;

// Copy a saved RAM area back, page by page, touching only pages that differ. A page whose bytes
// change may hold code the JIT compiled from other bytes, so its blocks are invalidated; blocks
// elsewhere survive the load (mainline clears the whole JIT on every state load, which would
// recompile the hot code after every rollback). Changed pages are collected by physical address.
void RestoreRam(Core::System& system, u8* live, const std::vector<u8>& saved, u32 virtual_base,
                std::unordered_set<u32>* changed_pages)
{
  auto& jit = system.GetJitInterface();
  const std::size_t size = saved.size();
  for (std::size_t offset = 0; offset < size; offset += RESTORE_PAGE)
  {
    const std::size_t length = std::min(RESTORE_PAGE, size - offset);
    if (std::memcmp(live + offset, saved.data() + offset, length) == 0)
      continue;
    std::memcpy(live + offset, saved.data() + offset, length);
    const u32 address = virtual_base + static_cast<u32>(offset);
    jit.InvalidateICache(address, static_cast<u32>(length), true);
    changed_pages->insert(address & PHYSICAL_MASK);
  }
}

// The JIT also remembers facts about single instructions: which ones write to the GPU FIFO, use
// paired quantization, or must not assume speculative constants. A restored page may now hold other
// instructions at those addresses, so what was learned there is forgotten. (The JIT's own
// non-forced invalidation does this word by word, far too slow for thousands of pages per load.)
void ForgetLearnedJitAddresses(Core::System& system, const std::unordered_set<u32>& changed_pages)
{
  auto* jit = dynamic_cast<JitBase*>(system.GetJitInterface().GetCore());
  if (!jit || changed_pages.empty())
    return;
  const auto in_changed_page = [&changed_pages](u32 address) {
    return changed_pages.contains(address & PHYSICAL_MASK & ~static_cast<u32>(RESTORE_PAGE - 1));
  };
  std::erase_if(jit->js.fifoWriteAddresses, in_changed_page);
  std::erase_if(jit->js.pairedQuantizeAddresses, in_changed_page);
  std::erase_if(jit->js.noSpeculativeConstantsAddresses, in_changed_page);
}

// The NAND, when it is the host backend (it always is today); its journal undoes NAND changes.
IOS::HLE::FS::HostFileSystem* HostNand(Core::System& system)
{
  IOS::HLE::EmulationKernel* ios = system.GetIOS();
  return ios ? dynamic_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get()) : nullptr;
}
}  // namespace

u64 RamChecksum(std::span<const u8> mem1, std::span<const u8> mem2)
{
  XXH3_state_t* const state = XXH3_createState();
  XXH3_64bits_reset(state);
  XXH3_64bits_update(state, mem1.data(), mem1.size());
  XXH3_64bits_update(state, mem2.data(), mem2.size());
  const u64 hash = XXH3_64bits_digest(state);
  XXH3_freeState(state);
  return hash;
}

bool InSnapshotDoState()
{
  return s_in_snapshot.load(std::memory_order_relaxed);
}

bool IsResimulating()
{
  return s_resimulating.load(std::memory_order_relaxed);
}

void SetResimulating(bool resimulating)
{
  s_resimulating.store(resimulating, std::memory_order_relaxed);
}

SnapshotScope::SnapshotScope()
{
  s_in_snapshot.store(true, std::memory_order_relaxed);
  // Host GPU state (EFB, texture cache, bounding box) never reaches emulated RAM in a session, so
  // snapshots leave it out: no GPU readback per save, no texture-cache reload per load.
  VideoCommon_SetRollbackSnapshot(true);
}

SnapshotScope::~SnapshotScope()
{
  VideoCommon_SetRollbackSnapshot(false);
  s_in_snapshot.store(false, std::memory_order_relaxed);
}

SnapshotRing::SnapshotRing(std::size_t slots) : m_slots(std::max<std::size_t>(slots, 2))
{
}

SnapshotRing::Slot* SnapshotRing::Find(s64 frame)
{
  for (Slot& slot : m_slots)
  {
    if (slot.frame == frame)
      return &slot;
  }
  return nullptr;
}

const SnapshotRing::Slot* SnapshotRing::Find(s64 frame) const
{
  for (const Slot& slot : m_slots)
  {
    if (slot.frame == frame)
      return &slot;
  }
  return nullptr;
}

bool SnapshotRing::Has(s64 frame) const
{
  return Find(frame) != nullptr;
}

void SnapshotRing::Reset(Core::System& system)
{
  for (Slot& slot : m_slots)
    slot.frame = -1;
  m_next = 0;
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
    nand->JournalStop();
}

void SnapshotRing::Save(Core::System& system, s64 frame)
{
  // Re-saving a frame (after a rewind) reuses its slot; otherwise take the next one.
  Slot* slot = Find(frame);
  if (!slot)
  {
    slot = &m_slots[m_next];
    m_next = (m_next + 1) % m_slots.size();
  }
  m_last = static_cast<std::size_t>(slot - m_slots.data());

  // Non-RAM state first, so anything DoState writes into RAM is in the RAM copy.
  {
    SnapshotScope scope;
    slot->state_size = State::SaveToBufferForRollback(system, slot->state);
  }

  auto& memory = system.GetMemory();
  slot->mem1.assign(memory.GetRAM(), memory.GetRAM() + memory.GetRamSize());
  if (memory.GetEXRAM())
    slot->mem2.assign(memory.GetEXRAM(), memory.GetEXRAM() + memory.GetExRamSize());
  else
    slot->mem2.clear();
  slot->l1_cache.assign(memory.GetL1Cache(), memory.GetL1Cache() + memory.GetL1CacheSize());
  slot->frame = frame;

  // NAND changes older than the oldest snapshot can never be undone again.
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
  {
    slot->nand_journal_mark = nand->LastJournalMark();
    u64 oldest = slot->nand_journal_mark;
    for (const Slot& other : m_slots)
    {
      if (other.frame >= 0)
        oldest = std::min(oldest, other.nand_journal_mark);
    }
    nand->JournalTrim(oldest);
  }
}

bool SnapshotRing::Load(Core::System& system, s64 frame)
{
  Slot* slot = Find(frame);
  if (!slot || slot->state_size == 0)
  {
    ERROR_LOG_FMT(CORE, "Rollback: no snapshot for frame {}", frame);
    return false;
  }

  // RAM before the device state: some devices rebuild pending IOS requests by parsing them from RAM
  // while they load (the emulated Bluetooth), and live RAM may already hold the reply to a request
  // that was still pending when this snapshot was taken. Nothing in the device-state load writes RAM
  // in a session: the one writer, the texture cache's flush of pending EFB copies, has nothing to
  // write while sessions keep EFB copies on the GPU.
  auto& memory = system.GetMemory();
  std::unordered_set<u32> changed_pages;
  RestoreRam(system, memory.GetRAM(), slot->mem1, MEM1_VIRTUAL, &changed_pages);
  if (memory.GetEXRAM() && !slot->mem2.empty())
    RestoreRam(system, memory.GetEXRAM(), slot->mem2, MEM2_VIRTUAL, &changed_pages);
  std::memcpy(memory.GetL1Cache(), slot->l1_cache.data(), slot->l1_cache.size());
  ForgetLearnedJitAddresses(system, changed_pages);

  bool ok;
  {
    SnapshotScope scope;
    ok = State::LoadFromBufferForRollback(
        system, std::span<u8>(slot->state.data(), slot->state_size));
  }
  if (!ok)
  {
    ERROR_LOG_FMT(CORE, "Rollback: snapshot for frame {} did not load", frame);
    return false;
  }
  // The NAND journal undid every change since this snapshot, or the NAND is not as it was.
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system); nand && nand->TakeJournalError())
  {
    ERROR_LOG_FMT(CORE, "Rollback: the NAND could not be restored to frame {}", frame);
    return false;
  }

  // Frames after this one are about to be re-run; their snapshots are re-saved as they go.
  for (Slot& other : m_slots)
  {
    if (other.frame > frame)
      other.frame = -1;
  }
  m_next = (static_cast<std::size_t>(slot - m_slots.data()) + 1) % m_slots.size();
  return true;
}

std::optional<u64> SnapshotRing::RamChecksum(s64 frame) const
{
  const Slot* slot = Find(frame);
  if (!slot || slot->state_size == 0)
    return std::nullopt;
  return Rollback::RamChecksum(slot->mem1, slot->mem2);
}

std::span<const u8> SnapshotRing::LastState() const
{
  const Slot& slot = m_slots[m_last];
  return {slot.state.data(), slot.state_size};
}

std::optional<GCPadStatus> InputOverride(int port)
{
  if (const RingPort* session_port = RingPort::Active())
    return session_port->Pad(port);
  if (auto pad = Harness::InputOverride(port))
    return pad;
  // In a session, nothing local reaches the machine before the session supplies pads either: the
  // boot polls every port too, and a host controller's status (its origin, for one) would be
  // latched by the game and diverge much later.
  if (Orca::SessionActive())
    return Orca::Net::DecodePad(Orca::Net::Pad{});
  return std::nullopt;
}

void OnFrameBoundary(const Core::CPUThreadGuard& guard)
{
  // A launcher profile's hook is installed before its loader has put the game in RAM: whatever
  // runs at that address until then is not the game's frame loop.
  if (const Orca::Profile* profile = Orca::ActiveProfile();
      profile && !Orca::FrameHookLive(guard, *profile))
  {
    return;
  }
  Orca::Status::GameStarted();
  // Inside a JIT block, pc and npc are not kept current: a snapshot taken here would record
  // whatever they last held (an exception vector, a stale branch target), and a load would put that
  // back as the next instruction. Pin both to the boundary so every snapshot records where it was
  // taken. The rest of the block (the boundary's own instruction and the branch after it) runs
  // normally and sets them again on exit.
  if (const Orca::Profile* profile = Orca::ActiveProfile(); profile && profile->frame_hook)
  {
    auto& ppc_state = guard.GetSystem().GetPPCState();
    ppc_state.pc = *profile->frame_hook;
    ppc_state.npc = *profile->frame_hook;
  }

  // An online match runs its session here.
  if (Orca::Online::Enabled())
  {
    const auto local_pad = [](int local_seat) {
      // A test script plays the seat this player was given; otherwise the host's first
      // controller, whatever the seat.
      if (const auto scripted = Harness::InputOverride(local_seat))
        return Orca::Net::EncodePad(*scripted);
      g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
      g_controller_interface.UpdateInput();
      return Orca::Net::EncodePad(Pad::GetStatus(0));
    };
    if (const auto rewound_to = OnlineMatch::OnBoundary(guard.GetSystem(), local_pad))
      Harness::RewindTo(*rewound_to);
  }

  Harness::OnFrameBoundary(guard);

  // Re-run frames are thrown away as soon as they are simulated: skip their host rendering (the
  // emulated GPU still runs, so RAM and timing match a rendered frame).
  VideoCommon_SetSkipRender(IsResimulating() || Orca::TestSkipRender());
}
}  // namespace Rollback
