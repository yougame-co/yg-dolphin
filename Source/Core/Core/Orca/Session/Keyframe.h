// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Rollback/Rollback.h"

// Drop-in keyframes: the host's whole machine at one frame boundary (device state, MEM1/MEM2, and
// the session NAND's files, which rollback snapshots leave out), zstd-compressed, for a friend who
// joins a running game. The friend loads it and catches up through the host's inputs since.
namespace Orca::Net
{
// One file or folder of the session NAND, by its path under the NAND's root ('/'-separated).
struct NandEntry
{
  std::string path;
  bool directory = false;
  std::vector<u8> data;
};

// The NAND's files under `root`, in name order. False if the tree can't be read.
bool ReadNandTree(const std::string& root, std::vector<NandEntry>* entries);
// Replaces everything under `root` with `entries` (the friend's own session NAND, which is a
// temporary folder: the caller checks).
bool ReplaceNandTree(const std::string& root, const std::vector<NandEntry>& entries);

// The compressed keyframe and back. `hash` is the hex XXH3 of the compressed bytes.
std::vector<u8> PackKeyframe(int frame, const Rollback::MachineImage& image,
                             const std::vector<NandEntry>& nand);
bool UnpackKeyframe(const std::vector<u8>& blob, int* frame, Rollback::MachineImage* image,
                    std::vector<NandEntry>* nand);
std::string KeyframeHash(const std::vector<u8>& blob);

// Where keyframes travel: the host puts one, a joining friend gets it by id, and it's deleted once
// loaded. Progress reports bytes done of total; returning false from it aborts.
class KeyframeStore
{
public:
  virtual ~KeyframeStore() = default;
  virtual bool Put(const std::string& id, const std::vector<u8>& data, std::string* error) = 0;
  virtual std::optional<std::vector<u8>> Get(const std::string& id, u64 size,
                                             const std::function<bool(u64, u64)>& progress,
                                             std::string* error) = 0;
  virtual void Delete(const std::string& id) = 0;
};

// ORCA_TEST_KEYFRAME_DIR: a folder both processes on this machine share (tests). Otherwise null
// until YouGame's keyframe endpoint exists (ORCA.md, "Drop-in").
std::unique_ptr<KeyframeStore> MakeKeyframeStore();
}  // namespace Orca::Net
