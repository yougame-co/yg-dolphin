// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Mod.h"

// Orca flat bundles: a game's data (its profile, patches, JIT seed and, for a launcher game, the
// launcher and the SD image) as at most eight files in one folder, signed. They ride as a YouGame
// listing's runtime files, which the app lays out in userData/orca/run/<slug>/, so Project+ and Brawl
// can change without an Orca release or an app release.
//
//   mod.json   format 2 (Mod.h ParseManifest): the profile, and the name of every other file
//   mod.sig    "ed25519 <public key> <signature>\n", 64 and 128 lowercase hex digits
//   ...        the files mod.json names, nothing else (hidden files such as the app's .copies.json
//              aside)
//
// The signature is over FLAT_SIGNED_HEADER and one "<name>\t<sha256>\t<bytes>\n" line for mod.json and
// every file it names but the SD image, sorted by name: mod.json pins the image by size and
// XXH3-128, as a legacy profile pins sd.raw. Only a key this Orca pins (TrustedKeys) may sign.
//
// A bundle is hostile input: every read is bounded, every name checked, and anything wrong refuses
// the whole bundle with a log line; the session then plays the built-in game (Data/Sys/Orca), never
// part of a bundle. A session finds the folder itself (FlatBundleDir), checks it once per process
// (ResolveBundle), and plays it in mod mode: its key is "orca2|..." over the signed lines (the JIT
// seed's left out: host-only) and the SD image's hash, so it never meets a legacy player.
namespace Orca::Mod
{
inline constexpr std::string_view FLAT_SIGNED_HEADER = "orca-flat-bundle 1\n";
inline constexpr u64 MAX_SIG_BYTES = 512;
// A folder with more entries than this is refused before anything in it is read.
inline constexpr size_t MAX_FOLDER_ENTRIES = 64;

using PublicKey = std::array<u8, 32>;
using Signature = std::array<u8, 64>;

struct FlatBundle
{
  std::string dir;  // with a trailing '/'
  Manifest manifest;
  // mod.json and every file it names but the SD image, sorted by name, as verified; `hash` is the
  // key's: the hash of those lines without the JIT seed's.
  Tree tree;
  std::vector<std::pair<std::string, std::string>> patches;  // (name, text), mod.json's order
  std::string sd_image_hash;  // the SD image's XXH3-128 as checked; empty without one
  PublicKey signer{};
};

// What mod.sig signs.
std::string FlatSignedMessage(std::span<const TreeFile> files);
// The key's tree hash for a flat bundle: TreeHash's lines for every file but the JIT seed.
std::string FlatTreeHash(std::span<const TreeFile> files, std::string_view jitwarm);
// mod.sig's contents, or nullopt.
std::optional<std::pair<PublicKey, Signature>> ParseSignature(std::string_view text);
bool VerifySignature(const PublicKey& key, const Signature& signature, std::string_view message);

// The keys a bundle may be signed with: the one this build pins (CMake ORCA_BUNDLE_PUBLIC_KEY), or,
// without one, the TEST key of the bundle-flat branch, whose private half never left the machine it
// was made on. A release must pin its own.
std::span<const PublicKey> TrustedKeys();
// Tests: trust these keys instead (empty: the pinned one again).
void TrustKeysForTests(std::span<const PublicKey> keys);
bool IsTestKey(const PublicKey& key);
std::string KeyHex(const PublicKey& key);

// Checks the folder `dir` as a flat bundle signed by one of `keys`: nullopt and why on anything
// wrong. Hashes the SD image (up to 2 GiB) only once everything else, the signature included, holds.
std::optional<FlatBundle> LoadFlatBundle(const std::string& dir, std::span<const PublicKey> keys,
                                         std::string* error);

// Where a session looks for a flat bundle: ORCA_MOD_DIR when set; else a launcher boot's own
// folder (where Project+'s sd.raw already lies); else, for a disc boot of a YouGame listing, the
// app's run folder beside the user folder: <-u user>/../run/<YOUGAME_GAME>/ (YouGame desktop
// orca.ts: userData/orca/{user,run/<slug>}). Empty when there is nowhere to look.
std::string FlatBundleDir(const std::string* executable_path);

// The session's bundle, decided once per process at boot (BootManager, before any game setting is
// read): a tree bundle (ORCA_MOD_DIR with a tree.json, mods phase 1, unchanged), a flat bundle that
// passed LoadFlatBundle, or none (legacy: no mod.json there, or one that was refused).
enum class BundleKind
{
  None,
  Tree,
  Flat,
};
void ResolveBundle(const std::string* executable_path);
// nullopt before ResolveBundle ran.
std::optional<BundleKind> ResolvedKind();
std::string ResolvedDir();
const FlatBundle* ResolvedFlat();
// Tests: forget the decision.
void ResetResolvedBundle();
}  // namespace Orca::Mod
