// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/SessionPort.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <thread>

#include <xxh3.h>

#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Rollback/Diag.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/System.h"

namespace Rollback
{
namespace
{
RingPort* s_active = nullptr;

// How long a time-sync wait holds this machine: one NTSC (60 Hz) frame.
constexpr std::chrono::microseconds FRAME_PERIOD{16'667};
}  // namespace

namespace
{
bool DesyncMapOn();
// Diag (YG_DESYNC_MAP=1): where each savestate section ends (DoMarker) in each saved frame's
// non-RAM state, so a checksum frame's state can be hashed section by section.
std::map<int, std::vector<std::pair<std::string, std::size_t>>> s_marks;
}  // namespace

RingPort::RingPort(Core::System& system, int max_rollback, bool unthrottled)
    : m_system(system), m_ring(static_cast<std::size_t>(max_rollback) + 2),
      m_unthrottled(unthrottled)
{
  // Until the session sets them, every port reads as connected with nothing pressed.
  m_pads.fill(Orca::Net::DecodePad(Orca::Net::Pad{}));
  ResetDesyncMaps();
  s_active = this;
  if (m_unthrottled)
    Core::SetIsThrottlerTempDisabled(true);
}

RingPort::~RingPort()
{
  // Diag: the side whose own check never failed logs its maps here, so both sides can be compared.
  DumpDesyncMaps(std::numeric_limits<int>::max(), "end");
  if (s_active == this)
    s_active = nullptr;
  m_ring.Reset(m_system);
  Rollback::SetResimulating(false);
  if (m_unthrottled)
    Core::SetIsThrottlerTempDisabled(false);
}

RingPort* RingPort::Active()
{
  return s_active;
}

bool RingPort::Save(int frame)
{
  // A stalled boundary asks for the same frame again; nothing has run since.
  if (m_saved_since_run == frame)
    return true;
  const auto start = std::chrono::steady_clock::now();
  std::vector<std::pair<std::string, const u8*>> marks;
  if (DesyncMapOn())
    PointerWrap::s_marker_log = &marks;
  const bool allocated = m_ring.Save(m_system, frame);
  PointerWrap::s_marker_log = nullptr;
  if (DesyncMapOn() && !marks.empty())
  {
    // A save that had to grow its buffer ran twice: keep the last pass's markers.
    std::size_t from = 0;
    for (std::size_t i = 1; i < marks.size(); ++i)
      if (marks[i].first == marks[0].first)
        from = i;
    const auto state = m_ring.LastState();
    auto& out = s_marks[frame];
    out.clear();
    for (std::size_t i = from; i < marks.size(); ++i)
    {
      const u8* p = marks[i].second;
      if (p >= state.data() && p <= state.data() + state.size())
        out.emplace_back(marks[i].first, static_cast<std::size_t>(p - state.data()));
    }
    while (s_marks.size() > 64)
      s_marks.erase(s_marks.begin());
  }
  double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (Diag::SnapTime())
    Diag::AddPortSave(ms);
  // A slot's first use allocates ~100 MB, so it is not a representative sample. Otherwise blend in
  // 1/8 of each sample, capped at 4x the estimate so one slow save barely moves it.
  if (!allocated)
  {
    if (m_save_ms > 0)
      ms = std::min(ms, 4 * m_save_ms);
    m_save_ms = m_save_ms == 0 ? ms : m_save_ms + (ms - m_save_ms) / 8;
  }
  m_saved_since_run = frame;
  return true;
}

bool RingPort::Load(int frame)
{
  // Loading re-anchors the throttle. Keep the current reference so the time the rollback takes is
  // made up afterwards.
  if (!m_resimulating && !m_reference_before_load)
    m_reference_before_load = m_system.GetCoreTiming().GetThrottleReference();
  m_saved_since_run = -1;
  const auto start = std::chrono::steady_clock::now();
  const bool loaded = m_ring.Load(m_system, frame);
  if (Diag::SnapTime())
  {
    Diag::AddPortLoad(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  }
  return loaded;
}

void RingPort::SetPads(int frame, const Orca::Net::Pads& pads)
{
  Orca::Net::ReplayRecordingScope::RecordPads(frame, pads);
  // Called once for every frame about to run.
  for (std::size_t port = 0; port < pads.size(); ++port)
    m_pads[port] = Orca::Net::DecodePad(pads[port]);
  m_saved_since_run = -1;
}

void RingPort::SetCatchingUp(bool catching_up)
{
  if (catching_up == m_catching_up)
    return;
  m_catching_up = catching_up;
  Rollback::SetResimulating(m_catching_up || m_resimulating);
  if (!catching_up)
  {
    m_reference_before_load.reset();
    ResetPacing();
  }
}

bool RingPort::LoadImage(MachineImage image, int frame)
{
  m_saved_since_run = -1;
  return m_ring.LoadMachineImage(m_system, std::move(image), frame);
}

void RingPort::SetResimulating(bool resimulating)
{
  // While re-running, emulation is unthrottled, rendering is skipped and audio is dropped.
  m_resimulating = resimulating;
  Rollback::SetResimulating(resimulating || m_catching_up);
  if (!resimulating && m_reference_before_load)
  {
    m_system.GetCoreTiming().SetThrottleReference(*m_reference_before_load);
    m_reference_before_load.reset();
  }
}

namespace
{
// DIAGNOSTIC build only (P+ desync study). YG_DESYNC_MAP=1: the session checksum is built from 64 KiB
// block hashes of the checksummed RAM (MEM1 then MEM2) in the same single pass as the normal one, the
// last few maps stay in memory, and nothing is logged until a desync, so the timing of a normal run
// is unchanged. On a desync both machines log their maps (and the non-RAM state hash) for the frames
// they kept; the logs show the first block that differs.
bool DesyncMapOn()
{
  static const bool s_on = [] {
    const char* v = std::getenv("YG_DESYNC_MAP");
    return v && v[0] == '1';
  }();
  return s_on;
}
constexpr std::size_t MAP_BUCKET = 64 * 1024;
constexpr std::size_t MAP_KEEP = 32;
// Only the CPU thread checksums and checks checksums.
std::map<int, std::vector<u64>> s_maps;
// Per-section hashes (top 32 bits) of each kept checksum frame's non-RAM state.
std::map<int, std::vector<std::pair<std::string, u32>>> s_sections;
// Section sizes, and the bytes (hex; 64-byte block hashes when large) of the I/O and CPU sections
// that differ between machines, per kept checksum frame.
std::map<int, std::vector<std::size_t>> s_section_sizes;
std::map<int, std::vector<std::pair<std::string, std::string>>> s_details;
bool DetailSection(const std::string& name)
{
  return name == "DVDInterface" || name == "IOS::HLE" || name == "ExpansionInterface" ||
         name == "PowerPC" || name == "AudioInterface" || name == "DSP" ||
         name == "CoreTimingEvents" || name == "CoreTiming" || name == "IOS";
}
std::string Detail(const u8* data, std::size_t size)
{
  std::string out;
  if (size <= 4096)
  {
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i)
      out += fmt::format("{:02x}", data[i]);
    return out;
  }
  out = "h64:";
  for (std::size_t at = 0; at < size; at += 64)
    out += fmt::format("{:08x}", static_cast<u32>(XXH3_64bits(data + at, std::min<std::size_t>(64, size - at)) >> 32));
  return out;
}
bool s_maps_logged = false;
}  // namespace

std::optional<u64> RingPort::SnapshotChecksum(int frame)
{
  if (!DesyncMapOn())
    return m_ring.RamChecksum(frame);
  auto map = m_ring.RamBucketHashes(frame, MAP_BUCKET);
  if (!map || map->empty())
    return std::nullopt;
  // RAM blocks only (the last entry is the non-RAM state hash, kept for the log, not compared).
  const u64 hash = XXH3_64bits(map->data(), (map->size() - 1) * sizeof(u64));
  s_maps[frame] = std::move(*map);
  while (s_maps.size() > MAP_KEEP)
    s_maps.erase(s_maps.begin());
  if (const auto it = s_marks.find(frame); it != s_marks.end())
  {
    const auto state = m_ring.StateAt(frame);
    std::vector<std::pair<std::string, u32>> sections;
    std::vector<std::size_t> sizes;
    std::vector<std::pair<std::string, std::string>> details;
    std::size_t prev = 0;
    for (const auto& [name, end] : it->second)
    {
      if (end < prev || end > state.size())
        break;
      sizes.push_back(end - prev);
      if (DetailSection(name))
        details.emplace_back(name, Detail(state.data() + prev, end - prev));
      std::string key = name;
      std::replace(key.begin(), key.end(), ' ', '_');
      sections.emplace_back(std::move(key),
                            static_cast<u32>(XXH3_64bits(state.data() + prev, end - prev) >> 32));
      prev = end;
    }
    if (prev <= state.size())
      sections.emplace_back("(tail)", static_cast<u32>(XXH3_64bits(state.data() + prev, state.size() - prev) >> 32));
    s_sections[frame] = std::move(sections);
    while (s_sections.size() > MAP_KEEP)
      s_sections.erase(s_sections.begin());
    s_section_sizes[frame] = std::move(sizes);
    s_details[frame] = std::move(details);
    while (s_section_sizes.size() > MAP_KEEP)
      s_section_sizes.erase(s_section_sizes.begin());
    while (s_details.size() > MAP_KEEP)
      s_details.erase(s_details.begin());
  }
  return hash;
}

void RingPort::ResetDesyncMaps()
{
  s_maps.clear();
  s_marks.clear();
  s_sections.clear();
  s_section_sizes.clear();
  s_details.clear();
  s_maps_logged = false;
}

void RingPort::OnDesync(int frame)
{
  DumpDesyncMaps(frame, "desync");
}

void RingPort::DumpDesyncMaps(int frame, const char* why)
{
  if (!DesyncMapOn() || s_maps_logged || s_maps.empty())
    return;
  s_maps_logged = true;
  NOTICE_LOG_FMT(ROLLBACK, "DESYNCMAP dump ({}) maps={} first={} last={}", why, s_maps.size(),
                 s_maps.begin()->first, s_maps.rbegin()->first);
  constexpr std::size_t PER_LINE = 64;
  for (const auto& [f, map] : s_maps)
  {
    if (f > frame)
      continue;
    const u64 ram = XXH3_64bits(map.data(), (map.size() - 1) * sizeof(u64));
    const std::size_t blocks = map.size() - 1;
    const std::size_t parts = (blocks + PER_LINE - 1) / PER_LINE;
    for (std::size_t p = 0; p < parts; ++p)
    {
      std::string line;
      for (std::size_t i = p * PER_LINE; i < std::min(blocks, (p + 1) * PER_LINE); ++i)
        line += fmt::format("{:08x}", static_cast<u32>(map[i] >> 32));
      NOTICE_LOG_FMT(ROLLBACK, "DESYNCMAP f={} ram={:016x} state={:016x} part {}/{} {}", f, ram,
                     map.back(), p, parts, line);
    }
    if (const auto it = s_sections.find(f); it != s_sections.end())
    {
      constexpr std::size_t SEC_PER_LINE = 40;
      const auto& sections = it->second;
      const std::size_t sec_parts = (sections.size() + SEC_PER_LINE - 1) / SEC_PER_LINE;
      for (std::size_t p = 0; p < sec_parts; ++p)
      {
        std::string line;
        for (std::size_t i = p * SEC_PER_LINE; i < std::min(sections.size(), (p + 1) * SEC_PER_LINE);
             ++i)
        {
          const auto sz = s_section_sizes.find(f);
          const std::size_t size =
              sz != s_section_sizes.end() && i < sz->second.size() ? sz->second[i] : 0;
          line += fmt::format(" {}={:08x}:{}", sections[i].first, sections[i].second, size);
        }
        NOTICE_LOG_FMT(ROLLBACK, "DESYNCMAP f={} sec {}/{}{}", f, p, sec_parts, line);
      }
    }
    if (const auto it = s_details.find(f); it != s_details.end())
    {
      constexpr std::size_t CHUNK = 3600;
      for (const auto& [name, hex] : it->second)
      {
        const std::size_t raw_parts = std::max<std::size_t>(1, (hex.size() + CHUNK - 1) / CHUNK);
        for (std::size_t rp = 0; rp < raw_parts; ++rp)
        {
          NOTICE_LOG_FMT(ROLLBACK, "DESYNCMAP f={} raw {} {}/{} {}", f, name, rp, raw_parts,
                         hex.substr(rp * CHUNK, CHUNK));
        }
      }
    }
  }
}

void RingPort::ResetPacing()
{
  m_reference_before_load.reset();
  m_system.GetCoreTiming().ResetThrottleToNow();
}

Orca::Net::Step StepSession(Orca::Net::Session& session, Orca::Net::Game& game, int completed_frame,
                            const std::function<Orca::Net::Pad()>& sample_local,
                            const std::function<bool()>& stopping,
                            std::chrono::microseconds stall_wait,
                            std::chrono::milliseconds give_up_after, bool* gave_up,
                            std::chrono::nanoseconds* waited_time,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                hold_silence,
                            OnlineMatch::StallBudget* stall_budget,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                room_recovering)
{
  return StepSession(session, game, completed_frame,
      std::function<Orca::Net::Pad(int)>([&](int seat) {
        return seat == session.LocalSeat() ? sample_local() : Orca::Net::Pad{};
      }), stopping, stall_wait, give_up_after, gave_up, waited_time, hold_silence,
      stall_budget, room_recovering);
}

Orca::Net::Step StepSession(Orca::Net::Session& session, Orca::Net::Game& game, int completed_frame,
                            const std::function<Orca::Net::Pad(int)>& sample_local,
                            const std::function<bool()>& stopping,
                            std::chrono::microseconds stall_wait,
                            std::chrono::milliseconds give_up_after, bool* gave_up,
                            std::chrono::nanoseconds* waited_time,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                hold_silence,
                            OnlineMatch::StallBudget* stall_budget,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                room_recovering)
{
  const auto step_start = std::chrono::steady_clock::now();
  Orca::Net::Pads local_pads{};
  const auto sample = [&] {
    const int lead = session.LocalSeat();
    local_pads[lead] = sample_local(lead);
    for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
      if (seat != lead && session.IsLocal(seat))
        local_pads[seat] = sample_local(seat);
  };
  sample();
  bool waited = false;
  // From the end of the first call that said wait or stall to the start of the last call.
  std::optional<std::chrono::steady_clock::time_point> wait_from;
  const auto end_wait = [&](std::chrono::steady_clock::time_point until) {
    if (waited_time && wait_from && until > *wait_from)
      *waited_time += until - *wait_from;
  };
  std::optional<std::chrono::steady_clock::time_point> held_since;
  std::optional<std::chrono::steady_clock::time_point> stall_start;
  const auto finish = [&](Orca::Net::Step result) {
    if (stall_budget)
      stall_budget->FinishStep(stall_start, std::chrono::steady_clock::now(), session.InputStalled(),
                               result.kind != Orca::Net::StepKind::Ended);
    return result;
  };
  std::chrono::steady_clock::time_point last_poll;
  // Whether this call has held the emulator longer than give_up_after. A gap of over a second
  // between polls means this process was suspended (e.g. the computer slept), not that the other
  // side went away, so the timer restarts.
  const auto held_too_long = [&] {
    const auto now = std::chrono::steady_clock::now();
    if (!held_since || now - last_poll > std::chrono::seconds(1))
      held_since = now;
    last_poll = now;
    if (give_up_after.count() <= 0 || now - *held_since <= give_up_after)
      return false;
    if (hold_silence && hold_silence(now))
    {
      if (stall_budget)
        stall_budget->ObserveRecovery(true);
      return false;
    }
    if (gave_up)
      *gave_up = true;
    return true;
  };
  const auto budget_too_long = [&](std::chrono::steady_clock::time_point now,
                                   std::chrono::steady_clock::time_point since) {
    if (!stall_budget || !stall_budget->Poll(now, since, room_recovering && room_recovering(now)))
      return false;
    WARN_LOG_FMT(ROLLBACK,
                 "Online: held on the other players for {} s of the last {} s: they count "
                 "as silent",
                 OnlineMatch::StallBudget::STALL_BUDGET.count(),
                 OnlineMatch::StallBudget::STALL_WINDOW.count());
    if (gave_up)
      *gave_up = true;
    return true;
  };
  while (true)
  {
    const auto call_start = std::chrono::steady_clock::now();
    const Orca::Net::Step step = session.OnBoundary(completed_frame, local_pads[session.LocalSeat()], &local_pads);
    if (step.kind == Orca::Net::StepKind::Wait || step.kind == Orca::Net::StepKind::Stall)
    {
      if (!wait_from)
        wait_from = std::chrono::steady_clock::now();
    }
    if (step.kind == Orca::Net::StepKind::Wait)
    {
      // Time sync: this machine is ahead. Hold one frame, then pace from now so the throttle
      // doesn't race to make up the held frame.
      if (stopping() || budget_too_long(std::chrono::steady_clock::now(), step_start) ||
          held_too_long())
      {
        end_wait(std::chrono::steady_clock::now());
        return finish({Orca::Net::StepKind::Ended, step.frame});
      }
      std::this_thread::sleep_for(FRAME_PERIOD);
      waited = true;
      // The session decides a wait before sampling, so the frame uses the pad read now.
      sample();
      continue;
    }
    if (step.kind != Orca::Net::StepKind::Stall)
    {
      end_wait(call_start);
      if (waited)
        game.ResetPacing();
      return finish(step);
    }
    const auto poll_time = std::chrono::steady_clock::now();
    if (!stall_start)
      stall_start = step_start;
    if (stopping() || budget_too_long(poll_time, *stall_start) || held_too_long())
    {
      end_wait(std::chrono::steady_clock::now());
      return finish({Orca::Net::StepKind::Ended, step.frame});
    }
    if (stall_wait.count() > 0)
      std::this_thread::sleep_for(stall_wait);
  }
}
}  // namespace Rollback
