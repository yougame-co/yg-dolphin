// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

// Orca's in-game UX for online play (docs: YouGame docs/ORCA_ONLINE_UX.md): the local controller
// from the YouGame app, the minimal overlay, the players' names in the game's own name tags.
namespace Orca::UX
{
// Bumped whenever anything here that writes emulated memory changes (the name tags; 2: the game
// patches, GamePatches.h, whose file is also hashed into the key; 3: a launcher profile's patches
// are applied too, PPLUS32.patches; 4: the main menu's text about going online, MenuText.h; 5: the
// menu's description box's copy of that text too; 6: a matchmade host's rules preset, Results.h;
// 7: each player's own controls in their name tag, and the port label for a player with controls
// and no name, NameTags.h; 8: the online rules for queue matches (OnlineRules.h, which replace 6)
// and the ranked set's stage, character and result flows (RankedPPlus.h, BrawlStages.h,
// CharOrder.h, RankedSet.h), with their input masks; 9: A on the character select only in the
// grid while the online rules lock, OnlineRules.h; 10: queue2's character select, ready timer and
// steered pick (Queue.h, the header's queue flags), Project+ booting to its main menu and a friend's
// drop-in from the menus (PPLUS32.patches, OnlineMenu.h FriendsMove), the character select's title
// (CssTitle.h), the casual stage pick and the later-game character order (RankedSteps.h,
// BrawlStages.h, RankedPPlus.h, CharOrder.h) with the gate's a_as and steer; 11, Orca 0.3.15: no
// Z on the results screen in any session, its replay save's dead end, OnlineRules.h SessionMasks;
// 12, Orca 0.3.16: the character select's A to pick, B to take the token back up and Start to
// lock in (OnlineRules.h CssBUnpicks, CharOrder.h, Queue.h; the raw latch under every header that
// locks), and the stage select's two cursors, proposals and agreement, and casual's simultaneous
// 15 s pick (StageCursors.h, RankedSteps.h, BrawlStages.h, RankedPPlus.h); 13, Orca 0.3.17: a
// ranked set's room stays open after its verdict and both games stay in their session on the
// results screen until each player leaves it (OnlineMatch's SetOverLeave; an older build would end
// its session at the verdict while the other plays on); 14, Orca 0.3.18: Orca's words in the
// game's own text boxes, the players' names and ratings on their character select plates and the
// queue's line in the rules bar (NativeText.h); 15, Orca 0.3.19: Random a pick like any
// character's, the joiner's Random steered in without turning its colour (Queue.h
// RANDOM_CHARACTER), and no skip in ranked; 16, design F: the plates without the rating and
// SEARCHING on the queue's own select, the rules bar without the timer the band shows, the
// relabels' marks (Relabel.h); 17: a friend's drop-in from the menus waits for the main menu to
// have built its pages (OnlineMenu.h FriendsMove MENU_BUILT), so the move comes at a later frame;
// 18: A on the ONLINE page's With Friends goes straight to the character select and B from there
// back to the ONLINE page on With Friends (no With Friends page: RSBE01.patches, PPLUS32.patches
// muProcWifi and the back out's helper), and A on BACK on the queue's own character select
// (OnlineRules.h CssBackTakesA, Queue.h FLAG_BACK_A, InputGate a_centres_stick); 19: no CPUs
// online and only the seats in use (OnlineSeats.h: the character select's record and panels
// cleared of CPUs, A kept off the player-type and name buttons on
// With Friends and a friend's drop-in and pressed for a player whose panel a CPU left, empty
// panels nobody plugged into hidden):
// the session hashes it into the compatibility key as `ux=<n>`, so two builds that would write
// different bytes (or mask different inputs, or part differently) never meet in a room.
constexpr int kCompatVersion = 19;

// The disc the game runs from (the boot's disc, or the default disc a launcher boots with): the
// overlay's kit reads the game's own fonts from it (Kit.h). Before Init.
void SetGameDisc(std::string path);
// Once, before the game boots in a session: registers the local pad source, the overlay, and the
// session's plug-in, frame and stall callbacks, and puts kCompatVersion in the compatibility key.
void Init();
// At exit, before the UI layer and logging shut down: stops the stream thread, unhooks the overlay.
void Shutdown();
}  // namespace Orca::UX
