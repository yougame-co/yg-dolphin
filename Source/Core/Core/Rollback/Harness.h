// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Test harness for the rollback core, driven by environment variables. Inert when none is set.
//
//   YG_INPUT=<file>       scripted GameCube pads, one rule per line (# comments):
//                           <from> <to> <port 1-4> <buttons/axes...>     hold during frames from..to
//                           mash <from> <to> <port> <period> <buttons/axes...>   3 frames per period
//                           fuzz <from> <to> <port> <seed>      pseudo-random play, new every 8 frames
//                         Prefix a rule with @<scene> to count from/to from entry into that scene.
//                         Buttons A B X Y Z L R START UP DOWN LEFT RIGHT; axes SX= SY= CX= CY=
//                         (0-255, 128 centre). Frames are the harness frame counter, which rewinds
//                         on a rollback load, so re-run frames read the same input.
//   YG_SCENES=1           log scene changes (needs a profile scene reader; Brawl only for now)
//   YG_EXIT_AFTER=<n>     stop emulation at the end of first-pass frame n
//   YG_HASHLOG=<file>     per-frame RAM hash (T0); ignored while YG_SYNCTEST is on
//   YG_SHOT_EVERY=<n>     screenshot every n first-pass frames from YG_SHOT_FROM (real backend)
//   YG_SYNCTEST=<k>       T1: save every frame, rewind k, re-run, compare RAM and device-state hashes
//   YG_SYNCTEST_FROM=<n>  first frame of the sync test (default 300)
//   YG_SYNCTEST_DIAG=1    byte-level RAM diffs, DoState section diffs and a CoreTiming event trace
//                         (with IPC replies) for the first mismatches; a one-time snapshot size
//                         breakdown
//   YG_SYNCTEST_RENDER=1  render re-run frames too (they skip host rendering by default, as in a
//                         match)
//   YG_SYNCTEST_CLEARJIT=1  drop every JIT block after each load, so re-runs compile blocks with
//                         other boundaries than the first pass did
//   YG_BENCH=<k>          timing: save every frame, every 60th frame load k back; logs normal and
//                         re-run frame, save and load averages
//   YG_DUMP_FRAMES_FROM=<n> / _TO=<m>  Dolphin's frame dump for first-pass frames n..m (with
//                         -C GFX.Settings.DumpFramesAsImages=True): re-runs present nothing, so a
//                         rollback run's dumps pair 1:1 with a plain run's
//   YG_PADREC=<file>      record every port's pad (wire form) and the RAM checksum per frame
//   YG_LOOPBACK=<file>    replay such a recording through a real Orca session (Loopback.h): seat 1
//                         is local, seat 2 a virtual remote whose inputs arrive late; it also sends
//                         the recording's checksums, so any divergence ends the session as a desync.
//                         YG_LOOPBACK_LAG (6) + _JITTER (3) frames late, _DELAY input delay (2),
//                         _MAX_ROLLBACK (7), _CHECKSUM_EVERY (10). A lag above max rollback plus the
//                         input delay makes the session stall. Exit well before the recording ends.
// Runs need an Orca session (ORCA_SESSION=1): the frame boundary hook comes from its profile. All
// modes run unthrottled. The frame counter is one per pass through the game's frame boundary. In a
// session the harness logs a hash of the game's session-NAND save at exit.

#pragma once

#include <optional>

#include "Core/Rollback/Rollback.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Rollback::Harness
{
bool Active();
std::optional<GCPadStatus> InputOverride(int port);
// A session (online or loopback) rolled back to the start of `frame` at the current boundary.
void RewindTo(int frame);
void OnFrameBoundary(const Core::CPUThreadGuard& guard);
}  // namespace Rollback::Harness
