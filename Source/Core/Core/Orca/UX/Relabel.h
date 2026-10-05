// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

// The game's own big words carry Orca's meaning (design F, Luis 2026-10-05: "relabelled for
// almost everything"): the character select's READY TO FIGHT! says what to do now (PRESS START TO
// LOCK IN!, WINNER PICKS FIRST!, LOSER PICKS NOW!), the stage select's STAGE SELECT says whose turn
// it is and what to do (P2 STRIKES 2, P1 BANS 1, P2 PICKS, PICK A STAGE), its BRAWL / VERSUS the
// room's mode (RANKED, CASUAL), and Project+'s stage select legend, in a ranked set, Orca's
// buttons (X STRIKE, A PROPOSE, B TAKE BACK).
//
// The character select's title's way (CssTitle.h), for more textures: Orca never draws into the
// game's art. It marks the texture so that it hashes to another name, one per label, and the
// texture pack (Data/Sys/Orca/Textures/RSBE01, Tools/orca/textures/make-labels.py) has our art
// for each name; without the pack the game's own words show, unchanged to the eye. The mark is
// one of two things nobody sees:
//   - a paletted texture (READY TO FIGHT!, C4): a transparent palette entry's blue, as CssTitle;
//   - an intensity texture (STAGE SELECT, the mode title and the legend's words, I4; the legend's
//     buttons, IA8): its first texel. Dolphin's texture cache hashes a sample of a texture's data
//     to see it change (Common/Hash.cpp GetHash64: every Step-th word, always the first), and the
//     custom texture's name hashes all of it (TextureInfo::CalculateTextureName), so a change in
//     the first 8 bytes is seen at once and names the label. The pack draws over the texture, so
//     the texel itself is never seen; without the pack an I4 mark is one texel in a corner, as
//     bright as the label's number makes it (up to 14 of 15).
//
// What each says is a pure function of emulated memory and the ports in effect at the boundary
// (the match block's header, the queue's and the character order's state, the stage flows'
// steps), written at every boundary, first runs and re-runs, only where the slot holds the game's
// value or one of the marks (anything else is not the texture this knows: left alone), only when
// it differs. The archives load fresh each time their screen starts, so a mark never outlives it;
// the game's own words come back by writing the game's value when nothing calls for a label.
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::Relabel
{
// READY TO FIGHT! (MenSelchrReady01_2).
enum class Band : u8
{
  Game = 0,
  LockIn = 1,       // PRESS START TO LOCK IN!
  WinnerFirst = 2,  // WINNER PICKS FIRST!
  LoserNow = 3,     // LOSER PICKS NOW!
};

// STAGE SELECT (MenSelmapBack07): whose turn and what. 1-6 strikes (P1 1-3, P2 1-3 left), 7-10
// bans (P1 1-2, P2 1-2), 11-12 picks (P1, P2), 13 a casual game's both-at-once pick.
using Line = u8;
constexpr Line LINE_GAME = 0;
constexpr Line LINE_PICK_A_STAGE = 13;

// The stage select's BRAWL / VERSUS (MenSelchrRtitle.30).
enum class Title : u8
{
  Game = 0,
  Ranked = 1,
  Casual = 2,
};

// One texture's mark: where it is and the values it takes.
struct Slot
{
  enum class Kind : u8
  {
    None,
    PaletteEntry,  // a u16 RGB5A3 entry: the game's value + step * label
    HighNibble,    // a byte's high nibble (an I4 texel): (the game's + label) mod 16
    Byte,          // a byte (an IA8 texel's intensity): the game's + label
  };
  Kind kind = Kind::None;
  u32 address = 0;
  u16 game = 0;
  int labels = 0;  // the highest label
  // The slot's value (u16 or byte) for `label` (0: the game's), given the value there now.
  u16 ValueFor(int label, u16 now) const;
  // The label a value there stands for, if it is the game's or a mark.
  std::optional<int> LabelOf(u16 now) const;
  u16 Read(const GuestMemory& memory) const;
  void Write(GuestMemory& memory, u16 value) const;
};

// The slots, when their screen is up and its archive loaded (Kind::None otherwise).
Slot FindBand(const GuestMemory& memory);
Slot FindLine(const GuestMemory& memory);
Slot FindTitle(const GuestMemory& memory);
std::array<Slot, 2> FindLegend(const GuestMemory& memory);  // Project+: the words, the buttons

// What each should say now (pure). `ports`: the ports in effect at the boundary.
Band WantedBand(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports);
Line WantedLine(const GuestMemory& memory);
Title WantedTitle(const GuestMemory& memory);
bool WantedLegend(const GuestMemory& memory);

// Whether READY TO FIGHT! is on screen: both players' tokens down on a character, as the game
// shows it (pure).
bool BandUp(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports);
// Whether the turn's timer is the band's (pure): the band is up and says one of Orca's labels. The
// rules bar (NativeText.h) leaves its own timer out then, and the overlay draws it at the band's
// right end in the band's lettering.
bool ClockInBand(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports);

// A stage step as the line names it: `kind` (Ranked::StepKind's value), `port` (0, 1, or 2 for
// both), `left` the strikes or bans still to make. Its line (pure).
Line LineFor(u8 kind, u8 port, int left);

// What the labels say after one boundary.
struct Shown
{
  Band band = Band::Game;
  Line line = LINE_GAME;
  Title title = Title::Game;
  bool legend = false;
  bool band_up = false;  // BandUp, where there is a band to label
  bool operator==(const Shown&) const = default;
};

// One boundary: writes each slot's wanted value where it differs. Returns how many slots were
// written; `shown` gets what they say afterwards (the game's where there is no slot).
int Apply(GuestMemory& memory, const std::vector<Events::PortInfo>& ports, Shown* shown = nullptr);

// The frame hook's part (Brawl rev 2 and Project+ only), first runs and re-runs, with the ports
// in effect at the boundary: Apply, and on first runs publishes what the labels say (Current).
// Returns this boundary's labels (NativeText takes ClockInBand from them: the same answer, read
// once).
Shown Frame(const Core::CPUThreadGuard& guard, bool resimulating,
            const std::vector<Events::PortInfo>& ports);
// Whether the turn's timer is the band's, from a boundary's labels (ClockInBand's answer).
inline bool ClockInBand(const Shown& shown)
{
  return shown.band != Band::Game && shown.band_up;
}
// The last labels published (any thread): the overlay leaves out the lines a label says and draws
// the turn's timer beside it (GameTimer).
Shown Current();
}  // namespace Orca::UX::Relabel
