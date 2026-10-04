// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The input gate: per-port masks over what the game reads from its controllers, for rules that
// take input away from a player (whose turn it is on a stage strike, locked buttons). ORCA.md,
// "Input gate".
//
// The masks for a frame are a pure function of emulated memory at its start. The rule code
// registers one source (SetSource); the frame-boundary hook evaluates it as its last step before
// the SI relatch (Rollback::OnFrameBoundary), when the boundary's snapshot or keyframe has been
// saved or loaded and the session has set the frame's pads: on every pass, first runs, re-runs,
// a joiner's catch-up and the frame after a keyframe load alike. So the masks always belong to the
// state that is about to run, and nothing of them is kept across a load. They apply where the game
// reads its ports (Rollback::InputOverride, SI_DeviceGCController); the session's wire, the local
// pad sampled for it and the harness's recorded pads stay raw. Both machines therefore apply the
// same masks to the same pads, and a re-run applies them again.

#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Rollback::InputGate
{
constexpr int PORTS = 4;

// What one port's player may not do for a frame: the game sees these buttons released and these
// sticks centred.
struct Mask
{
  // PAD_BUTTON_* and PAD_TRIGGER_* bits (Orca::Net::PAD_WIRE_BUTTONS; any other bit is ignored).
  // L or R also zeroes that trigger's analog value, A or B the analog A or B.
  u16 buttons = 0;
  bool main_stick = false;
  bool c_stick = false;
  // Buttons the game sees pressed whatever the player does (a rule acting for a player whose
  // timer ran out: the ranked stage flow's default pick). Applied after the mask; A, B, L and R
  // also read fully pressed.
  u16 press = 0;
  // Buttons the game sees pressed while the player holds A (applied after the mask, so a rule
  // masks A and reads the press as another button): a casual stage pick's A on vanilla Brawl's
  // and Project+'s stage select reads as X, which the rule sees and the screen doesn't act on
  // (taking the stage would leave the screen).
  u16 a_as = 0;
  // The main stick held where a rule puts it, whatever the player does (the queue's character
  // select steering a joiner's hand to the pick it made in its own game, UX/Queue.h). Applied after
  // the mask.
  bool steer = false;
  u8 steer_x = GCPadStatus::MAIN_STICK_CENTER_X;
  u8 steer_y = GCPadStatus::MAIN_STICK_CENTER_Y;

  bool Empty() const
  {
    return buttons == 0 && !main_stick && !c_stick && press == 0 && a_as == 0 && !steer;
  }
  bool operator==(const Mask&) const = default;
};
// Everything: the port reads as a connected pad with nothing pressed.
constexpr u16 ALL_BUTTONS = 0x1F7F;
constexpr Mask ALL{ALL_BUTTONS, true, true};

using Masks = std::array<Mask, PORTS>;

// `pad` as the game sees it through `mask`. Pure. The origin and error bits (PAD_GET_ORIGIN,
// PAD_USE_ORIGIN, PAD_ERR_STATUS) and isConnected are kept, and an unplugged pad is left as it is.
GCPadStatus Apply(const GCPadStatus& pad, const Mask& mask);

// The rule code's masks for the frame about to run: called on the CPU thread at every frame
// boundary (see above), with the guard for reading emulated memory. It must be a pure function of
// emulated memory (the same masks for the same memory, on every machine, every time; no host
// state, no clock, nothing that differs per machine such as PortInfo.remote), and it must not
// write emulated memory: the boundary's snapshot has been taken by then. Whatever it decides from
// the session's synced data (the plugged-in ports, say) it writes to emulated memory from the
// frame callback (Orca::Events::SetFrameCallback), which runs earlier at the same boundary, and
// reads back here. A change to what it returns changes what the game computes: bump
// Orca::UX::kCompatVersion with it. One source; setting it again replaces it, an empty function
// clears it (no masks from the next boundary on). Any thread.
using Source = std::function<Masks(const Core::CPUThreadGuard& guard)>;
void SetSource(Source source);

// The rules' raw view of the pads: called at every boundary right after the masks are decided,
// with the raw pads of the frame about to run (Rollback::RawInputOverride: the session's, the
// harness's), so a rule can see a press the gate keeps from the game (the queue's Start, B and Z,
// UX/Queue.h). Unlike the source it may write emulated memory, and only as a pure function of that
// memory and those pads: it runs after this boundary's snapshot or keyframe was saved or loaded, so
// no snapshot holds its write, and every pass over the boundary (a first run, a rollback's or a
// sync test's re-run, a joiner's first frame after a keyframe) writes it again from the state it
// starts from and the same pads, before the frame runs. What it wrote is then the game's memory for
// the next boundary's frame hook, saved in that boundary's snapshot like the rest. One latch;
// setting it again replaces it, an empty function clears it. Any thread.
using Latch = std::function<void(const Core::CPUThreadGuard& guard,
                                 const std::array<std::optional<GCPadStatus>, PORTS>& raw)>;
void SetLatch(Latch latch);

// Rollback::OnFrameBoundary, last, before the SI relatch: the masks for the frame about to run
// (the source's, with ORCA_TEST_GATE's added in a test run), then the latch.
void OnBoundary(const Core::CPUThreadGuard& guard);
// No masks (a boundary where the game isn't running yet). CPU thread.
void Clear();
// The masks in effect for the frame that runs now. CPU thread.
const Masks& Current();
// What the game reads from `port` (0-based) this frame: `pad` through Current(). CPU thread.
GCPadStatus GatePad(int port, const GCPadStatus& pad);

// ---- Test knob: ORCA_TEST_GATE (a test override, Orca::TestOverridesActive) ----
// Comma-separated rules `<port 1-4>:<what>[:<when>]`. <what> joins with '+' any of A B X Y Z L R
// START UP DOWN LEFT RIGHT STICK CSTICK ALL. <when>: `always` (the default), or `mem/<n>` (n 2-64;
// `mem` is `mem/2`): on one frame in n, those where byte (port - 1) of an XXH3 of 1 MB of the
// game's memory (0x80500000-0x80600000, its static data in both games) is a multiple of n. That
// changes from frame to frame, so masks computed from the wrong frame's memory show up in a sync
// test, a keyframe test or a drop-in; a larger n leaves scripted menu presses mostly intact.
// Example: `1:A+STICK:mem/8,2:START`.
struct TestRule
{
  int port = 0;  // 0-based
  Mask mask;
  int every = 0;  // `mem/<n>`: n; 0 for `always`
};
std::optional<std::vector<TestRule>> ParseTestSpec(std::string_view spec, std::string* error);
// The rules' masks for a frame whose memory hashes to `memory_hash`.
Masks TestMasks(const std::vector<TestRule>& rules, u64 memory_hash);
constexpr u32 TEST_HASH_START = 0x80500000;
constexpr u32 TEST_HASH_SIZE = 0x100000;
}  // namespace Rollback::InputGate
