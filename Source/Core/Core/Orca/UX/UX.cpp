// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/UX.h"

#include <chrono>
#include <cstdlib>
#include <utility>
#include <vector>

#include "Common/Logging/Log.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/BrawlStages.h"
#include "Core/Orca/UX/CharOrder.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "Core/Orca/UX/CssTitle.h"
#include "Core/Orca/UX/GamePatches.h"
#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/MenuText.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/NativeText.h"
#include "Core/Orca/UX/OnlineRules.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Orca/UX/OnlineSeats.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Orca/UX/Queue.h"
#include "Core/Orca/UX/RankedPPlus.h"
#include "Core/Orca/UX/RankedSet.h"
#include "Core/Orca/UX/Relabel.h"
#include "Core/Orca/UX/Results.h"
#include "Core/Orca/UX/StageCursors.h"
#include "Core/Rollback/InputGate.h"

namespace Orca::UX
{
namespace
{
// Adds `more`'s masks to `masks`: a button, stick or press any rule takes is taken.
void Merge(Rollback::InputGate::Masks& masks, const Rollback::InputGate::Masks& more)
{
  for (int port = 0; port < Rollback::InputGate::PORTS; ++port)
  {
    masks[port].buttons |= more[port].buttons;
    masks[port].press |= more[port].press;
    masks[port].a_as |= more[port].a_as;
    masks[port].main_stick = masks[port].main_stick || more[port].main_stick;
    masks[port].c_stick = masks[port].c_stick || more[port].c_stick;
    masks[port].a_centres_stick = masks[port].a_centres_stick || more[port].a_centres_stick;
    if (more[port].steer)
    {
      masks[port].steer = true;
      masks[port].steer_x = more[port].steer_x;
      masks[port].steer_y = more[port].steer_y;
    }
  }
}
}  // namespace

void SetGameDisc(std::string path)
{
  Kit::SetGameDisc(std::move(path));
}

void Init()
{
  StartControllerStreamFromEnvironment();
  Orca::Events::SetLocalPadSource([](bool for_frame) { return LocalPad(for_frame); });
  Orca::Events::SetInputAgeSource([] { return GetInputAge(); });
  InitOverlay();
  // The input gate's one source: every rule's masks for the frame about to run, ORed (the online
  // rules' locks, the ranked flows' turns), from memory alone.
  Rollback::InputGate::SetSource([](const Core::CPUThreadGuard& guard) {
    Rollback::InputGate::Masks masks{};
    if (Rules::ProfileRuleset() == Rules::Ruleset::None)
      return masks;
    GuardMemory memory(guard);
    // Every session's: no Z on the results screen (its replay save, a dead end in a session).
    Merge(masks, Rules::SessionMasks(memory));
    Merge(masks, Rules::GateMasks(memory));
    // No CPUs online (OnlineSeats.h): where no header locks (With Friends, a friend's drop-in), A
    // never reaches a player-type or name button, and A is pressed for a plugged-in player whose
    // own panel a CPU left (the game's own way to take it).
    Merge(masks, OnlineSeats::GateMasks(memory));
    Merge(masks, RankedPPlus::Masks(memory));
    BrawlStages::GateMasks(guard, &masks);
    Rollback::InputGate::Masks order = CharOrder::GateFrame(guard);
    // On a queue2 room's character select the ready timer gives the Start that goes on (Queue.h):
    // the character order's own Start, once its picks are in, is not pressed.
    if (Queue::OwnsStart(guard))
    {
      for (Rollback::InputGate::Mask& mask : order)
        mask.press &= static_cast<u16>(~PAD_BUTTON_START);
    }
    Merge(masks, order);
    Merge(masks, Queue::GateFrame(guard));
    return masks;
  });
  // The queue's character select sees the presses the gate keeps from the game (Start, B, Z):
  // each frame's raw buttons go into the match block before it runs (InputGate.h, Queue.h).
  Rollback::InputGate::SetLatch(
      [](const Core::CPUThreadGuard& guard,
         const std::array<std::optional<GCPadStatus>, Rollback::InputGate::PORTS>& raw) {
        Queue::LatchFrame(guard, raw);
        // The stage select's two cursors (StageCursors.h): each player's stick moves their own.
        StageCursors::LatchFrame(guard, raw);
      });
  // The name tags write emulated memory: the compatibility key carries their version.
  Orca::Events::SetUXCompatVersion(kCompatVersion);
  // At every frame boundary, first runs and re-runs, before the session saves anything there, with
  // the ports in effect at that frame (the session's synced plan; ORCA_UX_TEST_NAMES in harness
  // runs).
  Orca::Events::SetFrameCallback([](const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                                    const std::vector<Orca::Events::PortInfo>& ports, bool alone) {
    static bool s_logged = false;
    if (!std::exchange(s_logged, true))
      NOTICE_LOG_FMT(ROLLBACK, "Orca UX: frame hook live at frame {}", frame);
    ApplyGamePatches(guard);
    // The main menu's words about going online (a pure function of memory, like the patches).
    WriteMenuText(guard);
    // Reads only: the app hears what the player picked in the game's Online menu.
    ReadOnlineMenu(guard, resimulating, alone);
    // A friend who plugged in while the host is in the menus: both go to With Friends' character
    // select (game logic, a pure function of memory and the synced ports, first runs and re-runs).
    FriendsMove::Frame(guard, frame, resimulating, ports);
    // A ranked queue match's set, followed in the match block (a pure function of memory).
    RankedSet::Frame(guard, frame, resimulating);
    // Reads only: each game's result in a matchmade room, once its frames are final (Results.h).
    ReadResultsFrame(guard, frame);
    // Project+'s ranked stage flow (RankedPPlus.h): game logic, a pure function of memory, the
    // frame number and the synced ports, first runs and re-runs.
    RankedPPlus::Frame(guard, frame, resimulating, ports, alone);
    // A ranked set's stage flow on vanilla Brawl's stage select (a pure function of memory).
    BrawlStages::FrameHook(guard, resimulating, ports);
    // What the overlay draws of the stage select's two cursors (reads only, first runs).
    if (!resimulating && Rules::ProfileRuleset() != Rules::Ruleset::None)
    {
      GuardMemory memory(guard);
      StageCursors::View view = Rules::ProfileRuleset() == Rules::Ruleset::Brawl ?
                                    BrawlStages::CursorView(memory, ports) :
                                    RankedPPlus::CursorView(memory, ports, frame);
      StageCursors::Publish(std::move(view));
    }
    ProbeFrame(guard, frame);
    // The overlay kit dresses as the session's game (local display only).
    Kit::SetLook(Rules::ProfileRuleset() == Rules::Ruleset::PPlus ? Kit::Look::ProjectPlus :
                                                                    Kit::Look::Brawl);
    // This player's own controls in its own game, for the session to carry into a game it joins or
    // hosts (reads only; only while this game is its player's alone, so never a friend's game).
    if (alone)
      ReadOwnControlsFrame(guard, ports);
    // Test only, ORCA_UX_TEST_LATE_NAME=<frame>: a friend's name and controls that reached this
    // machine late. First runs before that frame see them empty, re-runs see them, as when a port's
    // values arrive between a frame's first run and its re-run: what the session's value channel
    // rules out (NameTags.h), shown desyncing.
    static const int s_late = [] {
      const char* v = std::getenv("ORCA_UX_TEST_LATE_NAME");
      return v ? std::atoi(v) : -1;
    }();
    // Checked every frame: a host that boots solo can take a real friend's drop-in later.
    if (s_late >= 0 && !resimulating && frame < s_late && TestKnobsAllowed())
    {
      std::vector<Orca::Events::PortInfo> early = ports;
      // PortInfo.remote differs per machine: the friend is whoever is not on port 1.
      for (Orca::Events::PortInfo& p : early)
      {
        if (p.port != 0)
        {
          p.name.clear();
          p.controls.clear();
        }
      }
      WriteNameTags(guard, early);
    }
    else
    {
      WriteNameTags(guard, ports);
    }
    // The online rules (OnlineRules.h): a queue room's header at an alone boundary, then the
    // locks it calls for (rules, stages, the ready timer), on first runs and re-runs alike.
    u8 plugged = 0;
    for (const Orca::Events::PortInfo& p : ports)
    {
      if (p.port >= 0 && p.port < 4)
        plugged |= static_cast<u8>(1 << p.port);
    }
    Rules::OnFrame(guard, frame, resimulating, alone, plugged);
    // No CPUs online (OnlineSeats.h): the record the character select builds its panels from, and
    // the panels themselves, from the header just written, the menu's pick and the ports (pure).
    OnlineSeats::ClearFrame(guard, frame, resimulating, plugged);
    // A ranked set's character order on later games' character select (CharOrder.h): its state
    // in the match block, which the input gate reads for the frame about to run.
    CharOrder::Frame(guard, frame, resimulating, ports);
    // The queue's character select (Queue.h): its ready flags and timer, the joiner's pick put in,
    // after the character order (a ranked later game's ready timer waits for it).
    Queue::Frame(guard, frame, resimulating, ports, alone);
    // The character select's title (CssTitle.h): CASUAL, RANKED or FRIENDS, from the header the
    // rules just wrote, the menu's pick and the ports (a pure function of them).
    CssTitle::Frame(guard, resimulating, ports);
    // The game's big words relabelled (Relabel.h): READY TO FIGHT!, STAGE SELECT, the stage
    // select's mode title and Project+'s legend, from the same block and ports (pure).
    const Relabel::Shown labels = Relabel::Frame(guard, resimulating, ports);
    // Orca's words in the character select's own text boxes (NativeText.h): each named player's
    // plate, and the rules bar's line in a queue room, from the block the writers above just left
    // and the synced ports (a pure function of them, first runs and re-runs).
    NativeText::Frame(guard, resimulating, ports, Relabel::ClockInBand(labels));
    // Only the seats in use on an online character select (OnlineSeats.h): empty panels nobody
    // plugged into are hidden, from the panels, the ports and the queue's ready flag (pure).
    OnlineSeats::SeatsFrame(guard, frame, resimulating, plugged);
    // Last, after every other writer here: a toggled patch group follows its condition as this
    // boundary's writers left it, so the hook run again at this boundary (a re-run from the
    // snapshot saved here) decides the same and writes nothing new (GamePatches.h).
    ApplyToggledGamePatches(guard);
  });
  // Nothing is presented while the game waits for a friend: show the last frame again, with the
  // overlay's "Waiting for <name>..." on it.
  Orca::Events::SetStallCallback([](int stalled_ms) { RepresentDuringStall(stalled_ms); });
  // The frame meter times the game's shown frames.
  Orca::Events::SetBoundaryCallback([](bool shown) { FrameBoundary(shown); });
  // The session's plug-in callback, for the overlay's joined/left toasts only (local UI; never a
  // source for memory writes: those take the frame callback's ports).
  Orca::Events::SetPlugInCallback([](int frame, const std::vector<Orca::Events::PortInfo>& ports) {
    const double now = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    Overlay().OnPorts(frame, ports, now);
  });
}

void Shutdown()
{
  Rollback::InputGate::SetSource({});
  Rollback::InputGate::SetLatch({});
  Orca::Events::SetStallCallback({});
  Orca::Events::SetBoundaryCallback({});
  Orca::Events::SetFrameCallback({});
  Orca::Events::SetPlugInCallback({});
  Orca::Events::SetLocalPadSource({});
  Orca::Events::SetInputAgeSource({});
  ShutdownOverlay();
  StopControllerStream();
}
}  // namespace Orca::UX
