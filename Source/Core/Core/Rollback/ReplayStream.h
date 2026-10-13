// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Keyframe.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/Rollback/Rollback.h"

namespace Core
{
class System;
}
namespace File
{
class IOFile;
}

// Saved matches and live watching (Session/Keyframe.h "Replay streams"). A player's Orca writes its
// history's final frames to a replay stream as they settle; a watcher or a saved replay plays one
// back from this machine's own origin, checking every checksum it carries. A watcher is a player
// of a stream that is still growing: it is no seat in the match, so it can't slow or change it.
//
// Test knobs for now (UX/Probe.h TestKnobsAllowed), until the app can ask for them:
//  - ORCA_TEST_REPLAY_OUT=<base>: write this machine's history to <base>-<n>.ygr (a fresh start or
//    a join begins another history, so another file). ORCA_TEST_REPLAY_ROOM=1 also sends it to the
//    room for its watchers (Online::SendReplayStream); <base> may then be empty.
//    ORCA_TEST_REPLAY_TIMING=1 logs Pump's cost in microseconds every 3600 frames and at shutdown.
//  - ORCA_TEST_REPLAY_IN=<file>: instead of a game, boot to the origin and play <file>; or a
//    ws:// or wss:// watch URL (the rooms Worker's /watch/<code> with a watch ticket), whose
//    {"t":"rs","b":<base64>} messages carry the stream, {"t":"end"} its end; or watch:<room>, which
//    gets its watch ticket from ORCA_SITE for the dev game, a fresh one for every connection. A
//    dropped connection comes back (about 30 s of tries) and skips the bytes already played;
//    {"t":"reset"} starts a new segment, whose header and overlapping frames must match.
//    ORCA_TEST_REPLAY_WATCH_DROP=<s> drops it once, that long after it connected.
//    ORCA_TEST_REPLAY_FOLLOW=1 keeps reading as the file grows, ORCA_TEST_REPLAY_DELAY (12) frames
//    behind its newest frame (a live watcher); ORCA_TEST_REPLAY_REALTIME=1 plays a finished file at
//    real speed rather than flat out; ORCA_TEST_REPLAY_SEEK=<a>:<b> goes back from frame b to a
//    snapshot taken at frame a, once. ORCA_TEST_REPLAY_REPACK=<file> writes a clean replay there
//    again as one compact stream.
// Stdout (session only): "orca replay-out <path>" per file written; a player prints "orca replay
// start ...", "orca replay at ..." every 600 frames, "orca replay seek ...", "orca replay
// reconnected <n>", "orca replay resumed ..." after a reset, and one
// "orca replay done ok|fail <key=value>... why=<text>" line.
namespace Rollback::ReplayStream
{
class Writer
{
public:
  static std::unique_ptr<Writer> FromEnv();
  explicit Writer(std::string base, bool to_room = false);
  ~Writer();
  // Appends `replay`'s frames before `end` (all final) with the checksums among `checksums`
  // (frame: RAM checksum at its start) that they cover, at most every 6 frames or 100 ms. `live`
  // is this machine's own newest frame, for watchers. A history other than the one being written
  // begins another file. False once writing failed.
  // With the room on (ORCA_TEST_REPLAY_ROOM), the room takes a stream only from a seated player
  // in a match, so it starts once `room_ready` (a session runs): the header, then everything
  // written so far in compact chunks, then each chunk as it is written. A reopened room starts
  // again with the same header and backlog.
  bool Pump(const Orca::Net::ReplayArchive& replay, int end, int live,
            const std::map<int, u64>* checksums, bool room_ready = false);

private:
  bool Emit(const std::vector<u8>& bytes);
  bool StartRoom(const Orca::Net::ReplayArchive& replay, int live);
  void LogTiming(bool final) const;

  struct Timing;
  std::unique_ptr<Timing> m_timing;

  std::string m_base;
  bool m_to_room = false;
  bool m_started = false;
  bool m_room_started = false;
  u64 m_room_generation = 0;
  // The checksums already written: sessions only retain their last few seconds, but a reopened
  // room needs the whole backlog's checksums too.
  std::map<int, u64> m_room_checksums;
  std::vector<u8> m_header;
  u32 m_segment = 0;
  std::unique_ptr<File::IOFile> m_file;
  int m_first = -1;
  u64 m_origin = 0;
  u32 m_seed = 0;
  int m_written_end = -1;
  std::chrono::steady_clock::time_point m_flushed{};
};

// What a player needs from the match it runs in: the restores it may do, and how it drives frames.
struct PlayerHost
{
  // Puts this machine's own origin back; its frame starts next. False: the run can't go on.
  std::function<bool()> restore_origin;
  // Puts back a seek point this player took, as `frame`.
  std::function<bool(MachineImage image, const std::vector<Orca::Net::NandEntry>& nand, int frame)>
      restore;
  // This machine's session NAND, for a seek point.
  std::function<bool(std::vector<Orca::Net::NandEntry>* nand)> read_nand;
  std::function<void(bool catching_up)> set_catching_up;
  std::function<void(int frame, const Orca::Net::Pads& pads)> set_pads;
  std::function<bool()> stopping;
  // Ends the run, with a message for the player.
  std::function<void(const std::string& message)> stop;
};

class Player
{
public:
  Player();
  ~Player();
  static std::unique_ptr<Player> FromEnv();
  // Plays the next recorded frame instead of a game, once this machine's origin is in. `running`
  // is the last frame run. Returns the frame the state now starts at when it was rewound (the
  // replay loaded, or a seek). Over() once it ended, either way.
  std::optional<int> Step(Core::System& system, const PlayerHost& host, int* running,
                          u64 origin_hash, int origin_frame, const OnlineMatch::FrameHook& on_frame);
  bool Over() const { return m_over; }

private:
  bool Poll(std::string* error);
  void WatchThread(std::string source);
  void Finish(const PlayerHost& host, bool ok, const std::string& why);

  std::string m_path;
  bool m_follow = false;
  bool m_realtime = false;
  int m_delay = 12;
  std::optional<std::pair<int, int>> m_seek;
  // ORCA_TEST_REPLAY_REPACK=<file>: after a whole replay played clean, write it there as one
  // compact stream (PackReplayStream).
  std::string m_repack;
  std::unique_ptr<File::IOFile> m_file;
  u64 m_offset = 0;
  // A watch URL's socket runs on its own thread, handing bytes over here.
  struct Watch;
  std::unique_ptr<Watch> m_watch;
  bool m_source_ended = false;
  std::unique_ptr<Orca::Net::ReplayStreamReader> m_reader;
  bool m_loaded = false;
  bool m_over = false;
  bool m_catching = true;
  int m_played = 0;
  int m_checked = 0;
  int m_mismatched = 0;
  int m_first_mismatch = -1;
  std::chrono::steady_clock::time_point m_started{}, m_last_data{};
  // While watching in real time: frames behind the stream's newest frame, and behind the
  // recording machine's own live frame (the stream's lag plus this one's).
  s64 m_behind_sum = 0;
  int m_behind_samples = 0;
  int m_behind_max = 0;
  s64 m_live_sum = 0;
  int m_live_max = 0;
  // When a watcher first reached real time (its catch-up time), or -1.
  double m_caught_up_ms = -1;
  int m_waits = 0;
  double m_waited_ms = 0;
  int m_last_progress = 0;
  MachineImage m_seek_image;
  std::vector<Orca::Net::NandEntry> m_seek_nand;
  bool m_seek_taken = false;
  bool m_seek_done = false;
};
}  // namespace Rollback::ReplayStream
