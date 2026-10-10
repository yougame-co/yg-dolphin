// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/Perf.h"

namespace
{
using namespace std::chrono_literals;
using Orca::Perf::AutoSwitch;
using Orca::Perf::Boundary;
using Orca::Perf::GfxChange;
using Orca::Perf::GfxKey;
using Orca::Perf::Meter;
using Orca::Perf::Mode;
using Orca::Perf::Rules;
using Orca::Perf::Window;

std::chrono::nanoseconds Ms(double ms)
{
  return std::chrono::nanoseconds(static_cast<s64>(ms * 1'000'000));
}

// Short windows, so a test plays a few hundred frames: 1 s windows, 10 frames of warm-up, 40 valid
// frames for a window to count.
Rules TestRules()
{
  Rules rules;
  rules.window_s = 1.0;
  rules.warm_frames = 10;
  rules.count_frames = 40;
  return rules;
}

// Drives a Meter frame by frame on a clock of its own.
class Driver
{
public:
  explicit Driver(Rules rules = TestRules()) : m_meter(rules) {}

  // One forward frame: `interval_ms` since the last forward boundary, of which `sleep_ms` the CPU
  // thread slept, `wait_ms` the session waited and `gpu_ms` went to binding and presenting.
  std::optional<Window> Frame(double interval_ms, double sleep_ms = 0, double wait_ms = 0,
                              double gpu_ms = 0, Boundary boundary = Forward())
  {
    m_now += Ms(interval_ms);
    m_totals.sleep += Ms(sleep_ms);
    m_totals.wait += Ms(wait_ms);
    m_totals.present += Ms(gpu_ms);
    boundary.paused_total = m_paused;
    boundary.rerun_total = m_rerun;
    std::optional<Window> window = m_meter.OnBoundary(boundary, m_now, m_totals);
    if (window)
      m_windows.push_back(*window);
    return window;
  }
  // A re-run boundary (rollback), `ms` after the last boundary.
  void Rerun(double ms, bool excluded = false)
  {
    m_now += Ms(ms);
    m_rerun += 1;
    Boundary boundary;
    boundary.first_run = false;
    boundary.excluded = excluded;
    boundary.paused_total = m_paused;
    boundary.rerun_total = m_rerun;
    m_meter.OnBoundary(boundary, m_now, m_totals);
  }
  void Pause(double ms)
  {
    m_now += Ms(ms);
    m_paused += Ms(ms);
  }
  void Scene(std::optional<bool> in_match) { m_meter.NoteScene(in_match); }
  // In a match, past the warm-up, and at the start of a window.
  void Warm()
  {
    Scene(true);
    NextWindow(1);
  }
  // Frames of `interval_ms` (of which `sleep_ms` asleep) until the next window comes out.
  Window NextWindow(double interval_ms, double sleep_ms = 0, Boundary boundary = Forward())
  {
    for (int i = 0; i < 100'000; ++i)
    {
      if (std::optional<Window> window = Frame(interval_ms, sleep_ms, 0, 0, boundary))
        return *window;
    }
    ADD_FAILURE() << "no window";
    return {};
  }
  static Boundary Forward()
  {
    Boundary boundary;
    boundary.first_run = true;
    return boundary;
  }
  const std::vector<Window>& Windows() const { return m_windows; }

private:
  Meter m_meter;
  std::chrono::nanoseconds m_now{1'000'000'000};
  std::chrono::nanoseconds m_paused{};
  Meter::Totals m_totals;
  u64 m_rerun = 0;
  std::vector<Window> m_windows;
};

Window Win(double cap, int n = 50, Mode mode = Mode::OneVsOne, bool pf = false)
{
  Window window;
  window.cap = cap;
  window.n = n;
  window.mode = mode;
  window.pf = pf;
  return window;
}
}  // namespace

TEST(OrcaPerf, WorkTakesOutSleepsAndWaits)
{
  EXPECT_DOUBLE_EQ(Orca::Perf::Work({20, 5, 3, 1}), 12);
  // Waits longer than the interval (a wait that began before it) leave no negative work.
  EXPECT_DOUBLE_EQ(Orca::Perf::Work({10, 8, 5, 0}), 0);
}

TEST(OrcaPerf, PercentilesAreNearestRank)
{
  std::vector<double> values;
  for (int i = 100; i >= 1; --i)
    values.push_back(i);
  EXPECT_DOUBLE_EQ(Orca::Perf::Percentile(values, 50), 50);
  EXPECT_DOUBLE_EQ(Orca::Perf::Percentile(values, 95), 95);
  EXPECT_DOUBLE_EQ(Orca::Perf::Percentile(values, 100), 100);
  EXPECT_DOUBLE_EQ(Orca::Perf::Percentile(values, 0), 1);
  EXPECT_DOUBLE_EQ(Orca::Perf::Percentile({7}, 95), 7);
  EXPECT_DOUBLE_EQ(Orca::Perf::Percentile({}, 50), 0);
}

TEST(OrcaPerf, CapIsWhatTheMachineCouldRunNotWhatItShowed)
{
  // A fast machine held at 60 by the throttle: 16.7 ms frames, 6.7 of them asleep. It shows 60 but
  // could run 100.
  Driver d;
  d.Warm();
  const Window w = d.NextWindow(16.7, 6.7);
  EXPECT_NEAR(w.cap, 100, 0.5);
  EXPECT_NEAR(w.w50, 10, 0.01);
  EXPECT_NEAR(w.w95, 10, 0.01);
  EXPECT_NEAR(w.fps, 1000 / 16.7, 0.5);
  EXPECT_GE(w.n, 55);
  EXPECT_EQ(w.mode, Mode::Solo);
}

TEST(OrcaPerf, ASlowMachineReadsUnder60)
{
  // 22.7 ms frames with no sleep at all: 44 fps, a3851fbb's rate.
  Driver d;
  d.Warm();
  const Window w = d.NextWindow(22.7, 0);
  EXPECT_NEAR(w.cap, 44, 0.1);
  EXPECT_NEAR(w.fps, 44, 0.5);
}

TEST(OrcaPerf, ReRunsCountAsTheMachinesWork)
{
  // A rollback re-runs 4 frames at 3 ms each between two forward frames: the next forward frame's
  // interval holds them, and so does its work.
  Driver d;
  d.Warm();
  d.Frame(16.7, 6.7);
  for (int i = 0; i < 4; ++i)
    d.Rerun(3);
  // 12 ms of re-runs, then the frame itself: 16.7 ms more, 6.7 of it asleep.
  d.Frame(16.7, 6.7);
  // One long, mostly asleep frame closes the window: three frames that worked 10, 22 and 10 ms.
  const Window w = d.NextWindow(1000, 1000 - 10);
  EXPECT_EQ(w.n, 3);
  EXPECT_NEAR(w.w95, 22, 0.01);
  EXPECT_NEAR(w.w50, 10, 0.01);
  EXPECT_NEAR(w.cap, 3 / 0.042, 0.1);
  EXPECT_EQ(w.rbf, 4);
}

TEST(OrcaPerf, SessionWaitsAreNotWork)
{
  // Time sync held this machine a frame: its 33 ms frame was 16.7 ms of waiting and 6.7 asleep.
  Driver d;
  d.Warm();
  for (int i = 0; i < 50; ++i)
    d.Frame(16.7, 6.7);
  d.Frame(33.4, 6.7, 16.7);
  const Window w = d.NextWindow(16.7, 6.7);
  // Counted as work, the wait would bring the cap down to about 97.
  EXPECT_NEAR(w.cap, 100, 0.2);
  EXPECT_NEAR(w.w95, 10, 0.01);
}

TEST(OrcaPerf, GpuShareIsReported)
{
  Driver d;
  d.Warm();
  std::optional<Window> w;
  while (!w)
    w = d.Frame(16.7, 2, 0, 9);
  EXPECT_NEAR(w->g95, 9, 0.01);
  EXPECT_NEAR(w->w95, 14.7, 0.01);
}

TEST(OrcaPerf, MenusAndTheMatchStartAreLeftOut)
{
  Driver d;
  // Menus: no frame counts, but the window still comes out every second.
  d.Scene(false);
  Window w = d.NextWindow(10);
  EXPECT_EQ(w.n, 0);
  EXPECT_EQ(w.cap, 0);
  EXPECT_GT(w.fps, 90);
  // A match starts: its first warm_frames frames don't count either.
  d.Scene(true);
  for (int i = 0; i < 10; ++i)
    d.Frame(10);
  w = d.NextWindow(10);
  EXPECT_GT(w.n, 0);
  const int first = w.n;
  // Back to a menu: the count starts over at the next match.
  d.Scene(false);
  d.Frame(10);
  d.Scene(true);
  w = d.NextWindow(10);
  EXPECT_LT(w.n, first);
  // The scene unreadable (a transition) changes nothing.
  d.Scene(std::nullopt);
  w = d.NextWindow(10);
  EXPECT_EQ(w.n, 100);
}

TEST(OrcaPerf, PausesHidingHoldsAndUnseenFramesAreLeftOut)
{
  Driver d;
  d.Warm();
  d.NextWindow(10);
  // A pause: the frame across it doesn't count, and neither does its time.
  d.Pause(5000);
  std::optional<Window> w = d.Frame(10);
  EXPECT_FALSE(w);
  w = d.NextWindow(10);
  EXPECT_EQ(w->n, 99);
  EXPECT_NEAR(w->fps, 100, 0.5);

  Boundary hidden = Driver::Forward();
  hidden.hidden = true;
  w = d.NextWindow(10, 0, hidden);
  EXPECT_EQ(w->n, 0);
  // The game still ran at speed while hidden.
  EXPECT_NEAR(w->fps, 100, 0.5);

  // A load the game held for 2 more frames: shown frames count it, the cap doesn't.
  Boundary held = Driver::Forward();
  held.held = true;
  held.held_frames = 2;
  d.Frame(50, 0, 0, 0, held);
  w = d.NextWindow(10);
  EXPECT_EQ(w->n, 95);

  Boundary unseen = Driver::Forward();
  unseen.excluded = true;
  w = d.NextWindow(1, 0, unseen);
  EXPECT_EQ(w->n, 0);
  EXPECT_EQ(w->fps, 0);
}

TEST(OrcaPerf, ACatchUpSpoilsTheNextFrame)
{
  Driver d;
  d.Warm();
  d.NextWindow(10);
  // A joiner's catch-up runs frames back to back (not first runs, excluded): the next forward
  // frame's interval spans it, so that frame doesn't count.
  for (int i = 0; i < 30; ++i)
    d.Rerun(1, true);
  const Window w = d.NextWindow(10);
  EXPECT_EQ(w.n, 96);
  EXPECT_NEAR(w.cap, 100, 0.5);
}

TEST(OrcaPerf, ModeFollowsThePeers)
{
  EXPECT_EQ(Orca::Perf::ModeFor(0), Mode::Solo);
  EXPECT_EQ(Orca::Perf::ModeFor(1), Mode::OneVsOne);
  EXPECT_EQ(Orca::Perf::ModeFor(2), Mode::Multi);
  EXPECT_EQ(Orca::Perf::ModeFor(3), Mode::Multi);
  Driver d;
  d.Warm();
  Boundary friend_in = Driver::Forward();
  friend_in.peers = 1;
  d.Frame(10, 0, 0, 0, friend_in);
  // The most peers seen in the window.
  EXPECT_EQ(d.NextWindow(10).mode, Mode::OneVsOne);
  EXPECT_EQ(d.NextWindow(10).mode, Mode::Solo);
}

TEST(OrcaPerf, AWindowCountsWithEnoughFramesSoloOr1v1)
{
  const Rules rules;
  EXPECT_TRUE(Win(50, 600, Mode::Solo).Counts(rules));
  EXPECT_TRUE(Win(50, 858, Mode::OneVsOne).Counts(rules));
  EXPECT_FALSE(Win(50, 599, Mode::OneVsOne).Counts(rules));
  // 3-4 player rooms dip normally.
  EXPECT_FALSE(Win(50, 900, Mode::Multi).Counts(rules));
}

TEST(OrcaPerf, TheSwitchFiresAfterFourSlowWindowsInARow)
{
  AutoSwitch s(TestRules());
  EXPECT_FALSE(s.Feed(Win(44)));
  EXPECT_FALSE(s.Feed(Win(43)));
  EXPECT_FALSE(s.Feed(Win(45)));
  EXPECT_TRUE(s.Feed(Win(46)));
  EXPECT_TRUE(s.Fired());
  EXPECT_DOUBLE_EQ(s.TriggerCap(), 44.5);
  // Once a run.
  for (int i = 0; i < 8; ++i)
    EXPECT_FALSE(s.Feed(Win(40)));
}

TEST(OrcaPerf, MenusAndBigRoomsDontBreakTheRun)
{
  AutoSwitch s(TestRules());
  EXPECT_FALSE(s.Feed(Win(50)));
  EXPECT_FALSE(s.Feed(Win(0, 0)));  // a menu
  EXPECT_FALSE(s.Feed(Win(50)));
  EXPECT_FALSE(s.Feed(Win(70, 50, Mode::Multi)));  // a 4-player room, fast or slow
  EXPECT_FALSE(s.Feed(Win(30, 50, Mode::Multi)));
  EXPECT_FALSE(s.Feed(Win(50)));
  EXPECT_FALSE(s.Feed(Win(50, 39)));  // too few frames
  EXPECT_TRUE(s.Feed(Win(50)));
}

TEST(OrcaPerf, AFastWindowPerformanceOrAChangeStartsOver)
{
  AutoSwitch s(TestRules());
  s.Feed(Win(50));
  s.Feed(Win(50));
  s.Feed(Win(50));
  EXPECT_FALSE(s.Feed(Win(58.0)));  // holds 58: not slow
  s.Feed(Win(50));
  s.Feed(Win(50));
  s.Feed(Win(50));
  EXPECT_FALSE(s.Feed(Win(50, 50, Mode::Solo, true)));  // already Performance
  s.Feed(Win(50));
  s.Feed(Win(50));
  s.Feed(Win(50));
  s.Restart();  // the player changed a setting
  EXPECT_FALSE(s.Feed(Win(50)));
  EXPECT_FALSE(s.Feed(Win(50)));
  EXPECT_FALSE(s.Feed(Win(50)));
  EXPECT_TRUE(s.Feed(Win(50)));
}

TEST(OrcaPerf, AFineMachineWithDipsNeverSwitches)
{
  // 2f80e4c8's shape: 58.6 most of the time, 49 in rollback-heavy windows, never four in a row.
  AutoSwitch s;
  for (int i = 0; i < 40; ++i)
  {
    const double cap = i % 4 == 3 ? 58.6 : (i % 2 ? 49.1 : 57.2);
    EXPECT_FALSE(s.Feed(Win(cap, 858))) << i;
  }
  // A normal machine at 60 with one dip.
  AutoSwitch fine;
  for (int i = 0; i < 40; ++i)
    EXPECT_FALSE(fine.Feed(Win(i == 20 ? 51 : 72, 858)));
}

TEST(OrcaPerf, ASlowMachineSwitchesInItsFirstMatch)
{
  // a3851fbb: 44 fps in 1v1, with the real rules, from the moment a match starts. Its 900 warm-up
  // frames take 20.5 s, so the first window (0-15 s) has no valid frame and the second (15-30 s)
  // too few (420) to count; windows 3 to 6 count, and the switch fires at the end of the sixth:
  // 90 s into the match, after 60 s of counted play.
  Driver d(Rules{});
  d.Scene(true);
  AutoSwitch s;
  std::vector<Window> windows;
  Boundary b = Driver::Forward();
  b.peers = 1;
  bool fired = false;
  while (!fired && windows.size() < 10)
  {
    windows.push_back(d.NextWindow(1000 / 44.0, 0, b));
    fired = s.Feed(windows.back());
  }
  EXPECT_TRUE(fired);
  ASSERT_EQ(windows.size(), 6u);
  EXPECT_EQ(windows[0].n, 0);
  EXPECT_FALSE(windows[1].Counts(Rules{}));
  for (std::size_t i = 2; i < windows.size(); ++i)
    EXPECT_TRUE(windows[i].Counts(Rules{})) << i;
  EXPECT_NEAR(s.TriggerCap(), 44, 0.1);
}

TEST(OrcaPerf, TheLinesAreVersionedJson)
{
  Window w;
  w.mode = Mode::OneVsOne;
  w.n = 858;
  w.cap = 51.66;
  w.w50 = 18.94;
  w.w95 = 24.1;
  w.g95 = 3.2;
  w.fps = 51.6;
  w.rbf = 212;
  w.res = 1;
  w.pf = true;
  w.as = true;
  EXPECT_EQ(Orca::Perf::WindowJson(w),
            R"({"v":1,"k":"win","m":"1v1","n":858,"cap":51.7,"w50":18.9,"w95":24.1,"g95":3.2,)"
            R"("fps":51.6,"rbf":212,"res":1,"pf":1,"as":1})");
  EXPECT_EQ(Orca::Perf::AutoJson(44.25), R"({"v":1,"k":"auto","cap":44.3})");
  Window empty;
  empty.cap = std::numeric_limits<double>::infinity();
  EXPECT_EQ(Orca::Perf::WindowJson(empty),
            R"({"v":1,"k":"win","m":"solo","n":0,"cap":0.0,"w50":0.0,"w95":0.0,"g95":0.0,)"
            R"("fps":0.0,"rbf":0,"res":0,"pf":0,"as":0})");
}

TEST(OrcaPerf, TheInfoLineFitsThreeHundredBytes)
{
  Orca::Perf::Info info;
  info.backend = "D3D";
  info.gpu = "NVIDIA GeForce RTX 3060";
  info.adapters = {"NVIDIA GeForce RTX 3060", "Intel(R) UHD Graphics 770"};
  info.adapter = 0;
  info.cpu = "12th Gen Intel(R) Core(TM) i5-12400F";
  info.threads = 12;
  info.os = "win";
  info.rev = "ccdc8e441a";
  EXPECT_EQ(
      Orca::Perf::InfoJson(info),
      R"({"v":1,"k":"info","be":"D3D","gpu":"NVIDIA GeForce RTX 3060",)"
      R"("ad":["NVIDIA GeForce RTX 3060","Intel(R) UHD Graphics 770"],"ai":0,)"
      R"("cpu":"12th Gen Intel(R) Core(TM) i5-12400F","th":12,"os":"win","rev":"ccdc8e441a"})");

  // Long names: cut to 40, then the CPU goes, then adapters past the first.
  const std::string long_name(80, 'x');
  info.gpu = long_name;
  info.adapters = {long_name, long_name, long_name, long_name};
  info.cpu = long_name;
  const std::string json = Orca::Perf::InfoJson(info);
  EXPECT_LE(json.size() + std::string("orca perf ").size(), Orca::Perf::LINE_BYTES);
  EXPECT_EQ(json.find("\"cpu\""), std::string::npos);
  EXPECT_NE(json.find(std::string(40, 'x')), std::string::npos);
  EXPECT_EQ(json.find(std::string(41, 'x')), std::string::npos);
}

TEST(OrcaPerf, NamesAreCleaned)
{
  EXPECT_EQ(Orca::Perf::CleanName("  AMD \"Radeon\"\\  RX\t580  "), "AMD Radeon RX580");
  EXPECT_EQ(Orca::Perf::CleanName("Apple M2 Pro"), "Apple M2 Pro");
  EXPECT_EQ(Orca::Perf::CleanName("caf\xc3\xa9"), "caf");
  EXPECT_EQ(Orca::Perf::CleanName(std::string(50, 'a') + " b").size(), 40u);
}

TEST(OrcaPerf, GfxTakesOnlyItsOwnKeysAndRanges)
{
  using Orca::Perf::ParseGfx;
  EXPECT_EQ(ParseGfx("gfx res 0"), (GfxChange{GfxKey::Res, 0}));
  EXPECT_EQ(ParseGfx("gfx res 4"), (GfxChange{GfxKey::Res, 4}));
  EXPECT_EQ(ParseGfx("gfx aniso -1"), (GfxChange{GfxKey::Aniso, -1}));
  EXPECT_EQ(ParseGfx("gfx aniso 4"), (GfxChange{GfxKey::Aniso, 4}));
  EXPECT_EQ(ParseGfx("gfx filter 2"), (GfxChange{GfxKey::Filter, 2}));
  EXPECT_EQ(ParseGfx("gfx resample 6"), (GfxChange{GfxKey::Resample, 6}));
  EXPECT_EQ(ParseGfx("gfx efb 0"), (GfxChange{GfxKey::Efb, 0}));
  EXPECT_EQ(ParseGfx("gfx gpudecode 1"), (GfxChange{GfxKey::GpuDecode, 1}));
  EXPECT_EQ(ParseGfx("gfx cpucull 1"), (GfxChange{GfxKey::CpuCull, 1}));
  for (const char* bad : {"gfx wireframe 1",
                          "gfx res 9",
                          "gfx res 5",
                          "gfx msaa 4",
                          "gfx res 1 x",
                          "gfx res",
                          "gfx",
                          "gfx aniso -2",
                          "gfx filter 3",
                          "gfx resample 7",
                          "gfx efb 2",
                          "gfx res +1",
                          "gfx res 1.0",
                          "gfx res -0x1",
                          "gfx Res 1",
                          "gfx shadermode 0",
                          "gfx fog 1",
                          "gfx widescreen 1",
                          "gfx postshader x",
                          "gfxres 1",
                          "res 1",
                          "gfx res 1000"})
  {
    EXPECT_FALSE(ParseGfx(bad)) << bad;
  }
}

TEST(OrcaPerf, ThePerformancePresetIsNativeWithoutScaledCopies)
{
  const auto preset = Orca::Perf::PerformancePreset();
  EXPECT_EQ(preset[0], (GfxChange{GfxKey::Res, 1}));
  EXPECT_EQ(preset[1], (GfxChange{GfxKey::Efb, 0}));
  EXPECT_EQ(preset[2], (GfxChange{GfxKey::Aniso, -1}));
  EXPECT_EQ(preset[3], (GfxChange{GfxKey::Filter, 0}));
  EXPECT_EQ(preset[4], (GfxChange{GfxKey::Resample, 0}));
}
