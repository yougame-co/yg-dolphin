// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Harness.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <xxhash.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/Orca/Profile.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/Rollback/Rollback.h"
#include "Core/System.h"

namespace Rollback::Harness
{
namespace
{
int EnvInt(const char* name, int fallback)
{
  const char* v = std::getenv(name);
  return v && *v ? std::atoi(v) : fallback;
}

const int s_scenes = EnvInt("YG_SCENES", 0);
const int s_synctest_k = EnvInt("YG_SYNCTEST", 0);
const int s_synctest_from = EnvInt("YG_SYNCTEST_FROM", 300);
const int s_exit_after = EnvInt("YG_EXIT_AFTER", 0);
const int s_shot_every = EnvInt("YG_SHOT_EVERY", 0);
const int s_shot_from = EnvInt("YG_SHOT_FROM", 0);
const int s_diag = EnvInt("YG_SYNCTEST_DIAG", 0);
const int s_bench_k = EnvInt("YG_BENCH", 0);
// Re-run frames skip host rendering, as they will in a match; YG_SYNCTEST_RENDER=1 renders them.
const int s_render_reruns = EnvInt("YG_SYNCTEST_RENDER", 0);
// YG_SYNCTEST_CLEARJIT=1 drops every JIT block after each load, as mainline's state loads do, so
// re-runs compile blocks with other boundaries than the first pass (as a peer with another
// rollback history would).
const int s_clear_jit = EnvInt("YG_SYNCTEST_CLEARJIT", 0);

// ---- Scene names (scene-relative input and logs) ----

// Super Smash Bros. Brawl (NTSC-U, revision 2): gfSceneManager* at 0x805a0060, its current scene at
// +0x4, the scene's name (char*) at +0x0 (scTitle, muMenuMain, scSelctCharacter, scMelee...).
std::string BrawlSceneName(const Core::CPUThreadGuard& guard)
{
  const u32 manager = PowerPC::MMU::HostRead<u32>(guard, 0x805a0060);
  if (manager < 0x80000000u)
    return {};
  const u32 scene = PowerPC::MMU::HostRead<u32>(guard, manager + 0x4);
  if (scene < 0x80000000u)
    return {};
  const u32 name_ptr = PowerPC::MMU::HostRead<u32>(guard, scene);
  if (name_ptr < 0x80000000u)
    return {};
  std::string name;
  for (u32 i = 0; i < 32; ++i)
  {
    const char c = static_cast<char>(PowerPC::MMU::HostRead<u8>(guard, name_ptr + i));
    if (c == '\0')
      break;
    name.push_back(c);
  }
  return name;
}

using SceneReader = std::string (*)(const Core::CPUThreadGuard&);

SceneReader SceneReaderForRunningGame()
{
  const SConfig& config = SConfig::GetInstance();
  if (config.GetGameID() == "RSBE01" && config.GetRevision() == 2)
    return BrawlSceneName;
  return nullptr;
}

FILE* OpenHashLog()
{
  const char* path = std::getenv("YG_HASHLOG");
  return path && *path ? std::fopen(path, "w") : nullptr;
}
FILE* const s_hashlog = OpenHashLog();

// ---- Scripted input ----

struct InputRule
{
  int from = 0, to = 0, port = 0, period = 0;
  u32 fuzz_seed = 0;  // nonzero: pseudo-random input, new every 8 frames
  std::string scene;  // nonempty: from/to count frames since this scene was entered
  u16 buttons = 0;
  int sx = -1, sy = -1, cx = -1, cy = -1;
};

std::vector<InputRule> LoadInputScript()
{
  std::vector<InputRule> rules;
  const char* path = std::getenv("YG_INPUT");
  if (!path || !*path)
    return rules;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line))
  {
    if (const auto hash = line.find('#'); hash != std::string::npos)
      line.resize(hash);
    std::istringstream words(line);
    std::string first;
    if (!(words >> first))
      continue;
    InputRule r;
    if (first.size() > 1 && first[0] == '@')
    {
      r.scene = first.substr(1);
      if (!(words >> first))
        continue;
    }
    if (first == "mash")
      words >> r.from >> r.to >> r.port >> r.period;
    else if (first == "fuzz")
      words >> r.from >> r.to >> r.port >> r.fuzz_seed;
    else
    {
      r.from = std::atoi(first.c_str());
      words >> r.to >> r.port;
    }
    r.port -= 1;
    std::string w;
    while (words >> w)
    {
      static const std::map<std::string, u16> names = {
          {"A", 0x0100},    {"B", 0x0200},     {"X", 0x0400},    {"Y", 0x0800},
          {"Z", 0x0010},    {"R", 0x0020},     {"L", 0x0040},    {"START", 0x1000},
          {"LEFT", 0x0001}, {"RIGHT", 0x0002}, {"DOWN", 0x0004}, {"UP", 0x0008}};
      if (auto it = names.find(w); it != names.end())
        r.buttons |= it->second;
      else if (w.rfind("SX=", 0) == 0)
        r.sx = std::atoi(w.c_str() + 3);
      else if (w.rfind("SY=", 0) == 0)
        r.sy = std::atoi(w.c_str() + 3);
      else if (w.rfind("CX=", 0) == 0)
        r.cx = std::atoi(w.c_str() + 3);
      else if (w.rfind("CY=", 0) == 0)
        r.cy = std::atoi(w.c_str() + 3);
    }
    if (r.port >= 0 && r.port < 4)
      rules.push_back(r);
  }
  NOTICE_LOG_FMT(ROLLBACK, "Harness input: {} rules from {}{}", rules.size(), path,
                 in.is_open() ? "" : " (could not open the file)");
  return rules;
}

// Loaded on first use (after logging is up), so a bad path shows in the log.
const std::vector<InputRule>& Input()
{
  static const std::vector<InputRule> s_rules = LoadInputScript();
  return s_rules;
}

// ---- Frame and scene bookkeeping ----

int s_frame = 0;       // the frame being run, or whose end we are at (rewinds on a load)
int s_max_frame = -1;  // highest frame reached on a first pass
bool s_stop_queued = false;
std::string s_scene;
int s_scene_start = 0;
// (scene, entry frame) at the end of each recent frame, so a load can restore them.
std::map<int, std::pair<std::string, int>> s_scene_history;

// ---- Hashing ----

constexpr u32 REGION_SIZE = 64 * 1024;
constexpr u32 MEM2_PHYSICAL = 0x10000000u;

std::vector<u64> HashRegions(Core::System& system)
{
  auto& memory = system.GetMemory();
  std::vector<u64> hashes;
  auto hash_area = [&](const u8* base, std::size_t size) {
    for (std::size_t offset = 0; offset < size; offset += REGION_SIZE)
    {
      const std::size_t length = std::min<std::size_t>(REGION_SIZE, size - offset);
      hashes.push_back(XXH3_64bits(base + offset, length));
    }
  };
  hash_area(memory.GetRAM(), memory.GetRamSize());
  if (memory.GetEXRAM())
    hash_area(memory.GetEXRAM(), memory.GetExRamSize());
  return hashes;
}

u32 RegionPhysical(std::size_t index, std::size_t mem1_regions)
{
  return index < mem1_regions ?
             static_cast<u32>(index) * REGION_SIZE :
             MEM2_PHYSICAL + static_cast<u32>(index - mem1_regions) * REGION_SIZE;
}

// ---- Sync test ----

struct FrameRecord
{
  s64 ticks = 0;  // CoreTiming ticks right after the save
  u64 ram = 0;
  u64 dev = 0;
  std::vector<u64> regions;
  // YG_SYNCTEST_DIAG only: full copies for byte-level diffs, DoState section ends, events run.
  std::vector<u8> mem1, mem2, dev_buf;
  std::vector<std::pair<std::string, std::size_t>> markers;
  std::vector<std::string> events;
};

std::map<int, FrameRecord> s_records;
std::vector<std::string> s_frame_events;  // CoreTiming events since the last frame end
u64 s_checked = 0, s_mismatches = 0, s_ram_mismatches = 0, s_loads = 0;

SnapshotRing& Ring()
{
  static SnapshotRing ring(static_cast<std::size_t>(std::max(s_synctest_k, s_bench_k)) + 2);
  return ring;
}

void LogByteDiffs(const FrameRecord& want, const FrameRecord& got)
{
  auto ram_diffs = [](const std::vector<u8>& a, const std::vector<u8>& b, u32 virtual_base,
                      const char* name) {
    const std::size_t n = std::min(a.size(), b.size());
    int ranges = 0;
    std::size_t i = 0;
    while (i < n)
    {
      if (a[i] == b[i])
      {
        ++i;
        continue;
      }
      std::size_t start = i, last = i, count = 0;
      while (i < n && i - last < 16)
      {
        if (a[i] != b[i])
        {
          last = i;
          ++count;
        }
        ++i;
      }
      if (ranges++ < 40)
      {
        std::string bytes;
        for (std::size_t k = start; k <= std::min(last, start + 7); ++k)
          bytes += fmt::format(" {:02x}>{:02x}", a[k], b[k]);
        NOTICE_LOG_FMT(ROLLBACK, "  {} diff {:08x}..{:08x} ({} bytes differ):{}", name,
                       virtual_base + start, virtual_base + last + 1, count, bytes);
      }
    }
    NOTICE_LOG_FMT(ROLLBACK, "  {}: {} differing ranges", name, ranges);
  };
  ram_diffs(want.mem1, got.mem1, 0x80000000u, "MEM1");
  ram_diffs(want.mem2, got.mem2, 0x90000000u, "MEM2");

  // DoState sections: attribute each differing byte to the section whose end marker follows it.
  std::map<std::string, std::size_t> per_section;
  std::map<std::string, std::string> detail;
  const std::size_t n = std::min(want.dev_buf.size(), got.dev_buf.size());
  for (std::size_t i = 0; i < n; ++i)
  {
    if (want.dev_buf[i] == got.dev_buf[i])
      continue;
    std::string section = "(after last marker)";
    std::size_t section_start = 0;
    for (const auto& [name, end] : want.markers)
    {
      if (i < end)
      {
        section = name;
        break;
      }
      section_start = end;
    }
    if (per_section[section]++ < 8)
    {
      detail[section] += fmt::format(" +{}:{:02x}>{:02x}", i - section_start, want.dev_buf[i],
                                     got.dev_buf[i]);
    }
  }
  if (want.dev_buf.size() != got.dev_buf.size())
    NOTICE_LOG_FMT(ROLLBACK, "  DoState size {} vs {}", want.dev_buf.size(), got.dev_buf.size());
  for (const auto& [name, count] : per_section)
  {
    NOTICE_LOG_FMT(ROLLBACK, "  DoState section {}: {} bytes differ:{}", name, count,
                   detail[name]);
  }

  std::size_t first = 0;
  while (first < want.events.size() && first < got.events.size() &&
         want.events[first] == got.events[first])
  {
    ++first;
  }
  NOTICE_LOG_FMT(ROLLBACK, "  events: {} first pass, {} re-run, first difference at #{}",
                 want.events.size(), got.events.size(), first);
  for (std::size_t k = (first > 3 ? first - 3 : 0); k < first + 6; ++k)
  {
    NOTICE_LOG_FMT(ROLLBACK, "    #{} first: {}  | re-run: {}", k,
                   k < want.events.size() ? want.events[k] : "-",
                   k < got.events.size() ? got.events[k] : "-");
  }
}

void SyncTest(Core::System& system)
{
  const int f = s_frame;
  const bool first_pass = f > s_max_frame;
  SnapshotRing& ring = Ring();

  std::vector<std::pair<std::string, const u8*>> marker_ptrs;
  if (s_diag)
    PointerWrap::s_marker_log = &marker_ptrs;
  ring.Save(system, f);
  PointerWrap::s_marker_log = nullptr;

  FrameRecord rec;
  rec.ticks = static_cast<s64>(system.GetCoreTiming().GetTicks());
  const auto state = ring.LastState();
  rec.dev = XXH3_64bits(state.data(), state.size());
  rec.regions = HashRegions(system);
  rec.ram = XXH3_64bits(rec.regions.data(), rec.regions.size() * sizeof(u64));
  if (s_diag)
  {
    rec.dev_buf.assign(state.begin(), state.end());
    for (const auto& [name, ptr] : marker_ptrs)
    {
      if (ptr >= state.data() && ptr <= state.data() + state.size())
        rec.markers.emplace_back(name, static_cast<std::size_t>(ptr - state.data()));
    }
    auto& memory = system.GetMemory();
    rec.mem1.assign(memory.GetRAM(), memory.GetRAM() + memory.GetRamSize());
    if (memory.GetEXRAM())
      rec.mem2.assign(memory.GetEXRAM(), memory.GetEXRAM() + memory.GetExRamSize());
    rec.events = std::move(s_frame_events);
    s_frame_events.clear();

    static bool s_sizes_logged = false;
    if (!s_sizes_logged)
    {
      s_sizes_logged = true;
      std::size_t previous = 0;
      for (const auto& [name, end] : rec.markers)
      {
        if (end - previous >= 16 * 1024)
          NOTICE_LOG_FMT(ROLLBACK, "  snapshot section {}: {} KB", name, (end - previous) / 1024);
        previous = end;
      }
      NOTICE_LOG_FMT(ROLLBACK, "  snapshot non-RAM total: {} KB", state.size() / 1024);
    }
  }

  if (first_pass)
  {
    s_records[f] = std::move(rec);
    s_records.erase(s_records.begin(),
                    s_records.lower_bound(f - (s_diag ? std::max(8, s_synctest_k + 1) : 64)));
  }
  else if (auto it = s_records.find(f); it != s_records.end())
  {
    ++s_checked;
    const FrameRecord& want = it->second;
    if (want.ram != rec.ram || want.dev != rec.dev)
    {
      // RAM decides pass or fail. Device state also holds the emulated icache (filled only when
      // the JIT compiles) and GPU dirty flags, which differ on a re-run without reaching RAM.
      ++s_mismatches;
      const bool ram_bad = want.ram != rec.ram;
      if (ram_bad)
        ++s_ram_mismatches;
      if ((ram_bad && s_ram_mismatches <= 20) || (!ram_bad && s_ram_mismatches == 0 && s_mismatches <= 5))
      {
        const std::size_t mem1_regions =
            (system.GetMemory().GetRamSize() + REGION_SIZE - 1) / REGION_SIZE;
        std::string diff;
        int shown = 0, total = 0;
        for (std::size_t i = 0; i < rec.regions.size() && i < want.regions.size(); ++i)
        {
          if (rec.regions[i] == want.regions[i])
            continue;
          ++total;
          if (shown++ < 12)
            diff += fmt::format(" {:08x}", RegionPhysical(i, mem1_regions));
        }
        NOTICE_LOG_FMT(ROLLBACK,
                       "Synctest MISMATCH frame {} (scene {}): ram {} dev {}; ticks {:+}; {} "
                       "regions differ:{}",
                       f, s_scene, ram_bad ? "DIFF" : "same", want.dev == rec.dev ? "same" : "DIFF",
                       rec.ticks - want.ticks, total, diff);
        if (s_diag && !want.mem1.empty() &&
            ((ram_bad && s_ram_mismatches <= 3) || (!ram_bad && s_mismatches <= 3)))
        {
          LogByteDiffs(want, rec);
        }
      }
    }
  }

  // Once per new frame, rewind k frames so the last k frames are re-run and compared.
  if (first_pass && ring.Has(f - s_synctest_k))
  {
    if (ring.Load(system, f - s_synctest_k))
    {
      ++s_loads;
      if (s_clear_jit)
        system.GetJitInterface().ClearSafe();
      s_frame = f - s_synctest_k;
      s_frame_events.clear();
      if (auto history = s_scene_history.find(s_frame); history != s_scene_history.end())
      {
        s_scene = history->second.first;
        s_scene_start = history->second.second;
      }
    }
  }

  if (first_pass && f % 600 == 0)
  {
    NOTICE_LOG_FMT(ROLLBACK,
                   "Synctest frame {} (scene {}): {} re-run frames checked, {} RAM mismatches, {} "
                   "device-state-only mismatches, {} loads",
                   f, s_scene, s_checked, s_ram_mismatches, s_mismatches - s_ram_mismatches,
                   s_loads);
  }
}

// ---- Bench ----

void Bench(Core::System& system, bool first_pass)
{
  using clock = std::chrono::steady_clock;
  static clock::time_point s_last = clock::now();
  static double s_first_ms = 0, s_rerun_ms = 0, s_save_ms = 0, s_load_ms = 0, s_first_max = 0,
                s_rerun_max = 0;
  static u64 s_first = 0, s_reruns = 0, s_saves = 0, s_bench_loads = 0;
  auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };

  // The frame that just ended, since the previous boundary (save and load excluded).
  const double frame = ms(clock::now() - s_last);
  if (s_frame >= 300)  // let the game boot first
  {
    double& total = first_pass ? s_first_ms : s_rerun_ms;
    double& max = first_pass ? s_first_max : s_rerun_max;
    total += frame;
    max = std::max(max, frame);
    ++(first_pass ? s_first : s_reruns);

    SnapshotRing& ring = Ring();
    auto t0 = clock::now();
    ring.Save(system, s_frame);
    s_save_ms += ms(clock::now() - t0);
    ++s_saves;
    // Rewind once per 60 new frames; the re-run passes this frame again without rewinding.
    if (first_pass && s_frame % 60 == 0 && ring.Has(s_frame - s_bench_k))
    {
      t0 = clock::now();
      if (ring.Load(system, s_frame - s_bench_k))
      {
        s_load_ms += ms(clock::now() - t0);
        ++s_bench_loads;
        s_frame -= s_bench_k;
      }
    }
    if (first_pass && s_first > 0 && s_first % 600 == 0)
    {
      NOTICE_LOG_FMT(ROLLBACK,
                     "Bench at frame {}: frame {:.2f} ms avg ({:.2f} max); re-run frame {:.2f} ms "
                     "avg ({:.2f} max) over {}; save {:.2f} ms avg; load({}) {:.2f} ms avg over {}",
                     s_frame, s_first_ms / s_first, s_first_max,
                     s_rerun_ms / std::max<u64>(s_reruns, 1), s_rerun_max, s_reruns,
                     s_save_ms / std::max<u64>(s_saves, 1), s_bench_k,
                     s_load_ms / std::max<u64>(s_bench_loads, 1), s_bench_loads);
      s_first_ms = s_rerun_ms = s_save_ms = s_load_ms = s_first_max = s_rerun_max = 0;
      s_first = s_reruns = s_saves = s_bench_loads = 0;
    }
  }
  s_last = clock::now();
}
}  // namespace

bool Active()
{
  return s_scenes || s_synctest_k > 0 || s_exit_after > 0 || !Input().empty() ||
         s_shot_every > 0 || s_hashlog != nullptr || s_bench_k > 0;
}

std::optional<GCPadStatus> InputOverride(int port)
{
  if (Input().empty())
    return std::nullopt;
  // The frame being run is the one after the last frame end.
  const int f = s_frame;
  u16 buttons = 0;
  int sx = 128, sy = 128, cx = 128, cy = 128;
  for (const InputRule& r : Input())
  {
    if (r.port != port)
      continue;
    int rf = f;  // the frame the rule's from/to are measured in
    if (!r.scene.empty())
    {
      if (r.scene != s_scene)
        continue;
      rf = f - s_scene_start;
    }
    if (rf < r.from || rf > r.to)
      continue;
    if (r.period > 0 && (rf - r.from) % r.period >= 3)
      continue;
    if (r.fuzz_seed)
    {
      // Deterministic per (seed, port, 8-frame segment): a held stick direction plus mostly one
      // attack, jump or shield button, never Start.
      const u64 h = XXH3_64bits_withSeed(
          &f, 0, (u64{r.fuzz_seed} << 32) ^ (static_cast<u64>(port) << 24) ^ (f / 8));
      static constexpr u16 BUTTONS[] = {0,      0x0100, 0x0200, 0x0400, 0x0800,
                                        0x0010, 0x0020, 0x0100, 0x0200, 0};
      buttons |= BUTTONS[(h >> 8) % 10];
      const int dir = static_cast<int>((h >> 16) % 9);  // 0 centre, 1-8 directions
      static constexpr int DX[] = {0, 1, 1, 0, -1, -1, -1, 0, 1};
      static constexpr int DY[] = {0, 0, 1, 1, 1, 0, -1, -1, -1};
      const int magnitude = 60 + static_cast<int>((h >> 24) % 68);
      sx = 128 + DX[dir] * magnitude;
      sy = 128 + DY[dir] * magnitude;
      if (((h >> 32) & 7) == 0)  // occasional C-stick smash
      {
        cx = 128 + DX[(h >> 36) % 9] * 127;
        cy = 128 + DY[(h >> 36) % 9] * 127;
      }
      continue;
    }
    buttons |= r.buttons;
    if (r.sx >= 0)
      sx = r.sx;
    if (r.sy >= 0)
      sy = r.sy;
    if (r.cx >= 0)
      cx = r.cx;
    if (r.cy >= 0)
      cy = r.cy;
  }
  GCPadStatus pad;
  pad.button = buttons;
  pad.stickX = static_cast<u8>(sx);
  pad.stickY = static_cast<u8>(sy);
  pad.substickX = static_cast<u8>(cx);
  pad.substickY = static_cast<u8>(cy);
  pad.triggerLeft = (buttons & 0x0040) ? 255 : 0;
  pad.triggerRight = (buttons & 0x0020) ? 255 : 0;
  pad.analogA = 0;
  pad.analogB = 0;
  pad.isConnected = true;
  return pad;
}

void OnFrameBoundary(const Core::CPUThreadGuard& guard)
{
  if (!Active())
    return;
  auto& system = guard.GetSystem();
  Core::SetIsThrottlerTempDisabled(true);
  if (s_diag && s_synctest_k > 0 && s_frame >= s_synctest_from - 1)
    CoreTiming::g_rollback_event_trace = &s_frame_events;

  static const SceneReader s_scene_reader = SceneReaderForRunningGame();
  if (s_scene_reader && (s_scenes || s_synctest_k > 0 || !Input().empty()))
  {
    std::string scene = s_scene_reader(guard);
    if (scene != s_scene)
    {
      if (s_scenes && s_frame > s_max_frame)
        NOTICE_LOG_FMT(ROLLBACK, "Scene frame {}: {} -> {}", s_frame, s_scene, scene);
      s_scene = std::move(scene);
      s_scene_start = s_frame;
    }
    s_scene_history[s_frame] = {s_scene, s_scene_start};
    if (s_scene_history.size() > 128)
      s_scene_history.erase(s_scene_history.begin());
  }

  const int frame_here = s_frame;
  const bool first_pass_here = frame_here > s_max_frame;  // false while re-running after a load
  if (s_hashlog && first_pass_here && s_synctest_k <= 0)
  {
    const auto regions = HashRegions(system);
    std::fprintf(s_hashlog, "%d %s %016llx\n", frame_here, s_scene.c_str(),
                 static_cast<unsigned long long>(
                     XXH3_64bits(regions.data(), regions.size() * sizeof(u64))));
    if (frame_here % 60 == 0)
      std::fflush(s_hashlog);
  }
  if (s_shot_every > 0 && frame_here >= s_shot_from && first_pass_here &&
      frame_here % s_shot_every == 0)
  {
    Core::SaveScreenShot(fmt::format("f{:06d}", frame_here));
  }
  if (s_synctest_k > 0 && frame_here >= s_synctest_from)
    SyncTest(system);
  else if (s_bench_k > 0)
    Bench(system, first_pass_here);

  // A load above rewound s_frame; the next frame end is the frame after it.
  ++s_frame;
  s_max_frame = std::max(s_max_frame, frame_here);
  // The next frame is a re-run if it has been run before.
  SetResimulating(s_frame <= s_max_frame && !s_render_reruns);

  // Stop at the end of first-pass frame n, in every mode.
  if (s_exit_after > 0 && first_pass_here && frame_here >= s_exit_after && !s_stop_queued)
  {
    s_stop_queued = true;
    NOTICE_LOG_FMT(ROLLBACK,
                   "Harness done at frame {}: {} re-run frames checked, {} RAM mismatches ({}), {} "
                   "device-state-only mismatches, {} loads",
                   frame_here, s_checked, s_ram_mismatches,
                   s_synctest_k > 0 ? (s_ram_mismatches == 0 ? "PASS" : "FAIL") : "no sync test",
                   s_mismatches - s_ram_mismatches, s_loads);
    // The game's save on the session NAND: equal across runs with and without rollback when NAND
    // writes inside rollback windows are undone correctly.
    if (Orca::SessionActive())
    {
      NOTICE_LOG_FMT(ROLLBACK, "Session save hash at exit: {:016x}",
                     Orca::HashSessionSave(SConfig::GetInstance().GetTitleID()));
    }
    Core::QueueHostJob([](Core::System& host_system) { Core::Stop(host_system); });
  }
}
}  // namespace Rollback::Harness
