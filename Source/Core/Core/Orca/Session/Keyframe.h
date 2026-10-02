// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/YouGameRoom.h"
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
  // A file the joiner's own boot also writes (Profile::boot_nand_frame): the keyframe carries only
  // its XXH3-64, and the joiner uses its own copy once the hash matches.
  bool reference = false;
  u64 hash = 0;
};

// A file's hash as references carry it.
u64 NandHash(const std::vector<u8>& data);
// The NAND's files under `root` as {path: hash} (no folders), for references.
std::map<std::string, u64> HashNandTree(const std::string& root);
// Replaces each reference in `entries` with the file of that path under `root` (the joiner's own
// NAND, after its boot). False, with the path in `error`, if one is missing or differs.
bool ResolveNandReferences(const std::string& root, std::vector<NandEntry>* entries,
                           std::string* error);

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

// AES-256-GCM over a packed keyframe, so YouGame's store only ever holds ciphertext: a fresh random
// key per keyframe, travelling only in the room's `kf` message to the joiner (KeyframeInfo::key,
// 64 hex). The body is IV (12) | ciphertext | tag (16); the frame is authenticated with it.
bool EncryptKeyframe(int frame, std::vector<u8>* blob, std::string* key_hex);
bool DecryptKeyframe(int frame, const std::string& key_hex, std::vector<u8>* blob);

// Where keyframes travel: the host puts one, a joining friend gets it, and it's deleted once loaded.
// Put and Get block (call them on a thread of their own); progress reports bytes done of total, and
// returning false from it aborts. Delete never blocks.
class KeyframeStore
{
public:
  virtual ~KeyframeStore() = default;
  virtual bool Put(const KeyframeInfo& info, const std::vector<u8>& data, std::string* error) = 0;
  virtual std::optional<std::vector<u8>> Get(const KeyframeInfo& info,
                                             const std::function<bool(u64, u64)>& progress,
                                             std::string* error) = 0;
  virtual void Delete(const std::string& id) = 0;
};

// A room ticket (with `fresh`, a newly minted one) and the store's address for the room
// (YouGameRoom::FreshTicket).
using TicketSource = std::function<bool(bool fresh, std::string* ticket, std::string* store_url,
                                        std::string* error)>;

// ORCA_TEST_KEYFRAME_DIR: a folder both processes on this machine share (tests). Otherwise
// YouGame's keyframe endpoint (ORCA.md, "Keyframe store"), authorized by room tickets from
// `tickets`.
std::unique_ptr<KeyframeStore> MakeKeyframeStore(TicketSource tickets);

// The HTTP store alone (tests point it at a local server).
std::unique_ptr<KeyframeStore> MakeHttpKeyframeStore(TicketSource tickets);
}  // namespace Orca::Net
