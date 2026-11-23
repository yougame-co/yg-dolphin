// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/FrameLog.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

#include <fmt/format.h>

#include "Common/IOFile.h"
#include "Common/Thread.h"
#include "Common/Version.h"
#include "Core/Orca/Profile.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

namespace Orca::FrameLog
{
std::atomic<bool> g_on{false};

namespace
{
#if defined(__APPLE__)
const mach_timebase_info_data_t& Timebase()
{
  static const mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    if (info.numer == 0 || info.denom == 0)
      info = {1, 1};
    return info;
  }();
  return timebase;
}
#endif

const char* ClockName()
{
#if defined(_WIN32)
  return "qpc";
#elif defined(__APPLE__)
  return "mach";
#else
  return "monotonic";
#endif
}

void Field(std::string* out, const std::optional<u64>& ticks, const Anchor& anchor)
{
  if (ticks)
    *out += fmt::format(",{},{:.3f}", *ticks, EpochMs(anchor, *ticks));
  else
    *out += ",,";
}
}  // namespace

u64 Ticks()
{
#if defined(_WIN32)
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<u64>(now.QuadPart);
#elif defined(__APPLE__)
  return mach_absolute_time();
#else
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<u64>(now.tv_sec) * 1000000000ULL + static_cast<u64>(now.tv_nsec);
#endif
}

u64 TicksPerSecond()
{
#if defined(_WIN32)
  static const u64 rate = [] {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return static_cast<u64>(frequency.QuadPart);
  }();
  return rate;
#elif defined(__APPLE__)
  // ticks * numer / denom = ns.
  const auto& timebase = Timebase();
  return static_cast<u64>(1e9 * timebase.denom / timebase.numer);
#else
  return 1000000000ULL;
#endif
}

u64 TicksFromHostSeconds(double seconds)
{
  if (!(seconds > 0))
    return 0;
#if defined(__APPLE__)
  const auto& timebase = Timebase();
  return static_cast<u64>(seconds * 1e9 * timebase.denom / timebase.numer + 0.5);
#else
  return static_cast<u64>(seconds * static_cast<double>(TicksPerSecond()) + 0.5);
#endif
}

const char* ModeName(Mode mode)
{
  switch (mode)
  {
  case Mode::Composed:
    return "composed";
  case Mode::Overlay:
    return "overlay";
  case Mode::None:
    return "none";
  case Mode::Failure:
    return "failure";
  case Mode::Unknown:
    break;
  }
  return "";
}

double EpochMs(const Anchor& anchor, u64 ticks)
{
  const double delta = ticks >= anchor.ticks ? static_cast<double>(ticks - anchor.ticks) :
                                               -static_cast<double>(anchor.ticks - ticks);
  return anchor.epoch_ms + delta * 1000.0 / static_cast<double>(anchor.per_second);
}

std::string HeaderLine()
{
  return "frame,x,src,poll_t,poll_ms,present_t,present_ms,display_t,display_ms,dq,mode,presents,"
         "end_t,end_ms\n";
}

std::string FormatLine(const Line& line, const Anchor& anchor)
{
  std::string out = fmt::format("{},{},", line.frame, line.excluded ? 1 : 0);
  if (line.source)
    out += line.source;
  Field(&out, line.poll, anchor);
  Field(&out, line.present, anchor);
  Field(&out, line.display, anchor);
  out += ',';
  if (line.display_q)
    out += line.display_q;
  out += fmt::format(",{},{}", ModeName(line.mode), line.presents);
  Field(&out, line.end, anchor);
  out += '\n';
  return out;
}

Line* Joiner::FindPresent(u64 key, bool backend)
{
  const auto matches = [&](const Line& line) {
    if (!line.present)
      return false;
    return backend ? line.present_backend == key : line.present_seq == key;
  };
  if (matches(m_open))
    return &m_open;
  for (auto it = m_closed.rbegin(); it != m_closed.rend(); ++it)
  {
    if (matches(*it))
      return &*it;
  }
  return nullptr;
}

void Joiner::Display(const Event& event)
{
  const bool backend = (event.flag & BACKEND_KEY) != 0;
  const Mode mode = static_cast<Mode>(event.flag & ~BACKEND_KEY);
  // DXGI: learn the refresh period from successive sync points.
  if (event.b != 0 && event.ticks != 0)
  {
    if (m_last_sync && event.b > m_last_sync->first && event.ticks > m_last_sync->second)
    {
      const double period = static_cast<double>(event.ticks - m_last_sync->second) /
                            static_cast<double>(event.b - m_last_sync->first);
      // 20-500 Hz.
      const double per_second = static_cast<double>(m_per_second);
      if (period >= per_second / 500 && period <= per_second / 20)
        m_period = period;
    }
    m_last_sync = std::make_pair(event.b, event.ticks);
  }
  Line* line = FindPresent(event.id, backend);
  if (!line)
  {
    ++m_unmatched;
    m_last_unmatched_key = event.id;
    return;
  }
  // DXGI reports the last present shown until another one is: the first report decides.
  if (line->settled && line->present)
    return;
  line->mode = mode;
  line->settled = true;
  if (event.ticks == 0)
  {
    line->display_q = 'd';
    return;
  }
  if (event.a == event.b)
  {
    line->display = event.ticks;
    line->display_q = 'x';
  }
  else if (event.b > event.a && m_period > 0)
  {
    const double back = static_cast<double>(event.b - event.a) * m_period;
    if (back < static_cast<double>(event.ticks))
    {
      line->display = event.ticks - static_cast<u64>(back + 0.5);
      line->display_q = 'e';
    }
  }
}

void Joiner::Feed(const Event& event)
{
  switch (event.kind)
  {
  case Kind::Poll:
    // The first read after the last forward boundary is the one this frame's input came from.
    if (!m_open.poll)
    {
      m_open.poll = event.ticks;
      m_open.source = static_cast<char>(event.flag);
    }
    break;
  case Kind::Present:
    m_last_present_seq = event.id;
    if (event.flag)
    {
      // The last picture presented while the frame ran is the one it is about.
      m_open.present = event.ticks;
      m_open.present_seq = event.id;
      m_open.present_backend.reset();
      ++m_open.presents;
    }
    break;
  case Kind::PresentId:
    if (Line* line = FindPresent(m_last_present_seq, false))
      line->present_backend = event.id;
    break;
  case Kind::Displayed:
    Display(event);
    break;
  case Kind::Boundary:
    m_open.frame = event.frame;
    m_open.excluded = event.flag != 0;
    m_open.end = event.ticks;
    if (!m_open.present)
      m_open.settled = true;
    m_closed.push_back(std::move(m_open));
    m_open = {};
    break;
  }
}

std::vector<Line> Joiner::Take(bool all)
{
  std::vector<Line> out;
  while (!m_closed.empty() && (all || m_closed.front().settled || m_closed.size() > m_hold))
  {
    out.push_back(std::move(m_closed.front()));
    m_closed.pop_front();
  }
  return out;
}

// ---- At run time ----

namespace
{
// About 34 s of events at 4 a frame: the writer drains it every 20 ms.
Ring<8192> s_ring;
std::atomic<u64> s_dropped{0};
std::atomic<u64> s_present_seq{0};
std::atomic<bool> s_stop{false};
File::IOFile s_file;
std::thread s_writer;

void Push(const Event& event)
{
  if (!s_ring.Push(event))
    s_dropped.fetch_add(1, std::memory_order_relaxed);
}

Anchor ReadAnchor()
{
  Anchor best;
  u64 best_span = std::numeric_limits<u64>::max();
  for (int i = 0; i < 5; ++i)
  {
    const u64 before = Ticks();
    const auto system = std::chrono::system_clock::now();
    const u64 after = Ticks();
    if (after - before < best_span)
    {
      best_span = after - before;
      best.ticks = before + (after - before) / 2;
      best.epoch_ms =
          std::chrono::duration<double, std::milli>(system.time_since_epoch()).count();
      best.per_second = TicksPerSecond();
    }
  }
  return best;
}

void WriterLoop()
{
  Common::SetCurrentThreadName("Orca FrameLog");
  Joiner joiner(TicksPerSecond());
  Anchor anchor = ReadAnchor();
  u64 dropped_said = 0;
  u64 unmatched_said = 0;
  u64 unmatched_at = Ticks();
  for (;;)
  {
    const bool stopping = s_stop.load(std::memory_order_acquire);
    while (const auto event = s_ring.Pop())
      joiner.Feed(*event);
    // Re-pair the clocks every second, so a long run follows the wall clock.
    if (Ticks() - anchor.ticks > TicksPerSecond())
      anchor = ReadAnchor();
    std::string out;
    for (const Line& line : joiner.Take(stopping))
      out += FormatLine(line, anchor);
    const u64 dropped = s_dropped.load(std::memory_order_relaxed);
    if (dropped != dropped_said)
    {
      out += fmt::format("# dropped {} events in all (the ring was full)\n", dropped);
      dropped_said = dropped;
    }
    // A display event that names no present: an unshown picture's, an already written line's, or
    // (every frame) a backend whose numbers don't match. Said at most every 5 s.
    if (joiner.UnmatchedDisplays() != unmatched_said &&
        (stopping || Ticks() - unmatched_at > 5 * TicksPerSecond()))
    {
      out += fmt::format("# display events matching no present: {} in all (last key {})\n",
                         joiner.UnmatchedDisplays(), joiner.LastUnmatchedKey());
      unmatched_said = joiner.UnmatchedDisplays();
      unmatched_at = Ticks();
    }
    if (!out.empty())
    {
      s_file.WriteString(out);
      s_file.Flush();
    }
    if (stopping)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  s_file.Close();
}

void Stop()
{
  if (!s_writer.joinable())
    return;
  g_on.store(false, std::memory_order_relaxed);
  s_stop.store(true, std::memory_order_release);
  s_writer.join();
}
}  // namespace

void Start()
{
  if (s_writer.joinable())
    return;
  const std::string path = GetEnv("ORCA_TEST_FRAMELOG");
  if (path.empty())
    return;
  // The test mark, as ORCA_TEST_PERF (Perf.cpp TestRun).
  if (GetEnv("ORCA_TEST_COMMANDS") != "1" || !SessionActive())
  {
    std::fprintf(stderr,
                 "Orca: ORCA_TEST_FRAMELOG is read only with ORCA_TEST_COMMANDS=1 in a session\n");
    return;
  }
  if (!s_file.Open(path, "wb"))
  {
    std::fprintf(stderr, "Orca: ORCA_TEST_FRAMELOG: can't write %s\n", path.c_str());
    return;
  }
  const Anchor anchor = ReadAnchor();
  s_file.WriteString(fmt::format("# orca framelog v1 rev={} clock={} ticks_per_s={} "
                                 "anchor_t={} anchor_ms={:.3f}\n",
                                 Common::GetScmRevGitStr().substr(0, 10), ClockName(),
                                 TicksPerSecond(), anchor.ticks, anchor.epoch_ms));
  s_file.WriteString(HeaderLine());
  s_file.Flush();
  s_writer = std::thread(WriterLoop);
  g_on.store(true, std::memory_order_relaxed);
  std::atexit(Stop);
  std::fprintf(stderr, "Orca: frame timing log to %s (ORCA_TEST_FRAMELOG)\n", path.c_str());
}

void NoteBoundary(s64 frame, bool excluded)
{
  Event event;
  event.kind = Kind::Boundary;
  event.frame = frame;
  event.flag = excluded ? 1 : 0;
  event.ticks = Ticks();
  Push(event);
}

void NotePoll(char source)
{
  Event event;
  event.kind = Kind::Poll;
  event.flag = static_cast<u8>(source);
  event.ticks = Ticks();
  Push(event);
}

u64 NotePresent(bool frame)
{
  const u64 seq = s_present_seq.fetch_add(1, std::memory_order_relaxed) + 1;
  Event event;
  event.kind = Kind::Present;
  event.id = seq;
  event.flag = frame ? 1 : 0;
  event.ticks = Ticks();
  Push(event);
  return seq;
}

u64 LastPresent()
{
  return s_present_seq.load(std::memory_order_relaxed);
}

void NotePresentId(u64 id)
{
  Event event;
  event.kind = Kind::PresentId;
  event.id = id;
  Push(event);
}

void NoteDisplayed(u64 key, bool backend, u64 ticks, u64 present_refresh, u64 sync_refresh,
                   Mode mode)
{
  Event event;
  event.kind = Kind::Displayed;
  event.id = key;
  event.ticks = ticks;
  event.a = present_refresh;
  event.b = sync_refresh;
  event.flag = static_cast<u8>(static_cast<u8>(mode) | (backend ? BACKEND_KEY : 0));
  Push(event);
}
}  // namespace Orca::FrameLog
