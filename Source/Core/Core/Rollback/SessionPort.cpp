// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/SessionPort.h"

#include <thread>

#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/System.h"

namespace Rollback
{
namespace
{
RingPort* s_active = nullptr;
}  // namespace

RingPort::RingPort(Core::System& system, int max_rollback, bool unthrottled)
    : m_system(system), m_ring(static_cast<std::size_t>(max_rollback) + 2),
      m_unthrottled(unthrottled)
{
  // Until the session sets them: every port connected with nothing pressed, on both machines.
  m_pads.fill(Orca::Net::DecodePad(Orca::Net::Pad{}));
  s_active = this;
  ApplyThrottle();
}

RingPort::~RingPort()
{
  if (s_active == this)
    s_active = nullptr;
  m_ring.Reset(m_system);
  Rollback::SetResimulating(false);
  Core::SetIsThrottlerTempDisabled(false);
}

RingPort* RingPort::Active()
{
  return s_active;
}

bool RingPort::Save(int frame)
{
  // Sparse: only frames a rollback may target or whose checksum is compared. The ring keeps the
  // newest max_rollback + 2 of them.
  m_ring.Save(m_system, frame);
  return true;
}

bool RingPort::Load(int frame)
{
  return m_ring.Load(m_system, frame);
}

void RingPort::SetPads(int frame, const Orca::Net::Pads& pads)
{
  // Called once for every frame about to run.
  for (std::size_t port = 0; port < pads.size(); ++port)
    m_pads[port] = Orca::Net::DecodePad(pads[port]);
  if (m_pacing_reset_frames > 0)
  {
    --m_pacing_reset_frames;
    ApplyThrottle();
  }
}

void RingPort::SetResimulating(bool resimulating)
{
  m_resimulating = resimulating;
  Rollback::SetResimulating(resimulating);
  ApplyThrottle();
}

std::optional<u64> RingPort::SnapshotChecksum(int frame)
{
  return m_ring.RamChecksum(frame);
}

void RingPort::ResetPacing()
{
  // While the throttler is off, CoreTiming keeps moving its reference to the current time; one
  // unthrottled frame is enough to forget the time spent stalled. (The session calls this just
  // before setting the pads of the frame that ends the stall, so that frame runs unthrottled.)
  m_pacing_reset_frames = 2;
  ApplyThrottle();
}

void RingPort::ApplyThrottle()
{
  // Re-run frames catch up to the present as fast as the machine allows.
  Core::SetIsThrottlerTempDisabled(m_unthrottled || m_resimulating || m_pacing_reset_frames > 0);
}

Orca::Net::Step StepSession(Orca::Net::Session& session, int completed_frame,
                            const Orca::Net::Pad& local_pad, const std::function<bool()>& stopping,
                            std::chrono::microseconds stall_wait)
{
  while (true)
  {
    const Orca::Net::Step step = session.OnBoundary(completed_frame, local_pad);
    if (step.kind != Orca::Net::StepKind::Stall)
      return step;
    if (stopping())
      return {Orca::Net::StepKind::Ended, step.frame};
    if (stall_wait.count() > 0)
      std::this_thread::sleep_for(stall_wait);
  }
}
}  // namespace Rollback
