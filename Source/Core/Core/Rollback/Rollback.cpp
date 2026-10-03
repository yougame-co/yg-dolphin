// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Rollback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <span>
#include <tuple>
#include <unordered_set>

#include <xxh3.h>

#if defined(_M_X86_64) || defined(__x86_64__)
#include <xmmintrin.h>
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
#include <intrin.h>
#endif

#include "Common/FPURoundMode.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/HW/GCPad.h"
#include "Core/Orca/JitWarm.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/Cow.h"
#include "Core/Rollback/Diag.h"
#include "Core/Rollback/Harness.h"
#include "Core/Rollback/InputGate.h"
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
// Whether the frame running now is shown (rendered): set at each boundary for the next frame, for
// the overlay's frame meter. CPU thread only.
bool s_frame_shown = true;

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
  // JitInterface only ever holds a JitBase (null for the interpreter). No dynamic_cast: the MSVC
  // build has RTTI off (/GR-), and C4541 is an error under /WX.
  auto* jit = static_cast<JitBase*>(system.GetJitInterface().GetCore());
  if (!jit || changed_pages.empty())
    return;
  const auto in_changed_page = [&changed_pages](u32 address) {
    return changed_pages.contains(address & PHYSICAL_MASK & ~static_cast<u32>(RESTORE_PAGE - 1));
  };
  std::erase_if(jit->js.fifoWriteAddresses, in_changed_page);
  std::erase_if(jit->js.pairedQuantizeAddresses, in_changed_page);
  std::erase_if(jit->js.noSpeculativeConstantsAddresses, in_changed_page);
}

// The NAND, which is always the host backend (FS::MakeFileSystem creates nothing else); its journal
// undoes NAND changes. A static_cast: no RTTI on MSVC (see above).
IOS::HLE::FS::HostFileSystem* HostNand(Core::System& system)
{
  IOS::HLE::EmulationKernel* ios = system.GetIOS();
  return ios ? static_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get()) : nullptr;
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

SnapshotRing::~SnapshotRing()
{
  Cow::Disarm(this);
}

void SnapshotRing::Forget(Slot* slot)
{
  if (slot->cow_id != 0)
    Cow::Drop(slot->cow_id);
  slot->cow_id = 0;
  slot->frame = -1;
}

bool SnapshotRing::UseCow(Core::System& system, bool* armed_now)
{
  *armed_now = false;
  // Tracking stopped under this ring (emulation stopped, or RAM mappings it can't follow): its
  // copy-on-write snapshots are gone.
  if (m_cow.value_or(false) && !Cow::IsArmedFor(this))
  {
    for (Slot& slot : m_slots)
    {
      if (slot.cow_id != 0)
      {
        slot.cow_id = 0;
        slot.frame = -1;
      }
    }
    m_cow.reset();
  }
  if (!m_cow)
  {
    m_cow = Orca::SessionActive() && !Orca::TestFullSnapshots() && Cow::ArmForSystem(system, this);
    *armed_now = *m_cow;
  }
  return *m_cow;
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
  const Slot* slot = Find(frame);
  return slot && (slot->cow_id == 0 || Cow::Has(slot->cow_id));
}

void SnapshotRing::Reset(Core::System& system)
{
  for (Slot& slot : m_slots)
  {
    slot.frame = -1;
    slot.cow_id = 0;
  }
  Cow::Disarm(this);
  m_cow.reset();
  m_next = 0;
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
    nand->JournalStop();
}

bool SnapshotRing::Save(Core::System& system, s64 frame)
{
  // Re-saving a frame (after a rewind) reuses its slot; otherwise take the next one.
  Slot* slot = Find(frame);
  if (!slot)
  {
    slot = &m_slots[m_next];
    m_next = (m_next + 1) % m_slots.size();
  }
  m_last = static_cast<std::size_t>(slot - m_slots.data());
  // Whatever the slot held is replaced (a re-saved frame's snapshot merges into the one before it).
  Forget(slot);
  bool armed_now;
  const bool cow = UseCow(system, &armed_now);
  const auto buffers = [slot] {
    return std::tuple{slot->state.data(), slot->mem1.capacity(), slot->mem2.capacity()};
  };
  const auto buffers_before = buffers();

  // Non-RAM state first, so anything DoState writes into RAM is in the RAM snapshot.
  const auto t0 = std::chrono::steady_clock::now();
  {
    SnapshotScope scope;
    slot->state_size = State::SaveToBufferForRollback(system, slot->state);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto ms = [](auto from, auto to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
  };

  auto& memory = system.GetMemory();
  // RAM as it is now, kept by write-protecting it: the pages written since the previous save are
  // read-only again and this snapshot's undo log is open. (0: tracking stopped; copy instead.)
  if (cow)
    slot->cow_id = Cow::Snapshot();
  if (slot->cow_id != 0)
  {
    if (Diag::SnapTime())
      Diag::AddCowSave(ms(t0, t1), ms(t1, std::chrono::steady_clock::now()));
  }
  else
  {
    Diag::CopyRam(&slot->mem1, memory.GetRAM(), memory.GetRamSize());
    const auto t2 = std::chrono::steady_clock::now();
    if (memory.GetEXRAM())
      Diag::CopyRam(&slot->mem2, memory.GetEXRAM(), memory.GetExRamSize());
    else
      slot->mem2.clear();
    if (Diag::SnapTime())
      Diag::AddSnapshotSave(ms(t0, t1), ms(t1, t2), ms(t2, std::chrono::steady_clock::now()));
  }
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
  // Arming protects all of RAM once: not what a save costs.
  return armed_now || buffers() != buffers_before;
}

bool SnapshotRing::Load(Core::System& system, s64 frame)
{
  return LoadSlot(system, frame, false);
}

bool SnapshotRing::LoadSlot(Core::System& system, s64 frame, bool redisplay)
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
  const auto start = std::chrono::steady_clock::now();
  auto& memory = system.GetMemory();
  std::unordered_set<u32> changed_pages;
  if (slot->cow_id != 0)
  {
    // The undo logs of this snapshot and every newer one give exactly the pages written since it.
    std::vector<u32> changed_blocks;
    if (!Cow::Restore(slot->cow_id, [&changed_blocks](u32 physical, u32 length) {
          for (u32 offset = 0; offset < length; offset += static_cast<u32>(RESTORE_PAGE))
            changed_blocks.push_back(physical + offset);
        }))
    {
      ERROR_LOG_FMT(CORE, "Rollback: the copy-on-write snapshot for frame {} is gone", frame);
      return false;
    }
    // As RestoreRam: MEM1 runs at 0x80000000 and MEM2 at 0x90000000, both physical | 0x80000000.
    auto& jit = system.GetJitInterface();
    for (const u32 physical : changed_blocks)
    {
      jit.InvalidateICache(physical | MEM1_VIRTUAL, static_cast<u32>(RESTORE_PAGE), true);
      changed_pages.insert(physical);
    }
  }
  else
  {
    RestoreRam(system, memory.GetRAM(), slot->mem1, MEM1_VIRTUAL, &changed_pages);
    if (memory.GetEXRAM() && !slot->mem2.empty())
      RestoreRam(system, memory.GetEXRAM(), slot->mem2, MEM2_VIRTUAL, &changed_pages);
  }
  std::memcpy(memory.GetL1Cache(), slot->l1_cache.data(), slot->l1_cache.size());
  ForgetLearnedJitAddresses(system, changed_pages);

  bool ok;
  {
    SnapshotScope scope;
    VideoCommon_SetSnapshotRedisplays(redisplay);
    ok = State::LoadFromBufferForRollback(
        system, std::span<u8>(slot->state.data(), slot->state_size));
    VideoCommon_SetSnapshotRedisplays(false);
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

  // Frames after this one are about to be re-run; their snapshots are re-saved as they go. (A
  // copy-on-write restore already dropped every snapshot saved after this one.)
  for (Slot& other : m_slots)
  {
    if (other.frame > frame || (other.cow_id != 0 && !Cow::Has(other.cow_id)))
      Forget(&other);
  }
  m_next = (static_cast<std::size_t>(slot - m_slots.data()) + 1) % m_slots.size();
  if (Diag::SnapTime())
  {
    Diag::AddRingLoad(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
        changed_pages.size());
  }
  return true;
}

bool SnapshotRing::Capture(Core::System& system, MachineImage* image, bool stop_journal)
{
  Common::UniqueBuffer<u8> state;
  std::size_t state_size;
  {
    SnapshotScope scope;
    state_size = State::SaveToBufferForRollback(system, state);
  }
  if (state_size == 0)
    return false;
  image->state.assign(state.data(), state.data() + state_size);
  auto& memory = system.GetMemory();
  image->mem1.assign(memory.GetRAM(), memory.GetRAM() + memory.GetRamSize());
  if (memory.GetEXRAM())
    image->mem2.assign(memory.GetEXRAM(), memory.GetEXRAM() + memory.GetExRamSize());
  else
    image->mem2.clear();
  image->l1_cache.assign(memory.GetL1Cache(), memory.GetL1Cache() + memory.GetL1CacheSize());
  if (stop_journal)
  {
    if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
      nand->JournalStop();
  }
  return true;
}

bool SnapshotRing::LoadImage(Core::System& system, MachineImage image, s64 frame)
{
  auto& memory = system.GetMemory();
  if (image.state.empty() || image.mem1.size() != memory.GetRamSize() ||
      image.mem2.size() != (memory.GetEXRAM() ? memory.GetExRamSize() : 0) ||
      image.l1_cache.size() != memory.GetL1CacheSize())
  {
    ERROR_LOG_FMT(CORE, "Rollback: a keyframe for frame {} doesn't fit this machine", frame);
    return false;
  }
  Slot* slot = Find(frame);
  if (!slot)
  {
    slot = &m_slots[m_next];
    m_next = (m_next + 1) % m_slots.size();
  }
  // A full-copy slot: Load restores mem1/mem2 only when the slot has no copy-on-write snapshot.
  Forget(slot);
  slot->state.reset(image.state.size());
  std::memcpy(slot->state.data(), image.state.data(), image.state.size());
  slot->state_size = image.state.size();
  slot->mem1 = std::move(image.mem1);
  slot->mem2 = std::move(image.mem2);
  slot->l1_cache = std::move(image.l1_cache);
  slot->frame = frame;
  slot->nand_journal_mark = 0;
  // A joining machine has nothing on screen yet: show the keyframe's frame, as a savestate would.
  return LoadSlot(system, frame, true);
}

std::optional<u64> SnapshotRing::RamChecksum(s64 frame) const
{
  const Slot* slot = Find(frame);
  if (!slot || slot->state_size == 0)
    return std::nullopt;
  // Copy-on-write: RAM as it was at the save, rebuilt from live RAM and the undo logs (no full copy
  // is kept, and the hash costs only the frames that ask for one).
  if (slot->cow_id != 0)
    return Cow::Checksum(slot->cow_id);
  return Rollback::RamChecksum(slot->mem1, slot->mem2);
}

std::span<const u8> SnapshotRing::LastState() const
{
  const Slot& slot = m_slots[m_last];
  return {slot.state.data(), slot.state_size};
}

std::optional<GCPadStatus> InputOverride(int port)
{
  if (const auto pad = RawInputOverride(port))
    return InputGate::GatePad(port, *pad);
  return std::nullopt;
}

std::optional<GCPadStatus> RawInputOverride(int port)
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

namespace
{
// The host's floating-point control register: FPCR on ARM64, MXCSR on x86-64 (0 elsewhere).
u64 HostFloatControl()
{
#if defined(_M_X86_64) || defined(__x86_64__)
  return _mm_getcsr();
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
  return _ReadStatusReg(ARM64_FPCR);
#elif defined(__aarch64__)
  u64 fpcr;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  return fpcr;
#else
  return 0;
#endif
}

// The CPU thread runs with the guest's floating-point mode (PowerPC::RoundingModeUpdated: FPSCR.RN
// and NI as the rounding mode and flush-to-zero, plus FPCR.AH on ARM64 with FEAT_AFP), in compiled
// C++ as much as in JIT code. Under FPCR.AH, FNEG and FABS leave a NaN's sign bit alone, and clang
// builds some integer constants with MOVI + FNEG: a {0, INT_MAX} pair came out {0, -1} in the
// drop-in session (2026-10-01). The hook's own code (the session, the room, snapshots) runs in the
// host's default mode; on the way out the guest's mode is set again from its FPSCR, which a
// snapshot load in the hook may have changed.
class HostFloatScope
{
public:
  explicit HostFloatScope(PowerPC::PowerPCState& ppc_state) : m_ppc_state(ppc_state)
  {
    const u64 guest = HostFloatControl();
    Common::FPU::LoadDefaultSIMDState();
    static bool s_logged = false;
    if (!s_logged && guest != HostFloatControl())
    {
      s_logged = true;
      NOTICE_LOG_FMT(ROLLBACK,
                     "Frame hook: the CPU thread arrived in the guest's floating-point mode "
                     "({:#x}); the hook runs in the host's ({:#x})",
                     guest, HostFloatControl());
    }
  }
  ~HostFloatScope() { PowerPC::RoundingModeUpdated(m_ppc_state); }
  HostFloatScope(const HostFloatScope&) = delete;
  HostFloatScope& operator=(const HostFloatScope&) = delete;

private:
  PowerPC::PowerPCState& m_ppc_state;
};
}  // namespace

u64 HostFloatControlForTests()
{
  return HostFloatControl();
}

void OnFrameBoundary(const Core::CPUThreadGuard& guard)
{
  HostFloatScope host_float(guard.GetSystem().GetPPCState());
  // A launcher profile's hook is installed before its loader has put the game in RAM: whatever
  // runs at that address until then is not the game's frame loop.
  if (const Orca::Profile* profile = Orca::ActiveProfile();
      profile && !Orca::FrameHookLive(guard, *profile))
  {
    InputGate::Clear();
    return;
  }
  Orca::Status::GameStarted();
  // The overlay's frame meter: the frame that just ended, shown unless the last boundary set it to
  // run as a re-run or a catch-up (below). Host time only.
  Orca::Events::NotifyBoundary(s_frame_shown);
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
      // controller, whatever the seat. A re-run's or a catch-up's read is never used: the app's
      // stream times only a frame's own (its input age).
      if (const auto scripted = Harness::InputOverride(local_seat))
        return Orca::Net::EncodePad(*scripted);
      if (const auto from_app = Orca::Events::LocalPad(!IsResimulating()))
        return Orca::Net::EncodePad(*from_app);
      g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
      g_controller_interface.UpdateInput();
      return Orca::Net::EncodePad(Pad::GetStatus(0));
    };
    const auto on_frame = [&guard](int frame, bool resimulating,
                                   const std::vector<Orca::Events::PortInfo>& ports, bool alone) {
      Orca::Events::NotifyFrame(guard, frame, resimulating, ports, alone);
    };
    if (const auto rewound_to = OnlineMatch::OnBoundary(guard.GetSystem(), local_pad, on_frame))
      Harness::RewindTo(*rewound_to);
  }

  Harness::OnFrameBoundary(guard);

  // The input gate's masks for the frame about to run (ORCA.md, "Input gate"): from memory as it
  // is now, after any snapshot or keyframe of this boundary was saved or loaded, on every pass.
  // Before the relatch below, which reads the ports through them.
  InputGate::OnBoundary(guard);

  // Latency (ORCA.md, "Input latency"): the pads just set for the frame about to run go into the SI
  // input buffers now. The game reads them a few ms into the frame from what the SI's last poll
  // latched, and that poll ran just before this hook, so without this every frame played the
  // previous frame's pads: one whole frame (16.7 ms) on every input, local and remote. On every
  // pass, re-runs and catch-up included, after any snapshot of this boundary was saved or loaded,
  // so a snapshot holds the poll's latch and every machine relatches the same pads.
  if (Orca::SessionActive())
    guard.GetSystem().GetSerialInterface().RelatchInputs();

  // Match start (ORCA.md): blocks the next match will run compile while it loads, and a match's own
  // compiles are remembered for the next. Only before a first pass (a re-run's RAM is the past), and
  // not while catching up, whose frames run back to back.
  if (Orca::SessionActive() && !IsResimulating() &&
      !(RingPort::Active() && RingPort::Active()->CatchingUp()))
  {
    Orca::JitWarm::OnBoundary(guard);
  }

  // Re-run frames are thrown away as soon as they are simulated: skip their host rendering (the
  // emulated GPU still runs, so RAM and timing match a rendered frame).
  VideoCommon_SetSkipRender(IsResimulating() || Orca::TestSkipRender());
  s_frame_shown = !IsResimulating();
}
}  // namespace Rollback
