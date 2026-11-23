// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/FrameLog.h"

namespace
{
using Orca::FrameLog::Anchor;
using Orca::FrameLog::Event;
using Orca::FrameLog::Joiner;
using Orca::FrameLog::Kind;
using Orca::FrameLog::Line;
using Orca::FrameLog::Mode;

// A 10 MHz clock (QPC's usual rate).
constexpr u64 RATE = 10'000'000;

Event Boundary(s64 frame, u64 ticks, bool excluded = false)
{
  Event e;
  e.kind = Kind::Boundary;
  e.frame = frame;
  e.ticks = ticks;
  e.flag = excluded ? 1 : 0;
  return e;
}
Event Poll(u64 ticks, char source = 'd')
{
  Event e;
  e.kind = Kind::Poll;
  e.ticks = ticks;
  e.flag = static_cast<u8>(source);
  return e;
}
Event Present(u64 seq, u64 ticks, bool frame = true)
{
  Event e;
  e.kind = Kind::Present;
  e.id = seq;
  e.ticks = ticks;
  e.flag = frame ? 1 : 0;
  return e;
}
Event PresentId(u64 id)
{
  Event e;
  e.kind = Kind::PresentId;
  e.id = id;
  return e;
}
Event Displayed(u64 key, bool backend, u64 ticks, u64 present_refresh = 0, u64 sync_refresh = 0,
                Mode mode = Mode::Unknown)
{
  Event e;
  e.kind = Kind::Displayed;
  e.id = key;
  e.ticks = ticks;
  e.a = present_refresh;
  e.b = sync_refresh;
  e.flag = static_cast<u8>(static_cast<u8>(mode) | (backend ? Orca::FrameLog::BACKEND_KEY : 0));
  return e;
}
}  // namespace

TEST(OrcaFrameLog, RingIsFifoAndRefusesWhenFull)
{
  auto ring = std::make_unique<Orca::FrameLog::Ring<4>>();
  for (u64 i = 1; i <= 4; ++i)
    EXPECT_TRUE(ring->Push(Poll(i)));
  EXPECT_FALSE(ring->Push(Poll(5)));
  for (u64 i = 1; i <= 4; ++i)
  {
    const auto e = ring->Pop();
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->ticks, i);
  }
  EXPECT_FALSE(ring->Pop().has_value());
  // It wraps.
  for (u64 round = 0; round < 10; ++round)
  {
    EXPECT_TRUE(ring->Push(Poll(100 + round)));
    EXPECT_EQ(ring->Pop()->ticks, 100 + round);
  }
}

TEST(OrcaFrameLog, RingTakesTwoProducers)
{
  auto ring = std::make_unique<Orca::FrameLog::Ring<1024>>();
  constexpr u64 EACH = 400;
  const auto produce = [&](char source) {
    for (u64 i = 0; i < EACH; ++i)
      while (!ring->Push(Poll(i, source)))
        std::this_thread::yield();
  };
  std::thread a(produce, 'a');
  std::thread b(produce, 'b');
  u64 next_a = 0, next_b = 0;
  while (next_a < EACH || next_b < EACH)
  {
    const auto e = ring->Pop();
    if (!e)
    {
      std::this_thread::yield();
      continue;
    }
    // Each producer's events come out in its own order.
    if (e->flag == 'a')
      EXPECT_EQ(e->ticks, next_a++);
    else
      EXPECT_EQ(e->ticks, next_b++);
  }
  a.join();
  b.join();
  EXPECT_FALSE(ring->Pop().has_value());
}

TEST(OrcaFrameLog, EpochFromAnchor)
{
  const Anchor anchor{1'000'000, 1'700'000'000'000.0, RATE};
  EXPECT_DOUBLE_EQ(Orca::FrameLog::EpochMs(anchor, 1'000'000), 1'700'000'000'000.0);
  EXPECT_DOUBLE_EQ(Orca::FrameLog::EpochMs(anchor, 1'000'000 + 166'667), 1'700'000'000'016.6667);
  EXPECT_DOUBLE_EQ(Orca::FrameLog::EpochMs(anchor, 1'000'000 - 10'000), 1'699'999'999'999.0);
}

TEST(OrcaFrameLog, JoinsPollPresentAndExactDisplay)
{
  Joiner joiner(RATE);
  // The frame before: no picture, so it is written at once.
  joiner.Feed(Boundary(9, 50));
  joiner.Feed(Poll(100, 'd'));
  joiner.Feed(Present(1, 200));
  joiner.Feed(PresentId(77));
  joiner.Feed(Boundary(10, 300));
  auto lines = joiner.Take();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].frame, 9);
  EXPECT_FALSE(lines[0].present.has_value());
  // Frame 10 waits for its display time.
  EXPECT_TRUE(joiner.Take().empty());
  joiner.Feed(Displayed(77, true, 450, 5, 5, Mode::None));
  lines = joiner.Take();
  ASSERT_EQ(lines.size(), 1u);
  const Line& line = lines[0];
  EXPECT_EQ(line.frame, 10);
  EXPECT_EQ(line.source, 'd');
  EXPECT_EQ(line.poll, 100u);
  EXPECT_EQ(line.present, 200u);
  EXPECT_EQ(line.present_backend, 77u);
  EXPECT_EQ(line.display, 450u);
  EXPECT_EQ(line.display_q, 'x');
  EXPECT_EQ(line.mode, Mode::None);
  EXPECT_EQ(line.presents, 1);
  EXPECT_EQ(line.end, 300u);
}

TEST(OrcaFrameLog, DisplayBeforeTheBoundaryAndOrcaKeys)
{
  // Metal: keyed by Orca's present number; the handler can run before the frame ends.
  Joiner joiner(RATE);
  joiner.Feed(Poll(100, 'a'));
  joiner.Feed(Present(5, 200));
  joiner.Feed(Displayed(5, false, 260));
  joiner.Feed(Boundary(3, 300));
  const auto lines = joiner.Take();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].display, 260u);
  EXPECT_EQ(lines[0].display_q, 'x');
  EXPECT_EQ(lines[0].source, 'a');
  // An Orca number does not match a backend one.
  Joiner other(RATE);
  other.Feed(Present(5, 200));
  other.Feed(Displayed(5, true, 260));
  EXPECT_EQ(other.UnmatchedDisplays(), 1u);
}

TEST(OrcaFrameLog, EstimatesFromTheRefreshPeriodAndIgnoresRepeats)
{
  Joiner joiner(RATE);
  constexpr u64 PERIOD = 166'667;  // 60 Hz at 10 MHz
  // Frame 1's present (DXGI 1) shown exactly at refresh 10.
  joiner.Feed(Present(1, 1000));
  joiner.Feed(PresentId(1));
  joiner.Feed(Boundary(1, 2000));
  joiner.Feed(Displayed(1, true, 10 * PERIOD, 10, 10));
  // Repeated (nothing newer shown yet): ignored, and the period is learned from the next sample.
  joiner.Feed(Present(2, 3000));
  joiner.Feed(PresentId(2));
  joiner.Feed(Displayed(1, true, 11 * PERIOD, 10, 11));
  joiner.Feed(Boundary(2, 4000));
  // Frame 2's present shown at refresh 12, sampled at refresh 13.
  joiner.Feed(Displayed(2, true, 13 * PERIOD, 12, 13, Mode::Composed));
  const auto lines = joiner.Take();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].display, 10 * PERIOD);
  EXPECT_EQ(lines[1].display_q, 'e');
  ASSERT_TRUE(lines[1].display.has_value());
  EXPECT_EQ(*lines[1].display, 12 * PERIOD);
  EXPECT_EQ(lines[1].mode, Mode::Composed);
  EXPECT_EQ(joiner.UnmatchedDisplays(), 0u);
}

TEST(OrcaFrameLog, NeverShownHeldAndFlushed)
{
  Joiner joiner(RATE, 2);
  // Shown never (Metal's presentedTime 0).
  joiner.Feed(Present(1, 10));
  joiner.Feed(Boundary(1, 20));
  joiner.Feed(Displayed(1, false, 0));
  auto lines = joiner.Take();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_FALSE(lines[0].display.has_value());
  EXPECT_EQ(lines[0].display_q, 'd');
  // No display event at all: held for two later frames, then written without one.
  joiner.Feed(Present(2, 30));
  joiner.Feed(Boundary(2, 40));
  joiner.Feed(Present(3, 50));
  joiner.Feed(Boundary(3, 60));
  EXPECT_TRUE(joiner.Take().empty());
  joiner.Feed(Present(4, 70));
  joiner.Feed(Boundary(4, 80));
  lines = joiner.Take();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].frame, 2);
  lines = joiner.Take(true);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1].frame, 4);
}

TEST(OrcaFrameLog, KeepsTheFirstPollAndTheLastFramePresent)
{
  Joiner joiner(RATE);
  joiner.Feed(Poll(100, 'd'));
  joiner.Feed(Poll(150, 'd'));
  joiner.Feed(Present(1, 200));
  joiner.Feed(Present(2, 210, false));  // not a frame (a redraw): not this frame's
  joiner.Feed(PresentId(9));            // belongs to present 2, which no line has
  joiner.Feed(Present(3, 220));
  joiner.Feed(Boundary(7, 300, true));
  const auto lines = joiner.Take(true);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].poll, 100u);
  EXPECT_EQ(lines[0].present, 220u);
  EXPECT_EQ(lines[0].present_seq, 3u);
  EXPECT_FALSE(lines[0].present_backend.has_value());
  EXPECT_EQ(lines[0].presents, 2);
  EXPECT_TRUE(lines[0].excluded);
}

TEST(OrcaFrameLog, FormatsCsv)
{
  const Anchor anchor{0, 1000.0, RATE};
  Line line;
  line.frame = 42;
  line.source = 'd';
  line.poll = 10'000;  // 1 ms
  line.present = 20'000;
  line.display = 25'000;
  line.display_q = 'x';
  line.mode = Mode::Overlay;
  line.presents = 1;
  line.end = 30'000;
  EXPECT_EQ(Orca::FrameLog::FormatLine(line, anchor),
            "42,0,d,10000,1001.000,20000,1002.000,25000,1002.500,x,overlay,1,30000,1003.000\n");
  Line empty;
  empty.frame = 1;
  empty.excluded = true;
  empty.end = 0;
  EXPECT_EQ(Orca::FrameLog::FormatLine(empty, anchor), "1,1,,,,,,,,,,0,0,1000.000\n");
  // The header names as many columns.
  const std::string header = Orca::FrameLog::HeaderLine();
  EXPECT_EQ(std::count(header.begin(), header.end(), ','), 13);
}
