// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/OnlineMatch.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#ifdef __APPLE__
#include <sys/clonefile.h>
#endif

#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Keyframe.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Status.h"
#include "Core/Rollback/SessionPort.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"

namespace Rollback::OnlineMatch
{
namespace
{
using Orca::Net::Pad;
using Orca::Net::Pads;
using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The host's keyframe, captured at a frame boundary and packed, compressed and stored on a thread
// of its own while the game goes on.
struct KeyframeJob
{
  std::thread thread;
  std::atomic<bool> done{false};
  bool ok = false;
  int frame = -1;
  Orca::Net::KeyframeInfo info;
  double capture_ms = 0, pack_ms = 0, put_ms = 0;
  u64 raw_size = 0;
  std::string error;
  ~KeyframeJob()
  {
    if (thread.joinable())
      thread.join();
  }
};

// A joining player's download of the host's keyframe, unpacked on its own thread.
struct Download
{
  std::thread thread;
  std::atomic<bool> done{false};
  std::atomic<int> percent{0};
  std::atomic<bool> cancel{false};
  bool ok = false;
  std::string error;
  Orca::Net::KeyframeInfo info;
  int frame = -1;
  MachineImage image;
  std::vector<Orca::Net::NandEntry> nand;
  double transfer_ms = 0, unpack_ms = 0;
  ~Download()
  {
    cancel = true;
    if (thread.joinable())
      thread.join();
  }
};

struct Match
{
  bool started = false;
  bool finished = false;
  // Emulation stopped since this match started: the next boot plays a new match.
  bool stale = false;
  std::unique_ptr<RingPort> port;
  std::unique_ptr<Orca::Net::Session> session;
  std::unique_ptr<Orca::Net::KeyframeStore> store;
  bool joining = false;
  int local_seat = 0;
  // The frame the current boundary completes (-1 at the first boundary).
  int running = -1;

  // Host. Every seat's pads for the frames played solo since the newest keyframe (the session
  // takes the log over while friends play, and hands it back).
  int log_base = 0;
  std::vector<Pads> log;
  // Local inputs the session had already sampled ahead (its input delay) when it ended.
  std::deque<Pad> queued_local;
  std::unique_ptr<KeyframeJob> job;
  std::optional<Orca::Net::KeyframeInfo> keyframe;
  double keyframe_capture_ms = 0;
  bool keyframe_wanted = false;
  // Players who arrived and wait for a keyframe before the session takes them in.
  std::map<int, std::string> waiting;

  // A joining player.
  std::unique_ptr<Download> download;
  int host_seat = 0;
  std::string host_name;
  bool host_here = false;
  Clock::time_point boot_time{}, offered_time{}, loaded_time{};
  double load_ms = 0;
  bool joined = false;
  int leave_at = -1;
  bool leave_requested = false;
  int last_percent = -1;

  u32 round_trip_sequence = 0;
  std::optional<Clock::time_point> last_return;
  Common::EventHook on_state_changed;
};

Match& TheMatch()
{
  static Match match;
  return match;
}

// Emulation is stopping, or the host asked the CPU thread to pause (a pause, a stop with
// confirmation, a PauseAndLock): its caller waits for the CPU thread to go idle, so a loop that
// holds the CPU thread here must give up rather than deadlock it.
bool Stopping(Core::System& system)
{
  return Core::GetState(system) == Core::State::Stopping ||
         system.GetCPU().GetState() != CPU::State::Running;
}

// Two frame periods at 60 Hz: a frame slower than this was a hitch.
constexpr std::chrono::microseconds HITCH_TIME{33'334};
// A friend whose game froze with the connection still open would stall this one forever: after
// this long, the host unplugs it and plays on; a joiner stops.
constexpr std::chrono::seconds PEER_SILENCE_LIMIT{10};
constexpr char PEER_SILENT_SENTENCE[] = "Your friend's game stopped responding";
// The session's rollback window (Config::max_rollback): RingPort keeps this many snapshots and two.
constexpr int MAX_ROLLBACK = 7;
// A keyframe this recent serves a new arrival as it is: the joiner replays up to this many frames.
constexpr int KEYFRAME_FRESH_FRAMES = 30 * 60;
// A joiner far behind the host runs unthrottled.
constexpr int CATCH_UP_BEHIND = 12;
// How long a joiner waits for the host's keyframe before it gives up.
constexpr std::chrono::seconds JOIN_LIMIT{90};

// A round-trip sample the session has not seen yet (the room numbers its samples).
std::optional<int> NewRoundTrip(u32* last_sequence)
{
  u32 sequence = 0;
  const int round_trip_ms = Orca::Online::RoundTripMs(&sequence);
  if (round_trip_ms < 0 || sequence == *last_sequence)
    return std::nullopt;
  *last_sequence = sequence;
  return round_trip_ms;
}

void StopEmulation(Core::System& system, const std::string& message)
{
  Core::DisplayMessage(message, 10000);
  if (!Stopping(system))
    Core::QueueHostJob([](Core::System& host_system) { Core::Stop(host_system); });
}

IOS::HLE::FS::HostFileSystem* HostNand(Core::System& system)
{
  IOS::HLE::EmulationKernel* ios = system.GetIOS();
  return ios ? static_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get()) : nullptr;
}

Orca::Net::Config SessionConfig()
{
  Orca::Net::Config config;
  config.seats = Orca::Net::MAX_SEATS;
  config.max_rollback = MAX_ROLLBACK;
  // Snapshot spacing is this machine's own business (it changes no frame's result), chosen from
  // the measured save time unless a test fixes it.
  config.snapshot_every = Orca::TestSnapshotEvery();
  return config;
}

// ---- Host ----

// Captures the state at the start of frame `frame` (the boundary's next frame) and stores it on a
// thread. Only where a snapshot may be taken: the frame-boundary hook, with every earlier frame
// final.
void StartKeyframe(Core::System& system, Match& match, int frame)
{
  auto job = std::make_unique<KeyframeJob>();
  job->frame = frame;
  const auto start = Clock::now();
  MachineImage image;
  if (!SnapshotRing::Capture(system, &image, !match.session))
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: capturing a keyframe at frame {} failed", frame);
    return;
  }
  // The NAND's files as of this boundary: a copy-on-write clone where the file system has one
  // (instant), else read now.
  IOS::HLE::FS::HostFileSystem* nand = HostNand(system);
  std::string clone;
  std::vector<Orca::Net::NandEntry> entries;
  bool nand_ok = nand != nullptr;
#ifdef __APPLE__
  if (nand)
  {
    clone = File::GetUserPath(D_USER_IDX) + fmt::format("OrcaKeyframeNand-{}", frame);
    File::DeleteDirRecursively(clone);
    if (clonefile(nand->HostRoot().c_str(), clone.c_str(), CLONE_NOFOLLOW) != 0)
      clone.clear();
  }
#endif
  if (nand && clone.empty())
    nand_ok = Orca::Net::ReadNandTree(nand->HostRoot(), &entries);
  job->capture_ms = MsSince(start);
  if (!nand_ok)
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: reading the NAND for a keyframe failed");
    return;
  }

  KeyframeJob* const raw = job.get();
  Orca::Net::KeyframeStore* const store = match.store.get();
  raw->thread = std::thread([raw, store, clone, image = std::move(image),
                             entries = std::move(entries)]() mutable {
    if (!clone.empty())
    {
      if (!Orca::Net::ReadNandTree(clone, &entries))
        raw->error = "reading the NAND clone failed";
      File::DeleteDirRecursively(clone);
    }
    const auto pack_start = Clock::now();
    u64 raw_size = image.state.size() + image.mem1.size() + image.mem2.size();
    for (const auto& e : entries)
      raw_size += e.data.size();
    raw->raw_size = raw_size;
    std::vector<u8> blob =
        raw->error.empty() ? Orca::Net::PackKeyframe(raw->frame, image, entries) : std::vector<u8>{};
    raw->pack_ms = MsSince(pack_start);
    if (raw->error.empty() && blob.empty())
      raw->error = "compression failed";
    if (raw->error.empty())
    {
      raw->info.frame = raw->frame;
      raw->info.size = blob.size();
      raw->info.hash = Orca::Net::KeyframeHash(blob);
      raw->info.id = fmt::format("kf-{}-{}", raw->frame, raw->info.hash.substr(0, 8));
      const auto put_start = Clock::now();
      raw->ok = store->Put(raw->info.id, blob, &raw->error);
      raw->put_ms = MsSince(put_start);
    }
    raw->done = true;
  });
  match.job = std::move(job);
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: keyframe of frame {} captured in {:.1f} ms", frame,
                 match.job->capture_ms);
}

// The host's solo frame: its own pad on port 1, every other port unplugged.
void RunSolo(Match& match, const std::function<Pad(int)>& local_pad)
{
  Pad pad;
  if (!match.queued_local.empty())
  {
    pad = match.queued_local.front();
    match.queued_local.pop_front();
  }
  else
  {
    pad = local_pad(0);
  }
  Pads pads;
  pads.fill(Orca::Net::UNPLUGGED_PAD);
  pads[0] = pad;
  const int frame = match.running + 1;
  match.port->SetPads(frame, pads);
  match.log.push_back(pads);
  match.running = frame;
}

void GoSolo(Match& match, const char* why)
{
  match.log = match.session->TakeLog(&match.log_base);
  const std::vector<Pad> pending = match.session->PendingLocal();
  match.queued_local.assign(pending.begin(), pending.end());
  Orca::Net::Stats stats = match.session->GetStats();
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: back to solo play at frame {} ({}): {} rollbacks, {} checksums matched",
                 match.running + 1, why, stats.rollbacks, stats.checksums_matched);
  match.session.reset();
  match.port->SetCatchingUp(false);
  Orca::Status::State("playing");
}

void HostEvents(Core::System& system, Match& match)
{
  for (const Orca::Net::PeerEvent& event : Orca::Online::TakePeerEvents())
  {
    if (event.kind == Orca::Net::PeerEvent::Kind::Arrived)
    {
      if (event.seat <= 0 || event.host)
      {
        WARN_LOG_FMT(ROLLBACK, "Drop-in: ignoring a player at seat {}{}", event.seat + 1,
                     event.host ? " that says it hosts" : "");
        continue;
      }
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {} arrived for port {}", event.name, event.seat + 1);
      Core::DisplayMessage(fmt::format("{} is joining on port {}", event.name, event.seat + 1), 4000);
      match.waiting[event.seat] = event.name;
      match.keyframe_wanted = true;
      Orca::Status::State("friend-joining");
    }
    else if (event.kind == Orca::Net::PeerEvent::Kind::Left)
    {
      match.waiting.erase(event.seat);
      if (match.session)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} left the room", event.seat + 1);
        match.session->DropSeat(event.seat);
      }
    }
  }
  if (Orca::Online::TakePrepareJoin())
    match.keyframe_wanted = true;

  // A finished keyframe: offer it to whoever waits, and drop the solo log before it.
  if (match.job && match.job->done)
  {
    std::unique_ptr<KeyframeJob> job = std::move(match.job);
    if (job->thread.joinable())
      job->thread.join();
    if (!job->ok)
    {
      ERROR_LOG_FMT(ROLLBACK, "Drop-in: keyframe of frame {} failed: {}", job->frame, job->error);
    }
    else
    {
      NOTICE_LOG_FMT(ROLLBACK,
                     "Drop-in: keyframe of frame {}: {:.1f} MB raw, {:.1f} MB stored; capture "
                     "{:.1f} ms, pack and compress {:.0f} ms, store {:.0f} ms",
                     job->frame, job->raw_size / 1e6, job->info.size / 1e6, job->capture_ms,
                     job->pack_ms, job->put_ms);
      // An earlier keyframe nobody took (an invite not accepted yet) is replaced.
      if (match.keyframe && match.store)
        match.store->Delete(match.keyframe->id);
      match.keyframe = job->info;
      if (!match.session && job->frame >= match.log_base &&
          job->frame <= match.log_base + static_cast<int>(match.log.size()))
      {
        match.log.erase(match.log.begin(), match.log.begin() + (job->frame - match.log_base));
        match.log_base = job->frame;
      }
    }
  }

  // Capture a keyframe once one is wanted and the newest is stale (or missing). In a session, only
  // at a boundary whose earlier frames are all final.
  const int next = match.running + 1;
  const bool fresh = match.keyframe && next - match.keyframe->frame <= KEYFRAME_FRESH_FRAMES;
  if (match.keyframe_wanted && !match.job && match.store)
  {
    if (fresh)
      match.keyframe_wanted = false;
    else if (!match.session || match.session->Settled())
    {
      match.keyframe_wanted = false;
      StartKeyframe(system, match, next);
    }
  }
  if (!match.store && !match.waiting.empty())
    WARN_LOG_FMT(ROLLBACK, "Drop-in: no keyframe store: a friend can't join yet");

  // Take in whoever waits once there is a keyframe for them.
  if (!match.keyframe || match.waiting.empty() || match.job)
    return;
  for (auto it = match.waiting.begin(); it != match.waiting.end();)
  {
    const int seat = it->first;
    const int keyframe_frame = match.keyframe->frame;
    if (!match.session)
    {
      if (keyframe_frame < match.log_base)
      {
        ++it;
        continue;
      }
      Orca::Net::Config config = SessionConfig();
      config.local_seat = 0;
      config.authority_seat = 0;
      config.start_frame = next;
      config.start_pad = match.log.empty() ? Pad{} : match.log.back()[0];
      std::array<Orca::Net::SeatPlan, Orca::Net::MAX_SEATS> plan{};
      plan[0] = {0, Orca::Net::NEVER, 0};
      config.plan = plan;
      // Whatever arrived while solo (a friend's last packets, after it left) belongs to no one.
      Orca::Online::Transport()->Receive();
      match.session = std::make_unique<Orca::Net::Session>(config, *match.port,
                                                           *Orca::Online::Transport());
      match.session->SetLog(match.log_base, std::move(match.log));
      match.log.clear();
      // Pads sampled while solo carry no delay; from here they do, and the frames the delay skips
      // repeat the pad held now (Config::start_pad).
      match.queued_local.clear();
    }
    match.session->AddPeer(seat, next, keyframe_frame);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {}", match.session->Describe());
    Orca::Online::OfferKeyframe(seat, *match.keyframe);
    NOTICE_LOG_FMT(ROLLBACK,
                   "Drop-in: port {} replays from keyframe frame {}, live from frame {}", seat + 1,
                   keyframe_frame, next);
    it = match.waiting.erase(it);
  }
  // A joiner deletes the keyframe it loaded: the next arrival gets a fresh one.
  if (match.waiting.empty())
    match.keyframe.reset();
}

// ---- A joining player ----

void StartDownload(Match& match, const Orca::Net::KeyframeInfo& info)
{
  auto download = std::make_unique<Download>();
  download->info = info;
  Download* const raw = download.get();
  Orca::Net::KeyframeStore* const store = match.store.get();
  raw->thread = std::thread([raw, store] {
    const auto start = Clock::now();
    std::string error;
    const auto blob = store->Get(
        raw->info.id, raw->info.size,
        [raw](u64 done, u64 total) {
          raw->percent = static_cast<int>(done * 100 / std::max<u64>(total, 1));
          return !raw->cancel.load();
        },
        &error);
    raw->transfer_ms = MsSince(start);
    if (!blob)
    {
      raw->error = error;
      raw->done = true;
      return;
    }
    if (Orca::Net::KeyframeHash(*blob) != raw->info.hash)
    {
      raw->error = "the keyframe arrived damaged";
      raw->done = true;
      return;
    }
    const auto unpack_start = Clock::now();
    raw->ok = Orca::Net::UnpackKeyframe(*blob, &raw->frame, &raw->image, &raw->nand) &&
              raw->frame == raw->info.frame;
    raw->unpack_ms = MsSince(unpack_start);
    if (!raw->ok)
      raw->error = "the keyframe doesn't unpack";
    raw->done = true;
  });
  match.download = std::move(download);
}

// Blocks at the joiner's first boundary until the host's keyframe is loaded. Returns the frame it
// holds (the state is now the start of it), or nullopt (emulation stops; the reason is shown).
std::optional<int> JoinHost(Core::System& system, Match& match)
{
  match.boot_time = Clock::now();
  Orca::Status::State("joining 0");
  std::string last_status;
  while (!Stopping(system))
  {
    for (const Orca::Net::PeerEvent& event : Orca::Online::TakePeerEvents())
    {
      if (event.kind == Orca::Net::PeerEvent::Kind::Arrived && event.host)
      {
        match.host_here = true;
        match.host_seat = event.seat;
        match.host_name = event.name.empty() ? "your friend" : event.name;
        Core::DisplayMessage(fmt::format("Joining {}'s game...", match.host_name), 60000);
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Left && event.host)
      {
        StopEmulation(system, "Your friend left before you could join");
        Orca::Status::Error("peer_left", "Your friend left before you could join");
        return std::nullopt;
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Keyframe && !match.download)
      {
        match.offered_time = Clock::now();
        match.host_seat = event.seat;
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: the host offers keyframe {} (frame {}, {:.1f} MB)",
                       event.keyframe.id, event.keyframe.frame, event.keyframe.size / 1e6);
        StartDownload(match, event.keyframe);
      }
    }
    if (Orca::Online::RoomEnded())
    {
      const std::string status = Orca::Online::StatusLine();
      StopEmulation(system, fmt::format("Couldn't join: {}", status));
      return std::nullopt;
    }
    if (match.download)
    {
      // Download is the first half of joining; catching up is the second.
      const int percent = match.download->percent / 2;
      if (percent != match.last_percent)
      {
        match.last_percent = percent;
        Orca::Status::State(fmt::format("joining {}", percent));
      }
      if (match.download->done)
        break;
    }
    if (Clock::now() - match.boot_time > JOIN_LIMIT)
    {
      StopEmulation(system, "Couldn't join: your friend's game never sent its state");
      Orca::Status::Error("network", "Your friend's game never sent its state");
      return std::nullopt;
    }
    if (std::string status = Orca::Online::StatusLine(); status != last_status)
    {
      NOTICE_LOG_FMT(NETPLAY, "Orca: {}", status);
      last_status = std::move(status);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!match.download || !match.download->done)
    return std::nullopt;
  std::unique_ptr<Download> download = std::move(match.download);
  if (download->thread.joinable())
    download->thread.join();
  if (!download->ok)
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: keyframe {}: {}", download->info.id, download->error);
    StopEmulation(system, "Couldn't join: " + download->error);
    Orca::Status::Error("network", "Couldn't load your friend's game");
    return std::nullopt;
  }

  // The host's NAND, then its machine. The session NAND is a temporary folder of this session's.
  const auto load_start = Clock::now();
  IOS::HLE::FS::HostFileSystem* nand = HostNand(system);
  if (!nand || !Core::WiiRootIsTemporary() ||
      !Orca::Net::ReplaceNandTree(nand->HostRoot(), download->nand))
  {
    StopEmulation(system, "Couldn't join: the game's save folder couldn't be replaced");
    Orca::Status::Error("internal", "Couldn't load your friend's game");
    return std::nullopt;
  }
  nand->ReloadFst();
  const int frame = download->frame;
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
  if (!match.port->LoadImage(std::move(download->image), frame))
  {
    StopEmulation(system, "Couldn't join: your friend's game state didn't load");
    Orca::Status::Error("internal", "Couldn't load your friend's game");
    return std::nullopt;
  }
  match.load_ms = MsSince(load_start);
  match.loaded_time = Clock::now();
  if (match.store)
    match.store->Delete(download->info.id);
  NOTICE_LOG_FMT(ROLLBACK,
                 "Drop-in: loaded keyframe frame {}: waited {:.0f} ms for it, transfer {:.0f} ms, "
                 "unpack {:.0f} ms, load {:.0f} ms",
                 frame, std::chrono::duration<double, std::milli>(match.offered_time - match.boot_time).count(),
                 download->transfer_ms, download->unpack_ms, match.load_ms);

  Orca::Net::Config config = SessionConfig();
  config.local_seat = match.local_seat;
  config.authority_seat = match.host_seat;
  config.start_frame = frame;
  std::array<Orca::Net::SeatPlan, Orca::Net::MAX_SEATS> plan{};
  plan[match.host_seat] = {0, Orca::Net::NEVER, 0};
  config.plan = plan;
  match.session =
      std::make_unique<Orca::Net::Session>(config, *match.port, *Orca::Online::Transport());
  match.port->SetCatchingUp(true);
  match.running = frame - 1;
  return frame;
}
}  // namespace

std::optional<int> OnBoundary(Core::System& system,
                              const std::function<Orca::Net::Pad(int local_seat)>& local_pad)
{
  Match& match = TheMatch();
  if (match.stale)
  {
    // A new boot in the same process (the room was left when the last one stopped).
    match.session.reset();
    match.port.reset();
    match.job.reset();
    match.download.reset();
    match = {};
  }
  if (match.finished)
    return std::nullopt;

  std::optional<int> rewound_to;
  if (!match.started)
  {
    match.started = true;
    // Once emulation has stopped (the CPU thread is gone), leave the room.
    if (!match.on_state_changed)
    {
      match.on_state_changed = Core::AddOnStateChangedCallback([](Core::State state) {
        if (state != Core::State::Uninitialized)
          return;
        End("emulation stopped");
        TheMatch().stale = true;
      });
    }
    match.store = Orca::Net::MakeKeyframeStore();
    if (const std::string leave = Orca::GetEnv("ORCA_TEST_LEAVE_AT"); !leave.empty())
      match.leave_at = std::atoi(leave.c_str());
    Orca::Online::Start();
    // The role decides whether this boundary plays (the host) or waits (a joiner); it is known
    // as soon as the room has its ticket.
    std::optional<bool> joining;
    while (!(joining = Orca::Online::Joining()) && !Orca::Online::RoomEnded() && !Stopping(system))
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    match.joining = joining.value_or(false);
    if (match.joining)
    {
      while (Orca::Online::Seat() < 0 && !Orca::Online::RoomEnded() && !Stopping(system))
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      match.local_seat = Orca::Online::Seat();
      if (!match.store)
      {
        StopEmulation(system, "Couldn't join: this Orca has nowhere to get your friend's game from");
        End("no keyframe store");
        return std::nullopt;
      }
      rewound_to = JoinHost(system, match);
      if (!rewound_to)
      {
        End("couldn't join");
        return std::nullopt;
      }
    }
    else
    {
      match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
      Orca::Status::State("playing");
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: playing solo on port 1; room {}", Orca::Online::StatusLine());
    }
  }

  if (!match.joining)
  {
    if (!Orca::Online::RoomEnded())
      HostEvents(system, match);
    if (!match.session)
    {
      RunSolo(match, local_pad);
      return std::nullopt;
    }
  }
  if (!match.session)
    return std::nullopt;

  // Round trips feed the session's adaptive input delay.
  if (const auto round_trip_ms = NewRoundTrip(&match.round_trip_sequence))
    match.session->OnRoundTrip(*round_trip_ms);
  // A frame that took more than two frame periods of wall time was a hitch on this machine (a disc
  // read, a shader compile): the other side stalls on it, and no input delay would have helped.
  const auto now = Clock::now();
  if (match.last_return && now - *match.last_return > HITCH_TIME && !match.port->CatchingUp())
    match.session->OnLocalHitch();

  if (match.joining && match.leave_at >= 0 && match.running >= match.leave_at &&
      !match.leave_requested)
  {
    match.leave_requested = true;
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: leaving at frame {}", match.running);
    match.session->RequestLeave();
  }

  for (int attempt = 0; attempt < 2; ++attempt)
  {
    bool peer_silent = false;
    const Orca::Net::Step step = StepSession(
        *match.session, *match.port, match.running, local_pad(match.local_seat),
        [&system] { return Stopping(system); }, std::chrono::milliseconds{1}, PEER_SILENCE_LIMIT,
        &peer_silent);
    match.last_return = Clock::now();
    if (step.kind == Orca::Net::StepKind::Run || step.kind == Orca::Net::StepKind::Rollback)
    {
      match.running = step.frame;
      // Every half minute of play with friends, the session's numbers (checksums matched...).
      if (step.kind == Orca::Net::StepKind::Run && !match.session->Resimulating() &&
          step.frame % 1800 == 0 && !match.port->CatchingUp())
      {
        LogStats(fmt::format("at frame {}", step.frame).c_str());
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {}", match.session->Describe());
      }
      if (match.joining)
      {
        // Unthrottled, without rendering, while far behind the host.
        const int behind = match.session->AuthorityFrame() - step.frame;
        match.port->SetCatchingUp(match.session->CatchingUp() || behind > CATCH_UP_BEHIND);
        if (match.session->CatchingUp())
        {
          const int percent = 50 + std::clamp(50 - behind / 4, 0, 49);
          if (percent != match.last_percent)
          {
            match.last_percent = percent;
            Orca::Status::State(fmt::format("joining {}", percent));
          }
        }
        else if (!match.joined && match.session->Plugged(match.local_seat, step.frame))
        {
          match.joined = true;
          Orca::Status::State("playing");
          const double catch_up = MsSince(match.loaded_time);
          NOTICE_LOG_FMT(ROLLBACK,
                         "Drop-in: plugged into port {} at frame {}: catch-up {:.0f} ms, {:.0f} ms "
                         "since the keyframe was offered, {:.0f} ms since this Orca's first frame",
                         match.local_seat + 1, step.frame, catch_up, MsSince(match.offered_time),
                         MsSince(match.boot_time));
          Core::DisplayMessage(fmt::format("Joined {}'s game on port {}", match.host_name,
                                           match.local_seat + 1),
                               4000);
        }
        if (match.session->LeftDone())
        {
          NOTICE_LOG_FMT(ROLLBACK, "Drop-in: unplugged at frame {}",
                         match.session->Plan()[match.local_seat].unplug_from);
          LogStats("at leave");
          End("left");
          StopEmulation(system, "You left your friend's game");
          return rewound_to;
        }
      }
      else if (match.session->Idle() && match.waiting.empty())
      {
        GoSolo(match, "no friends left");
      }
      if (step.kind == Orca::Net::StepKind::Rollback)
        return step.frame;
      return rewound_to;
    }
    if (step.kind != Orca::Net::StepKind::Ended)
    {
      // StepSession handles every other step itself; reaching this is a bug.
      ERROR_LOG_FMT(ROLLBACK, "Online: unexpected session step {} at frame {}",
                    static_cast<int>(step.kind), step.frame);
      return rewound_to;
    }
    if (Stopping(system))
      return rewound_to;
    const std::string error = match.session->Error();
    if (!match.joining && attempt == 0 && match.session->CurrentFrame() == match.running + 1)
    {
      // The host plays on: whoever stopped answering, or broke the session, is unplugged.
      WARN_LOG_FMT(ROLLBACK, "Drop-in: unplugging every friend at frame {}: {}", match.running + 1,
                   peer_silent ? "no word from them" : error);
      Core::DisplayMessage(peer_silent ? std::string(PEER_SILENT_SENTENCE) :
                                         fmt::format("Your friend was disconnected: {}", error),
                           6000);
      if (error.empty())
      {
        // Silent friends: unplug them and step again (the session still runs).
        match.session->DropAllPeers();
        continue;
      }
      GoSolo(match, "the session failed");
      RunSolo(match, local_pad);
      return rewound_to;
    }
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: session state at the end: {}", match.session->Describe());
    if (peer_silent)
    {
      WARN_LOG_FMT(ROLLBACK, "Online: no word from the host for {} s at frame {}",
                   PEER_SILENCE_LIMIT.count(), match.running);
      Orca::Online::ReportPeerStalled(PEER_SILENT_SENTENCE);
    }
    Orca::Online::ReportSessionEnd(error);
    End("session ended");
    StopEmulation(system, peer_silent ? std::string(PEER_SILENT_SENTENCE) :
                                        fmt::format("Online match ended: {}",
                                                    error.empty() ? "stopped" : error));
    return rewound_to;
  }
  return rewound_to;
}

void LogStats(const char* when)
{
  const Match& match = TheMatch();
  if (!match.session)
    return;
  const Orca::Net::Stats& stats = match.session->GetStats();
  NOTICE_LOG_FMT(
      ROLLBACK,
      "Online match {}: frame {}, {} rollbacks, {} re-run frames, deepest {}, {} stalls ({} "
      "counted), {} waits, {} hitches here, {} there, frame advantage {:.2f}, input delay {} "
      "(+{} -{} reverted {}), {} checksums matched, confirmed frame {}, round trip {} ms, {} "
      "snapshots (every {}, {:.2f} ms each){}{}",
      when, match.session->CurrentFrame(), stats.rollbacks, stats.resimulated_frames,
      stats.deepest_rollback, stats.stalls, stats.counted_stalls, stats.waits, stats.hitches,
      stats.peer_hitches, stats.frame_advantage,
      stats.delay, stats.delay_raises, stats.delay_lowers, stats.delay_reverts,
      stats.checksums_matched, stats.confirmed_frame, Orca::Online::RoundTripMs(), stats.saves,
      stats.snapshot_every, match.port ? match.port->SaveMs() : 0.0,
      match.session->Error().empty() ? "" : "; ", match.session->Error());
}

bool Active()
{
  const Match& match = TheMatch();
  return match.started && !match.finished;
}

void End(const char* reason)
{
  Match& match = TheMatch();
  if (!match.started || match.finished)
    return;
  match.finished = true;
  LogStats(fmt::format("ended ({})", reason).c_str());
  match.session.reset();
  match.port.reset();
  match.download.reset();
  match.job.reset();
  if (match.keyframe && match.store)
    match.store->Delete(match.keyframe->id);
  Orca::Online::Shutdown();
}
}  // namespace Rollback::OnlineMatch
