// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class PointerWrap;

void VideoCommon_DoState(PointerWrap& p);

// Orca: skip host rendering on throwaway re-run frames. The GX command stream is still parsed, so
// BP/CP/XF registers, TMEM, PE tokens and interrupts and the emulated GPU timing stay exactly as in a
// rendered frame; vertex loading, draws, EFB/XFB copies, clears and presentation are skipped. A
// skipped texture-only copy still writes its constant placeholder to RAM, so RAM matches a rendered
// frame. Refused in dual core. In single core the GPU still runs in time slots, so a change at the
// frame boundary can also cover the tail of the previous frame's commands; RAM is unaffected.
void VideoCommon_SetSkipRender(bool skip);
bool VideoCommon_IsSkippingRender();

// Orca: true while a rollback snapshot is measured, saved or loaded (set around the snapshot's
// DoState; must be the same for all three). Host GPU state is then left alone: no EFB readback,
// no texture-cache save or reload, no bounding-box readback. None of it reaches emulated RAM in a
// session (copies are texture-only), and skipping it saves a GPU sync per snapshot; after a load,
// EFB-copy effects show the newest frame's copy for a frame. Encoded GPU work is submitted (no
// wait) after each snapshot save or load.
void VideoCommon_SetRollbackSnapshot(bool snapshot);
bool VideoCommon_IsRollbackSnapshot();
