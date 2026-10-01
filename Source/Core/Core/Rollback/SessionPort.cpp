// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/SessionPort.h"

#include <algorithm>
#include <optional>
#include <thread>

#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/System.h"

namespace Rollback
{
namespace
{
RingPort* s_active = nullptr;

// Brawl's frame period (NTSC, 60 Hz): how long a time-sync wait holds this machine.
constexpr std::chrono::microseconds FRAME_PERIOD{16'667};
}  // namespace

RingPort::RingPort(Core::System& system, int max_rollback, bool unthrottled)
    : m_system(system), m_ring(static_cast<std::size_t>(max_rollback) + 2),
      m_unthrottled(unthrottled)
{
  // Until the session sets them: every port connected with nothing pressed, on both machines.
  m_pads.fill(Orca::Net::DecodePad(Orca::Net::Pad{}));
  s_active = this;
  if (m_unthrottled)
    Core::SetIsThrottlerTempDisabled(true);
}

RingPort::~RingPort()
{
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
  // A boundary the session stalls at asks for the same frame again; nothing ran since.
  if (m_saved_since_run == frame)
    return true;
  const auto start = std::chrono::steady_clock::now();
  const bool allocated = m_ring.Save(m_system, frame);
  double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  // A slot's first use allocates and faults in ~100 MB: not what a save costs. After that, an
  // eighth of each sample, at most four times the estimate: one slow save (a busy machine) barely
  // moves it.
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
  // Loading re-anchors the throttle at the loaded time; keep the reference the present frame was
  // paced by, so the time the rollback takes is made up afterwards.
  if (!m_resimulating && !m_reference_before_load)
    m_reference_before_load = m_system.GetCoreTiming().GetThrottleReference();
  m_saved_since_run = -1;
  return m_ring.Load(m_system, frame);
}

void RingPort::SetPads(int frame, const Orca::Net::Pads& pads)
{
  // Called once for every frame about to run.
  for (std::size_t port = 0; port < pads.size(); ++port)
    m_pads[port] = Orca::Net::DecodePad(pads[port]);
  m_saved_since_run = -1;
}

void RingPort::SetResimulating(bool resimulating)
{
  // While re-running, CoreTiming treats the speed as unlimited (Rollback::IsResimulating), host
  // rendering is skipped and the Mixer drops the re-run's audio.
  m_resimulating = resimulating;
  Rollback::SetResimulating(resimulating);
  if (!resimulating && m_reference_before_load)
  {
    m_system.GetCoreTiming().SetThrottleReference(*m_reference_before_load);
    m_reference_before_load.reset();
  }
}

std::optional<u64> RingPort::SnapshotChecksum(int frame)
{
  return m_ring.RamChecksum(frame);
}

void RingPort::ResetPacing()
{
  m_reference_before_load.reset();
  m_system.GetCoreTiming().ResetThrottleToNow();
}

Orca::Net::Step StepSession(Orca::Net::Session& session, RingPort& port, int completed_frame,
                            const Orca::Net::Pad& local_pad, const std::function<bool()>& stopping,
                            std::chrono::microseconds stall_wait,
                            std::chrono::milliseconds give_up_after, bool* gave_up)
{
  bool waited = false;
  std::optional<std::chrono::steady_clock::time_point> held_since;
  std::chrono::steady_clock::time_point last_poll;
  // Whether this call has held the emulator longer than give_up_after. A gap between two polls
  // much longer than the loop's own sleeps means this process wasn't running (stopped, or the Mac
  // slept): that says nothing about the other side, so the count starts again.
  const auto held_too_long = [&] {
    const auto now = std::chrono::steady_clock::now();
    if (!held_since || now - last_poll > std::chrono::seconds(1))
      held_since = now;
    last_poll = now;
    if (give_up_after.count() <= 0 || now - *held_since <= give_up_after)
      return false;
    if (gave_up)
      *gave_up = true;
    return true;
  };
  while (true)
  {
    const Orca::Net::Step step = session.OnBoundary(completed_frame, local_pad);
    if (step.kind == Orca::Net::StepKind::Wait)
    {
      // Time sync: this machine is ahead. Hold one frame, then pace from now (otherwise the
      // throttle would race to make the held frame up).
      if (stopping() || held_too_long())
        return {Orca::Net::StepKind::Ended, step.frame};
      std::this_thread::sleep_for(FRAME_PERIOD);
      waited = true;
      continue;
    }
    if (step.kind != Orca::Net::StepKind::Stall)
    {
      if (waited)
        port.ResetPacing();
      return step;
    }
    if (stopping() || held_too_long())
      return {Orca::Net::StepKind::Ended, step.frame};
    if (stall_wait.count() > 0)
      std::this_thread::sleep_for(stall_wait);
  }
}
}  // namespace Rollback
