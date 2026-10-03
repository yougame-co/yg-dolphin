// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/OnlineMatch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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
#include "Common/StringUtil.h"
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
#include "VideoCommon/OnScreenDisplay.h"

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
  // Seats taken in whose controller hasn't plugged in yet: the app hears "friend-joined" once the
  // last of them has.
  std::set<int> plugging;
  // Host: its room ended without this player leaving it (a dropped connection, a server gone quiet,
  // someone else hosting the code). Said once, then another room opens after a backoff that grows
  // until one works; `home_code` is the code of the last room that did.
  bool room_lost = false;
  Clock::time_point room_lost_at{}, reopen_at{};
  std::chrono::seconds reopen_backoff{0};
  std::string home_code;
  // A new room of its own, opened a moment after a leave (ReopenRoom): a quit that follows the
  // leave at once (the app's Stop) never asks for its ticket.
  std::optional<Clock::time_point> reopen_pending;
  // A player on another port than 1 (it joined a friend's game, then went solo) has no room: the
  // app hears so once, at the next boundary (after whatever the leave itself printed).
  bool tell_no_room = false;

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
  // The local controller's input age totals at the last line (Orca::Events::InputAge).
  Orca::Events::InputAge stats_input{};

  // Each port's player name and the frame it applies from (Orca::Events::PortInfo), decided by the
  // host: its own once its room roster has it, a friend's when the session takes it in. A joiner
  // takes the host's (they travel with the keyframe offer), so both games name every frame the
  // same. The ports the UI was last told of.
  // Per seat, who held it from which frame (a few entries: a frame re-run after a player left, or
  // after another took the seat, still gets the name it had).
  std::map<int, std::vector<Orca::Net::KeyframeInfo::Name>> port_names;
  std::vector<Orca::Events::PortInfo> told_ports;
  // A joiner's host left during a re-run: handled at the next boundary that isn't one.
  std::optional<std::string> host_gone;
};

// Desyncs this process saw, every match (the stats line's "ds").
int s_desyncs = 0;

// The solo pause (BeginPause). s_solo_idle: the last boundary left this game alone with nothing of
// drop-in under way (false while a boundary runs). s_pausing: a pause went ahead and hasn't ended;
// boundaries take nobody in meanwhile. Sequentially consistent: a pause either sees a boundary
// that has begun (and is refused) or that boundary sees the pause.
std::atomic<bool> s_solo_idle{false};
std::atomic<bool> s_pausing{false};
// Host time paused, for the stats line (TakePausedTime). The start is the host thread's (pause and
// resume both come through Core::SetState there); 0 while not paused.
std::atomic<s64> s_paused_since_ns{0};
std::atomic<s64> s_paused_ns{0};

s64 NowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

void AddName(Match& match, Orca::Net::KeyframeInfo::Name name);
std::vector<Orca::Net::KeyframeInfo::Name> AllNames(const Match& match);
void SetNames(Match& match, const std::vector<Orca::Net::KeyframeInfo::Name>& names);
std::vector<Orca::Events::PortInfo> PortsAt(Match& match, int frame);
void Leave(Match& match);

Match& TheMatch()
{
  static Match match;
  return match;
}

// This game plays alone, and nothing of drop-in is under way (SoloPauseAllowed).
bool SoloIdle(const Match& match)
{
  return match.started && !match.finished && match.port && !match.session && !match.joining &&
         !match.download && !match.job && !match.keyframe_wanted && match.waiting.empty() &&
         match.plugging.empty() && !match.port->CatchingUp();
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
// No keyframe of a boot's first frames: Project+'s first frames after its launcher hands over (3-9)
// run differently on a machine whose JIT holds other blocks (a synctest that drops the JIT after
// every load differs there, in CoreTiming ticks and RAM; Brawl over frames 0-600 does not), and a
// joiner's JIT always does. A friend who arrives earlier waits until the host's frame 300 (5 s).
constexpr int FIRST_KEYFRAME_FRAME = 300;
// A joiner far behind the host runs unthrottled.
constexpr int CATCH_UP_BEHIND = 12;
// How long a joiner waits for the host's keyframe before it gives up.
constexpr std::chrono::seconds JOIN_LIMIT{90};
// How long the rooms Worker holds the seat of a player whose connection dropped (AWAY_MS).
constexpr std::chrono::seconds ROOM_HOLD{90};

// The input delay the player chose ("delay N" from the app, with "caps delay"), or nullopt for the
// adaptive one.
std::optional<int> ChosenDelay()
{
  return Orca::Status::Cap("delay") ? Orca::Online::ChosenDelay() : std::nullopt;
}

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

// A new session takes the room's latest round trips at once, so its input delay covers the link
// from the first frame; later ones come one at a time (NewRoundTrip).
void SeedRoundTrips(Match& match)
{
  for (const int round_trip_ms : Orca::Online::RecentRoundTrips(&match.round_trip_sequence))
    match.session->OnRoundTrip(round_trip_ms);
}

// A joining player's one line about its join: each replaces the last, and the join's end takes it
// away (a "Joining..." line that waits as long as the join may take must not outlast it).
void ShowJoinMessage(std::string message, u32 ms)
{
  if (std::ranges::all_of(message, Common::IsPrintableCharacter))
    OSD::AddTypedMessage(OSD::MessageType::OrcaJoin, std::move(message), ms);
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
  // So is the input delay: the one the player chose, from the session's first frame.
  config.fixed_delay = ChosenDelay();
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
    // With the game's open files flushed and closed: a write still in a stdio buffer isn't on disk
    // for the clone to see (the joiner would get a short file, or a hole).
    nand->CloseHostFiles();
    if (clonefile(nand->HostRoot().c_str(), clone.c_str(), CLONE_NOFOLLOW) != 0)
      clone.clear();
    nand->ReopenHostFiles();
  }
#endif
  if (nand && clone.empty())
  {
    // With the game's open files flushed and closed: Windows can't read them while they're open.
    nand->CloseHostFiles();
    nand_ok = Orca::Net::ReadNandTree(nand->HostRoot(), &entries);
    nand->ReopenHostFiles();
  }
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
  // Flushed and closed first, as for a keyframe's clone: the hashes must be of this boundary's
  // bytes.
  nand->CloseHostFiles();
  if (clonefile(nand->HostRoot().c_str(), clone.c_str(), CLONE_NOFOLLOW) != 0)
    clone.clear();
  nand->ReopenHostFiles();
#endif
  if (clone.empty())
  {
    // No copy-on-write clone: a plain copy now, with the game's open files flushed and closed
    // (Windows can't read them while they're open), so the thread hashes this boundary's NAND.
    const auto copy_start = Clock::now();
    clone = File::GetUserPath(D_USER_IDX) + "OrcaBootNand";
    File::DeleteDirRecursively(clone);
    nand->CloseHostFiles();
    const bool copied = File::Copy(nand->HostRoot(), clone);
    nand->ReopenHostFiles();
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: boot NAND copied for hashing in {:.1f} ms ({})",
                   MsSince(copy_start), copied ? "ok" : "failed");
    if (!copied)
    {
      File::DeleteDirRecursively(clone);
      clone.clear();
    }
  }
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
    pad = local_pad(match.local_seat);
  }
  Pads pads;
  pads.fill(Orca::Net::UNPLUGGED_PAD);
  pads[match.local_seat] = pad;
  const int frame = match.running + 1;
  match.port->SetPads(frame, pads);
  match.log.push_back(pads);
  match.running = frame;
  // With no keyframe in play (none stored, none being made), only the newest frames can serve the
  // next one: keep the log from growing without end (a solo game with no room makes none).
  if (!match.keyframe && !match.job && match.log.size() > 4 * KEYFRAME_FRESH_FRAMES)
  {
    const int drop = static_cast<int>(match.log.size()) - KEYFRAME_FRESH_FRAMES;
    match.log.erase(match.log.begin(), match.log.begin() + drop);
    match.log_base += drop;
  }
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
  match.plugging.clear();
  if (announce)
    Orca::Status::State("friend-left " + reason);
}

void GoSolo(Match& match, const char* why, const char* state = "playing")
{
  // The frames the UX's readers saw with friends ran on guessed inputs too: they start over.
  Orca::Events::NoteResync();
  match.log = match.session->TakeLog(&match.log_base);
  const std::vector<Pad> pending = match.session->PendingLocal();
  match.queued_local.assign(pending.begin(), pending.end());
  Orca::Net::Stats stats = match.session->GetStats();
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: back to solo play at frame {} ({}): {} rollbacks, {} checksums matched",
                 match.running + 1, why, stats.rollbacks, stats.checksums_matched);
  match.session.reset();
  match.plugging.clear();
  match.port->SetCatchingUp(false);
  // A session that ended mid re-run left the port in re-run mode (unthrottled, unrendered, muted).
  match.port->SetResimulating(false);
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
  match.waiting.clear();
  match.seated.clear();
  match.plugging.clear();
  match.keyframe_wanted = false;
  // A keyframe being packed, a download, and the old store's transfers (a delete retries for
  // seconds) wind down off this thread, which never waits for them: the job and the download first,
  // since both use the store.
  std::thread([job = std::move(match.job), download = std::move(match.download),
               store = std::move(match.store)]() mutable {
    job.reset();
    download.reset();
    store.reset();
  }).detach();
  match.store = MakeStore();
}

// A room this player left on purpose (or for a friend's): whatever KeepRoomOpen knew of the old one
// is over.
void ForgetRoom(Match& match)
{
  match.room_lost = false;
  match.reopen_backoff = {};
  match.home_code.clear();
  match.reopen_pending.reset();
  match.tell_no_room = false;
}

// How long after a leave the new room opens (KeepRoomOpen): the app's Stop sends "leave" and then
// "quit" at once, and the quit should find no ticket request under way.
constexpr std::chrono::seconds REOPEN_DELAY{1};

// The room once this player is solo again: a new one of its own while it plays port 1 (friends can
// drop in on the ports after it), none on another port (drop-in hosts play port 1: "no-room").
// Either way the app can still send "join".
void ReopenRoom(Match& match)
{
  ForgetRoom(match);
  Orca::Online::ShutdownInBackground();
  if (match.local_seat == 0)
    match.reopen_pending = Clock::now() + REOPEN_DELAY;
  else
    match.tell_no_room = true;
}

// Host (port 1, not joining), at each boundary: its room ended without this player leaving it (the
// connection dropped or went quiet, the server restarted, someone else was hosting the code). Its
// friends can't drop in until it has another, so it says so once (soft, with "caps join": the game
// plays on) and reopens, 2 s later and then backing off to 30 s between tries, until one works.
// After a dropped connection the same code comes first, while the server holds the seat (90 s): the
// app's player comes back into it, and invites already sent still lead to this game. Anything else
// (someone else holds the room, or the hold is over) gets a fresh code.
void KeepRoomOpen(Match& match)
{
  if (match.local_seat != 0)
    return;
  // The room ReopenRoom asked for, once its moment comes.
  if (match.reopen_pending)
  {
    if (Clock::now() < *match.reopen_pending)
      return;
    match.reopen_pending.reset();
    Orca::Online::StartRoom("", false);
    return;
  }
  if (!Orca::Online::RoomEnded())
  {
    if (Orca::Online::InRoom())
    {
      if (match.room_lost)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: hosting room {} again", Orca::Online::Code());
        // The app's line about the lost room is over (the page keeps a soft error as the run's
        // last word until another line comes).
        if (Soft())
          Orca::Status::Event("playing");
      }
      match.room_lost = false;
      match.reopen_backoff = {};
      match.home_code = Orca::Online::Code();
    }
    return;
  }
  // A session ends with its room (it fails at its next step, and the host goes solo): from then on.
  if (match.session)
    return;
  // No code: this player left the room itself (the app's Leave, from an app without "leave").
  const std::string code = Orca::Online::RoomErrorCode();
  if (code.empty())
    return;
  const auto now = Clock::now();
  // Someone else holds that room (a friend hosting it, or another run of this game on the same
  // account): never back into it, whatever ends the tries after this one.
  if (Orca::Online::RoomTaken())
    match.home_code.clear();
  if (!match.room_lost)
  {
    match.room_lost = true;
    match.room_lost_at = now;
    match.reopen_backoff = std::chrono::seconds(2);
    match.reopen_at = now + match.reopen_backoff;
    const std::string why = Orca::Online::StatusLine();
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: this game's room ended ({}: {}); another opens in {} s",
                   code, why, match.reopen_backoff.count());
    Core::DisplayMessage(why, 6000);
    if (Soft())
    {
      Orca::Status::Report("network", Orca::Online::RoomTaken() ?
                                          why :
                                          fmt::format("{}: your game opens its room again", why));
    }
    // Whoever waited for a keyframe was in that room.
    ResetDropIn(match);
  }
  if (now < match.reopen_at)
    return;
  const bool held = now - match.room_lost_at < ROOM_HOLD;
  const std::string reopen = held ? match.home_code : std::string();
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: reopening {} (the last ended: {}: {})",
                 reopen.empty() ? std::string("a new room") : "room " + reopen, code,
                 Orca::Online::StatusLine());
  Orca::Online::ShutdownInBackground();
  Orca::Online::StartRoom(reopen, false);
  // The next try, if this one fails too.
  match.reopen_backoff = std::min(match.reopen_backoff * 2, std::chrono::seconds(30));
  match.reopen_at = now + match.reopen_backoff;
}

// A joining player on its own again (it left, its host did, the session failed, or the join never
// finished): it plays on from the state it has, every other controller unplugged. `own_boot`: no
// keyframe was ever loaded, so this is its own game, played on port 1.
void BecomeSolo(Match& match, const char* why, const char* state, bool own_boot = false)
{
  Orca::Events::NoteResync();
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
  // Whatever the join last said ("Joining...") is over.
  OSD::DiscardTypedMessage(OSD::MessageType::OrcaJoin);
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
      match.plugging.erase(event.seat);
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
  bool capture_failed = false;
  if (match.keyframe_wanted && !match.job && match.store)
  {
    if (fresh)
      match.keyframe_wanted = false;
    else if (next >= FIRST_KEYFRAME_FRAME && (!match.session || match.session->Settled()))
    {
      match.keyframe_wanted = false;
      StartKeyframe(system, match, next);
      if (!match.job)
      {
        capture_failed = true;
        match.keyframe_wanted = !match.waiting.empty();
      }
    }
  }
  if (!match.store && !match.waiting.empty())
    WARN_LOG_FMT(ROLLBACK, "Drop-in: no keyframe store: a friend can't join yet");

  // Take in whoever waits once there is a keyframe for them. Not while a solo pause winds the CPU
  // thread down (BeginPause): the session's first step would give up half done.
  // Solo, nor on the stale keyframe a capture that failed here would have replaced: a friend who
  // replayed from it would replay frames the frame hook already told the app were this player's
  // alone (AloneAt). The capture is tried again at the next boundary. (A keyframe that went stale
  // while it was packed and stored is fine: no frame since its capture was alone, SoloIdle.)
  if (!match.keyframe || match.waiting.empty() || match.job || s_pausing ||
      (capture_failed && !match.session))
  {
    return;
  }
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
      SeedRoundTrips(match);
      match.session->SetLog(match.log_base, std::move(match.log));
      match.log.clear();
      // Pads sampled while solo carry no delay; from here they do, and the frames the delay skips
      // repeat the pad held now (Config::start_pad).
      match.queued_local.clear();
    }
    match.session->AddPeer(seat, next, keyframe_frame);
    match.seated.insert(seat);
    match.plugging.insert(seat);
    AddName(match, {seat, next, it->second});
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {}", match.session->Describe());
    Orca::Net::KeyframeInfo offer = *match.keyframe;
    offer.names = AllNames(match);
    // Players already in the game hear the newcomer's name before its plug frame (the room sends it
    // ahead of the session packets that plug it in).
    Orca::Online::BroadcastNames(offer.names);
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
        ShowJoinMessage(fmt::format("Joining {}'s game...", match.host_name),
                        static_cast<u32>(std::chrono::milliseconds(JOIN_LIMIT).count()));
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
      // Only ever forward: a join's percent never goes back.
      if (percent > match.last_percent)
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
  // The NAND is read and then replaced by path: no file of it may stay open (Windows can't read
  // or delete an open one). The keyframe's state reopens what it has open.
  if (nand)
    nand->CloseHostFiles();
  std::string missing;
  if (nand && !Orca::Net::ResolveNandReferences(nand->HostRoot(), &download->nand, &missing))
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: this boot's NAND doesn't match the host's: {}", missing);
    // This game plays on solo, with its own files.
    nand->ReopenHostFiles();
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
  // This machine's game is now the host's: the UX's readers start over from it.
  Orca::Events::NoteResync();
  if (match.store)
    match.store->Delete(download->info.id);
  NOTICE_LOG_FMT(ROLLBACK,
                 "Drop-in: loaded keyframe frame {}: waited {:.0f} ms for it, transfer {:.0f} ms, "
                 "unpack {:.0f} ms, load {:.0f} ms",
                 frame, std::chrono::duration<double, std::milli>(match.offered_time - match.boot_time).count(),
                 download->transfer_ms, download->unpack_ms, match.load_ms);

  // A join during play takes its seat from the friend's room now (a launch join had it at once).
  match.local_seat = Orca::Online::Seat();
  SetNames(match, download->info.names);
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
  SeedRoundTrips(match);
  match.port->SetCatchingUp(true);
  match.running = frame - 1;
  // The stats line's frame rate starts over: the frames up to the keyframe were never run here.
  match.stats_frame = -1;
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
    // The app's "leave" and "join" (TakeCommands doesn't run during a launch join): nothing of this
    // game changed yet, so it can give up, or wait in another friend's room instead.
    if (Orca::Status::Cap("leave") && Orca::Online::TakeLeaveRequest())
    {
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: left before joining, at the app's request");
      Leave(match);
      return std::nullopt;
    }
    if (Orca::Status::Cap("join"))
    {
      if (const auto code = Orca::Online::TakeJoinRequest(); code && *code != Orca::Online::Code())
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: joining room {} instead", *code);
        OSD::DiscardTypedMessage(OSD::MessageType::OrcaJoin);
        ResetDropIn(match);
        Orca::Online::ShutdownInBackground();
        Orca::Online::StartRoom(*code, true);
        match.boot_time = Clock::now();
        match.offered_time = match.boot_time;
        match.host_here = false;
        match.last_percent = -1;
        match.join_status.clear();
        Orca::Status::State("joining 0");
      }
    }
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
  if (match.joining && match.session && match.session->CatchingUp())
  {
    // Still replaying the host's game: its controller never plugged in, so there is no unplug frame
    // to agree on (the host just lets a seat still catching up go). Leaving the room is the leave:
    // the host drops the seat when the room says it went.
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: left while catching up, frame {}", match.running);
    BecomeSolo(match, "left while catching up", nullptr);
    Orca::Status::Event("left");
    return;
  }
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
    // Still downloading the host's game: give up on it. A launch joiner never loaded a keyframe:
    // the game it plays on is its own boot, on port 1 (as FailJoin decides).
    BecomeSolo(match, "left before joining", nullptr, match.loaded_time == Clock::time_point{});
  }
  else
  {
    // A host leaves its room even with nobody in it: the page reads "left" as Orca having left
    // the room, and then shows the new one.
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
  ForgetRoom(match);
  Orca::Online::ShutdownInBackground();
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
  // Too early in this game's own boot to have written what the host's keyframe only names (the
  // profile's boot_nand_frame): run the boot there first, unthrottled and unseen, as a launch join
  // does, then wait for the keyframe.
  if (const Orca::Profile* profile = Orca::ActiveProfile();
      profile && profile->boot_nand_frame &&
      match.running + 1 < static_cast<int>(*profile->boot_nand_frame))
  {
    match.join_in_play = false;
    match.boot_ahead_to = static_cast<int>(*profile->boot_nand_frame);
    match.port->SetCatchingUp(true);
  }
  Orca::Status::State("joining 0");
}

// Takes the app's commands at a boundary where the game may change hands: never mid re-run, never
// during a launch join.
void TakeCommands(Match& match)
{
  // A command sent before the app's "caps" line waits here (queued in Online) until it arrives.
  if (!Orca::Status::Cap("join") && !Orca::Status::Cap("leave"))
    return;
  if (match.session &&
      (match.session->Resimulating() || match.session->CurrentFrame() != match.running + 1))
  {
    return;
  }
  if (match.joining && !match.join_in_play && !match.session)
    return;
  if (Orca::Status::Cap("leave") && Orca::Online::TakeLeaveRequest())
    Leave(match);
  if (Orca::Status::Cap("join"))
  {
    if (const auto code = Orca::Online::TakeJoinRequest())
      JoinInPlay(match, *code);
  }
}

// ---- Ports, for the in-game UX ----

// Host: a seat's new holder, from its plug frame. A few entries per seat are kept.
void AddName(Match& match, Orca::Net::KeyframeInfo::Name name)
{
  auto& history = match.port_names[name.seat];
  if (!history.empty() && history.back().from >= name.from)
    history.pop_back();
  history.push_back(std::move(name));
  if (history.size() > 4)
    history.erase(history.begin());
}

std::vector<Orca::Net::KeyframeInfo::Name> AllNames(const Match& match)
{
  std::vector<Orca::Net::KeyframeInfo::Name> all;
  for (const auto& [seat, history] : match.port_names)
    all.insert(all.end(), history.begin(), history.end());
  return all;
}

// A joiner: the host's names, as they are now (they replace this machine's own view).
void SetNames(Match& match, const std::vector<Orca::Net::KeyframeInfo::Name>& names)
{
  match.port_names.clear();
  std::vector<Orca::Net::KeyframeInfo::Name> sorted = names;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const auto& a, const auto& b) { return a.from < b.from; });
  for (auto& name : sorted)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} is {} from frame {}", name.seat + 1, name.name,
                   name.from);
    match.port_names[name.seat].push_back(std::move(name));
  }
}

std::string NameAt(const Match& match, int seat, int frame)
{
  const auto it = match.port_names.find(seat);
  if (it == match.port_names.end())
    return "";
  // The newest holder from before this frame (entries are in frame order).
  for (auto entry = it->second.rbegin(); entry != it->second.rend(); ++entry)
  {
    if (frame >= entry->from)
      return entry->name;
  }
  return "";
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
      AddName(match, {match.local_seat, frame, std::move(own)});
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
  const Orca::Net::DirectSummary direct = Orca::Online::Direct();
  link.transport = direct.transport;
  link.link_rtt_ms = direct.rtt_ms;

  const auto now = Clock::now();
  // Time paused is no part of the interval: fps counts running time.
  match.stats_time += TakePausedTime();
  if (match.stats_frame < 0)
  {
    match.stats_time = now;
    match.stats_frame = match.running;
    match.stats_last = stats;
    match.stats_input = Orca::Events::GetInputAge();
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
  num("tx", link.transport);
  num("lrtt", link.link_rtt_ms);
  // The local controller's input age over the interval (ControllerSource.h): the mean age of the
  // newest adapter report at the frames whose pad changed, less its hop from the app ("pa", ms),
  // and the adapter's reports per frame ("rpf"); -1 when no frame read an adapter. A restarted
  // stream counts from zero.
  const Orca::Events::InputAge input = Orca::Events::GetInputAge();
  const Orca::Events::InputAge& input_last =
      input.frames < match.stats_input.frames ? Orca::Events::InputAge{} : match.stats_input;
  const u64 input_changes = input.changes - input_last.changes;
  const u64 input_frames = input.frames - input_last.frames;
  num("pa", input_changes > 0 ?
                std::round((input.age_ms - input_last.age_ms) / input_changes * 10) / 10 :
                -1);
  num("rpf", input_frames > 0 ?
                 std::round(double(input.reports - input_last.reports) / input_frames * 10) / 10 :
                 -1);
  Orca::Status::Stats(picojson::value(o).serialize());
  match.stats_time = now;
  match.stats_frame = match.running;
  match.stats_last = stats;
  match.stats_input = input;
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

namespace
{
std::optional<int> Boundary(Core::System& system,
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

  // Only once the room it left has finished leaving: its emptied mirror is the page's last word
  // about it, and the page must not see that room again after "no-room".
  if (match.tell_no_room && !Orca::Online::RoomsLeaving())
  {
    match.tell_no_room = false;
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: no room of its own on port {}", match.local_seat + 1);
    Orca::Status::State("no-room");
  }

  // The in-game UX's frame hook: before this boundary's snapshot or keyframe, so what it writes is
  // in them, and on re-runs too.
  {
    const int frame = match.running + 1;
    const bool resimulating = match.session && match.session->Resimulating();
    const std::vector<Orca::Events::PortInfo> ports = PortsAt(match, frame);
    on_frame(frame, resimulating, ports,
             !resimulating &&
                 AloneAt(frame, SoloIdle(match), Orca::Online::DropInPending(),
                         match.keyframe ? std::optional(match.keyframe->frame) : std::nullopt));
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
    if (!Stopping(system))
      KeepRoomOpen(match);
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

  // Round trips feed the session's adaptive input delay, unless the player chose one: each player's
  // delay is its own, so a change here needs nothing from the others (Session::SetFixedDelay).
  if (const auto round_trip_ms = NewRoundTrip(&match.round_trip_sequence))
    match.session->OnRoundTrip(*round_trip_ms);
  match.session->SetFixedDelay(ChosenDelay());
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
      if (event.kind == Orca::Net::PeerEvent::Kind::Names)
      {
        // A name that applies to frames already run (it arrived after its plug frame): those frames
        // ran without it, and the host's with it. Re-run them; the frame hook writes it as it goes.
        // A port shows a name only while plugged in (PortsAt): a newcomer's name, sent when the
        // host takes it in, matters from its plug frame, which is still to come (or not even
        // decided).
        const std::vector<Orca::Net::KeyframeInfo::Name> before = AllNames(match);
        int earliest = INT32_MAX;
        for (const auto& name : event.keyframe.names)
        {
          if (std::find(before.begin(), before.end(), name) != before.end())
            continue;
          const Orca::Net::SeatPlan& plan = match.session->Plan()[name.seat];
          const int from = std::max(name.from, plan.plug_from);
          if (plan.plug_from == Orca::Net::NEVER || from > match.running ||
              from >= plan.unplug_from)
            continue;
          earliest = std::min(earliest, from);
        }
        SetNames(match, event.keyframe.names);
        if (earliest <= match.running)
        {
          if (match.session->RerunFrom(earliest))
          {
            NOTICE_LOG_FMT(ROLLBACK, "Drop-in: a name for frame {} arrived at frame {}: re-running",
                           earliest, match.running + 1);
          }
          else
          {
            WARN_LOG_FMT(ROLLBACK, "Drop-in: a name for frame {} arrived at frame {}, too late to "
                         "re-run", earliest, match.running + 1);
          }
        }
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Left && event.host)
        match.host_gone = event.reason;
    }
    // Only at a boundary that isn't a re-run: going solo mid re-run would leave half-replayed frames.
    if (match.host_gone && !match.session->Resimulating())
    {
      Orca::Net::PeerEvent event;
      event.reason = *std::exchange(match.host_gone, std::nullopt);
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
        *match.session, *match.port, match.running, [&] { return local_pad(match.local_seat); },
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
    {
      match.stats_rbmax = std::max(match.stats_rbmax, running_before - step.frame + 1);
      // The loaded snapshot holds the hook's writes from that frame's first run, made with the ports
      // known then; a rollback can be caused by exactly those ports changing (a seat unplugged in the
      // past). Run it again for the frame landed on, whichever snapshot each machine had.
      on_frame(step.frame, true, PortsAt(match, step.frame), false);
    }
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
          // The host's frame runs on too, so `behind` can grow back for a moment: never backwards.
          if (percent > match.last_percent)
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
          ShowJoinMessage(
              fmt::format("Joined {}'s game on port {}", match.host_name, match.local_seat + 1),
              4000);
        }
        if (match.session->LeftDone() && step.kind == Orca::Net::StepKind::Run &&
            !match.session->Resimulating())
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
      else
      {
        // Host: a friend taken in plugs in on a frame it runs for the first time; once nobody else
        // is on the way, the app hears they joined (it clears "Your friend is joining").
        if (step.kind == Orca::Net::StepKind::Run && !match.session->Resimulating() &&
            std::erase_if(match.plugging,
                          [&](int seat) { return match.session->Plugged(seat, step.frame); }) > 0 &&
            match.plugging.empty() && match.waiting.empty())
        {
          Orca::Status::State("friend-joined");
        }
        if (match.session->Idle() && match.waiting.empty())
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
    if (!match.joining && match.session->CurrentFrame() == match.running + 1 &&
        (attempt == 0 || !error.empty()))
    {
      // The host plays on: whoever stopped answering, or broke the session, is unplugged. When the
      // host's own room went away, that is what happened to them all: KeepRoomOpen says so (once)
      // and opens another, so the app doesn't hear "your friend left" about it.
      const bool room_gone = Orca::Online::RoomEnded();
      WARN_LOG_FMT(ROLLBACK, "Drop-in: unplugging every friend at frame {}: {}", match.running + 1,
                   peer_silent ? "no word from them" : error);
      if (!room_gone)
      {
        Core::DisplayMessage(peer_silent ? std::string(PEER_SILENT_SENTENCE) :
                                           fmt::format("Your friend was disconnected: {}", error),
                             6000);
      }
      if (error.empty())
      {
        // Silent friends: unplug them and step again (the session still runs).
        DropFriends(match, "stalled");
        continue;
      }
      if (Orca::Online::IsDesync(error))
        ++s_desyncs;
      DropFriends(match, Orca::Online::IsDesync(error) ? "desync" : "network",
                  !room_gone && attempt == 0);
      GoSolo(match, "the session failed");
      RunSolo(match, local_pad);
      return rewound_to;
    }
    WARN_LOG_FMT(ROLLBACK, "Online: session ended at frame {}: {}", match.running + 1,
                 error.empty() ? (peer_silent ? "no word from the host" : "no error") : error);
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
}  // namespace

std::optional<int> OnBoundary(Core::System& system,
                              const std::function<Orca::Net::Pad(int local_seat)>& local_pad,
                              const FrameHook& on_frame)
{
  s_solo_idle = false;
  const std::optional<int> rewound_to = Boundary(system, local_pad, on_frame);
  s_solo_idle = SoloIdle(TheMatch());
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
      "counted), {} waits, {} hitches here, {} there, frame advantage {:.2f}, input delay {}{} "
      "(+{} -{} reverted {}), {} checksums matched, confirmed frame {}, round trip {} ms, link {}, "
      "{} snapshots (every {}, {:.2f} ms each){}{}",
      when, match.session->CurrentFrame(), stats.rollbacks, stats.resimulated_frames,
      stats.deepest_rollback, stats.stalls, stats.counted_stalls, stats.waits, stats.hitches,
      stats.peer_hitches, stats.frame_advantage, stats.delay,
      stats.delay_fixed ? " (chosen)" : "", stats.delay_raises, stats.delay_lowers,
      stats.delay_reverts,
      stats.checksums_matched, stats.confirmed_frame, Orca::Online::RoundTripMs(),
      Orca::Online::Direct().line, stats.saves,
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
  s_solo_idle = false;
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

bool AloneAt(int frame, bool solo_idle, bool drop_in_pending, std::optional<int> stored_keyframe)
{
  return solo_idle && !drop_in_pending &&
         !(stored_keyframe && frame - *stored_keyframe <= KEYFRAME_FRESH_FRAMES);
}

bool SoloPauseAllowed()
{
  // A session with no room (tests): nobody else's game.
  if (!Orca::Online::Enabled())
    return true;
  return s_solo_idle && !Orca::Online::DropInPending();
}

bool BeginPause()
{
  const bool was_pausing = s_pausing.exchange(true);
  if (!SoloPauseAllowed())
  {
    if (!was_pausing)
      s_pausing = false;
    return false;
  }
  s64 none = 0;
  s_paused_since_ns.compare_exchange_strong(none, NowNs());
  return true;
}

void EndPause()
{
  s_pausing = false;
  if (const s64 since = s_paused_since_ns.exchange(0); since != 0)
    s_paused_ns += NowNs() - since;
}

bool Pausing()
{
  return s_pausing;
}

std::chrono::nanoseconds TakePausedTime()
{
  return std::chrono::nanoseconds(s_paused_ns.exchange(0));
}
}  // namespace Rollback::OnlineMatch
