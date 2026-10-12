// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/ReplayStream.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/Version.h"
#include "Common/WebSocket.h"
#include "Core/HW/Memmap.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/YouGameRoom.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/System.h"

namespace Rollback::ReplayStream
{
namespace
{
using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

u64 MemoryChecksum(Core::System& system)
{
  auto& memory = system.GetMemory();
  return RamChecksum({memory.GetRAM(), memory.GetRamSize()},
                     {memory.GetEXRAM(), memory.GetEXRAM() ? memory.GetExRamSize() : 0});
}
}  // namespace

// ---- Writer ----

std::unique_ptr<Writer> Writer::FromEnv()
{
  if (!Orca::UX::TestKnobsAllowed())
    return nullptr;
  std::string base = Orca::GetEnv("ORCA_TEST_REPLAY_OUT");
  const bool to_room = Orca::GetEnv("ORCA_TEST_REPLAY_ROOM") == "1";
  if (base.empty() && !to_room)
    return nullptr;
  return std::make_unique<Writer>(std::move(base), to_room);
}

Writer::Writer(std::string base, bool to_room) : m_base(std::move(base)), m_to_room(to_room)
{
}

// The next whole unit to the file and, for watchers, to the room.
bool Writer::Emit(const std::vector<u8>& bytes)
{
  if (m_file && (!m_file->WriteBytes(bytes.data(), bytes.size()) || !m_file->Flush()))
    return false;
  if (m_to_room)
    Orca::Online::SendReplayStream(bytes);
  return true;
}

Writer::~Writer() = default;

bool Writer::Pump(const Orca::Net::ReplayArchive& replay, int end, int live,
                  const std::map<int, u64>* checksums)
{
  if (replay.first_frame < 0 || replay.frames.empty())
    return true;
  end = std::min(end, replay.EndFrame());
  const u32 seed = replay.frames[0].seed;
  if (!m_started || replay.first_frame != m_first || replay.origin_hash != m_origin ||
      seed != m_seed || end < m_written_end)
  {
    // Another history: a file of its own, from its first frame.
    ++m_segment;
    const std::string path =
        m_base.empty() ? std::string() : fmt::format("{}-{}.ygr", m_base, m_segment);
    m_file = path.empty() ? nullptr : std::make_unique<File::IOFile>(path, "wb");
    Orca::Net::ReplayStreamInfo info;
    info.compat = Orca::Online::CompatibilityKey();
    info.build = Common::GetScmRevGitStr();
    if (const Orca::Profile* profile = Orca::ActiveProfile())
      info.game_id = profile->game_id;
    info.origin_hash = replay.origin_hash;
    info.first_frame = replay.first_frame;
    info.segment = m_segment;
    info.room = Orca::Online::Code();
    const std::vector<u8> header = Orca::Net::PackReplayStreamHeader(info);
    if ((m_file && !*m_file) || !Emit(header))
    {
      ERROR_LOG_FMT(ROLLBACK, "Replay: couldn't write {}", path);
      return false;
    }
    m_started = true;
    m_first = replay.first_frame;
    m_origin = replay.origin_hash;
    m_seed = seed;
    m_written_end = m_first;
    NOTICE_LOG_FMT(ROLLBACK, "Replay: writing history {} (from frame {}, origin {:016x}) to {}",
                   m_segment, m_first, m_origin, path);
    Orca::Status::Line(fmt::format("orca replay-out {}", path.empty() ? "room" : path));
  }
  const int pending = end - m_written_end;
  if (pending <= 0 || (pending < 6 && Clock::now() - m_flushed < std::chrono::milliseconds(100)))
    return true;
  Orca::Net::ReplayChunk chunk;
  chunk.first = m_written_end;
  chunk.live = live;
  const int count = std::min(pending, 3600);
  chunk.frames.reserve(static_cast<size_t>(count));
  for (int f = chunk.first; f < chunk.first + count; ++f)
  {
    Orca::Net::ReplayFrame frame = *replay.At(f);
    // A frame recorded before its hook ran (none should be final) takes the last ports.
    if (!frame.ports)
    {
      frame.ports = chunk.frames.empty() ?
                        std::make_shared<const std::vector<Orca::Events::PortInfo>>() :
                        chunk.frames.back().ports;
    }
    chunk.frames.push_back(std::move(frame));
  }
  if (checksums)
  {
    for (auto it = checksums->upper_bound(chunk.first);
         it != checksums->end() && it->first <= chunk.first + count; ++it)
    {
      chunk.checksums.emplace_back(it->first, it->second);
    }
  }
  const std::vector<u8> bytes = Orca::Net::PackReplayChunk(chunk, chunk.first == replay.first_frame);
  if (bytes.empty() || !Emit(bytes))
  {
    ERROR_LOG_FMT(ROLLBACK, "Replay: couldn't write frames {}-{}", chunk.first,
                  chunk.first + count);
    return false;
  }
  m_written_end = chunk.first + count;
  m_flushed = Clock::now();
  return true;
}

// ---- Player ----

// A watch URL's stream, handed from its socket's thread to the player.
struct Player::Watch
{
  std::thread thread;
  std::atomic<bool> stop{false};
  std::mutex mutex;
  std::vector<u8> bytes;
  bool ended = false;
  std::string error;
};

Player::Player() = default;

Player::~Player()
{
  if (m_watch)
  {
    m_watch->stop = true;
    if (m_watch->thread.joinable())
      m_watch->thread.join();
  }
}

void Player::WatchThread(std::string url)
{
  Watch& watch = *m_watch;
  const auto fail = [&watch](std::string why) {
    std::lock_guard lock(watch.mutex);
    if (watch.error.empty())
      watch.error = std::move(why);
    watch.ended = true;
  };
  Common::WebSocket socket;
  std::string error;
  if (!socket.Connect(url, std::chrono::seconds(10), &error))
  {
    fail("couldn't connect to the match: " + error);
    return;
  }
  auto last_ping = Clock::now();
  while (!watch.stop)
  {
    if (Clock::now() - last_ping > std::chrono::seconds(15))
    {
      socket.SendText(R"({"t":"ping"})", std::chrono::seconds(2));
      last_ping = Clock::now();
    }
    const std::optional<std::string> text = socket.ReceiveText(std::chrono::milliseconds(100));
    if (!text)
    {
      if (!socket.IsOpen())
      {
        std::lock_guard lock(watch.mutex);
        watch.ended = true;
        return;
      }
      continue;
    }
    picojson::value value;
    if (!picojson::parse(value, *text).empty() || !value.is<picojson::object>())
      continue;
    const picojson::object& message = value.get<picojson::object>();
    const auto type = message.find("t");
    if (type == message.end() || !type->second.is<std::string>())
      continue;
    const std::string& t = type->second.get<std::string>();
    if (t == "rs")
    {
      const auto b = message.find("b");
      if (b == message.end() || !b->second.is<std::string>())
        continue;
      const std::string& text64 = b->second.get<std::string>();
      size_t pad = 0;
      while (pad < 2 && pad < text64.size() && text64[text64.size() - 1 - pad] == '=')
        ++pad;
      const std::optional<std::vector<u8>> bytes =
          text64.size() % 4 == 0 ?
              Orca::Net::DecodeInlineKeyframe(text64, text64.size() / 4 * 3 - pad) :
              std::nullopt;
      if (!bytes)
      {
        fail("the match's stream is damaged");
        return;
      }
      std::lock_guard lock(watch.mutex);
      watch.bytes.insert(watch.bytes.end(), bytes->begin(), bytes->end());
    }
    else if (t == "end" || t == "reset")
    {
      // A new history (a fresh start) begins another stream: this one is over.
      {
        std::lock_guard lock(watch.mutex);
        watch.ended = true;
      }
      socket.SendText(R"({"t":"leave"})", std::chrono::seconds(1));
      socket.Close();
      return;
    }
  }
  socket.Close();
}

std::unique_ptr<Player> Player::FromEnv()
{
  if (!Orca::UX::TestKnobsAllowed())
    return nullptr;
  std::string path = Orca::GetEnv("ORCA_TEST_REPLAY_IN");
  if (path.empty())
    return nullptr;
  auto play = std::make_unique<Player>();
  play->m_path = std::move(path);
  play->m_follow = Orca::GetEnv("ORCA_TEST_REPLAY_FOLLOW") == "1";
  play->m_realtime = Orca::GetEnv("ORCA_TEST_REPLAY_REALTIME") == "1";
  if (const std::string delay = Orca::GetEnv("ORCA_TEST_REPLAY_DELAY"); !delay.empty())
    play->m_delay = std::clamp(std::atoi(delay.c_str()), 0, 600);
  if (const std::string seek = Orca::GetEnv("ORCA_TEST_REPLAY_SEEK"); !seek.empty())
  {
    const size_t colon = seek.find(':');
    if (colon != std::string::npos)
    {
      const int a = std::atoi(seek.substr(0, colon).c_str());
      const int b = std::atoi(seek.substr(colon + 1).c_str());
      if (a > 0 && b > a)
        play->m_seek = std::make_pair(a, b);
    }
  }
  play->m_repack = Orca::GetEnv("ORCA_TEST_REPLAY_REPACK");
  play->m_started = Clock::now();
  play->m_last_data = play->m_started;
  return play;
}

// Feeds the reader whatever the file has gained. False, with `error`, if it isn't a valid stream
// for this build.
bool Player::Poll(std::string* error)
{
  if (!m_reader)
    m_reader = std::make_unique<Orca::Net::ReplayStreamReader>(Orca::Online::CompatibilityKey());
  if (m_path.starts_with("ws://") || m_path.starts_with("wss://"))
  {
    if (!m_watch)
    {
      m_watch = std::make_unique<Watch>();
      m_watch->thread = std::thread(&Player::WatchThread, this, m_path);
    }
    std::vector<u8> bytes;
    {
      std::lock_guard lock(m_watch->mutex);
      bytes.swap(m_watch->bytes);
      if (m_watch->ended)
        m_source_ended = true;
      if (!m_watch->error.empty() && !m_reader->Info())
      {
        *error = m_watch->error;
        return false;
      }
    }
    if (bytes.empty())
      return true;
    m_last_data = Clock::now();
    return m_reader->Feed(bytes.data(), bytes.size(), error);
  }
  if (!m_file)
  {
    m_file = std::make_unique<File::IOFile>(m_path, "rb");
    if (!*m_file)
    {
      m_file.reset();
      if (!m_follow)
      {
        *error = "the replay file can't be opened";
        return false;
      }
      return true;  // a live stream not started yet
    }
  }
  const u64 size = m_file->GetSize();
  if (size <= m_offset)
    return true;
  std::vector<u8> bytes(static_cast<size_t>(size - m_offset));
  if (!m_file->Seek(static_cast<s64>(m_offset), File::SeekOrigin::Begin) ||
      !m_file->ReadBytes(bytes.data(), bytes.size()))
  {
    *error = "the replay file can't be read";
    return false;
  }
  m_offset = size;
  m_last_data = Clock::now();
  return m_reader->Feed(bytes.data(), bytes.size(), error);
}

void Player::Finish(const PlayerHost& host, bool ok, const std::string& why)
{
  const auto ms = [](double frames) { return frames * 1000.0 / 60.0; };
  const double avg = m_behind_samples ? static_cast<double>(m_behind_sum) / m_behind_samples : 0.0;
  const double live_avg =
      m_behind_samples ? static_cast<double>(m_live_sum) / m_behind_samples : 0.0;
  Orca::Status::Line(fmt::format(
      "orca replay done {} frames={} checked={} mismatched={} first_mismatch={} seek={} "
      "behind_avg_ms={:.0f} behind_max_ms={:.0f} live_avg_ms={:.0f} live_max_ms={:.0f} "
      "caught_up_ms={:.0f} waits={} waited_ms={:.0f} wall_ms={:.0f} why={}",
      ok && m_mismatched == 0 ? "ok" : "fail", m_played, m_checked, m_mismatched, m_first_mismatch,
      m_seek_done ? "done" : (m_seek ? "missed" : "none"), ms(avg), ms(m_behind_max), ms(live_avg),
      ms(m_live_max), m_caught_up_ms, m_waits, m_waited_ms, MsSince(m_started),
      why.empty() ? "-" : why));
  NOTICE_LOG_FMT(ROLLBACK, "Replay: {} after {} frames, {} checksums checked, {} differ ({})",
                 ok ? "over" : "failed", m_played, m_checked, m_mismatched, why);
  m_over = true;
  host.stop(ok ? "The replay is over" : "The replay couldn't be played: " + why);
}

std::optional<int> Player::Step(Core::System& system, const PlayerHost& host, int* running,
                                u64 origin_hash, int origin_frame,
                                const OnlineMatch::FrameHook& on_frame)
{
  if (m_over)
    return std::nullopt;
  std::optional<int> rewound;
  std::string error;
  if (!Poll(&error))
  {
    Finish(host, false, error);
    return std::nullopt;
  }
  if (!m_loaded)
  {
    // A live stream may not have its header yet.
    while (!m_reader->Info())
    {
      if (host.stopping())
        return std::nullopt;
      if (m_source_ended || Clock::now() - m_last_data > std::chrono::seconds(30))
      {
        Finish(host, false, "the replay never started");
        return std::nullopt;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      if (!Poll(&error))
      {
        Finish(host, false, error);
        return std::nullopt;
      }
    }
    const Orca::Net::ReplayStreamInfo& info = *m_reader->Info();
    if (info.origin_hash != origin_hash || info.first_frame != origin_frame)
    {
      Finish(host, false, "the replay starts from another origin");
      return std::nullopt;
    }
    if (!host.restore_origin())
    {
      Finish(host, false, "couldn't restore this game's local boot");
      return std::nullopt;
    }
    *running = origin_frame - 1;
    m_loaded = true;
    m_catching = !m_realtime && !m_follow;
    host.set_catching_up(m_catching);
    rewound = origin_frame;
    Orca::Status::Line(fmt::format("orca replay start build={} game={} first={} origin={:016x}",
                                   info.build, info.game_id, info.first_frame, info.origin_hash));
  }
  int frame = *running + 1;
  if (m_seek && m_seek_taken && !m_seek_done && frame == m_seek->second)
  {
    const int to = m_seek->first;
    if (!host.restore(std::move(m_seek_image), m_seek_nand, to))
    {
      Finish(host, false, "couldn't seek in the replay");
      return std::nullopt;
    }
    *running = to - 1;
    host.set_catching_up(m_catching);
    m_seek_done = true;
    m_seek_nand.clear();
    Orca::Status::Line(fmt::format("orca replay seek {} -> {}", frame, to));
    frame = to;
    rewound = to;
  }
  Orca::Net::ReplayArchive& archive = m_reader->Archive();
  if (m_follow)
  {
    // A live watcher stays `delay` frames behind the newest final frame, so a late chunk doesn't
    // freeze the picture; it waits when closer, and plays out the tail once the stream goes quiet.
    if (archive.EndFrame() - frame <= m_delay)
    {
      const auto wait_start = Clock::now();
      ++m_waits;
      while (archive.EndFrame() - frame <= m_delay)
      {
        const bool quiet =
            m_source_ended || Clock::now() - m_last_data > std::chrono::seconds(5);
        if (quiet && frame < archive.EndFrame())
          break;
        if (quiet)
        {
          m_waited_ms += MsSince(wait_start);
          Finish(host, true, "the stream ended");
          return std::nullopt;
        }
        if (host.stopping())
          return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!Poll(&error))
        {
          Finish(host, false, error);
          return std::nullopt;
        }
      }
      m_waited_ms += MsSince(wait_start);
    }
  }
  else if (frame >= archive.EndFrame())
  {
    if (!m_repack.empty() && m_mismatched == 0)
    {
      const std::vector<u8> bytes =
          Orca::Net::PackReplayStream(*m_reader->Info(), archive, m_reader->Checksums());
      File::IOFile out(m_repack, "wb");
      const bool written = !bytes.empty() && out && out.WriteBytes(bytes.data(), bytes.size());
      Orca::Status::Line(fmt::format("orca replay repack {} {} bytes", written ? "ok" : "fail",
                                     bytes.size()));
    }
    Finish(host, true, "");
    return std::nullopt;
  }
  const int behind = archive.EndFrame() - frame;
  // A saved replay plays flat out unless asked for real time. A watcher plays flat out while far
  // behind (joining late, or after a stall) and in real time once back near its delay; the gap
  // between the two thresholds keeps it from flapping.
  bool catching = !m_realtime;
  if (m_follow && !m_realtime)
    catching = m_catching ? behind > m_delay + 2 : behind > m_delay + 30;
  if (catching != m_catching)
  {
    host.set_catching_up(catching);
    m_catching = catching;
    if (!catching && m_caught_up_ms < 0)
      m_caught_up_ms = MsSince(m_started);
  }
  if (m_follow && !catching)
  {
    const int behind_live = std::max(behind, m_reader->LiveFrame() - frame);
    m_behind_sum += behind;
    m_live_sum += behind_live;
    ++m_behind_samples;
    m_behind_max = std::max(m_behind_max, behind);
    m_live_max = std::max(m_live_max, behind_live);
  }
  // A seek point: the state at this frame's start, before its hook, as an origin is taken.
  if (m_seek && !m_seek_taken && frame == m_seek->first)
  {
    m_seek_taken =
        host.read_nand(&m_seek_nand) && SnapshotRing::Capture(system, &m_seek_image, false);
    Orca::Status::Line(fmt::format("orca replay seek-point {} {}", frame,
                                   m_seek_taken ? "taken" : "failed"));
  }
  Orca::Net::ReplayFrame* const record = archive.At(frame);
  {
    Orca::Net::ReplayScope scope(record, true, true);
    on_frame(frame, true, *record->ports, false);
  }
  if (const auto it = m_reader->Checksums().find(frame); it != m_reader->Checksums().end())
  {
    const u64 hash = MemoryChecksum(system);
    ++m_checked;
    if (hash != it->second)
    {
      ++m_mismatched;
      if (m_first_mismatch < 0)
      {
        m_first_mismatch = frame;
        WARN_LOG_FMT(ROLLBACK, "Replay: frame {} is {:016x} here, {:016x} in the recording", frame,
                     hash, it->second);
      }
    }
  }
  host.set_pads(frame, record->pads);
  *running = frame;
  ++m_played;
  if (frame - m_last_progress >= 600)
  {
    m_last_progress = frame;
    Orca::Status::Line(fmt::format("orca replay at {} of {} behind {} checked {} mismatched {}",
                                   frame, archive.EndFrame(), behind, m_checked, m_mismatched));
  }
  return rewound;
}
}  // namespace Rollback::ReplayStream
