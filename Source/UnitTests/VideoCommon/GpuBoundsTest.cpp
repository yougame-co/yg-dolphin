// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/TextureCacheBase.h"

// Orca: the GPU's guest-chosen writes into guest RAM stay inside it, and a copy that fits is
// checked exactly as before.

TEST(OrcaEfbCopyBounds, AStrideUnderARowCoversTheLastRow)
{
  // A normal copy: rows `stride` apart, the stride at least a row.
  EXPECT_EQ(EFBCopyCheckedRange(640, 120, 640), 640u * 120);
  EXPECT_EQ(EFBCopyCheckedRange(640, 120, 1024), 1024u * 120);
  // A smaller stride: the last row starts at 119 * stride and is a full row long.
  EXPECT_EQ(EFBCopyCheckedRange(640, 120, 32), 119u * 32 + 640);
  EXPECT_EQ(EFBCopyCheckedRange(16384, 256, 0), 16384u);
  EXPECT_EQ(EFBCopyCheckedRange(640, 1, 0), 640u);
  EXPECT_EQ(EFBCopyCheckedRange(640, 0, 32), 0u);
  // The largest the registers allow: no overflow.
  EXPECT_EQ(EFBCopyCheckedRange(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF),
            u64{0xFFFFFFFF} * 0xFFFFFFFF);
}
