// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/JitWarm.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <xxh3.h>

#include "Common/CommonPaths.h"
#include "Common/FPURoundMode.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/Orca/Profile.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace Orca::JitWarm
{
namespace
{
constexpr u32 FILE_MAGIC = 0x4D574A4F;  // "OJWM"
constexpr u32 FILE_VERSION = 1;
constexpr u32 MEM2_PHYSICAL = 0x10000000;
constexpr std::size_t MAX_RANGES = 32;
constexpr u32 MAX_RANGE_BYTES = 0x10000;

// Super Smash Bros. Brawl (NTSC-U, revision 2), Project+ included: gfSceneManager* at 0x805a0060, its
// current scene at +0x4, the scene's name (char*) at +0x0 (Rollback/Harness.cpp, Orca/UX/Probe.cpp).
// A match is the scMelee scene, from the end of the loading that follows character and stage
// select (the loading image stays up ~165 frames in Brawl, ~20 in Project+).
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr std::string_view MATCH_SCENE = "scMelee";

// Warm-up: from a match's first frame for WARM_FRAMES frames at most. The first sweep over the
// known blocks gets BUDGET_US per frame boundary (the loading frames run ~1-2 ms of emulation each);
// the blocks whose words weren't in RAM are looked at again every SWEEP_EVERY frames, LATE_BUDGET_US
// per boundary, since that happens during play.
constexpr int WARM_FRAMES = 600;
constexpr int SWEEP_EVERY = 30;
constexpr int BUDGET_US = 8000;
constexpr int LATE_BUDGET_US = 1000;
// Recording: the blocks the game compiles in a match's first RECORD_FRAMES frames; one counts when
// its words are unchanged STABLE_AFTER frames after its compile (so never a block whose code was
// rewritten after its compile without an invalidation: compiling it ahead could run other words
// than the game's first run would).
constexpr int RECORD_FRAMES = 600;
static_assert(WARM_FRAMES <= RECORD_FRAMES);
constexpr int STABLE_AFTER = 60;

template <typename T>
void Put(std::vector<u8>* out, T value)
{
  const std::size_t at = out->size();
  out->resize(at + sizeof(T));
  std::memcpy(out->data() + at, &value, sizeof(T));
}

class Reader
{
public:
  explicit Reader(std::span<const u8> bytes) : m_bytes(bytes) {}
  template <typename T>
  bool Get(T* value)
  {
    if (m_bytes.size() - m_at < sizeof(T))
      return false;
    std::memcpy(value, m_bytes.data() + m_at, sizeof(T));
    m_at += sizeof(T);
    return true;
  }
  bool AtEnd() const { return m_at == m_bytes.size(); }

private:
  std::span<const u8> m_bytes;
  std::size_t m_at = 0;
};

struct Key
{
  u32 effective, physical, flags;
  u64 hash;
  bool operator==(const Key&) const = default;
};
struct KeyHash
{
  std::size_t operator()(const Key& k) const
  {
    return static_cast<std::size_t>(
        XXH3_64bits_withSeed(&k.hash, sizeof(k.hash),
                             (u64{k.effective} << 32) ^ (u64{k.physical} << 3) ^ k.flags));
  }
};
Key KeyOf(const Block& b)
{
  return {b.effective, b.physical, b.flags, b.hash};
}

std::string FilePath()
{
  const Profile* profile = ActiveProfile();
  return File::GetUserPath(D_CACHE_IDX) + (profile ? profile->game_id : "unknown") + ".jitwarm";
}

// The running game is one whose scenes Orca can read: Brawl rev 2, or a launcher profile booting it.
bool KnownGame()
{
  const Profile* profile = ActiveProfile();
  if (!profile || profile->revision != 2)
    return false;
  return profile->IsLauncher() ? profile->disc == "RSBE01" : profile->game_id == "RSBE01";
}

// Whether the current scene is a match; nullopt while the scene can't be read (boot, a transition).
std::optional<bool> InMatch(const Core::CPUThreadGuard& guard)
{
  const auto mapped = [&guard](u32 address) {
    return PowerPC::MMU::HostIsRAMAddress(guard, address);
  };
  if (!mapped(SCENE_MANAGER))
    return std::nullopt;
  const u32 manager = PowerPC::MMU::HostRead<u32>(guard, SCENE_MANAGER);
  if (!mapped(manager + 0x4))
    return std::nullopt;
  const u32 scene = PowerPC::MMU::HostRead<u32>(guard, manager + 0x4);
  if (!mapped(scene))
    return std::nullopt;
  const u32 name = PowerPC::MMU::HostRead<u32>(guard, scene);
  for (u32 i = 0; i <= MATCH_SCENE.size(); ++i)
  {
    if (!mapped(name + i))
      return std::nullopt;
    const char c = static_cast<char>(PowerPC::MMU::HostRead<u8>(guard, name + i));
    if (i == MATCH_SCENE.size())
      return c == '\0';
    if (c != MATCH_SCENE[i])
      return false;
  }
  return false;
}

enum class Status : u8
{
  Waiting,   // its words aren't in RAM (yet): looked at again next sweep
  Present,   // the JIT already had it
  Compiled,  // compiled here
  GivenUp,   // no block came out, or one spanning other ranges (erased unused)
};

struct Fresh
{
  Block block;
  int compiled_at = 0;
};

class State
{
public:
  ~State()
  {
    if (m_io.joinable())
      m_io.join();
  }

  void OnBlockFinalized(const JitBlock& jit_block);
  void OnBoundary(const Core::CPUThreadGuard& guard);

private:
  bool Enabled();
  void StartMatch();
  void EndMatch();
  void StopWarm();
  void Warm(const Core::CPUThreadGuard& guard);
  void CheckStable(int up_to_frame);
  void FinishRecording();
  void LogWarm();
  std::span<const u8> Mem1() const;
  std::span<const u8> Mem2() const;

  // Set once.
  bool m_checked = false;
  bool m_enabled = false;
  int m_budget_us = BUDGET_US;
  bool m_log_frames = false;  // ORCA_JITWARM_LOG=1: a line per frame boundary that compiled
  std::string m_path;

  // Scenes.
  bool m_seen_other_scene = false;  // a match starts only from another scene, not on a join
  bool m_in_match = false;
  int m_match_frame = 0;
  int m_matches = 0;

  // The blocks known before this match, and what this match did with them.
  std::vector<Block> m_known;
  std::vector<Status> m_status;
  // The blocks this sweep looks at, by `when` (what a match runs first, first), and those still
  // waiting for the next sweep.
  std::vector<u32> m_queue, m_waiting;
  std::size_t m_cursor = 0;
  int m_next_sweep = 0;
  bool m_warming = false;
  bool m_compiling = false;  // inside a warm-up compile: not the game's own

  // This match's own compiles.
  bool m_recording = false;
  std::deque<Fresh> m_fresh;
  std::vector<Block> m_stable;
  int m_unstable = 0;

  // Warm-up figures for the log.
  int m_compiled = 0, m_present = 0, m_discarded = 0, m_no_room = 0, m_sweeps = 0;
  int m_first_frame = -1, m_last_frame = -1;
  double m_ms = 0, m_max_frame_ms = 0;

  // The file is read and written on m_io; StartMatch takes what it left in m_ready.
  std::thread m_io;
  std::mutex m_mutex;
  std::optional<std::vector<Block>> m_ready;
  bool m_load_started = false;
};

State& GetState()
{
  static State s_state;
  return s_state;
}

bool State::Enabled()
{
  if (m_checked)
    return m_enabled;
  m_checked = true;
  m_enabled = SessionActive() && KnownGame() && GetEnv("ORCA_JITWARM") != "0";
  if (const std::string budget = GetEnv("ORCA_JITWARM_BUDGET_US"); !budget.empty())
    m_budget_us = std::clamp(std::atoi(budget.c_str()), 0, 100'000);
  m_log_frames = GetEnv("ORCA_JITWARM_LOG") == "1";
  m_path = FilePath();
  if (m_enabled)
    NOTICE_LOG_FMT(ROLLBACK, "JIT warm-up: on ({}, {} us per frame)", m_path, m_budget_us);
  return m_enabled;
}

std::span<const u8> State::Mem1() const
{
  auto& memory = Core::System::GetInstance().GetMemory();
  return {memory.GetRAM(), memory.GetRAM() ? memory.GetRamSizeReal() : 0};
}

std::span<const u8> State::Mem2() const
{
  auto& memory = Core::System::GetInstance().GetMemory();
  return {memory.GetEXRAM(), memory.GetEXRAM() ? memory.GetExRamSizeReal() : 0};
}

void State::OnBlockFinalized(const JitBlock& jit_block)
{
  if (!m_recording || m_compiling)
    return;
  Block b;
  b.effective = jit_block.effectiveAddress;
  b.physical = jit_block.physicalAddress;
  b.flags = jit_block.feature_flags;
  b.when = static_cast<u16>(std::clamp(m_match_frame, 0, 0xFFFF));
  for (const auto& [start, end] : jit_block.physical_addresses)
  {
    if (b.ranges.size() == MAX_RANGES)
      return;
    b.ranges.emplace_back(start, end);
  }
  // Instruction-BAT-mapped code only (InstructionBATTranslate).
  auto& mmu = Core::System::GetInstance().GetMMU();
  if (mmu.InstructionBATTranslate(b.effective) != b.physical)
    return;
  const std::optional<u64> hash = HashRanges(Mem1(), Mem2(), b.ranges);
  if (!hash)
    return;
  b.hash = *hash;
  m_fresh.push_back({std::move(b), m_match_frame});
}

void State::StartMatch()
{
  if (m_io.joinable())
    m_io.join();
  {
    std::lock_guard lock(m_mutex);
    if (m_ready)
    {
      m_known = std::move(*m_ready);
      m_ready.reset();
    }
  }
  m_in_match = true;
  m_match_frame = 0;
  ++m_matches;
  m_queue.resize(m_known.size());
  std::iota(m_queue.begin(), m_queue.end(), 0u);
  std::stable_sort(m_queue.begin(), m_queue.end(),
                   [this](u32 a, u32 b) { return m_known[a].when < m_known[b].when; });
  m_waiting.clear();
  m_status.assign(m_known.size(), Status::Waiting);
  m_cursor = 0;
  m_next_sweep = 0;
  m_warming = !m_known.empty() && m_budget_us > 0;
  m_compiled = m_present = m_discarded = m_no_room = m_sweeps = 0;
  m_first_frame = m_last_frame = -1;
  m_ms = m_max_frame_ms = 0;
  m_recording = true;
  m_fresh.clear();
  m_stable.clear();
  m_unstable = 0;
  NOTICE_LOG_FMT(ROLLBACK, "JIT warm-up: match {} starts, {} blocks known", m_matches,
                 m_known.size());
}

void State::EndMatch()
{
  m_in_match = false;
  StopWarm();
  if (m_recording)
    FinishRecording();
}

void State::StopWarm()
{
  if (!m_warming)
    return;
  m_warming = false;
  LogWarm();
}

void State::LogWarm()
{
  int waiting = 0, given_up = 0;
  for (const Status status : m_status)
  {
    waiting += status == Status::Waiting;
    given_up += status == Status::GivenUp;
  }
  NOTICE_LOG_FMT(ROLLBACK,
                 "JIT warm-up: match {}: {} of {} blocks compiled ahead ({:.1f} ms, frames {} to "
                 "{}, at most {:.1f} ms in one), {} already compiled, {} not in RAM, {} given up "
                 "({} discarded), {} sweeps, {} boundaries without room",
                 m_matches, m_compiled, m_known.size(), m_ms, m_first_frame, m_last_frame,
                 m_max_frame_ms, m_present, waiting, given_up, m_discarded, m_sweeps, m_no_room);
}

void State::CheckStable(int up_to_frame)
{
  const auto mem1 = Mem1(), mem2 = Mem2();
  while (!m_fresh.empty() && m_fresh.front().compiled_at <= up_to_frame)
  {
    Fresh& f = m_fresh.front();
    if (HashRanges(mem1, mem2, f.block.ranges) == f.block.hash)
      m_stable.push_back(std::move(f.block));
    else
      ++m_unstable;
    m_fresh.pop_front();
  }
}

void State::FinishRecording()
{
  // The blocks known go to the merge: warming is over by now (WARM_FRAMES <= RECORD_FRAMES).
  StopWarm();
  m_recording = false;
  // The youngest are checked now: at a match's end their code is still loaded.
  CheckStable(m_match_frame);
  NOTICE_LOG_FMT(ROLLBACK,
                 "JIT warm-up: match {} compiled {} blocks itself in its first {} frames ({} "
                 "rewritten since, not kept)",
                 m_matches, m_stable.size(), m_match_frame, m_unstable);
  if (m_io.joinable())
    m_io.join();
  // Merged and written off the CPU thread; the next match takes the result.
  // Found compiled this match, or compiled here: young again.
  std::vector<bool> seen(m_status.size());
  for (std::size_t i = 0; i < m_status.size(); ++i)
    seen[i] = m_status[i] == Status::Present || m_status[i] == Status::Compiled;
  m_io = std::thread([this, known = std::move(m_known), seen = std::move(seen),
                      learned = std::move(m_stable)]() mutable {
    std::vector<Block> merged = Merge(std::move(known), seen, std::move(learned));
    const std::vector<u8> bytes = Encode(merged);
    const std::string temp = m_path + ".tmp";
    bool written = false;
    if (File::CreateFullPath(m_path))
    {
      File::IOFile out(temp, "wb");
      written = out.WriteBytes(bytes.data(), bytes.size()) && out.Close();
    }
    if (!written || !File::Rename(temp, m_path))
      WARN_LOG_FMT(ROLLBACK, "JIT warm-up: could not write {}", m_path);
    const std::size_t count = merged.size();
    {
      std::lock_guard lock(m_mutex);
      m_ready = std::move(merged);
    }
    INFO_LOG_FMT(ROLLBACK, "JIT warm-up: {} blocks kept ({} bytes)", count, bytes.size());
  });
  m_known.clear();
  m_stable.clear();
  m_status.clear();
  m_queue.clear();
  m_waiting.clear();
}

void State::Warm(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto* jit = static_cast<JitBase*>(system.GetJitInterface().GetCore());
  if (!jit)
    return;
  if (m_cursor == 0 && m_match_frame < m_next_sweep)
    return;
  auto& ppc_state = system.GetPPCState();
  auto& mmu = system.GetMMU();
  JitBaseBlockCache& cache = *jit->GetBlockCache();
  const CPUEmuFeatureFlags flags = ppc_state.feature_flags;
  const auto mem1 = Mem1(), mem2 = Mem2();
  const auto start = std::chrono::steady_clock::now();
  // The first sweep compiles nearly everything, during the loading; a later one mostly finds the
  // other blocks still not in RAM, during play.
  const auto deadline =
      start + std::chrono::microseconds(m_sweeps == 0 ? m_budget_us : LATE_BUDGET_US);
  bool compiled_here = false;
  const int compiled_before = m_compiled, discarded_before = m_discarded;
  int not_in_ram = 0;
  while (true)
  {
    if (m_cursor == m_queue.size())
    {
      // What is still waiting is looked at again in SWEEP_EVERY frames.
      m_queue.swap(m_waiting);
      m_waiting.clear();
      m_cursor = 0;
      m_next_sweep = m_match_frame + SWEEP_EVERY;
      ++m_sweeps;
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline)
      break;
    const u32 index = m_queue[m_cursor];
    const Block& b = m_known[index];
    Status& status = m_status[index];
    if (b.flags != flags || mmu.InstructionBATTranslate(b.effective) != b.physical)
    {
      m_waiting.push_back(index);
      ++m_cursor;
      continue;
    }
    if (cache.GetBlockFromStartAddress(b.effective, flags))
    {
      status = Status::Present;
      ++m_present;
      ++m_cursor;
      continue;
    }
    if (HashRanges(mem1, mem2, b.ranges) != b.hash)
    {
      // Not loaded (yet), or other code at that address this match.
      ++not_in_ram;
      m_waiting.push_back(index);
      ++m_cursor;
      continue;
    }
    if (!jit->CanCompileFromHook())
    {
      ++m_no_room;
      break;  // this block again at the next boundary
    }
    ++m_cursor;
    // The dispatcher compiles with the guest's floating-point mode in force; so does this.
    m_compiling = true;
    PowerPC::RoundingModeUpdated(ppc_state);
    jit->CompileFromHook(b.effective);
    Common::FPU::LoadDefaultSIMDState();
    m_compiling = false;
    compiled_here = true;
    JitBlock* block = cache.GetBlockFromStartAddress(b.effective, flags);
    bool same = block != nullptr;
    if (same)
    {
      auto it = b.ranges.begin();
      for (const auto& [range_start, range_end] : block->physical_addresses)
      {
        if (it == b.ranges.end() || it->first != range_start || it->second != range_end)
        {
          same = false;
          break;
        }
        ++it;
      }
      same = same && it == b.ranges.end();
    }
    if (!same)
    {
      // Not the recorded block (its words came out other than RAM's): as if never compiled.
      if (block)
        cache.EraseSingleBlock(*block);
      ++m_discarded;
      status = Status::GivenUp;
      continue;
    }
    ++m_compiled;
    status = Status::Compiled;
  }
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  m_ms += ms;
  m_max_frame_ms = std::max(m_max_frame_ms, ms);
  if (compiled_here)
  {
    if (m_first_frame < 0)
      m_first_frame = m_match_frame;
    m_last_frame = m_match_frame;
  }
  if (m_log_frames)
  {
    NOTICE_LOG_FMT(ROLLBACK,
                   "JIT warm-up: frame {}: {} compiled, {} discarded, {} not in RAM, {:.2f} ms",
                   m_match_frame, m_compiled - compiled_before, m_discarded - discarded_before,
                   not_in_ram, ms);
  }
  if (m_queue.empty())
    StopWarm();  // nothing left waiting
}

void State::OnBoundary(const Core::CPUThreadGuard& guard)
{
  if (!Enabled())
    return;
  if (!m_load_started)
  {
    m_load_started = true;
    m_io = std::thread([this] {
      std::vector<u8> bytes;
      if (File::IOFile in(m_path, "rb"); in)
      {
        bytes.resize(in.GetSize());
        if (!in.ReadBytes(bytes.data(), bytes.size()))
          bytes.clear();
      }
      std::optional<std::vector<Block>> blocks = bytes.empty() ? std::nullopt : Decode(bytes);
      if (!bytes.empty() && !blocks)
        WARN_LOG_FMT(ROLLBACK, "JIT warm-up: {} is not readable, starting over", m_path);
      std::lock_guard lock(m_mutex);
      if (!m_ready)
        m_ready = blocks ? std::move(*blocks) : std::vector<Block>{};
    });
  }

  const std::optional<bool> in_match = InMatch(guard);
  if (!in_match)
    return;
  if (!m_in_match)
  {
    if (!*in_match)
    {
      m_seen_other_scene = true;
      return;
    }
    if (!m_seen_other_scene)
      return;  // joined a match already running: its code is compiled as it runs
    StartMatch();
  }
  else if (!*in_match)
  {
    EndMatch();
    m_seen_other_scene = true;
    return;
  }
  else
  {
    ++m_match_frame;
  }

  if (m_recording)
  {
    CheckStable(m_match_frame - STABLE_AFTER);
    if (m_match_frame >= RECORD_FRAMES)
      FinishRecording();
  }
  if (m_warming)
  {
    if (m_match_frame >= WARM_FRAMES)
      StopWarm();
    else
      Warm(guard);
  }
}
}  // namespace

std::optional<u64> HashRanges(std::span<const u8> mem1, std::span<const u8> mem2,
                              const std::vector<std::pair<u32, u32>>& ranges)
{
  if (ranges.empty())
    return std::nullopt;
  u64 hash = 0x4F7263614A495457ull;
  for (const auto& [start, end] : ranges)
  {
    if (end <= start || end - start > MAX_RANGE_BYTES)
      return std::nullopt;
    const u8* p = nullptr;
    if (end <= mem1.size())
      p = mem1.data() + start;
    else if (start >= MEM2_PHYSICAL && end - MEM2_PHYSICAL <= mem2.size())
      p = mem2.data() + (start - MEM2_PHYSICAL);
    if (!p)
      return std::nullopt;
    hash = XXH3_64bits_withSeed(p, end - start, hash ^ ((u64{start} << 32) | end));
  }
  return hash;
}

std::vector<u8> Encode(const std::vector<Block>& blocks)
{
  std::vector<u8> out;
  out.reserve(16 + blocks.size() * 32);
  Put<u32>(&out, FILE_MAGIC);
  Put<u32>(&out, FILE_VERSION);
  Put<u32>(&out, static_cast<u32>(blocks.size()));
  for (const Block& b : blocks)
  {
    Put<u32>(&out, b.effective);
    Put<u32>(&out, b.physical);
    Put<u32>(&out, b.flags);
    Put<u64>(&out, b.hash);
    Put<u16>(&out, b.when);
    Put<u8>(&out, b.age);
    Put<u8>(&out, static_cast<u8>(b.ranges.size()));
    for (const auto& [start, end] : b.ranges)
    {
      Put<u32>(&out, start);
      Put<u32>(&out, end);
    }
  }
  Put<u64>(&out, XXH3_64bits(out.data(), out.size()));
  return out;
}

std::optional<std::vector<Block>> Decode(std::span<const u8> bytes)
{
  if (bytes.size() < 3 * sizeof(u32) + sizeof(u64))
    return std::nullopt;
  const std::span<const u8> body = bytes.first(bytes.size() - sizeof(u64));
  u64 checksum;
  std::memcpy(&checksum, bytes.data() + body.size(), sizeof(checksum));
  if (XXH3_64bits(body.data(), body.size()) != checksum)
    return std::nullopt;
  Reader in(body);
  u32 magic, version, count;
  if (!in.Get(&magic) || !in.Get(&version) || !in.Get(&count) || magic != FILE_MAGIC ||
      version != FILE_VERSION || count > 2 * MAX_BLOCKS)
  {
    return std::nullopt;
  }
  std::vector<Block> blocks(count);
  for (Block& b : blocks)
  {
    u8 ranges;
    if (!in.Get(&b.effective) || !in.Get(&b.physical) || !in.Get(&b.flags) || !in.Get(&b.hash) ||
        !in.Get(&b.when) || !in.Get(&b.age) || !in.Get(&ranges) || ranges == 0 ||
        ranges > MAX_RANGES)
    {
      return std::nullopt;
    }
    b.ranges.resize(ranges);
    for (auto& [start, end] : b.ranges)
    {
      if (!in.Get(&start) || !in.Get(&end) || end <= start || end - start > MAX_RANGE_BYTES)
        return std::nullopt;
    }
  }
  if (!in.AtEnd())
    return std::nullopt;
  return blocks;
}

std::vector<Block> Merge(std::vector<Block> known, const std::vector<bool>& seen,
                         std::vector<Block> learned)
{
  for (std::size_t i = 0; i < known.size(); ++i)
  {
    if (i < seen.size() && seen[i])
      known[i].age = 0;
    else if (known[i].age < 255)
      ++known[i].age;
  }
  std::unordered_map<Key, std::size_t, KeyHash> index;
  index.reserve(known.size() + learned.size());
  for (std::size_t i = 0; i < known.size(); ++i)
    index.emplace(KeyOf(known[i]), i);
  for (Block& b : learned)
  {
    b.age = 0;
    const auto [it, inserted] = index.emplace(KeyOf(b), known.size());
    if (inserted)
    {
      known.push_back(std::move(b));
      continue;
    }
    Block& old = known[it->second];
    old.age = 0;
    old.when = std::min(old.when, b.when);
  }
  std::erase_if(known, [](const Block& b) { return b.age > MAX_AGE; });
  if (known.size() > MAX_BLOCKS)
  {
    // The youngest first, and among them what a match runs first.
    std::stable_sort(known.begin(), known.end(), [](const Block& a, const Block& b) {
      return a.age != b.age ? a.age < b.age : a.when < b.when;
    });
    known.resize(MAX_BLOCKS);
  }
  return known;
}

void OnBlockFinalized(const JitBlock& block)
{
  GetState().OnBlockFinalized(block);
}

void OnBoundary(const Core::CPUThreadGuard& guard)
{
  GetState().OnBoundary(guard);
}
}  // namespace Orca::JitWarm
