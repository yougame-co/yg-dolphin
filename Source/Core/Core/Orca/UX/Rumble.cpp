// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Rumble.h"

#include <mutex>

#include <fmt/format.h>

#include "Core/Core.h"
#include "Core/HW/SI/SI.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "Core/Rollback/Rollback.h"
#include "Core/System.h"
#include "InputCommon/ControlReference/ControlReference.h"

namespace Orca::UX::Rumble
{
namespace
{
std::mutex s_mutex;
Tracker s_tracker;

void Print(const std::optional<State>& state)
{
  if (state)
    Status::Line(FormatLine(*state));
}

Target LocalTarget()
{
  const std::optional<LocalDevice> device = LastLocalDevice();
  if (!device)
    return {};
  return {device->source == PadSource::Adapter ? Device::Adapter : Device::Pad, device->index};
}
}  // namespace

bool MotorOn(u32 channel_out)
{
  // The direct command (0x40) a GameCube controller polls with: mode in OUTPUT0, the motor in
  // OUTPUT1 (0 stop, 1 on, 2 hard stop).
  return ((channel_out >> 16) & 0xff) == 0x40 && (channel_out & 0xff) == 1;
}

std::optional<State> Tracker::Boundary(bool motor, bool live, int port, Target target)
{
  if (m_since_line < MIN_GAP_FRAMES)
    ++m_since_line;
  State want = m_sent;
  if (motor && live && target.device != Device::None)
  {
    m_off_run = 0;
    want = {true, port, target};
  }
  else if (m_sent.on && ++m_off_run >= OFF_FRAMES)
  {
    want.on = false;
  }
  // Off is off, whatever port or device it was on.
  if (want == m_sent || (!want.on && !m_sent.on) || m_since_line < MIN_GAP_FRAMES)
    return std::nullopt;
  m_sent = want;
  m_since_line = 0;
  if (!want.on)
    m_off_run = 0;
  return m_sent;
}

std::optional<State> Tracker::Halt()
{
  m_off_run = 0;
  if (!m_sent.on)
    return std::nullopt;
  m_sent.on = false;
  m_since_line = 0;
  return m_sent;
}

std::string FormatLine(const State& state)
{
  const char* device = state.target.device == Device::Adapter ? "adapter" :
                       state.target.device == Device::Pad     ? "pad" :
                                                                "none";
  return fmt::format("orca rumble {} {} {} {}", state.port + 1, state.on ? 1 : 0, device,
                     state.target.device == Device::None ? 0 : state.target.index);
}

void Frame(const Core::CPUThreadGuard& guard, int local_port)
{
  // Rollback::IsResimulating also covers a joiner's catch-up, which the caller's flag may not.
  if (local_port < 0 || local_port >= SerialInterface::MAX_SI_CHANNELS ||
      Rollback::IsResimulating() || !Status::Cap(Status::RUMBLE_CAP))
  {
    return;
  }
  const u32 out = guard.GetSystem().GetSerialInterface().GetChannelOut(local_port);
  // The same focus rule as the pad: no rumble while the game gets no input.
  const bool live = ControlReference::GetInputGate();
  std::optional<State> state;
  {
    std::lock_guard lock(s_mutex);
    state = s_tracker.Boundary(MotorOn(out), live, local_port, LocalTarget());
  }
  Print(state);
}

void Stall(int stalled_ms)
{
  if (stalled_ms >= STALL_HALT_MS)
    Halt();
}

void Halt()
{
  std::optional<State> state;
  {
    std::lock_guard lock(s_mutex);
    state = s_tracker.Halt();
  }
  Print(state);
}
}  // namespace Orca::UX::Rumble
