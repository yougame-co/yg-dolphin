// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/OnlineMenu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/CssTitle.h"
#include "Core/Orca/UX/MatchBlock.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Orca/UX/Queue.h"

namespace Orca::UX
{
namespace
{
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER_SCENE = 0x4;
constexpr u32 MANAGER_SEQUENCE = 0x10;
constexpr u32 MANAGER_EXIT_CODE = 0x284;
constexpr u32 MANAGER_STEP = 0x288;
constexpr u32 SCENE_NAME_MAX = 32;

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

// The name of the object (a scene or a sequence) the manager's field at `field` points to.
std::string ReadManagerName(const GuestMemory& m, u32 field)
{
  if (!Pointer(m, SCENE_MANAGER))
    return {};
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + field))
    return {};
  const u32 object = m.Read32(manager + field);
  if (!Pointer(m, object))
    return {};
  const u32 name = m.Read32(object);
  std::string out;
  for (u32 i = 0; i < SCENE_NAME_MAX && m.Valid(name + i); ++i)
  {
    const char c = static_cast<char>(m.Read8(name + i));
    if (c == '\0')
      return out;
    out.push_back(c);
  }
  return {};  // not a name
}

// Brawl rev 2 itself or a launcher profile booting it (Project+): the addresses are that
// executable's.
bool BrawlExecutable()
{
  const Orca::Profile* profile = Orca::ActiveProfile();
  return profile && profile->revision == 2 &&
         (profile->IsLauncher() ? profile->disc == "RSBE01" : profile->game_id == "RSBE01");
}
}  // namespace

std::string ReadSceneName(const GuestMemory& m)
{
  return ReadManagerName(m, MANAGER_SCENE);
}

std::string ReadSequenceName(const GuestMemory& m)
{
  return ReadManagerName(m, MANAGER_SEQUENCE);
}

MenuState ReadMenuState(const GuestMemory& m)
{
  MenuState state;
  const std::string scene = ReadSceneName(m);
  if (scene == "muMenuMain")
    state.where = MenuState::Where::Menu;
  else if (scene == "scMemoryChange")
    state.where = MenuState::Where::Leaving;
  if (state.where != MenuState::Where::Other)
  {
    const u32 manager = m.Read32(SCENE_MANAGER);
    if (Pointer(m, manager + MANAGER_EXIT_CODE))
      state.exit_code = m.Read32(manager + MANAGER_EXIT_CODE);
  }
  return state;
}

std::optional<OnlinePick> PickForExitCode(u32 exit_code)
{
  switch (exit_code)
  {
  case 24:
  case 25:
  case 26:
  case 27:
    return OnlinePick::Friends;
  case 30:
    return OnlinePick::Casual;
  case 31:
    return OnlinePick::Ranked;
  default:
    return std::nullopt;
  }
}

std::string_view MenuEventText(OnlinePick pick, bool queue)
{
  switch (pick)
  {
  case OnlinePick::Casual:
    return queue ? "online casual" : "online casual local";
  case OnlinePick::Ranked:
    return queue ? "online ranked" : "online ranked local";
  case OnlinePick::Friends:
    return "online friends";
  }
  return {};
}

std::optional<OnlinePick> OnlineMenuReader::Boundary(const MenuState& state, bool resimulating,
                                                     bool alone, u64 resyncs)
{
  m_busy.reset();
  // A re-run frame repeats a frame whose first run was already read (or that a rollback undid).
  if (resimulating)
    return std::nullopt;
  using Where = MenuState::Where;
  if (!m_started || resyncs != m_resyncs)
  {
    // A new start: whatever this boundary shows is where the game is, not something that happened.
    m_started = true;
    m_resyncs = resyncs;
    m_from_menu = state.where == Where::Menu;
    m_seen = state.where == Where::Leaving;
    return std::nullopt;
  }
  switch (state.where)
  {
  case Where::Menu:
    m_from_menu = true;
    m_seen = false;
    return std::nullopt;
  case Where::Other:
    m_from_menu = false;
    m_seen = false;
    return std::nullopt;
  case Where::Leaving:
    break;
  }
  // The menu's exit code shows from the first boundary between scenes; once per exit.
  if (!m_from_menu || m_seen || state.exit_code == 0)
    return std::nullopt;
  m_seen = true;
  const std::optional<OnlinePick> pick = PickForExitCode(state.exit_code);
  if (!pick)
    return std::nullopt;
  // Made while friends play (or one is about to land): never announced, now or later; the session
  // hears of it once (TakeBusyPick).
  if (!alone)
  {
    m_busy = pick;
    return std::nullopt;
  }
  return pick;
}

std::optional<OnlinePick> OnlineMenuReader::TakeBusyPick()
{
  return std::exchange(m_busy, std::nullopt);
}

namespace
{
// The frame hook's last first-run reading (CPU thread: the hook and the session's boundary).
bool s_on_menus = false;
bool s_between_scenes = false;
std::optional<OnlinePick> s_css_pick;
std::optional<OnlinePick> s_lobby_pick;
}  // namespace

void AnnounceOnlinePick(OnlinePick pick)
{
  // With the app's "host" the queue searches for Casual and Ranked while the player waits on the
  // character select.
  const bool queue = Orca::Status::Cap("host");
  Orca::Status::Menu(MenuEventText(pick, queue));
  // With the app's `queue2` the player waits on the queue's own character select, and the page
  // searches once they are ready (UX/Queue.h); before it, as soon as they pick.
  if (queue && pick != OnlinePick::Friends && Orca::Status::Cap("queue2"))
  {
    Search::End();
    Queue::Begin(pick == OnlinePick::Ranked);
  }
  else if (queue && pick != OnlinePick::Friends)
  {
    Queue::End();
    Search::Begin(pick == OnlinePick::Ranked);
  }
  else
  {
    Queue::End();
    Search::End();
  }
}

std::optional<OnlinePick> TakeLobbyPick()
{
  return std::exchange(s_lobby_pick, std::nullopt);
}

bool MenusScene(std::string_view scene, std::string_view sequence)
{
  return (scene == "muMenuMain" || scene == "scSelctCharacter") && !SequenceHoldsDropIn(sequence);
}

bool OnTheMenus()
{
  return s_on_menus;
}

std::optional<OnlinePick> QueuePickForCssCode(u32 code)
{
  const std::optional<OnlinePick> pick = PickForExitCode(code);
  if (pick == OnlinePick::Friends)
    return std::nullopt;
  return pick;
}

std::optional<OnlinePick> CssPick()
{
  return s_css_pick;
}

bool PickStands(OnlinePick pick)
{
  return s_between_scenes || s_css_pick == pick;
}

void ReadOnlineMenu(const Core::CPUThreadGuard& guard, bool resimulating, bool alone)
{
  // Whatever the session didn't take at the boundary it was seen is stale.
  s_lobby_pick.reset();
  if (resimulating || !BrawlExecutable())
    return;
  static OnlineMenuReader s_reader;
  GuardMemory memory(guard);
  const MenuState state = ReadMenuState(memory);
  const std::string scene = ReadSceneName(memory);
  s_on_menus = MenusScene(scene, ReadSequenceName(memory));
  s_between_scenes = state.where == MenuState::Where::Leaving;
  s_css_pick = scene == "scSelctCharacter" ? QueuePickForCssCode(CssTitle::ReadPick(memory)) :
                                             std::nullopt;
  if (const auto pick = s_reader.Boundary(state, false, alone, Orca::Events::Resyncs()))
  {
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: exit code {}: orca menu {}", state.exit_code,
                   MenuEventText(*pick, Orca::Status::Cap("host")));
    AnnounceOnlinePick(*pick);
    return;
  }
  // Casual or Ranked picked with friends in this game (or one about to land): the session decides
  // at this boundary, after the hook (a host leaves its friends lobby for the queue, then
  // announces it). With Friends while friends play stays theirs, as before.
  if (const auto busy = s_reader.TakeBusyPick(); busy && *busy != OnlinePick::Friends)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: exit code {} ({}) while this game isn't alone",
                   state.exit_code, MenuEventText(*busy, true));
    s_lobby_pick = busy;
  }
  // Backed out of the character select to the main menu while searching (or matched and still
  // alone), or out of the queue's own character select: the search is over.
  if (alone && state.where == MenuState::Where::Menu &&
      (Search::Current() != Search::State::None || Queue::Active()))
  {
    NOTICE_LOG_FMT(ROLLBACK,
                   "Online menu: back on the main menu while searching: orca menu cancel");
    Search::End();
    Queue::End();
    Orca::Status::Menu("cancel");
  }
}

namespace Search
{
namespace
{
std::mutex s_mutex;
State s_state = State::None;
bool s_ranked = false;
}  // namespace

void Begin(bool ranked)
{
  std::lock_guard lock(s_mutex);
  s_state = State::Searching;
  s_ranked = ranked;
}

void Matched(bool hosting)
{
  std::lock_guard lock(s_mutex);
  // A join's own messages ("Joining <name>'s game...") take over from the search's.
  s_state = hosting && s_state != State::None ? State::Found : State::None;
}

void End()
{
  std::lock_guard lock(s_mutex);
  s_state = State::None;
}

State Current()
{
  std::lock_guard lock(s_mutex);
  return s_state;
}

bool Ranked()
{
  std::lock_guard lock(s_mutex);
  return s_ranked;
}

std::pair<std::string, std::string> Lines()
{
  State state;
  bool ranked;
  {
    std::lock_guard lock(s_mutex);
    state = s_state;
    ranked = s_ranked;
  }
  const std::string beta = ranked ? "RANKED BETA" : "";
  if (state == State::Searching)
    return {"Searching for an opponent… (B: back)", beta};
  if (state == State::Found)
  {
    const std::string name = Orca::Online::SeatName(1);
    return {name.empty() ? std::string("Opponent found · waiting for them…") :
                           fmt::format("Opponent found · waiting for {}…", name),
            beta};
  }
  return {};
}
}  // namespace Search

namespace
{
std::atomic<bool> s_drop_in_held{false};

// Versus and its relatives: a friend who plugs in there is already where friends play (the
// character select comes round again after a match).
bool IsVersusSequence(std::string_view sequence)
{
  constexpr std::array<std::string_view, 4> versus{"sqVsMelee", "sqSpMelee", "sqToMelee",
                                                    "sqQuMelee"};
  return std::ranges::find(versus, sequence) != versus.end();
}
}  // namespace

bool SequenceHoldsDropIn(std::string_view sequence)
{
  // sora_scene's sequences (Project+ keeps them all): the solo modes, the Subspace Emissary, and
  // the Vault's screens (trophies, the stage builder, replays, the coin launcher).
  constexpr std::array<std::string_view, 16> held{
      "sqSingleSimple", "sqSingleAllstar", "sqSingleBoss",  "sqEvent",
      "sqTraining",     "sqHomerun",       "sqTargetBreak", "sqKumite",
      "sqAdventure",    "sqCoinShooter",   "sqCoinshooter", "sqEdit",
      "sqReplay",       "sqTyFigDisp",     "sqChallenger",  "sqDebugDefault"};
  return std::ranges::find(held, sequence) != held.end();
}

bool DropInHeld()
{
  return s_drop_in_held.load();
}

namespace FriendsMove
{
Outputs Decide(const Inputs& in)
{
  namespace B = MatchBlock;
  // Ports 2-4: a friend's.
  constexpr u8 FRIEND_PORTS = 0x0E;
  Outputs out;
  // Nothing kept yet (the dead code's bytes) and no friend: nothing to keep. Solo play never writes
  // these bytes.
  const bool kept = in.tag == B::FRIENDS_TAG_VALUE;
  if (!kept && !(in.plugged & FRIEND_PORTS))
    return out;
  out.keep = true;
  // Kept from now on. Nothing seen yet: the host's own port, so a friend who plugs in now is news;
  // ports plugged in before FIRST_FRIEND_FRAME were there from the start (a harness's test ports),
  // since no friend ever plugs in that early.
  const u8 seen = kept                                ? in.seen :
                  in.frame < FIRST_FRIEND_FRAME ? in.plugged :
                                                        static_cast<u8>(in.plugged & ~FRIEND_PORTS);
  bool pending = kept && in.pending != 0;
  if (in.plugged & ~seen & FRIEND_PORTS)
    pending = true;
  // Nobody to take along, a queue match, or already on Versus' way to its character select.
  if (!(in.plugged & FRIEND_PORTS) || in.queue || IsVersusSequence(in.sequence))
    pending = false;
  // The main menu running with its pages built (MENU_BUILT): one still loading its files can't be
  // left, and its pages aren't on screen yet anyway.
  const bool menu = in.scene == "muMenuMain" && in.step == STEP_RUNNING && in.menu_built &&
                    in.exit_code == 0 && in.frame >= 0;
  const u32 since = kept ? in.menu_since : 0;
  // How long the main menu has run, built, with a friend waiting for it.
  out.menu_since = pending && menu ? (since != 0 ? since : static_cast<u32>(in.frame) + 1) : 0;
  if (out.menu_since != 0 &&
      static_cast<u32>(in.frame) + 1 - out.menu_since >= MENU_SETTLE_FRAMES)
  {
    out.move = true;
    pending = false;
    out.menu_since = 0;
  }
  out.seen = in.plugged;
  out.pending = pending ? 1 : 0;
  return out;
}

bool Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports)
{
  namespace B = MatchBlock;
  if (!BrawlExecutable())
    return false;
  GuardMemory m(guard);
  Inputs in;
  in.sequence = ReadSequenceName(m);
  // What the session's drop-in reads at this boundary (first runs only: a re-run's view of an older
  // frame is not where the game is). Tests only, ORCA_TEST_HOLD=<from>-<to>: held over those frames
  // too, as if the host played a single-player mode there.
  static const std::pair<int, int> s_test_hold = [] {
    const char* v = std::getenv("ORCA_TEST_HOLD");
    int from = -1, to = -1;
    if (v && std::sscanf(v, "%d-%d", &from, &to) != 2)
      from = to = -1;
    return std::pair(from, to);
  }();
  if (!resimulating)
  {
    const bool test_hold =
        frame >= s_test_hold.first && frame < s_test_hold.second && TestKnobsAllowed();
    s_drop_in_held = SequenceHoldsDropIn(in.sequence) || test_hold;
  }
  in.present = B::Present(m) && m.Valid(B::FRIENDS) && m.Valid(B::FRIENDS_END - 1);
  if (!in.present || !Pointer(m, SCENE_MANAGER))
    return false;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_EXIT_CODE) || !Pointer(m, manager + MANAGER_STEP))
    return false;
  for (const Events::PortInfo& p : ports)
  {
    if (p.port >= 0 && p.port < 4)
      in.plugged |= static_cast<u8>(1 << p.port);
  }
  in.queue = m.Read32(B::MAGIC) == B::MAGIC_VALUE && m.Read8(B::VERSION) == B::VERSION_VALUE &&
             m.Read8(B::MODE) != B::MODE_NONE;
  in.scene = ReadSceneName(m);
  in.step = m.Read32(manager + MANAGER_STEP);
  in.exit_code = m.Read32(manager + MANAGER_EXIT_CODE);
  if (in.scene == "muMenuMain")
  {
    // ReadSceneName read the scene's name through this pointer, so it is one.
    const u32 menu = m.Read32(manager + MANAGER_SCENE);
    in.menu_built = m.Valid(menu + MENU_BUILT) && m.Read8(menu + MENU_BUILT) != 0;
  }
  in.frame = frame;
  in.tag = m.Read8(B::FRIENDS_TAG);
  in.seen = m.Read8(B::FRIENDS_SEEN);
  in.pending = m.Read8(B::FRIENDS_PENDING);
  in.menu_since = m.Read32(B::FRIENDS_MENU_SINCE);
  const Outputs out = Decide(in);
  if (!out.keep)
    return false;
  // Only the bytes that differ.
  const auto put8 = [&](u32 address, u8 value) {
    if (m.Read8(address) != value)
      m.Write8(address, value);
  };
  put8(B::FRIENDS_SEEN, out.seen);
  put8(B::FRIENDS_PENDING, out.pending);
  if (m.Read32(B::FRIENDS_MENU_SINCE) != out.menu_since)
    m.Write32(B::FRIENDS_MENU_SINCE, out.menu_since);
  put8(B::FRIENDS_TAG, B::FRIENDS_TAG_VALUE);
  if (!out.move)
    return false;
  // The main menu's own exit (muMenuMain, 0x81176828): the code, then the step.
  m.Write32(manager + MANAGER_EXIT_CODE, EXIT_WITH_FRIENDS);
  m.Write32(manager + MANAGER_STEP, STEP_EXIT);
  if (!resimulating)
  {
    NOTICE_LOG_FMT(ROLLBACK,
                   "Online menu: frame {}: a friend plugged in, the main menu goes to With Friends' "
                   "character select (exit code {})",
                   frame, EXIT_WITH_FRIENDS);
  }
  return true;
}
}  // namespace FriendsMove
}  // namespace Orca::UX
