// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Online drop-in play (Orca::Online: friends over a YouGame room), run at the frame boundary. The
// host plays solo from its boot; a friend who joins loads the host's keyframe, catches up through
// the host's inputs since, and plugs into the next port, from where a rollback session over the
// room's transport runs both games (RingPort is the emulator side). See ORCA.md, "Drop-in".

#pragma once

#include <chrono>
#include <functional>
#include <optional>

#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Session.h"

namespace Core
{
class System;
}

namespace Rollback::OnlineMatch
{
// One frame boundary. The first call joins the room; a joiner blocks there until it has loaded the
// host's keyframe (or emulation stops). `local_pad(seat)` gives this player's pad for the given
// seat. Returns the frame the state was rewound to here (a rollback, or a joiner's keyframe): the
// state is now the start of that frame.
// `on_frame(frame, resimulating, ports, alone)` runs first at every boundary
// (Orca::Events::NotifyFrame), before anything is saved there; `ports` are the controllers plugged
// in at that frame; `alone`: AloneAt this frame (false on re-runs).
using FrameHook = std::function<void(int frame, bool resimulating,
                                     const std::vector<Orca::Events::PortInfo>& ports, bool alone)>;
std::optional<int> OnBoundary(Core::System& system,
                              const std::function<Orca::Net::Pad(int local_seat)>& local_pad,
                              const FrameHook& on_frame);

// A keyframe this recent serves a new arrival as it is: the joiner replays up to this many frames
// (30 s at 60 fps) and catches up from there.
constexpr int KEYFRAME_FRESH_FRAMES = 30 * 60;

// The frame hook's `alone` at the first run of `frame`: what this game does then is this player's
// alone, so the UX may announce it (UX/OnlineMenu.h). It plays solo with nothing of drop-in under
// way as the last boundary left it (`solo_idle`, SoloIdle: no friend plugged in or on the way, not
// joining or catching up, no keyframe wanted or being made); nothing waits for this boundary to
// take it (`drop_in_pending`, Orca::Online::DropInPending: a friend's arrival, a prepare-join or an
// invite, the app's join or leave); and no keyframe it stored that nobody took yet
// (`stored_keyframe`, its frame: a prepare-join's or an invite's) is still fresh, since a friend
// who arrives within KEYFRAME_FRESH_FRAMES of it replays every frame since, which makes them the
// friend's too.
bool AloneAt(int frame, bool solo_idle, bool drop_in_pending, std::optional<int> stored_keyframe);

// Whether the frame hook may write the online rules' header at the first run of `frame` though the
// frame may not be this player's alone (UX/OnlineRules.h SetHeaderFree): no other game runs this
// frame from a state before it. This game plays solo with nothing a friend would replay
// (`solo_quiet`: no session, not joining, no keyframe being made or downloaded, nobody plugging in,
// not catching up), and no keyframe it stored for the header it calls for now (`stored_keyframe`,
// its frame) is still fresh. A friend's arrival, a prepare-join or a keyframe wanted don't stop it,
// as they stop AloneAt: their keyframe is made at this boundary or later, after the hook, so it
// carries what the hook wrote. So the room's header lands at the boundary after the room's welcome
// even when the opponent's hello comes with it.
bool HeaderFreeAt(int frame, bool solo_quiet, std::optional<int> stored_keyframe);

// The host's wait for the header its room calls for, before it makes or offers a keyframe
// (HostEvents): a keyframe carries the header in this game's memory, and the joiner replays it.
enum class HeaderWait
{
  Ready,  // make or offer the keyframe
  Wait,   // not yet: the header lands at a boundary SetHeaderFree allows
  Fail,   // a queue room: the match can't start (`orca error mismatch`, the room left)
};
// A friends room whose game never carries the header it calls for (a session already running when
// it changed) makes its keyframe anyway after this many boundaries with a friend waiting (the
// joiner refuses a header that locks, as before): 2 s.
constexpr int HEADER_WAIT_BOUNDARIES = 120;
// A queue room never makes a keyframe without the room's header: after this many boundaries with
// the opponent waiting (the header lands within one; a keyframe already being made first finishes),
// 5 s, the join fails.
constexpr int HEADER_FAIL_BOUNDARIES = 300;
// One boundary of the wait: `in_place`, UX/OnlineRules.h HeaderInPlace; `someone_waiting`, a friend
// waits for a keyframe; `queue_room`, this game hosts a room the queue made. `*waited` counts the
// boundaries a friend has waited for the header.
HeaderWait StepHeaderWait(int* waited, bool in_place, bool someone_waiting, bool queue_room);

// A former joiner's way home (ORCA.md "Drop-in"; Luis, 2026-10-05: a guest whose host left goes back
// to their own lobby). A player that joined a friend's game and then went solo (its host left, it
// left, its session ended) plays on the port it was given, with no room: nobody can drop in, and the
// queue's own character select never gets its header there (it wants port 1), so Start would do
// nothing. At its first alone boundary on the menus it moves to port 1 and a room of its own opens.
struct HomeInputs
{
  // The port it plays (0: port 1, nothing to do).
  int local_seat = 0;
  // Joining a friend's game, or in a session: not alone.
  bool joining = false;
  bool session = false;
  // The room it was in is still up (a ranked room it waits in for the verdict, or stays in after
  // the set): that room's own ending takes it home (Linger, SetOverLeave).
  bool room_up = false;
  // SoloIdle, and nothing of drop-in the next boundary will take (Orca::Online::DropInPending: the
  // app's join wins).
  bool solo_idle = false;
  bool drop_in_pending = false;
  // The game is on the menus at this boundary (UX/OnlineMenu.h OnTheMenus): the main menu or a
  // character select, never a fight, a stage select, a results screen, between two scenes, or a
  // single-player mode.
  bool on_menus = false;
  // The queue's room left this player's own game to put back (MaybeRestoreQueueImage), which moves
  // it to port 1 itself.
  bool queue_image = false;
  // Waiting in a ranked room for its verdict (Linger).
  bool lingering = false;
};
bool MayComeHome(const HomeInputs& in);

// What the session does with a With Anyone pick (Casual or Ranked) the frame hook saw while the
// game wasn't alone (UX/OnlineMenu.h TakeLobbyPick), at the same boundary. Luis, 2026-10-05: Casual
// and Ranked drop the friends lobby for a 2-player room the matchmaker fills. In a shared menu
// either player's press makes the pick, and every game sees it at the same frame.
enum class LobbyPickStep
{
  // Nothing: a joiner's game is its host's (the host leaves, and this game hears host-left, then
  // comes home on that pick's character select and announces it there: AnnounceHomePick); a queue
  // room's menus are locked; without the app's "host" nothing would search ("... local"), so the
  // friends' game stays theirs, as before.
  Ignore,
  // Nobody to leave by now (the app's leave or join went first, or the last friend has gone and the
  // session only idles out its last frames): just announce the pick.
  Announce,
  // A host with friends in its game or on their way: it leaves the room (its friends play on
  // without it: `orca state host-left`), prints `orca state left lobby`, and announces the pick.
  Leave,
};
struct LobbyPickInputs
{
  // A joiner's game; the port it plays (a former joiner hosts nobody); in a room the queue made;
  // the app's "host".
  bool joining = false;
  int local_seat = 0;
  bool queue_room = false;
  bool host_cap = false;
  // A session, and whether it only idles out its last frames with nobody else in it or on the way
  // (Session::Idle: the last friend went, IDLE_GRACE_FRAMES).
  bool session = false;
  bool session_idle = false;
  // A friend waiting for its keyframe, plugging in or seated.
  bool drop_in_friends = false;
  // A friend's arrival the room has and the host hasn't taken yet (Online::ArrivalPending): left
  // in, it would be taken into a game under the queue's header and refuse it (`orca error
  // mismatch`).
  bool arrival_pending = false;
  // Kept for an invite nobody has taken yet: a stored keyframe, one being made, or one wanted (a
  // prepare-join). No player.
  bool keyframe_kept = false;
};
struct LobbyPickPlan
{
  LobbyPickStep step = LobbyPickStep::Ignore;
  // An announced pick drops what drop-in kept for an invite (made under the header before the
  // pick): a friend who comes later gets a keyframe of the queue's own character select. Never
  // with a session still running.
  bool drop_keyframe = false;
};
LobbyPickPlan DecideLobbyPick(const LobbyPickInputs& in);

// A former joiner coming home (ComeHome) on a character select a With Anyone pick opened (UX/
// OnlineMenu.h CssPick: its host left the friends lobby at that pick, whoever pressed it): whether
// it announces that pick there (AnnounceOnlinePick: the queue's own character select, so its Start
// readies it as it does its host's). Only with the app's "host" (without it nothing would search),
// and never over a queue or search already under way.
bool AnnounceHomePick(bool css_pick, bool host_cap, bool queue_or_search);

// True while this process plays online (solo in its room, or with friends).
bool Active();
// Logs the session's stats ("Online match <when>: ...").
void LogStats(const char* when);
// Logs the session's stats (and why it ended), ends the session and leaves the room. Safe to call
// more than once; called by itself when emulation stops or the session ends.
void End(const char* reason);

// The player's pause in a session (the app's "pause", Core::SetState). A pause may stop only this
// player's game: allowed in a session without a room (tests), and online while the last frame
// boundary left this game alone with nothing of drop-in under way (no friend plugged in or on the
// way, not joining or catching up, no keyframe wanted or being made) and nothing waits for the
// next one (Orca::Online::DropInPending: a friend arriving, an invite, a join or a leave).
// Any thread.
bool SoloPauseAllowed();
// Core::SetState(Paused) in a session: whether the pause may go ahead (SoloPauseAllowed). From a
// yes until EndPause, boundaries take no friend in, so one that still runs while the CPU thread
// winds down never starts a session it can't step (a step gives up while the CPU is stepping).
bool BeginPause();
// Core::SetState(Running): the pause, if any, is over.
void EndPause();
// Between a BeginPause that said yes and EndPause: the core is paused by the player's pause (not
// still stepping at boot, nor stopped by an HLE reload or a panic).
bool Pausing();
// Host time spent paused since the last call. The stats line leaves it out of its interval, so
// its fps counts running time only.
std::chrono::nanoseconds TakePausedTime();
}  // namespace Rollback::OnlineMatch
