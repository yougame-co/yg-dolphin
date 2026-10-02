// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/OnlineMatch.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <picojson.h>

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
#include "Core/Orca/Session/Events.h"
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
  // NAND bytes named by hash, not sent (the joiner's boot writes them).
  u64 referenced = 0;
  std::string error;
  ~KeyframeJob()
  {
    if (thread.joinable())
      thread.join();
  }
};

// The host's NAND file hashes at Profile::boot_nand_frame, read on a thread from a clone.
struct BootNandJob
{
  std::thread thread;
  std::atomic<bool> done{false};
  std::map<std::string, u64> hashes;
  // The boot's /tmp files themselves: a join later in the game resolves the host's references from
  // these (the game may have rewritten its own /tmp since).
  std::map<std::string, std::vector<u8>> tmp_files;
  ~BootNandJob()
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
  // Players who arrived and wait for a keyframe before the session takes them in, and the seats
  // the session took in (until the room says they left).
  std::map<int, std::string> waiting;
  std::set<int> seated;

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
  // The joiner runs its own boot to this frame before loading (Profile::boot_nand_frame).
  int boot_ahead_to = 0;
  std::string join_status;
  // Host: its NAND's file hashes at Profile::boot_nand_frame, which keyframes name instead of
  // sending.
  std::unique_ptr<BootNandJob> boot_nand;
  int last_percent = -1;

  u32 round_trip_sequence = 0;
  std::optional<Clock::time_point> last_return;
  Common::EventHook on_state_changed;

  // A join the app asked for during play ("join <code>"): this game plays on solo in the friend's
  // room until the keyframe is in.
  bool join_in_play = false;
  // Why the join failed (PumpJoin), for the caller to report.
  std::string join_error_code, join_error;

  // The "orca stats" line: the numbers at the last line, and the deepest rollback and the stall time
  // since.
  Clock::time_point stats_time{};
  int stats_frame = -1;
  Orca::Net::Stats stats_last{};
  int stats_rbmax = 0;
  double stats_stall_ms = 0;

  // Each port's player name and the frame it applies from (Orca::Events::PortInfo), decided by the
  // host: its own once its room roster has it, a friend's when the session takes it in. A joiner
  // takes the host's (they travel with the keyframe offer), so both games name every frame the
  // same. The ports the UI was last told of.
  std::map<int, Orca::Net::KeyframeInfo::Name> port_names;
  std::vector<Orca::Events::PortInfo> told_ports;
};

// Desyncs this process saw, every match (the stats line's "ds").
int s_desyncs = 0;

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
  std::map<std::string, u64> boot;
  if (match.boot_nand && match.boot_nand->done)
    boot = match.boot_nand->hashes;
  raw->thread = std::thread([raw, store, clone, image = std::move(image),
                             entries = std::move(entries), boot = std::move(boot)]() mutable {
    if (!clone.empty())
    {
      if (!Orca::Net::ReadNandTree(clone, &entries))
        raw->error = "reading the NAND clone failed";
      File::DeleteDirRecursively(clone);
    }
    // What the joiner's own boot writes travels as a hash: the boot's scratch files in /tmp only.
    // A save the host's boot made can depend on what the host pressed (Project+ writes Brawl's
    // saves before frame 600 when the player presses on), so saves always travel.
    for (Orca::Net::NandEntry& entry : entries)
    {
      if (entry.directory || !entry.path.starts_with("tmp/"))
        continue;
      const auto it = boot.find(entry.path);
      const u64 hash = Orca::Net::NandHash(entry.data);
      if (it == boot.end() || it->second != hash)
        continue;
      raw->referenced += entry.data.size();
      entry.reference = true;
      entry.hash = hash;
      entry.data.clear();
      entry.data.shrink_to_fit();
    }
    const auto pack_start = Clock::now();
    u64 raw_size = image.state.size() + image.mem1.size() + image.mem2.size();
    for (const auto& e : entries)
      raw_size += e.data.size();
    raw->raw_size = raw_size;
    std::vector<u8> blob =
        raw->error.empty() ? Orca::Net::PackKeyframe(raw->frame, image, entries) : std::vector<u8>{};
    if (raw->error.empty() && blob.empty())
      raw->error = "compression failed";
    // The store sees ciphertext only; the key goes to the joiner inside the room.
    if (raw->error.empty() && !Orca::Net::EncryptKeyframe(raw->frame, &blob, &raw->info.key))
      raw->error = "encryption failed";
    raw->pack_ms = MsSince(pack_start);
    if (raw->error.empty())
    {
      raw->info.frame = raw->frame;
      raw->info.size = blob.size();
      raw->info.hash = Orca::Net::KeyframeHash(blob);
      raw->info.id = fmt::format("kf-{}-{}", raw->frame, raw->info.hash.substr(0, 8));
      const auto put_start = Clock::now();
      raw->ok = store->Put(raw->info, blob, &raw->error);
      raw->put_ms = MsSince(put_start);
    }
    raw->done = true;
  });
  match.job = std::move(job);
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: keyframe of frame {} captured in {:.1f} ms", frame,
                 match.job->capture_ms);
}

// Host: at Profile::boot_nand_frame, note what the boot wrote to the NAND (a joiner's boot writes the
// same, so keyframes name those files instead of sending them).
void MaybeHashBootNand(Core::System& system, Match& match)
{
  const Orca::Profile* profile = Orca::ActiveProfile();
  if (!profile || !profile->boot_nand_frame || match.boot_nand ||
      match.running + 1 != static_cast<int>(*profile->boot_nand_frame))
  {
    return;
  }
  IOS::HLE::FS::HostFileSystem* nand = HostNand(system);
  if (!nand)
    return;
  auto job = std::make_unique<BootNandJob>();
  std::string clone;
#ifdef __APPLE__
  clone = File::GetUserPath(D_USER_IDX) + "OrcaBootNand";
  File::DeleteDirRecursively(clone);
  if (clonefile(nand->HostRoot().c_str(), clone.c_str(), CLONE_NOFOLLOW) != 0)
    clone.clear();
#endif
  // Without a clone, the live NAND is read on the thread: a file the game writes meanwhile hashes
  // wrong, and a keyframe then sends it whole (references only match identical bytes).
  BootNandJob* const raw = job.get();
  const std::string root = clone.empty() ? nand->HostRoot() : clone;
  raw->thread = std::thread([raw, root, cloned = !clone.empty()] {
    raw->hashes = Orca::Net::HashNandTree(root);
    std::vector<Orca::Net::NandEntry> entries;
    if (Orca::Net::ReadNandTree(root, &entries))
    {
      for (Orca::Net::NandEntry& entry : entries)
      {
        if (!entry.directory && entry.path.starts_with("tmp/"))
          raw->tmp_files[entry.path] = std::move(entry.data);
      }
    }
    if (cloned)
      File::DeleteDirRecursively(root);
    raw->done = true;
  });
  match.boot_nand = std::move(job);
}

// A solo frame: this player's pad on its port (port 1 for a host), every other port unplugged.
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
  pads[match.local_seat] = pad;
  const int frame = match.running + 1;
  match.port->SetPads(frame, pads);
  match.log.push_back(pads);
  match.running = frame;
}

// Host: unplug every friend still in the session, telling each (and the app) why.
void DropFriends(Match& match, const std::string& reason, bool announce = true)
{
  for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
  {
    if (seat == match.local_seat || match.session->Plan()[seat].live_from == Orca::Net::NEVER)
      continue;
    Orca::Online::DropPeer(seat, reason);
    match.session->DropSeat(seat);
  }
  match.seated.clear();
  if (announce)
    Orca::Status::State("friend-left " + reason);
}

void GoSolo(Match& match, const char* why, const char* state = "playing")
{
  match.log = match.session->TakeLog(&match.log_base);
  const std::vector<Pad> pending = match.session->PendingLocal();
  match.queued_local.assign(pending.begin(), pending.end());
  Orca::Net::Stats stats = match.session->GetStats();
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: back to solo play at frame {} ({}): {} rollbacks, {} checksums matched",
                 match.running + 1, why, stats.rollbacks, stats.checksums_matched);
  match.session.reset();
  match.port->SetCatchingUp(false);
  if (state)
    Orca::Status::State(state);
}

// With the app's "caps join", what used to end the game (a join that failed, the host leaving, a
// desync) leaves this player solo instead.
bool Soft()
{
  return Orca::Status::Cap("join");
}

std::unique_ptr<Orca::Net::KeyframeStore> MakeStore()
{
  return Orca::Net::MakeKeyframeStore(
      [](bool fresh, std::string* ticket, std::string* store_url, std::string* error) {
        return Orca::Online::FreshTicket(fresh, ticket, store_url, error);
      });
}

// Drops what drop-in had in flight for the room being left: the keyframe and its transfers, whoever
// waited for it. A store cancelled once stays cancelled: the next room gets a new one.
void ResetDropIn(Match& match)
{
  if (match.keyframe && match.store)
    match.store->Delete(match.keyframe->id);
  match.keyframe.reset();
  if (match.store)
    match.store->Cancel();
  match.job.reset();
  match.download.reset();
  match.waiting.clear();
  match.seated.clear();
  match.keyframe_wanted = false;
  match.store = MakeStore();
}

// The room once this player is solo again: a new one of its own while it plays port 1 (friends can
// drop in on the ports after it), none on another port (drop-in hosts play port 1). Either way the
// app can still send "join".
void ReopenRoom(Match& match)
{
  Orca::Online::Shutdown();
  if (match.local_seat == 0)
    Orca::Online::StartRoom("", false);
}

// A joining player on its own again (it left, its host did, the session failed, or the join never
// finished): it plays on from the state it has, every other controller unplugged. `own_boot`: no
// keyframe was ever loaded, so this is its own game, played on port 1.
void BecomeSolo(Match& match, const char* why, const char* state, bool own_boot = false)
{
  if (match.session)
  {
    GoSolo(match, why, nullptr);
  }
  else
  {
    match.log.clear();
    match.log_base = match.running + 1;
    match.queued_local.clear();
  }
  if (own_boot)
    match.local_seat = 0;
  match.joining = false;
  match.join_in_play = false;
  match.joined = false;
  match.host_here = false;
  match.leave_requested = false;
  match.leave_at = -1;
  match.last_percent = -1;
  match.port->SetCatchingUp(false);
  ResetDropIn(match);
  ReopenRoom(match);
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: playing solo on port {} from frame {} ({})",
                 match.local_seat + 1, match.running + 1, why);
  if (state)
    Orca::Status::State(state);
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
      const bool was_waiting = match.waiting.erase(event.seat) > 0;
      // Taken in (it may have unplugged already, and the host gone solo): the app still hears it.
      const bool was_playing = match.seated.erase(event.seat) > 0;
      if (match.session)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} left the room{}{}", event.seat + 1,
                       event.reason.empty() ? "" : ": ", event.reason);
        match.session->DropSeat(event.seat);
      }
      // The app hears why a friend went (its bye: "desync", "stalled"), as the friend's own does.
      if (was_waiting || was_playing)
        Orca::Status::State("friend-left " + (event.reason.empty() ? std::string("left") : event.reason));
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
      // Whoever waits for it gets another try.
      match.keyframe_wanted = !match.waiting.empty();
    }
    else
    {
      NOTICE_LOG_FMT(ROLLBACK,
                     "Drop-in: keyframe of frame {}: {:.1f} MB raw ({:.1f} MB of NAND named, not "
                     "sent), {:.1f} MB stored; capture {:.1f} ms, pack and compress {:.0f} ms, "
                     "store {:.0f} ms",
                     job->frame, job->raw_size / 1e6, job->referenced / 1e6, job->info.size / 1e6,
                     job->capture_ms,
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
      if (!match.job)
        match.keyframe_wanted = !match.waiting.empty();
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
    match.seated.insert(seat);
    match.port_names[seat] = {seat, next, it->second};
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {}", match.session->Describe());
    Orca::Net::KeyframeInfo offer = *match.keyframe;
    for (const auto& [named_seat, name] : match.port_names)
    {
      if (named_seat == match.local_seat || match.seated.count(named_seat))
        offer.names.push_back(name);
    }
    Orca::Online::OfferKeyframe(seat, offer);
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
    auto blob = store->Get(
        raw->info,
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
    std::vector<u8> plain = std::move(*blob);
    if (Orca::Net::KeyframeHash(plain) != raw->info.hash ||
        !Orca::Net::DecryptKeyframe(raw->info.frame, raw->info.key, &plain))
    {
      raw->error = "the keyframe arrived damaged";
      raw->done = true;
      return;
    }
    const auto unpack_start = Clock::now();
    raw->ok = Orca::Net::UnpackKeyframe(plain, &raw->frame, &raw->image, &raw->nand) &&
              raw->frame == raw->info.frame;
    raw->unpack_ms = MsSince(unpack_start);
    if (!raw->ok)
      raw->error = "the keyframe doesn't unpack";
    raw->done = true;
  });
  match.download = std::move(download);
}

enum class JoinPump
{
  Waiting,
  Ready,
  Failed,
};

// One look at the room for a joiner: the host's arrival and its keyframe offer, the download's
// progress. Never blocks. Ready once the keyframe is downloaded and unpacked; Failed stops
// emulation (the reason is shown).
JoinPump PumpJoin(Match& match, std::string* last_status)
{
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
        match.join_error_code = "peer_left";
        match.join_error = "Your friend left before you could join";
        return JoinPump::Failed;
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
      // The room's own code (room_full, room_mismatch, kicked, network) is reported by the caller.
      match.join_error_code.clear();
      match.join_error = fmt::format("Couldn't join: {}", Orca::Online::StatusLine());
      return JoinPump::Failed;
    }
    if (std::string status = Orca::Online::StatusLine(); status != *last_status)
    {
      NOTICE_LOG_FMT(NETPLAY, "Orca: {}", status);
      *last_status = std::move(status);
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
        return JoinPump::Ready;
    }
    if (Clock::now() - match.boot_time > JOIN_LIMIT)
    {
      match.join_error_code = "network";
      match.join_error = "Your friend's game never sent its state";
      return JoinPump::Failed;
    }
  }
  return JoinPump::Waiting;
}

// A join that failed before anything of this game changed. With the app's "caps join" this player
// plays on solo where it was; otherwise the game stops, as it always did.
void FailJoin(Core::System& system, Match& match)
{
  const std::string sentence =
      match.join_error.empty() ? std::string("Couldn't join your friend's game") : match.join_error;
  // An empty code: the room ended, and says why itself.
  std::string code = match.join_error_code;
  if (code.empty())
    code = Orca::Online::RoomErrorCode();
  Orca::Status::Report(code.empty() ? "network" : code, sentence);
  match.join_error_code.clear();
  match.join_error.clear();
  if (Soft())
  {
    Core::DisplayMessage(sentence, 6000);
    BecomeSolo(match, "the join failed", "playing", match.loaded_time == Clock::time_point{});
    return;
  }
  StopEmulation(system, sentence);
  End("couldn't join");
}

enum class Load
{
  Done,
  // Nothing of this game changed: FailJoin reports it.
  Failed,
  // Half loaded: the game stopped.
  Broken,
};

// Loads the host's downloaded keyframe and starts the joiner's session. On Done, `*frame` is the
// frame the state now starts.
Load LoadKeyframe(Core::System& system, Match& match, int* frame_out)
{
  std::unique_ptr<Download> download = std::move(match.download);
  if (download->thread.joinable())
    download->thread.join();
  if (!download->ok)
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: keyframe {}: {}", download->info.id, download->error);
    match.join_error_code = "network";
    match.join_error = "Couldn't load your friend's game";
    return Load::Failed;
  }

  // The host's NAND, then its machine. The session NAND is a temporary folder of this session's.
  const auto load_start = Clock::now();
  IOS::HLE::FS::HostFileSystem* nand = HostNand(system);
  // Files the host's keyframe only names: this machine's boot wrote them too. Those this boot
  // kept (a join later in the game, after the game rewrote its /tmp) first, then the NAND as it is.
  if (match.boot_nand && match.boot_nand->done)
  {
    for (Orca::Net::NandEntry& entry : download->nand)
    {
      if (!entry.reference)
        continue;
      const auto it = match.boot_nand->tmp_files.find(entry.path);
      if (it == match.boot_nand->tmp_files.end() || Orca::Net::NandHash(it->second) != entry.hash)
        continue;
      entry.data = it->second;
      entry.reference = false;
    }
  }
  std::string missing;
  if (nand && !Orca::Net::ResolveNandReferences(nand->HostRoot(), &download->nand, &missing))
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: this boot's NAND doesn't match the host's: {}", missing);
    match.join_error_code = "room_mismatch";
    match.join_error = "Your friend's game data differs from yours";
    return Load::Failed;
  }
  // A join during play: this game's own snapshots, and the NAND journal they undo with, are of
  // another game. A new ring (the old one's reset stops the journal) before the NAND changes under it.
  match.port.reset();
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
  if (!nand || !Core::WiiRootIsTemporary() ||
      !Orca::Net::ReplaceNandTree(nand->HostRoot(), download->nand))
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: replacing the NAND failed (nand {}, temporary {})",
                  nand != nullptr, Core::WiiRootIsTemporary());
    StopEmulation(system, "Couldn't join: the game's save folder couldn't be replaced");
    Orca::Status::Error("internal", "Couldn't load your friend's game");
    End("couldn't join");
    return Load::Broken;
  }
  nand->ReloadFst();
  const int frame = download->frame;
  if (!match.port->LoadImage(std::move(download->image), frame))
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: loading keyframe frame {} into the ring failed", frame);
    StopEmulation(system, "Couldn't join: your friend's game state didn't load");
    Orca::Status::Error("internal", "Couldn't load your friend's game");
    End("couldn't join");
    return Load::Broken;
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

  // A join during play takes its seat from the friend's room now (a launch join had it at once).
  match.local_seat = Orca::Online::Seat();
  match.port_names.clear();
  for (const Orca::Net::KeyframeInfo::Name& name : download->info.names)
    match.port_names[name.seat] = name;
  for (const auto& [seat, name] : match.port_names)
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} is {} from frame {}", seat + 1, name.name, name.from);
  match.join_in_play = false;
  match.log.clear();
  match.log_base = frame;
  match.queued_local.clear();
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
  *frame_out = frame;
  return Load::Done;
}

// A launch join (ORCA_JOIN, or an invite the app opened): blocks at the joiner's boundary until the
// host's keyframe is in. The frame the state now starts, or nullopt: the join failed (FailJoin
// reported it; with "caps join" this player now plays solo) or emulation is stopping.
std::optional<int> JoinHost(Core::System& system, Match& match)
{
  JoinPump pump = JoinPump::Waiting;
  while (!Stopping(system))
  {
    pump = PumpJoin(match, &match.join_status);
    if (pump != JoinPump::Waiting)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (pump == JoinPump::Waiting)
    return std::nullopt;
  if (pump == JoinPump::Failed)
  {
    FailJoin(system, match);
    return std::nullopt;
  }
  int frame = -1;
  switch (LoadKeyframe(system, match, &frame))
  {
  case Load::Done:
    return frame;
  case Load::Failed:
    FailJoin(system, match);
    return std::nullopt;
  case Load::Broken:
    return std::nullopt;
  }
  return std::nullopt;
}

// ---- The app's commands (embed mode, with "caps join") ----

// "leave": unplug from friends and play on solo. A joiner asks its host to unplug it at an agreed
// frame ("left" once it has); a host leaves its room (its friends play on without it) and opens a
// new one, so it can still invite.
void Leave(Match& match)
{
  if (match.joining && match.session)
  {
    if (!match.leave_requested)
    {
      match.leave_requested = true;
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: leaving at the app's request, frame {}", match.running);
      match.session->RequestLeave();
    }
    return;
  }
  if (match.joining)
  {
    // Still downloading the host's game: give up on it.
    BecomeSolo(match, "left before joining", nullptr);
  }
  else if (match.session || !match.waiting.empty())
  {
    if (match.session)
      GoSolo(match, "left", nullptr);
    ResetDropIn(match);
    ReopenRoom(match);
  }
  Orca::Status::Event("left");
}

// "join <code>": into a friend's game during play. This game leaves its room (friends in it play
// on without it) and goes on solo while the friend's keyframe downloads; then it loads it.
void JoinInPlay(Match& match, const std::string& code)
{
  if (code == Orca::Online::Code() && match.joining)
    return;
  if (match.session)
    GoSolo(match, "joining another game", nullptr);
  ResetDropIn(match);
  Orca::Online::Shutdown();
  Orca::Online::StartRoom(code, true);
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: joining room {} during play, frame {}", code, match.running + 1);
  match.joining = true;
  match.join_in_play = true;
  match.boot_time = Clock::now();
  match.offered_time = match.boot_time;
  match.host_here = false;
  match.joined = false;
  match.leave_requested = false;
  match.last_percent = -1;
  match.join_status.clear();
  Orca::Status::State("joining 0");
}

// Takes the app's commands at a boundary where the game may change hands: never mid re-run, never
// during a launch join.
void TakeCommands(Match& match)
{
  if (!Soft())
    return;
  if (match.session &&
      (match.session->Resimulating() || match.session->CurrentFrame() != match.running + 1))
  {
    return;
  }
  if (match.joining && !match.join_in_play && !match.session)
    return;
  if (Orca::Online::TakeLeaveRequest())
    Leave(match);
  if (const auto code = Orca::Online::TakeJoinRequest())
    JoinInPlay(match, *code);
}

// ---- Ports, for the in-game UX ----

std::string NameAt(const Match& match, int seat, int frame)
{
  const auto it = match.port_names.find(seat);
  return it != match.port_names.end() && frame >= it->second.from ? it->second.name : "";
}

// The controllers plugged in at `frame`: in a session, by its synced plan; solo, this player's own.
// Names as the host decided them (Match::port_names).
std::vector<Orca::Events::PortInfo> PortsAt(Match& match, int frame)
{
  // A host names its own port once its roster has it, from this frame on (a joiner learns when
  // from the keyframe offer). A joiner's names, its own included, all come from its host.
  if (!match.joining && !match.port_names.count(match.local_seat) &&
      Orca::Online::Seat() == match.local_seat)
  {
    if (std::string own = Orca::Online::SeatName(match.local_seat); !own.empty())
      match.port_names[match.local_seat] = {match.local_seat, frame, std::move(own)};
  }
  std::vector<Orca::Events::PortInfo> ports;
  if (!match.session)
  {
    ports.push_back({match.local_seat, NameAt(match, match.local_seat, frame), false});
    return ports;
  }
  for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
  {
    if (match.session->Plugged(seat, frame))
      ports.push_back({seat, NameAt(match, seat, frame), seat != match.local_seat});
  }
  return ports;
}

// ---- Stats ----

// The numbers for the app ("orca stats", Orca::Events), about once a second; the link stats each
// boundary.
void PublishStats(Match& match)
{
  if (match.session && match.session->Resimulating())
    return;
  const Orca::Net::Stats stats = match.session ? match.session->GetStats() : Orca::Net::Stats{};
  int peers = 0;
  if (match.session)
  {
    for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
    {
      if (seat != match.local_seat && match.session->Plugged(seat, match.running))
        ++peers;
    }
  }
  Orca::Events::LinkStats link;
  link.online = peers > 0;
  link.delay = stats.delay;
  link.deepest_rollback = match.stats_rbmax;
  link.stalls = stats.stalls;
  link.hitches = stats.hitches;
  link.peer_hitches = stats.peer_hitches;
  link.round_trip_ms = Orca::Online::RoundTripMs();
  link.confirmed_frame = stats.confirmed_frame;

  const auto now = Clock::now();
  if (match.stats_frame < 0)
  {
    match.stats_time = now;
    match.stats_frame = match.running;
    match.stats_last = stats;
  }
  const double seconds = std::chrono::duration<double>(now - match.stats_time).count();
  // A new session restarts its counters.
  const bool reset = stats.rollbacks < match.stats_last.rollbacks ||
                     stats.stalls < match.stats_last.stalls;
  const Orca::Net::Stats& last = reset ? Orca::Net::Stats{} : match.stats_last;
  if (seconds > 0)
    link.rollbacks_per_second = (stats.rollbacks - last.rollbacks) / seconds;
  Orca::Events::PublishLinkStats(link);
  if (seconds < 1.0)
    return;
  picojson::object o;
  const auto num = [&o](const char* key, double value) { o[key] = picojson::value(value); };
  num("f", match.running);
  num("seat", match.local_seat);
  num("peers", peers);
  num("ping", link.round_trip_ms);
  num("delay", stats.delay);
  num("rb", stats.rollbacks - last.rollbacks);
  num("rbf", stats.resimulated_frames - last.resimulated_frames);
  num("rbmax", match.stats_rbmax);
  num("st", stats.stalls - last.stalls);
  num("stms", std::round(match.stats_stall_ms));
  num("ds", s_desyncs);
  num("fps", std::round((match.running - match.stats_frame) / seconds * 10) / 10);
  num("hi", stats.hitches);
  num("phi", stats.peer_hitches);
  Orca::Status::Stats(picojson::value(o).serialize());
  match.stats_time = now;
  match.stats_frame = match.running;
  match.stats_last = stats;
  match.stats_rbmax = 0;
  match.stats_stall_ms = 0;
}

// While the session waits on a friend inside StepSession: how long, for the overlay.
void PublishStall(Match& match, double stalled_ms)
{
  Orca::Events::LinkStats link = Orca::Events::GetLinkStats();
  link.stalled_ms = static_cast<int>(stalled_ms);
  link.waiting_seat = -1;
  if (stalled_ms > 0)
  {
    if (match.joining)
      link.waiting_seat = match.host_seat;
    else if (!match.seated.empty())
      link.waiting_seat = *match.seated.begin();
  }
  Orca::Events::PublishLinkStats(link);
}
}  // namespace

std::optional<int> OnBoundary(Core::System& system,
                              const std::function<Orca::Net::Pad(int local_seat)>& local_pad,
                              const FrameHook& on_frame)
{
  Match& match = TheMatch();
  if (match.stale)
  {
    // A new boot in the same process (the room was left when the last one stopped).
    if (match.store)
      match.store->Cancel();
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
    match.store = MakeStore();
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
      // Until the keyframe is in, this machine runs its own boot, unthrottled and unseen, as far
      // as the profile's boot_nand_frame: the NAND files that boot writes needn't travel.
      match.boot_time = Clock::now();
      Orca::Status::State("joining 0");
      match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
      match.port->SetCatchingUp(true);
      if (const Orca::Profile* profile = Orca::ActiveProfile(); profile && profile->boot_nand_frame)
        match.boot_ahead_to = static_cast<int>(*profile->boot_nand_frame);
    }
    else
    {
      match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
      Orca::Status::State("playing");
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: playing solo on port 1; room {}", Orca::Online::StatusLine());
    }
  }

  // The in-game UX's frame hook: before this boundary's snapshot or keyframe, so what it writes is
  // in them, and on re-runs too.
  {
    const int frame = match.running + 1;
    const bool resimulating = match.session && match.session->Resimulating();
    const std::vector<Orca::Events::PortInfo> ports = PortsAt(match, frame);
    on_frame(frame, resimulating, ports);
    if (!resimulating && ports != match.told_ports)
    {
      match.told_ports = ports;
      Orca::Events::NotifyPlugIn(frame, ports);
    }
  }

  TakeCommands(match);

  if (match.joining && !match.session && match.join_in_play)
  {
    // A join during play: this game goes on solo until the friend's keyframe is in.
    int frame = -1;
    switch (PumpJoin(match, &match.join_status))
    {
    case JoinPump::Waiting:
      RunSolo(match, local_pad);
      PublishStats(match);
      return std::nullopt;
    case JoinPump::Failed:
      FailJoin(system, match);
      break;
    case JoinPump::Ready:
      switch (LoadKeyframe(system, match, &frame))
      {
      case Load::Done:
        rewound_to = frame;
        break;
      case Load::Failed:
        FailJoin(system, match);
        break;
      case Load::Broken:
        return std::nullopt;
      }
      break;
    }
    if (match.finished)
      return std::nullopt;
  }
  else if (match.joining && !match.session)
  {
    if (match.running + 1 < match.boot_ahead_to)
    {
      if (PumpJoin(match, &match.join_status) == JoinPump::Failed)
      {
        FailJoin(system, match);
        if (match.finished)
          return std::nullopt;
      }
      else
      {
        Pads pads;
        pads.fill(Orca::Net::UNPLUGGED_PAD);
        pads[0] = Pad{};
        match.port->SetPads(match.running + 1, pads);
        ++match.running;
        return std::nullopt;
      }
    }
    else
    {
      rewound_to = JoinHost(system, match);
      // Failed (with "caps join" this player now plays solo), or stopping.
      if (!rewound_to && (match.joining || match.finished || Stopping(system)))
        return std::nullopt;
    }
  }

  if (!match.joining)
  {
    MaybeHashBootNand(system, match);
    if (!Orca::Online::RoomEnded())
      HostEvents(system, match);
    if (!match.session)
    {
      RunSolo(match, local_pad);
      PublishStats(match);
      return rewound_to;
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

  // A joiner whose host left: the game it joined is gone. The host's bye says whether their games
  // went apart.
  if (match.joining)
  {
    for (const Orca::Net::PeerEvent& event : Orca::Online::TakePeerEvents())
    {
      if (event.kind != Orca::Net::PeerEvent::Kind::Left || !event.host)
        continue;
      LogStats("host left");
      if (event.reason == "desync")
        ++s_desyncs;
      if (Soft())
      {
        // The host's game is gone, this one goes on: every other controller unplugs.
        if (event.reason == "desync")
          Orca::Status::Report("desync", "Your games went out of sync, so the match ended");
        Core::DisplayMessage(fmt::format("{} left: you play on", match.host_name), 6000);
        BecomeSolo(match, "host left", "host-left");
        RunSolo(match, local_pad);
        return rewound_to;
      }
      if (event.reason == "desync")
        Orca::Status::Error("desync", "Your games went out of sync, so the match ended");
      else
        Orca::Status::Error("peer_left", "Your friend's game ended");
      End("host left");
      StopEmulation(system, event.reason == "desync" ?
                                "Your games went out of sync, so the match ended" :
                                "Your friend's game ended");
      return rewound_to;
    }
  }

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
    // A stall (waiting on a friend's input) is shown as it lasts: StepSession polls `stopping`
    // while it waits.
    const auto step_start = Clock::now();
    auto stall_published = step_start;
    bool stall_shown = false;
    const int stalls_before = match.session->GetStats().stalls;
    const int running_before = match.running;
    const Orca::Net::Step step = StepSession(
        *match.session, *match.port, match.running, local_pad(match.local_seat),
        [&] {
          const auto now = Clock::now();
          if (now - stall_published > std::chrono::milliseconds(10))
          {
            stall_published = now;
            stall_shown = true;
            PublishStall(match, MsSince(step_start));
            Orca::Events::NotifyStall(static_cast<int>(MsSince(step_start)));
          }
          return Stopping(system);
        },
        std::chrono::milliseconds{1}, PEER_SILENCE_LIMIT, &peer_silent);
    match.last_return = Clock::now();
    if (stall_shown)
      PublishStall(match, 0);
    if (match.session && match.session->GetStats().stalls > stalls_before)
      match.stats_stall_ms += MsSince(step_start);
    if (step.kind == Orca::Net::StepKind::Rollback)
      match.stats_rbmax = std::max(match.stats_rbmax, running_before - step.frame + 1);
    if (step.kind == Orca::Net::StepKind::Run || step.kind == Orca::Net::StepKind::Rollback)
    {
      match.running = step.frame;
      PublishStats(match);
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
          if (Soft())
          {
            Core::DisplayMessage(fmt::format("You left {}'s game", match.host_name), 4000);
            BecomeSolo(match, "left", nullptr);
            Orca::Status::Event("left");
            if (step.kind == Orca::Net::StepKind::Rollback)
              return step.frame;
            return rewound_to;
          }
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
        DropFriends(match, "stalled");
        continue;
      }
      if (Orca::Online::IsDesync(error))
        ++s_desyncs;
      DropFriends(match, Orca::Online::IsDesync(error) ? "desync" : "network");
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
    const bool survivable = Orca::Online::ReportSessionEnd(error);
    if (Orca::Online::IsDesync(error))
      ++s_desyncs;
    if (Soft() && survivable && match.joining)
    {
      // Unplugged from the host's game; this one goes on.
      Core::DisplayMessage(peer_silent ? std::string(PEER_SILENT_SENTENCE) :
                                         "The connection to your friend's game ended: you play on",
                           6000);
      BecomeSolo(match, "the session ended", "host-left");
      RunSolo(match, local_pad);
      return rewound_to;
    }
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
  // Transfers in flight give up now: their threads are joined below.
  if (match.store)
    match.store->Cancel();
  match.session.reset();
  match.port.reset();
  match.download.reset();
  match.job.reset();
  if (match.keyframe && match.store)
    match.store->Delete(match.keyframe->id);
  Orca::Online::Shutdown();
}
}  // namespace Rollback::OnlineMatch
