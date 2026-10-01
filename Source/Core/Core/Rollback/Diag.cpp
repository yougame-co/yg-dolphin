// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Diag.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

#include <fmt/format.h>

#if defined(_M_X86_64)
#include <emmintrin.h>
#endif

#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace Rollback::Diag
{
namespace
{
using Clock = std::chrono::steady_clock;

int EnvInt(const char* name, int fallback)
{
  const char* value = std::getenv(name);
  return value && *value ? std::atoi(value) : fallback;
}

const int s_frametime_from = EnvInt("YG_FRAMETIME", -1);
const bool s_keep_throttle = EnvInt("YG_THROTTLE", 0) != 0;
const bool s_snaptime = EnvInt("YG_SNAPTIME", 0) != 0;
const bool s_ntcopy = EnvInt("YG_NTCOPY", 0) != 0;
const int s_pagediff_from = EnvInt("YG_PAGEDIFF", -1);

constexpr std::size_t PAGE = 4096;
constexpr std::size_t BUCKET = 1024 * 1024;
constexpr std::array<int, 3> GAPS{1, 2, 4};

double Ms(Clock::time_point from, Clock::time_point to)
{
  return std::chrono::duration<double, std::milli>(to - from).count();
}

// Mean, percentiles and max of a sample, as one log fragment.
template <typename T>
std::string Describe(std::vector<T> v)
{
  if (v.empty())
    return "no samples";
  std::sort(v.begin(), v.end());
  double sum = 0;
  for (const T x : v)
    sum += static_cast<double>(x);
  const auto at = [&](std::size_t i) { return static_cast<double>(v[i]); };
  return fmt::format("mean {:.2f} p50 {:.1f} p90 {:.1f} p99 {:.1f} max {:.1f}", sum / v.size(),
                     at(v.size() / 2), at(v.size() * 9 / 10), at(v.size() * 99 / 100),
                     at(v.size() - 1));
}

struct FrameTimes
{
  std::vector<float> ms;
  Clock::time_point first{}, last{};
} s_frames;

struct SnapTimes
{
  double state = 0, mem1 = 0, mem2 = 0;
  int saves = 0;
  std::vector<float> port_saves, port_loads;
} s_snap;

struct PageDiff
{
  struct Copy
  {
    int frame = -1;
    std::vector<u8> mem1, mem2;
  };
  std::deque<Copy> history;  // newest first, frames f-1 .. f-4
  // [gap index][0 = MEM1, 1 = MEM2]: pages differing per frame
  std::array<std::array<std::vector<int>, 2>, GAPS.size()> counts;
  std::vector<u64> mem1_buckets, mem2_buckets;  // 1-frame gap, dirty pages per 1 MB
} s_pages;

int CountDirty(const u8* live, const std::vector<u8>& old, std::vector<u64>* buckets)
{
  int dirty = 0;
  for (std::size_t offset = 0; offset < old.size(); offset += PAGE)
  {
    const std::size_t length = std::min(PAGE, old.size() - offset);
    if (std::memcmp(live + offset, old.data() + offset, length) == 0)
      continue;
    ++dirty;
    if (buckets)
      ++(*buckets)[offset / BUCKET];
  }
  return dirty;
}

void SamplePages(Core::System& system, int frame)
{
  auto& memory = system.GetMemory();
  const u8* mem1 = memory.GetRAM();
  const u8* mem2 = memory.GetEXRAM();
  const std::size_t mem1_size = memory.GetRamSize();
  const std::size_t mem2_size = mem2 ? memory.GetExRamSize() : 0;
  if (s_pages.mem1_buckets.empty())
  {
    s_pages.mem1_buckets.resize((mem1_size + BUCKET - 1) / BUCKET);
    s_pages.mem2_buckets.resize((mem2_size + BUCKET - 1) / BUCKET);
  }
  for (std::size_t g = 0; g < GAPS.size() && frame >= s_pagediff_from; ++g)
  {
    const auto old = std::find_if(s_pages.history.begin(), s_pages.history.end(),
                                  [&](const auto& c) { return c.frame == frame - GAPS[g]; });
    if (old == s_pages.history.end())
      continue;
    s_pages.counts[g][0].push_back(
        CountDirty(mem1, old->mem1, g == 0 ? &s_pages.mem1_buckets : nullptr));
    if (mem2)
    {
      s_pages.counts[g][1].push_back(
          CountDirty(mem2, old->mem2, g == 0 ? &s_pages.mem2_buckets : nullptr));
    }
  }
  // Keep frames f .. f-3 for the next boundary (it needs f, f-1 and f-3).
  PageDiff::Copy copy;
  if (s_pages.history.size() >= GAPS.back())
  {
    copy = std::move(s_pages.history.back());
    s_pages.history.pop_back();
  }
  copy.frame = frame;
  copy.mem1.assign(mem1, mem1 + mem1_size);
  if (mem2)
    copy.mem2.assign(mem2, mem2 + mem2_size);
  s_pages.history.push_front(std::move(copy));
}

std::string TopBuckets(const std::vector<u64>& buckets, u32 base)
{
  u64 total = 0;
  for (const u64 b : buckets)
    total += b;
  if (total == 0)
    return "none";
  std::vector<std::size_t> order(buckets.size());
  for (std::size_t i = 0; i < order.size(); ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(), [&](auto a, auto b) { return buckets[a] > buckets[b]; });
  std::string out;
  for (std::size_t i = 0; i < std::min<std::size_t>(8, order.size()) && buckets[order[i]]; ++i)
  {
    out += fmt::format("{}{:08x} {:.1f}%", out.empty() ? "" : ", ",
                       base + static_cast<u32>(order[i] * BUCKET),
                       100.0 * static_cast<double>(buckets[order[i]]) / static_cast<double>(total));
  }
  return out;
}
}  // namespace

bool KeepThrottle()
{
  return s_keep_throttle;
}

bool SnapTime()
{
  return s_snaptime;
}

void OnFirstPassBoundary(Core::System& system, int frame)
{
  if (s_frametime_from >= 0 && frame >= s_frametime_from)
  {
    const auto now = Clock::now();
    if (frame == s_frametime_from)
      s_frames.first = now;
    else if (s_frames.last != Clock::time_point{})
      s_frames.ms.push_back(static_cast<float>(Ms(s_frames.last, now)));
    s_frames.last = now;
  }
  if (s_pagediff_from >= 0 && frame >= s_pagediff_from - GAPS.back())
    SamplePages(system, frame);
}

void LogSummary()
{
  if (!s_frames.ms.empty())
  {
    const auto& v = s_frames.ms;
    const double wall = Ms(s_frames.first, s_frames.last) / 1000.0;
    const auto over = [&](float limit) { return std::count_if(v.begin(), v.end(), [&](float x) { return x > limit; }); };
    NOTICE_LOG_FMT(ROLLBACK,
                   "Diag frame times from frame {}: {} frames in {:.1f} s ({:.1f} fps), ms {}, "
                   ">16.7 ms {}, >33 ms {}, >50 ms {}",
                   s_frametime_from, v.size(), wall, v.size() / wall, Describe(v), over(16.7f),
                   over(33.334f), over(50.f));
  }
  if (s_snap.saves > 0)
  {
    NOTICE_LOG_FMT(ROLLBACK,
                   "Diag snapshot saves ({} copy): {}, state {:.2f} ms, MEM1 {:.2f} ms, MEM2 {:.2f} ms",
                   s_ntcopy ? "non-temporal" : "plain", s_snap.saves, s_snap.state / s_snap.saves,
                   s_snap.mem1 / s_snap.saves, s_snap.mem2 / s_snap.saves);
  }
  if (!s_snap.port_saves.empty() || !s_snap.port_loads.empty())
  {
    NOTICE_LOG_FMT(ROLLBACK, "Diag session port: {} saves, ms {}; {} loads, ms {}",
                   s_snap.port_saves.size(), Describe(s_snap.port_saves), s_snap.port_loads.size(),
                   Describe(s_snap.port_loads));
  }
  for (std::size_t g = 0; g < GAPS.size(); ++g)
  {
    if (s_pages.counts[g][0].empty())
      continue;
    NOTICE_LOG_FMT(ROLLBACK, "Diag dirty 4 KB pages vs {} frame(s) earlier, {} frames: MEM1 {}; MEM2 {}",
                   GAPS[g], s_pages.counts[g][0].size(), Describe(s_pages.counts[g][0]),
                   Describe(s_pages.counts[g][1]));
  }
  if (!s_pages.counts[0][0].empty())
  {
    NOTICE_LOG_FMT(ROLLBACK, "Diag dirty pages by 1 MB (1-frame gap): MEM1 {}; MEM2 {}",
                   TopBuckets(s_pages.mem1_buckets, 0x80000000u),
                   TopBuckets(s_pages.mem2_buckets, 0x90000000u));
  }
}

void CopyRam(std::vector<u8>* dst, const u8* src, std::size_t size)
{
#if defined(_M_X86_64)
  if (s_ntcopy)
  {
    dst->resize(size);
    u8* const out = dst->data();
    const std::size_t head =
        std::min(size, (16 - (reinterpret_cast<std::uintptr_t>(out) & 15)) & std::size_t{15});
    std::memcpy(out, src, head);
    std::size_t i = head;
    for (; i + 64 <= size; i += 64)
    {
      const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
      const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 16));
      const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 32));
      const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 48));
      _mm_stream_si128(reinterpret_cast<__m128i*>(out + i), a);
      _mm_stream_si128(reinterpret_cast<__m128i*>(out + i + 16), b);
      _mm_stream_si128(reinterpret_cast<__m128i*>(out + i + 32), c);
      _mm_stream_si128(reinterpret_cast<__m128i*>(out + i + 48), d);
    }
    std::memcpy(out + i, src + i, size - i);
    _mm_sfence();
    return;
  }
#endif
  dst->assign(src, src + size);
}

void AddSnapshotSave(double state_ms, double mem1_ms, double mem2_ms)
{
  s_snap.state += state_ms;
  s_snap.mem1 += mem1_ms;
  s_snap.mem2 += mem2_ms;
  ++s_snap.saves;
}

void AddPortSave(double ms)
{
  s_snap.port_saves.push_back(static_cast<float>(ms));
}

void AddPortLoad(double ms)
{
  s_snap.port_loads.push_back(static_cast<float>(ms));
}
}  // namespace Rollback::Diag
