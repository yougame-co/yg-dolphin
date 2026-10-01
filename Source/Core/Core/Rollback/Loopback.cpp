// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Loopback.h"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <fstream>
#include <map>
#include <sstream>

#include <fmt/format.h>

namespace Rollback::Loopback
{
namespace
{
std::string PadHex(const Orca::Net::Pad& pad)
{
  std::string hex;
  for (const u8 byte : pad)
    hex += fmt::format("{:02x}", byte);
  return hex;
}

// The whole field as hex, or false (no exceptions: the build has them off).
template <typename T>
bool ParseHex(std::string_view text, T* value)
{
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *value, 16);
  return error == std::errc{} && end == text.data() + text.size() && !text.empty();
}

bool ParsePad(std::string_view hex, Orca::Net::Pad* pad)
{
  if (hex.size() != pad->size() * 2)
    return false;
  for (std::size_t i = 0; i < pad->size(); ++i)
  {
    if (!ParseHex(hex.substr(i * 2, 2), &(*pad)[i]))
      return false;
  }
  return true;
}
}  // namespace

std::string FormatRecordedFrame(int frame, const Orca::Net::Pads& pads, u64 checksum)
{
  return fmt::format("{} {} {} {} {} {:016x}\n", frame, PadHex(pads[0]), PadHex(pads[1]),
                     PadHex(pads[2]), PadHex(pads[3]), checksum);
}

std::vector<RecordedFrame> LoadRecording(const std::string& path)
{
  std::ifstream in(path);
  std::map<int, RecordedFrame> frames;
  std::string line;
  while (std::getline(in, line))
  {
    std::istringstream fields(line);
    int frame = -1;
    std::array<std::string, Orca::Net::MAX_SEATS> pads;
    std::string checksum;
    if (!(fields >> frame >> pads[0] >> pads[1] >> pads[2] >> pads[3] >> checksum) || frame < 0)
      continue;
    RecordedFrame& recorded = frames[frame];
    for (std::size_t port = 0; port < pads.size(); ++port)
    {
      if (!ParsePad(pads[port], &recorded.pads[port]))
        return {};
    }
    if (checksum.size() != 16 || !ParseHex(checksum, &recorded.checksum))
      return {};
  }
  std::vector<RecordedFrame> recording;
  for (const auto& [frame, recorded] : frames)
  {
    if (frame != static_cast<int>(recording.size()))
      return {};
    recording.push_back(recorded);
  }
  return recording;
}

VirtualRemote::VirtualRemote(const std::vector<RecordedFrame>& recording, int seat, int local_seat,
                             int lag, int jitter, int input_delay, int checksum_every)
    : m_recording(recording), m_seat(seat), m_local_seat(local_seat), m_lag(lag),
      m_jitter(jitter), m_input_delay(input_delay), m_checksum_every(checksum_every)
{
}

int VirtualRemote::Lag() const
{
  if (m_jitter <= 0)
    return m_lag;
  u32 x = static_cast<u32>(m_ticks) * 2654435761u;
  x ^= x >> 13;
  return m_lag + static_cast<int>(x % static_cast<u32>(m_jitter + 1));
}

void VirtualRemote::Send(const Orca::Net::Packet& packet)
{
  if (!packet.pads.empty())
  {
    m_local_newest =
        std::max(m_local_newest, packet.first_frame + static_cast<int>(packet.pads.size()) - 1);
  }
}

std::vector<Orca::Net::Packet> VirtualRemote::Receive()
{
  const int position = m_ticks - Lag();  // the frame the remote player is running
  const int newest =
      std::min(position + m_input_delay, static_cast<int>(m_recording.size()) - 1);
  std::vector<Orca::Net::Packet> packets;
  while (m_delivered < newest)
  {
    Orca::Net::Packet packet;
    packet.seat = m_seat;
    packet.first_frame = m_delivered + 1;
    const int last = std::min(newest, m_delivered + 16);
    for (int frame = packet.first_frame; frame <= last; ++frame)
      packet.pads.push_back(m_recording[frame].pads[m_seat]);
    packet.ack[m_local_seat] = m_local_newest;
    packet.current_frame = std::max(position, 0);
    const int checksum_frame = m_last_checksum + m_checksum_every;
    if (checksum_frame <= position && checksum_frame < static_cast<int>(m_recording.size()))
    {
      packet.checksum_frame = checksum_frame;
      packet.checksum = m_recording[checksum_frame].checksum;
      m_last_checksum = checksum_frame;
    }
    m_delivered = last;
    packets.push_back(std::move(packet));
  }
  return packets;
}
}  // namespace Rollback::Loopback
