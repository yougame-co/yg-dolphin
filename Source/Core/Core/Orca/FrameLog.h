// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

// Test only: a per-frame timing log for latency tests. See ORCA.md, "Frame timing log".
//
// ORCA_TEST_FRAMELOG=<path>, read only with the test mark (ORCA_TEST_COMMANDS=1 in a session, as
// ORCA_TEST_PERF), writes one CSV line per forward frame (Orca/Perf.h: a frame that ran for the
// first time in real time, not a rollback re-run):
//   frame       the session frame the line is about (the frame that ended at its boundary)
//   x           1 when Perf leaves it out of the speed numbers (catch-up, replay, unseen, fast)
//   src         where the lead seat's pad came from: d Dolphin's own (keyboard, GameCube adapter,
//               SDL), a the app's input stream, s a test script; empty with no poll
//   poll        when Orca read that pad for this frame (after the controller interface's poll)
//   present     when the present call for the picture issued while this frame ran was made
//   display     when the swap chain says that picture reached the screen, where the backend
//               tells: D3D11/D3D12 from DXGI frame statistics, Metal from presentedTime
//   dq          x display read exactly; e estimated from the refresh count (DXGI); d never shown
//   mode        DXGI's composition mode for that present (composed, overlay, none, failure)
//   presents    how many pictures were presented while this frame ran
//   end         the frame's end (its forward boundary)
// Every time comes twice: <name>_t, the raw high-resolution clock (QPC on Windows,
// mach_absolute_time on macOS, CLOCK_MONOTONIC ns elsewhere; the header gives ticks per second),
// and <name>_ms, Unix epoch milliseconds. Unknown values are empty.
//
// Off, each hook is one relaxed load of a bool and a branch. On, hooks push a small event into a
// fixed lock-free ring; a writer thread joins and writes them, so the CPU thread never touches
// the disk. Observation only: nothing here reaches emulation, the replay or the checksums.
namespace Orca::FrameLog
{
extern std::atomic<bool> g_on;
inline bool On()
{
  return g_on.load(std::memory_order_relaxed);
}

// The raw high-resolution clock, and its rate.
u64 Ticks();
u64 TicksPerSecond();
// A host time in seconds (CACurrentMediaTime's base, which Metal's presentedTime uses) in ticks.
u64 TicksFromHostSeconds(double seconds);

enum class Mode : u8
{
  Unknown,
  Composed,
  Overlay,
  None,
  Failure,
};
const char* ModeName(Mode mode);

enum class Kind : u8
{
  Boundary,   // a forward frame ended: frame, flag 1 excluded, ticks
  Poll,       // the lead seat's pad was read: flag the source ('d', 'a', 's'), ticks
  Present,    // a present call: id Orca's present number, flag 1 when it shows a frame, ticks
  PresentId,  // the backend's own number for the last present (DXGI's present count): id
  Displayed,  // id the present, ticks the display time (0: never shown), a/b DXGI's
              // PresentRefreshCount/SyncRefreshCount (equal: exact), flag the mode, plus
              // BACKEND_KEY when id is the backend's number rather than Orca's
};
constexpr u8 BACKEND_KEY = 0x80;

struct Event
{
  Kind kind = Kind::Boundary;
  u8 flag = 0;
  s64 frame = 0;
  u64 ticks = 0;
  u64 id = 0;
  u64 a = 0;
  u64 b = 0;
};

// A bounded multi-producer, single-consumer ring (Vyukov's): Push never blocks or allocates and
// fails when full.
template <std::size_t N>
class Ring
{
  static_assert(N >= 2 && (N & (N - 1)) == 0, "a power of two");

public:
  Ring()
  {
    for (std::size_t i = 0; i < N; ++i)
      m_slots[i].seq.store(i, std::memory_order_relaxed);
  }
  bool Push(const Event& event)
  {
    u64 pos = m_head.load(std::memory_order_relaxed);
    for (;;)
    {
      Slot& slot = m_slots[pos & (N - 1)];
      const u64 seq = slot.seq.load(std::memory_order_acquire);
      const s64 diff = static_cast<s64>(seq) - static_cast<s64>(pos);
      if (diff == 0)
      {
        if (m_head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
        {
          slot.event = event;
          slot.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      }
      else if (diff < 0)
      {
        return false;
      }
      else
      {
        pos = m_head.load(std::memory_order_relaxed);
      }
    }
  }
  // The consumer only.
  std::optional<Event> Pop()
  {
    Slot& slot = m_slots[m_tail & (N - 1)];
    if (slot.seq.load(std::memory_order_acquire) != m_tail + 1)
      return std::nullopt;
    const Event event = slot.event;
    slot.seq.store(m_tail + N, std::memory_order_release);
    ++m_tail;
    return event;
  }

private:
  struct Slot
  {
    std::atomic<u64> seq{0};
    Event event;
  };
  std::array<Slot, N> m_slots;
  std::atomic<u64> m_head{0};
  u64 m_tail = 0;
};

// Ticks to Unix epoch ms, from one paired reading of both clocks.
struct Anchor
{
  u64 ticks = 0;
  double epoch_ms = 0;
  u64 per_second = 1;
};
double EpochMs(const Anchor& anchor, u64 ticks);

struct Line
{
  s64 frame = 0;
  bool excluded = false;
  u64 end = 0;
  char source = 0;
  std::optional<u64> poll;
  std::optional<u64> present;
  u64 present_seq = 0;                // Orca's number for it
  std::optional<u64> present_backend;  // the backend's (DXGI's present count)
  int presents = 0;
  std::optional<u64> display;
  char display_q = 0;  // 'x', 'e', 'd'
  Mode mode = Mode::Unknown;
  // Nothing more is coming for it: its display event came, or it has no present.
  bool settled = false;
};
std::string HeaderLine();
std::string FormatLine(const Line& line, const Anchor& anchor);

// Joins the events into lines, in the order the ring gave them. Pure: tests drive it.
class Joiner
{
public:
  // per_second: the clock's rate, to sanity-check a refresh period; hold: how many later frames a
  // line waits for its display time before it is written without one.
  explicit Joiner(u64 per_second, std::size_t hold = 30) : m_per_second(per_second), m_hold(hold)
  {
  }
  void Feed(const Event& event);
  // Lines ready to write, oldest first: display known (or never coming), or held long enough;
  // with `all`, every closed line.
  std::vector<Line> Take(bool all = false);
  u64 UnmatchedDisplays() const { return m_unmatched; }
  u64 LastUnmatchedKey() const { return m_last_unmatched_key; }

private:
  Line* FindPresent(u64 key, bool backend);
  void Display(const Event& event);

  u64 m_per_second;
  std::size_t m_hold;
  Line m_open;
  std::deque<Line> m_closed;
  u64 m_last_present_seq = 0;
  // DXGI: the last sample's sync point, for the refresh period.
  std::optional<std::pair<u64, u64>> m_last_sync;  // refresh count, ticks
  double m_period = 0;                             // ticks per refresh, 0 unknown
  u64 m_unmatched = 0;
  u64 m_last_unmatched_key = 0;
};

// ---- At run time ----

// Reads ORCA_TEST_FRAMELOG once at startup (after the command line); with the test mark and a file
// it could open, starts the writer and turns the hooks on.
void Start();
// Each forward frame's boundary (Perf::OnBoundary). CPU thread.
void NoteBoundary(s64 frame, bool excluded);
// The lead seat's pad was read for a first-run frame. CPU thread.
void NotePoll(char source);
// Just before the backend's present call. `frame`: it shows an emulated frame. Returns Orca's
// number for it.
u64 NotePresent(bool frame);
// The number NotePresent last returned.
u64 LastPresent();
// The backend's own number for the present it just made (DXGI's present count).
void NotePresentId(u64 id);
// A present reached the screen at `ticks` (0: it never did). `backend`: key is the number given
// to NotePresentId, else NotePresent's. DXGI: present_refresh and sync_refresh as its statistics
// give them, ticks its SyncQPCTime; others pass 0, 0.
void NoteDisplayed(u64 key, bool backend, u64 ticks, u64 present_refresh, u64 sync_refresh,
                   Mode mode);
}  // namespace Orca::FrameLog
