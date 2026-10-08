// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Bundle.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <system_error>

#include <ed25519.h>
#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/GamePatches.h"

namespace Orca::Mod
{
namespace
{
// TEST KEY (bundle-flat branch, 2026-10-08): trusted only when a build pins no key of its own
// (CMake -DORCA_BUNDLE_PUBLIC_KEY=<64 hex digits>). Its private half was made for the branch's tests
// and was never committed; a release must pin the real key, never this one.
constexpr std::string_view TEST_PUBLIC_KEY =
    "afb9463d5c16f0f617432d76f75f37a8c649e36d7c626088ba00b19dea115a43";

std::optional<u8> Nibble(char c)
{
  if (c >= '0' && c <= '9')
    return static_cast<u8>(c - '0');
  if (c >= 'a' && c <= 'f')
    return static_cast<u8>(c - 'a' + 10);
  return std::nullopt;
}

// Lowercase hex of exactly N bytes.
template <size_t N>
std::optional<std::array<u8, N>> FromHex(std::string_view hex)
{
  if (hex.size() != 2 * N)
    return std::nullopt;
  std::array<u8, N> out{};
  for (size_t i = 0; i < N; ++i)
  {
    const auto hi = Nibble(hex[2 * i]), lo = Nibble(hex[2 * i + 1]);
    if (!hi || !lo)
      return std::nullopt;
    out[i] = static_cast<u8>(*hi << 4 | *lo);
  }
  return out;
}

// A file of the bundle read whole, refused over max_bytes before anything is read.
bool ReadBounded(const std::string& path, u64 max_bytes, std::string* out, std::string* why)
{
  File::IOFile f(path, "rb");
  if (!f)
  {
    *why = "can't be read";
    return false;
  }
  const u64 size = f.GetSize();
  if (size > max_bytes)
  {
    *why = fmt::format("is over {} bytes", max_bytes);
    return false;
  }
  out->resize(size);
  if (size != 0 && !f.ReadBytes(out->data(), size))
  {
    *why = "can't be read";
    return false;
  }
  return true;
}

struct Resolution
{
  std::string looked_in;
  BundleKind kind = BundleKind::None;
  std::optional<FlatBundle> flat;
};
std::optional<Resolution> s_resolved;
std::vector<PublicKey> s_test_keys;
}  // namespace

std::string FlatSignedMessage(std::span<const TreeFile> files)
{
  std::string text(FLAT_SIGNED_HEADER);
  for (const TreeFile& f : files)
    text += fmt::format("{}\t{}\t{}\n", f.path, f.sha256, f.bytes);
  return text;
}

std::string FlatTreeHash(std::span<const TreeFile> files, std::string_view jitwarm)
{
  std::string text(FLAT_SIGNED_HEADER);
  for (const TreeFile& f : files)
  {
    if (f.path != jitwarm)
      text += fmt::format("{}\t{}\t{}\n", f.path, f.sha256, f.bytes);
  }
  return Sha256Hex(text);
}

std::optional<std::pair<PublicKey, Signature>> ParseSignature(std::string_view text)
{
  if (text.ends_with("\r\n"))
    text.remove_suffix(2);
  else if (text.ends_with('\n'))
    text.remove_suffix(1);
  constexpr std::string_view PREFIX = "ed25519 ";
  if (!text.starts_with(PREFIX))
    return std::nullopt;
  text.remove_prefix(PREFIX.size());
  if (text.size() != 64 + 1 + 128 || text[64] != ' ')
    return std::nullopt;
  const auto key = FromHex<32>(text.substr(0, 64));
  const auto signature = FromHex<64>(text.substr(65));
  if (!key || !signature)
    return std::nullopt;
  return std::make_pair(*key, *signature);
}

bool VerifySignature(const PublicKey& key, const Signature& signature, std::string_view message)
{
  return ed25519_verify(signature.data(), reinterpret_cast<const unsigned char*>(message.data()),
                        message.size(), key.data()) == 1;
}

void TrustKeysForTests(std::span<const PublicKey> keys)
{
  s_test_keys.assign(keys.begin(), keys.end());
}

std::span<const PublicKey> TrustedKeys()
{
  if (!s_test_keys.empty())
    return s_test_keys;
  static const std::vector<PublicKey> keys = [] {
    std::vector<PublicKey> out;
#ifdef ORCA_BUNDLE_PUBLIC_KEY
    const std::string_view pinned = ORCA_BUNDLE_PUBLIC_KEY;
#else
    const std::string_view pinned = TEST_PUBLIC_KEY;
#endif
    if (const auto key = FromHex<32>(pinned))
      out.push_back(*key);
    else
      ERROR_LOG_FMT(BOOT, "Orca bundle: the pinned key is not 64 lowercase hex digits: no bundle "
                          "will be trusted");
    return out;
  }();
  return keys;
}

bool IsTestKey(const PublicKey& key)
{
  return FromHex<32>(TEST_PUBLIC_KEY) == key;
}

std::string KeyHex(const PublicKey& key)
{
  std::string hex;
  for (const u8 b : key)
    hex += fmt::format("{:02x}", b);
  return hex;
}

std::optional<FlatBundle> LoadFlatBundle(const std::string& dir_in, std::span<const PublicKey> keys,
                                         std::string* error)
{
  namespace fs = std::filesystem;
  const auto fail = [error](std::string why) -> std::optional<FlatBundle> {
    *error = std::move(why);
    return std::nullopt;
  };
  std::string dir = dir_in;
  if (dir.empty())
    return fail("no folder");
  if (dir.back() != '/' && dir.back() != '\\')
    dir += '/';

  // The folder: regular files only (hidden ones aside: the app's .copies.json, a .DS_Store), no
  // more than a flat bundle holds. Nothing is read yet.
  std::set<std::string> present;
  {
    std::error_code ec;
    size_t entries = 0;
    for (fs::directory_iterator it(StringToPath(dir), ec), end; !ec && it != end; it.increment(ec))
    {
      if (++entries > MAX_FOLDER_ENTRIES)
        return fail(fmt::format("more than {} entries in the folder", MAX_FOLDER_ENTRIES));
      const std::string name = PathToString(it->path().filename());
      if (name.starts_with('.'))
        continue;
      std::error_code status_ec;
      const fs::file_status status = it->symlink_status(status_ec);
      if (status_ec)
        return fail(fmt::format("{} can't be read", name));
      if (fs::is_symlink(status))
        return fail(fmt::format("{} is a link (a bundle's files are its own)", name));
      if (!fs::is_regular_file(status))
        return fail(fmt::format("{} is not a file (a flat bundle has no folders)", name));
      present.insert(name);
    }
    if (ec)
      return fail(fmt::format("the folder can't be read ({})", ec.message()));
  }
  if (present.size() > FLAT_MAX_FILES)
  {
    return fail(fmt::format("{} files in the folder; a flat bundle has at most {}", present.size(),
                            FLAT_MAX_FILES));
  }

  // mod.json, format 2.
  std::string json, why;
  if (!present.contains(std::string(FLAT_MANIFEST)))
    return fail("no mod.json");
  if (!ReadBounded(dir + std::string(FLAT_MANIFEST), MAX_MOD_JSON_BYTES, &json, &why))
    return fail("mod.json " + why);
  std::string parse_error;
  std::optional<Manifest> manifest = ParseManifest(json, &parse_error);
  if (!manifest)
    return fail(parse_error);
  if (manifest->format != 2)
    return fail("mod.json is format 1 (a tree bundle's); a flat bundle's is format 2");

  // Exactly the files mod.json names, mod.json and mod.sig.
  const std::vector<std::string> named = manifest->FlatFiles();
  std::set<std::string> expected(named.begin(), named.end());
  expected.emplace(FLAT_MANIFEST);
  expected.emplace(FLAT_SIGNATURE);
  for (const std::string& name : present)
  {
    if (!expected.contains(name))
      return fail(fmt::format("{} is in the folder, and mod.json doesn't name it", name));
  }
  for (const std::string& name : expected)
  {
    if (!present.contains(name))
      return fail(fmt::format("{} is missing", name));
  }

  // mod.sig, by a key this Orca trusts.
  std::string sig_text;
  if (!ReadBounded(dir + std::string(FLAT_SIGNATURE), MAX_SIG_BYTES, &sig_text, &why))
    return fail("mod.sig " + why);
  const auto signature = ParseSignature(sig_text);
  if (!signature)
    return fail("mod.sig is not \"ed25519 <public key> <signature>\" in lowercase hex");
  if (std::ranges::find(keys, signature->first) == keys.end())
  {
    return fail(fmt::format("mod.sig is by key {}, which this Orca doesn't trust",
                            KeyHex(signature->first)));
  }

  // Every file but the SD image, read (bounded) and hashed; the patches are kept as read.
  FlatBundle bundle;
  bundle.dir = dir;
  std::vector<TreeFile> files;
  files.push_back({std::string(FLAT_MANIFEST), Sha256Hex(json), json.size()});
  for (const std::string& name : named)
  {
    if (manifest->sd_image && name == manifest->sd_image->file)
      continue;
    const bool patches = std::ranges::find(manifest->patches, name) != manifest->patches.end();
    const u64 limit = name == manifest->launcher ? MAX_LAUNCHER_BYTES :
                      name == manifest->jitwarm  ? MAX_JITWARM_BYTES :
                                                   MAX_PATCHES_BYTES;
    std::string contents;
    if (!ReadBounded(dir + name, limit, &contents, &why))
      return fail(fmt::format("{} {}", name, why));
    files.push_back({name, Sha256Hex(contents), contents.size()});
    if (patches)
      bundle.patches.emplace_back(name, std::move(contents));
  }
  std::ranges::sort(files, {}, &TreeFile::path);
  if (!VerifySignature(signature->first, signature->second, FlatSignedMessage(files)))
  {
    return fail("mod.sig doesn't match the files (one changed since the bundle was signed, or the "
                "signature is damaged)");
  }

  // Signed, so these parse; checked anyway rather than run half of them.
  for (const auto& [name, text] : bundle.patches)
  {
    std::string patch_error;
    if (!UX::ParseGamePatches(text, &patch_error))
      return fail(fmt::format("{}: {}", name, patch_error));
  }

  // The SD image last (up to 2 GiB): its size, then its XXH3-128, as mod.json pins them.
  if (manifest->sd_image)
  {
    const SdImageFile& image = *manifest->sd_image;
    const std::string path = dir + image.file;
    const u64 size = File::GetSize(path);
    if (size != image.bytes)
    {
      return fail(fmt::format("{} is {} bytes, not the {} mod.json pins", image.file, size,
                              image.bytes));
    }
    const std::string hash = HashFile(path);
    if (hash != image.xxh3)
    {
      return fail(fmt::format("{} is not the SD image mod.json pins (XXH3-128 {}, not {})",
                              image.file, hash.empty() ? "unreadable" : hash, image.xxh3));
    }
    bundle.sd_image_hash = hash;
  }

  bundle.tree.hash = FlatTreeHash(files, manifest->jitwarm);
  bundle.tree.files = std::move(files);
  bundle.manifest = std::move(*manifest);
  bundle.signer = signature->first;
  return bundle;
}

std::string FlatBundleDir(const std::string* executable_path)
{
  std::string dir = GetEnv("ORCA_MOD_DIR");
  if (dir.empty())
  {
    if (executable_path)
    {
      SplitPath(*executable_path, &dir, nullptr, nullptr);
    }
    else
    {
      // A YouGame listing's slug (desktop orca-policy.ts SLUG_RE).
      const std::string slug = GetEnv("YOUGAME_GAME");
      if (slug.empty() || slug.size() > 80 || !std::ranges::all_of(slug, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
          }))
      {
        return {};
      }
      dir = File::GetUserPath(D_USER_IDX);
      if (dir.empty())
        return {};
      if (dir.back() != '/' && dir.back() != '\\')
        dir += '/';
      dir += "../run/" + slug;
    }
  }
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
    dir += '/';
  return dir;
}

void ResolveBundle(const std::string* executable_path)
{
  const std::string dir = FlatBundleDir(executable_path);
  // Once per process: a second boot from the same folder keeps the first check (and its hashes).
  if (s_resolved && s_resolved->looked_in == dir)
    return;
  Resolution resolution;
  resolution.looked_in = dir;
  const bool explicit_dir = !GetEnv("ORCA_MOD_DIR").empty();
  const auto decide = [&](BundleKind kind) {
    resolution.kind = kind;
    s_resolved = std::move(resolution);
  };
  if (!SessionActive() || dir.empty())
    return decide(BundleKind::None);
  // Mods phase 1: a run folder the app wrote with tree.json (checked by LoadModProfile at boot).
  if (explicit_dir && File::Exists(dir + "tree.json"))
    return decide(BundleKind::Tree);
  if (!File::Exists(dir + std::string(FLAT_MANIFEST)))
  {
    if (explicit_dir)
    {
      ERROR_LOG_FMT(BOOT, "Orca bundle: ORCA_MOD_DIR {} has neither tree.json nor mod.json: "
                          "playing the built-in game",
                    dir);
    }
    return decide(BundleKind::None);
  }

  const auto start = std::chrono::steady_clock::now();
  std::string why;
  // (Every filesystem call there takes an error code: nothing in it throws.)
  std::optional<FlatBundle> bundle = LoadFlatBundle(dir, TrustedKeys(), &why);
  // The boot this folder was found for: a launcher boot runs the bundle's launcher, a disc boot a
  // bundle without one (BootManager's PrepareModBoot would refuse the session instead).
  if (bundle && executable_path && PathToFileName(*executable_path) != bundle->manifest.launcher)
  {
    why = fmt::format("the boot runs {}, not the bundle's launcher ({})",
                      PathToFileName(*executable_path),
                      bundle->manifest.launcher.empty() ? "none" : bundle->manifest.launcher);
    bundle.reset();
  }
  else if (bundle && !executable_path && !bundle->manifest.launcher.empty())
  {
    why = fmt::format("the bundle boots through its launcher ({}), and this boot is a disc's",
                      bundle->manifest.launcher);
    bundle.reset();
  }
  if (!bundle)
  {
    ERROR_LOG_FMT(BOOT, "Orca bundle: {} refused: {}. Playing the built-in game.", dir, why);
    return decide(BundleKind::None);
  }
  NOTICE_LOG_FMT(BOOT,
                 "Orca bundle: {} ({} files, tree {}, SD {}), signed by {}{}, checked in {} ms", dir,
                 bundle->tree.files.size() + (bundle->manifest.sd_image ? 2 : 1),
                 bundle->tree.hash,
                 bundle->sd_image_hash.empty() ? "none" : bundle->sd_image_hash,
                 KeyHex(bundle->signer),
                 IsTestKey(bundle->signer) ? " (the TEST key: not for a release)" : "",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count());
  resolution.flat = std::move(bundle);
  decide(BundleKind::Flat);
}

std::optional<BundleKind> ResolvedKind()
{
  return s_resolved ? std::optional(s_resolved->kind) : std::nullopt;
}

std::string ResolvedDir()
{
  return s_resolved && s_resolved->kind != BundleKind::None ? s_resolved->looked_in : std::string{};
}

const FlatBundle* ResolvedFlat()
{
  return s_resolved && s_resolved->flat ? &*s_resolved->flat : nullptr;
}

void ResetResolvedBundle()
{
  s_resolved.reset();
}
}  // namespace Orca::Mod
