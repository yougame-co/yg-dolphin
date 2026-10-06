// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

// Orca mods, phase 1 (YouGame docs/ORCA_MODS.md, "Phase 1 contract"): a session reads its game from
// a mod bundle laid out as a run folder (ORCA_MOD_DIR) instead of Data/Sys/Orca/<profile>.*:
//
//   mod.json        engine settings and what the mod offers (ParseManifest)
//   tree.json       written by the app: {"tree": "<treeHash>", "files": [{path, sha256, bytes}...]}
//   launcher.dol    when mod.json names one
//   sd/...          the SD card as files, when mod.json has "sd" (built into an image, BuildSdImage)
//   orca/patches/<name>.patches, orca/textures/..., orca/jitwarm
//
// Without ORCA_MOD_DIR nothing here runs and Orca is exactly what it was (legacy profiles).
namespace Orca::Mod
{
// The script API version this engine implements; mod.json's engine.api must not be above it.
inline constexpr int ENGINE_API = 1;
// The SD image builder's version, in the cached image's name and the key (through the image hash).
inline constexpr std::string_view SD_BUILDER = "fat1";
// An SD card is SDSC: at most 2 GiB (above, Dolphin presents SDHC, which needs the game's IOS to
// support SDv2, and the card's size reaches emulated memory through the CSD). Its size is whole
// 32 KiB clusters (FatFsUtil's largest cluster).
inline constexpr u64 SD_MAX_BYTES = u64{2} << 30;
inline constexpr u64 SD_MIN_BYTES = u64{64} << 20;
inline constexpr u64 SD_ALIGN = 32768;
// A bundle path (the contract's 100 characters: the run folder userData\orca\run\<slug>\<hash>\
// takes about 135 on Windows, and laid-out paths stay under 240).
inline constexpr size_t MAX_PATH_BYTES = 100;
inline constexpr size_t MAX_FILES = 20000;
// FAT's limit for one file.
inline constexpr u64 MAX_FILE_BYTES = (u64{4} << 30) - 1;
// Today's FIRST_KEYFRAME_FRAME (Rollback/OnlineMatch.cpp) and FIRST_FRIEND_FRAME (UX/OnlineMenu.h).
inline constexpr u32 DEFAULT_FIRST_KEYFRAME_FRAME = 300;

// --- The tree ----------------------------------------------------------------------------------

struct TreeFile
{
  std::string path;
  std::string sha256;  // 64 lowercase hex digits
  u64 bytes = 0;
  bool operator==(const TreeFile&) const = default;
};

struct Tree
{
  std::string hash;  // the treeHash, 64 lowercase hex digits
  std::vector<TreeFile> files;  // sorted by path, byte order

  const TreeFile* Find(std::string_view path) const;
  // The files under a folder ("sd/"), in tree order.
  std::vector<const TreeFile*> Under(std::string_view folder) const;
};

// Why a bundle path is refused, or nullopt when it is fine: printable ASCII without <>:"\|?*, '/'
// between non-empty components, none of them `.`/`..`, starting with a dot, ending in a dot or a
// space, or a Windows device name (CON, NUL, COM1, ...), at most MAX_PATH_BYTES bytes. A bundle never
// holds a tree.json of its own (the app writes the run folder's), so ParseTree refuses one listed.
std::optional<std::string> PathProblem(std::string_view path);
// Files the tree hash leaves out: orca/jitwarm and orca/test/... (host-only).
bool HostOnly(std::string_view path);
// The contract's treeHash: lowercase hex SHA-256 of "<path>\t<sha256>\t<bytes>\n" for every file
// but the host-only ones, in the files' order (callers keep them sorted by path).
std::string TreeHash(std::span<const TreeFile> files);
// sha256 of "bytes=<n> cluster=<c> fats=<f>\n" and the SD card's tree lines (the cached image's
// name: everything the build depends on but the builder's version).
std::string SdTreeHash(const Tree& tree, u64 bytes, u32 cluster_bytes = 0, u8 fats = 1);
std::string Sha256Hex(std::string_view data);
// tree.json: every path valid and unique (also without case, and never both a file and a folder),
// sorted, and "tree" equal to the hash of the list.
std::optional<Tree> ParseTree(std::string_view json, std::string* error);

// --- mod.json ----------------------------------------------------------------------------------

// The compiled UX modules a mod runs (phase 1), for a known game executable. Each turns on only
// while its guard words hold (BuiltinGuards).
enum class Builtin : u8
{
  None,
  BrawlRev2,       // "brawl-rev2": vanilla Brawl (NTSC-U rev 2)
  BrawlRev2PPlus,  // "brawl-rev2-pplus": Project+ on Brawl rev 2
};
std::optional<Builtin> BuiltinFromName(std::string_view name);
std::string_view BuiltinName(Builtin builtin);

struct KeptWord
{
  u32 address = 0;
  u32 game = 0;
  u32 loader = 0;
  bool operator==(const KeptWord&) const = default;
};

struct Manifest
{
  int format = 0;
  int engine_api = 0;
  std::string min_orca;
  std::string title;
  std::string disc_id;
  u16 disc_revision = 0;
  std::string launcher;           // a file name at the bundle's root, or empty
  std::optional<u64> sd_bytes;    // the SD card's pinned size
  u32 sd_cluster_bytes = 0;       // its cluster (0: FatFs's for the size)
  u8 sd_fats = 1;                 // its FATs
  u32 frame_hook = 0;
  u32 frame_hook_word = 0;
  std::optional<u32> boot_nand_frame;
  u32 first_keyframe_frame = DEFAULT_FIRST_KEYFRAME_FRAME;
  std::optional<u32> jit_clear_frame;
  std::vector<KeptWord> keep_game_code;
  std::optional<u64> save_title;
  // <System>.<Section>.<Key> = value, keys from ForcedAllowlist() only, in key order.
  std::vector<std::pair<std::string, std::string>> forced;
  Builtin builtin = Builtin::None;
  std::vector<std::string> rulesets;
  std::vector<std::string> offers;
};

// The `forced` keys a mod may set. Switches that determinism or safety depend on (dual core, FMA,
// the RTC switch, SD writes and folder sync, SKIP_EFB_COPY_TO_RAM, cheats, graphics mods) are never
// on it; the SD card itself follows mod.json's "sd".
std::span<const std::string_view> ForcedAllowlist();
// The caps a mod may offer (Status.h).
std::span<const std::string_view> OfferableCaps();
// The disc's own title ID (a Wii disc: 00010000 and its 4-character game code).
u64 DiscTitleId(std::string_view disc_id);

// Every field validated; anything unknown or malformed refuses (nullopt and why).
std::optional<Manifest> ParseManifest(std::string_view json, std::string* error);

// --- The session -------------------------------------------------------------------------------

// ORCA_MOD_DIR is set in a session: the boot loads the game from that bundle (mod mode).
bool Requested();
// ORCA_MOD_DIR with a trailing '/', or empty.
std::string Dir();

// What a mod-mode profile carries besides the legacy fields (Profile::mod).
struct ModInfo
{
  std::string dir;  // with a trailing '/'
  Tree tree;
  Manifest manifest;
  // The built SD image's XXH3-128 (32 hex digits), set at boot; empty without "sd".
  std::string sd_image_hash;
};
// The running profile's mod: null in legacy mode (a profile from Data/Sys/Orca) or outside a
// session. Every mod-mode branch keys on this, so legacy sessions run exactly the code they ran.
const ModInfo* ActiveMod();

// The SD card for a mod with "sd": the image built from exactly the tree's sd/ files (never a
// directory scan), deterministic FAT (sorted tree, zero times), `bytes` long, cached in
// `cache_dir` as <SdTreeHash>-<SD_BUILDER>.raw (written atomically, reused while its hash matches
// the one recorded when it was built, at most the newest two kept). On success, the image's path
// and its XXH3-128.
struct SdImage
{
  std::string path;
  std::string hash;
  bool built = false;  // built now rather than reused
};
std::optional<SdImage> PrepareSdImage(const ModInfo& mod, const std::string& cache_dir,
                                      std::string* error);
// The builder itself: `files` are (path in the image, source path, bytes), sorted by image path.
struct SdSource
{
  std::string image_path;
  std::string source_path;
  u64 bytes = 0;
};
bool BuildSdImage(std::span<const SdSource> files, u64 bytes, const std::string& out_path,
                  std::string* error, u32 cluster_bytes = 0, u8 fats = 1);

// --- Builtins and their guards -----------------------------------------------------------------

// A word a builtin's modules rely on: static code (or data) of the executable they were written
// against, which a mod whose codes moved something no longer has.
struct GuardWord
{
  u32 address = 0;
  u32 word = 0;
};
std::span<const GuardWord> BuiltinGuards(Builtin builtin);
// Whether every guard word holds, reading memory through `read` (nullopt: not mapped).
bool GuardsHold(Builtin builtin, const std::function<std::optional<u32>(u32)>& read);
// Mod mode, on the CPU thread at every entry into the UX modules (the frame callback, with its
// frame, and the input gate's source and latch): checks the active mod's guards against memory as
// it is now. The modules run only while they hold. Not holding is a failure (GuardsFailed) once they
// held before, or still at the frame callback's firstKeyframeFrame: until then a loader's codes may
// not have landed yet.
void RefreshGuards(const Core::CPUThreadGuard& guard, std::optional<int> frame = std::nullopt);
// A new boot's profile: nothing checked yet (Profile.cpp SetActiveProfile).
void ResetGuards();
// What RefreshGuards does with its reading of memory (tests drive it directly).
void NoteGuards(bool held, std::optional<int> frame);
// Mod mode: whether the builtin's caps are settled for now: no builtin, or its guards have held, or
// they failed. Until then `orca caps` and "ready" wait (Status::CapsSettled).
bool GuardsDecided();
// Mod mode: the active mod's builtin is `builtin` and its guards held at the last refresh.
bool BuiltinOn(Builtin builtin);
// Mod mode: the active mod's builtin, guards or not (host-only readers: the JIT warm-up's and the
// harness's scene readers).
Builtin DeclaredBuiltin();
// Mod mode: the guards were checked and failed at least once since boot (the caps they gate are
// then not offered, if they haven't been printed yet).
bool GuardsFailed();
// Whether this mod offers `cap` (mod.json "offers"), its builtin supports it, and its guards have
// held since boot and never failed.
bool Offers(std::string_view cap);

// Today's FIRST_KEYFRAME_FRAME / FIRST_FRIEND_FRAME, or mod.json's firstKeyframeFrame.
u32 FirstKeyframeFrame();
}  // namespace Orca::Mod
