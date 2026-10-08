// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <ed25519.h>
#include <fmt/format.h>
#include <gtest/gtest.h>
#include <picojson.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Version.h"
#include "Core/Orca/Bundle.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Online.h"

namespace
{
using Orca::Mod::PublicKey;

void SetEnv(const char* name, const std::string& value)
{
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  if (value.empty())
    unsetenv(name);
  else
    setenv(name, value.c_str(), 1);
#endif
}

// Sets an environment variable for one scope.
class ScopedEnv
{
public:
  ScopedEnv(const char* name, const std::string& value) : m_name(name), m_old(Orca::GetEnv(name))
  {
    SetEnv(name, value);
  }
  ~ScopedEnv() { SetEnv(m_name, m_old); }

private:
  const char* m_name;
  std::string m_old;
};

std::string RepoPath(const std::string& relative)
{
  return (std::filesystem::path(__FILE__).parent_path() / "../../../../" / relative).string();
}

std::string Hex(const u8* data, size_t size)
{
  std::string hex;
  for (size_t i = 0; i < size; ++i)
    hex += fmt::format("{:02x}", data[i]);
  return hex;
}

std::vector<u8> FromHex(const std::string& hex)
{
  std::vector<u8> out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out.push_back(static_cast<u8>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return out;
}

// A signing key made from a fixed seed (Externals/ed25519, as the engine verifies).
struct Signer
{
  unsigned char public_key[32]{};
  unsigned char private_key[64]{};

  explicit Signer(u8 fill)
  {
    unsigned char seed[32];
    std::fill(std::begin(seed), std::end(seed), fill);
    ed25519_create_keypair(public_key, private_key, seed);
  }
  PublicKey Key() const
  {
    PublicKey key;
    std::copy(std::begin(public_key), std::end(public_key), key.begin());
    return key;
  }
  std::string Sign(const std::string& message) const
  {
    unsigned char signature[64];
    ed25519_sign(signature, reinterpret_cast<const unsigned char*>(message.data()), message.size(),
                 public_key, private_key);
    return fmt::format("ed25519 {} {}\n", Hex(public_key, 32), Hex(signature, 64));
  }
};

// Project+'s flat mod.json as make-bundle.py --flat writes it, for a card of `sd_bytes` with this
// XXH3-128.
std::string PPlusFlat(u64 sd_bytes, const std::string& xxh3)
{
  return fmt::format(R"({{
  "format": 2,
  "engine": {{"api": 1, "minOrca": "0.3.35"}},
  "title": "Project+",
  "disc": {{"id": "RSBE01", "revision": 2}},
  "launcher": "Project+ Netplay Launcher.dol",
  "sdImage": {{"file": "sd.raw", "bytes": {}, "xxh3": "{}"}},
  "patches": ["PPLUS32.patches"],
  "jitwarm": "PPLUS32.jitwarm",
  "textures": "engine",
  "frameHook": {{"address": "0x80017504", "word": "0x90170100"}},
  "bootNandFrame": 600,
  "firstKeyframeFrame": 300,
  "jitClearFrame": 2,
  "keepGameCode": [["0x8001CD24", "0x38631198", "0x806311A0"]],
  "saveTitle": "0x0001000052534245",
  "forced": {{"Dolphin.Core.CustomRTCValue": 1735689600}},
  "builtin": "brawl-rev2-pplus",
  "rulesets": ["pplus-2024"],
  "offers": ["results", "locks", "queue2"]
}})",
                     sd_bytes, xxh3);
}

constexpr char BRAWL_FLAT[] = R"({
  "format": 2,
  "engine": {"api": 1, "minOrca": "0.3.35"},
  "disc": {"id": "RSBE01", "revision": 2},
  "patches": ["RSBE01.patches"],
  "jitwarm": "RSBE01.jitwarm",
  "frameHook": {"address": "0x80017504", "word": "0x90170100"},
  "bootNandFrame": 1800,
  "builtin": "brawl-rev2",
  "rulesets": ["brawl"],
  "offers": ["results", "locks", "queue2"]
})";

const std::string PATCHES = "# A patch file with no patches.\n";

// A flat bundle folder: its files as written, then mod.sig over them (computed here, not with the
// engine's own helpers, so the two can disagree).
struct Flat
{
  std::string dir;
  std::map<std::string, std::string> files;  // every file but mod.sig, mod.json included
  std::string sd_name;                       // left out of the signed lines

  void Put(const std::string& name, const std::string& contents)
  {
    files[name] = contents;
    ASSERT_TRUE(File::WriteStringToFile(dir + name, contents)) << name;
  }
  std::string Message() const
  {
    std::string text = "orca-flat-bundle 1\n";
    for (const auto& [name, contents] : files)  // std::map: sorted by name, byte order
    {
      if (name != sd_name)
        text += fmt::format("{}\t{}\t{}\n", name, Orca::Mod::Sha256Hex(contents), contents.size());
    }
    return text;
  }
  void Sign(const Signer& signer)
  {
    ASSERT_TRUE(File::WriteStringToFile(dir + "mod.sig", signer.Sign(Message())));
  }
  std::optional<Orca::Mod::FlatBundle> Load(const Signer& signer, std::string* error) const
  {
    const PublicKey key = signer.Key();
    return Orca::Mod::LoadFlatBundle(dir, std::span(&key, 1), error);
  }
};

// Project+ (launcher, a small card, patches, JIT seed) in `dir`, signed.
Flat MakePPlus(const std::string& dir, const Signer& signer)
{
  Flat f{dir, {}, "sd.raw"};
  File::CreateFullPath(dir);
  f.Put("Project+ Netplay Launcher.dol", std::string(4096, '\x5a'));
  f.Put("sd.raw", std::string(65536, '\x11'));
  f.Put("PPLUS32.patches", PATCHES);
  f.Put("PPLUS32.jitwarm", "warm");
  f.Put("mod.json", PPlusFlat(65536, Orca::HashFile(dir + "sd.raw")));
  f.Sign(signer);
  return f;
}

Flat MakeBrawl(const std::string& dir, const Signer& signer)
{
  Flat f{dir, {}, ""};
  File::CreateFullPath(dir);
  f.Put("RSBE01.patches", PATCHES);
  f.Put("RSBE01.jitwarm", "warm");
  f.Put("mod.json", BRAWL_FLAT);
  f.Sign(signer);
  return f;
}

std::string TempDir()
{
  return File::CreateTempDir() + "/";
}

bool Refused(const std::string& json)
{
  std::string error;
  return !Orca::Mod::ParseManifest(json, &error) && !error.empty();
}

std::string With(std::string json, const std::string& key, const std::string& value)
{
  picojson::value v;
  picojson::parse(v, json);
  picojson::value field;
  EXPECT_EQ(picojson::parse(field, value), "") << value;
  v.get<picojson::object>()[key] = field;
  return v.serialize();
}

std::string Without(std::string json, const std::string& key)
{
  picojson::value v;
  picojson::parse(v, json);
  v.get<picojson::object>().erase(key);
  return v.serialize();
}
}  // namespace

// Python's signer (Tools/orca/mods/sign-bundle.py, ed25519ref.py) and the engine's verifier agree:
// the same key and signature from the same seed, and the message is the documented one.
TEST(OrcaBundle, SignatureVector)
{
  std::string text;
  ASSERT_TRUE(File::ReadFileToString(RepoPath("Tools/orca/mods/flat-sig-vector.json"), text));
  picojson::value v;
  ASSERT_EQ(picojson::parse(v, text), "");
  const std::vector<u8> seed = FromHex(v.get("seed").get<std::string>());
  ASSERT_EQ(seed.size(), 32u);
  unsigned char public_key[32], private_key[64], signature[64];
  ed25519_create_keypair(public_key, private_key, seed.data());
  EXPECT_EQ(Hex(public_key, 32), v.get("public").get<std::string>());
  const std::string message = v.get("message").get<std::string>();
  ed25519_sign(signature, reinterpret_cast<const unsigned char*>(message.data()), message.size(),
               public_key, private_key);
  EXPECT_EQ(Hex(signature, 64), v.get("signature").get<std::string>());

  const std::string sig_line = fmt::format("ed25519 {} {}\n", v.get("public").get<std::string>(),
                                           v.get("signature").get<std::string>());
  const auto parsed = Orca::Mod::ParseSignature(sig_line);
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(Orca::Mod::VerifySignature(parsed->first, parsed->second, message));
  EXPECT_FALSE(Orca::Mod::VerifySignature(parsed->first, parsed->second, message + "x"));
  // The vector's message is FlatSignedMessage's for those two lines.
  const std::vector<Orca::Mod::TreeFile> lines{
      {"PPLUS32.patches", Orca::Mod::Sha256Hex("# none\n"), 7},
      {"mod.json", Orca::Mod::Sha256Hex("{}"), 2}};
  EXPECT_EQ(Orca::Mod::FlatSignedMessage(lines), message);

  // mod.sig's form: one line, lowercase hex, one optional newline.
  EXPECT_TRUE(Orca::Mod::ParseSignature(sig_line.substr(0, sig_line.size() - 1)));
  EXPECT_FALSE(Orca::Mod::ParseSignature(sig_line + "\n"));
  EXPECT_FALSE(Orca::Mod::ParseSignature("ed25519  " + sig_line.substr(8)));
  std::string upper = sig_line;
  std::ranges::transform(upper, upper.begin(), [](char c) { return std::toupper(c); });
  EXPECT_FALSE(Orca::Mod::ParseSignature(upper));
  EXPECT_FALSE(Orca::Mod::ParseSignature(""));
  EXPECT_FALSE(Orca::Mod::ParseSignature(sig_line.substr(0, 100)));
}

// The pinned key is the TEST key unless the build sets one.
TEST(OrcaBundle, PinnedKey)
{
  const auto keys = Orca::Mod::TrustedKeys();
  ASSERT_EQ(keys.size(), 1u);
#ifndef ORCA_BUNDLE_PUBLIC_KEY
  EXPECT_TRUE(Orca::Mod::IsTestKey(keys[0]));
#endif
  const Signer other(7);
  EXPECT_FALSE(Orca::Mod::IsTestKey(other.Key()));
}

TEST(OrcaBundle, FlatNames)
{
  for (const char* good : {"Project+ Netplay Launcher.dol", "sd.raw", "RSBE01.patches",
                           "PPLUS32.jitwarm", "mod (1).patches", "a", "_x-y+z"})
  {
    EXPECT_FALSE(Orca::Mod::FlatNameProblem(good)) << good;
  }
  for (const std::string& bad :
       {std::string(""), std::string("../RSBE01.patches"), std::string("x/RSBE01.patches"),
        std::string("x\\RSBE01.patches"), std::string(".hidden"), std::string("a..b"),
        std::string("CON.patches"), std::string("com1"), std::string("x."), std::string("x "),
        std::string(" x"), std::string("mod.json"), std::string("MOD.SIG"),
        std::string("tree.json"), std::string(101, 'a'), std::string("caf\xc3\xa9"),
        std::string("a:b"), std::string("a*b"), std::string("tab\tname")})
  {
    EXPECT_TRUE(Orca::Mod::FlatNameProblem(bad)) << bad;
  }
}

TEST(OrcaBundle, ParsesTheFlatForm)
{
  const std::string pplus = PPlusFlat(2097152000, "5293a846d4de6139fd3ccd61faa9ac8d");
  std::string error;
  const auto m = Orca::Mod::ParseManifest(pplus, &error);
  ASSERT_TRUE(m) << error;
  EXPECT_EQ(m->format, 2);
  EXPECT_EQ(m->launcher, "Project+ Netplay Launcher.dol");
  ASSERT_TRUE(m->sd_image);
  EXPECT_EQ(m->sd_image->file, "sd.raw");
  EXPECT_EQ(m->sd_image->bytes, 2097152000u);
  EXPECT_EQ(m->sd_image->xxh3, "5293a846d4de6139fd3ccd61faa9ac8d");
  EXPECT_EQ(m->patches, std::vector<std::string>{"PPLUS32.patches"});
  EXPECT_EQ(m->jitwarm, "PPLUS32.jitwarm");
  EXPECT_FALSE(m->sd_bytes);
  EXPECT_EQ(m->FlatFiles(), (std::vector<std::string>{"Project+ Netplay Launcher.dol", "sd.raw",
                                                      "PPLUS32.patches", "PPLUS32.jitwarm"}));
  EXPECT_TRUE(Orca::Mod::ParseManifest(BRAWL_FLAT, &error)) << error;

  // Format 2 has no built card; format 1 has no flat files.
  EXPECT_TRUE(Refused(With(pplus, "sd", R"({"bytes": 2097152000})")));
  EXPECT_TRUE(Refused(With(pplus, "format", "1")));
  EXPECT_TRUE(Refused(With(pplus, "format", "3")));
  EXPECT_TRUE(Refused(With(Without(BRAWL_FLAT, "patches"), "format", "1") + ""));
  // Names: flat, never one of the bundle's own, never twice (also without case).
  EXPECT_TRUE(Refused(With(pplus, "launcher", R"("../Project+ Netplay Launcher.dol")")));
  EXPECT_TRUE(Refused(With(pplus, "launcher", R"("run/launcher.dol")")));
  EXPECT_TRUE(Refused(With(pplus, "launcher", R"("launcher.txt")")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "patches", R"(["../RSBE01.patches"])")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "patches", R"(["RSBE01.ini"])")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "patches", R"(["mod.json"])")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "patches", R"(["a.patches", "A.patches"])")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "jitwarm", R"("RSBE01.patches")")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "jitwarm", R"("mod.sig")")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "jitwarm", "1")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "patches", R"("RSBE01.patches")")));
  EXPECT_TRUE(Refused(
      With(BRAWL_FLAT, "patches", R"(["a.patches", "b.patches", "c.patches", "d.patches", "e.patches"])")));
  // At most eight files with mod.json and mod.sig: a launcher, a card, four patches and a seed are 9.
  EXPECT_TRUE(Refused(With(pplus, "patches", R"(["a.patches", "b.patches", "c.patches", "d.patches"])")));
  EXPECT_FALSE(Refused(With(pplus, "patches", R"(["a.patches", "b.patches", "c.patches"])")));
  // The pinned card.
  EXPECT_TRUE(Refused(With(pplus, "sdImage", R"({"file": "sd.raw", "bytes": 2147483649, "xxh3": "5293a846d4de6139fd3ccd61faa9ac8d"})")));
  EXPECT_TRUE(Refused(With(pplus, "sdImage", R"({"file": "sd.raw", "bytes": 0, "xxh3": "5293a846d4de6139fd3ccd61faa9ac8d"})")));
  EXPECT_TRUE(Refused(With(pplus, "sdImage", R"({"file": "sd.raw", "bytes": 100, "xxh3": "5293A846D4DE6139FD3CCD61FAA9AC8D"})")));
  EXPECT_TRUE(Refused(With(pplus, "sdImage", R"({"file": "sd.raw", "bytes": 100})")));
  EXPECT_TRUE(Refused(With(pplus, "sdImage", R"({"file": "sd/sd.raw", "bytes": 100, "xxh3": "5293a846d4de6139fd3ccd61faa9ac8d"})")));
  EXPECT_TRUE(Refused(With(pplus, "sdImage", R"({"file": "sd.raw", "bytes": 100, "xxh3": "5293a846d4de6139fd3ccd61faa9ac8d", "sha256": "x"})")));
  EXPECT_TRUE(Refused(With(pplus, "sdImage", R"({"file": "PPLUS32.patches", "bytes": 100, "xxh3": "5293a846d4de6139fd3ccd61faa9ac8d"})")));
  // Textures stay the engine's.
  EXPECT_TRUE(Refused(With(pplus, "textures", R"("bundle")")));
  EXPECT_FALSE(Refused(Without(pplus, "textures")));
}

TEST(OrcaBundle, LoadsASignedBundle)
{
  const Signer signer(1);
  const std::string root = TempDir();
  Flat p = MakePPlus(root + "pplus/", signer);
  // The app's own hidden file beside the bundle (YouGame desktop orca-files.ts) is left alone.
  ASSERT_TRUE(File::WriteStringToFile(p.dir + ".copies.json", "{}"));
  std::string error;
  const auto bundle = p.Load(signer, &error);
  ASSERT_TRUE(bundle) << error;
  EXPECT_EQ(bundle->dir, p.dir);
  EXPECT_EQ(bundle->sd_image_hash, Orca::HashFile(p.dir + "sd.raw"));
  ASSERT_EQ(bundle->patches.size(), 1u);
  EXPECT_EQ(bundle->patches[0].first, "PPLUS32.patches");
  EXPECT_EQ(bundle->patches[0].second, PATCHES);
  // The tree: every signed file by name; its hash leaves the JIT seed out.
  ASSERT_EQ(bundle->tree.files.size(), 4u);
  EXPECT_EQ(bundle->tree.files[0].path, "PPLUS32.jitwarm");
  EXPECT_EQ(bundle->tree.files[3].path, "mod.json");
  EXPECT_EQ(bundle->tree.hash, Orca::Mod::FlatTreeHash(bundle->tree.files, "PPLUS32.jitwarm"));
  EXPECT_NE(bundle->tree.hash, Orca::Mod::FlatTreeHash(bundle->tree.files, ""));

  const auto profile = Orca::LoadFlatProfile(*bundle, &error);
  ASSERT_TRUE(profile) << error;
  EXPECT_EQ(profile->game_id, "PPLUS32");
  EXPECT_TRUE(profile->IsLauncher());
  EXPECT_EQ(profile->disc, "RSBE01");
  EXPECT_EQ(profile->sd_image, "sd.raw");
  EXPECT_EQ(profile->jit_clear_frame, 2u);
  ASSERT_EQ(profile->forced.size(), 2u);
  EXPECT_EQ(profile->forced[0].first.key, "WiiSDCard");
  ASSERT_TRUE(profile->mod);
  EXPECT_TRUE(profile->mod->flat);
  EXPECT_TRUE(profile->mod->EngineTextures());
  EXPECT_EQ(profile->mod->JitWarmPath(), p.dir + "PPLUS32.jitwarm");
  EXPECT_EQ(profile->mod->sd_image_hash, bundle->sd_image_hash);

  // The launcher boot uses the card as it is, with the hash checked at load.
  auto boot = *profile;
  const std::string launcher = p.dir + "Project+ Netplay Launcher.dol";
  if (const char* disc = std::getenv("ORCA_TEST_DISC"))
  {
    bool wrong_disc = false;
    ASSERT_TRUE(Orca::PrepareModBoot(&boot, &launcher, disc, "", 0, &error, &wrong_disc)) << error;
    EXPECT_EQ(boot.sd_image_path, p.dir + "sd.raw");
    EXPECT_EQ(boot.sd_image_hash, bundle->sd_image_hash);
  }

  Flat b = MakeBrawl(root + "brawl/", signer);
  const auto brawl = b.Load(signer, &error);
  ASSERT_TRUE(brawl) << error;
  const auto brawl_profile = Orca::LoadFlatProfile(*brawl, &error);
  ASSERT_TRUE(brawl_profile) << error;
  EXPECT_EQ(brawl_profile->game_id, "RSBE01");
  EXPECT_FALSE(brawl_profile->IsLauncher());
  EXPECT_TRUE(brawl_profile->forced.empty());
  EXPECT_TRUE(brawl->sd_image_hash.empty());
  File::DeleteDirRecursively(root);
}

// Hostile bundles: each refused with a reason, whole (LoadFlatBundle returns nothing).
TEST(OrcaBundle, RefusesTamperedBundles)
{
  const Signer signer(1), stranger(2);
  int n = 0;
  const auto refused = [&](const std::string& what, const std::function<void(Flat&)>& tamper,
                           const std::string& reason, bool pplus = true) {
    const std::string root = TempDir();
    Flat f = pplus ? MakePPlus(root + "b/", signer) : MakeBrawl(root + "b/", signer);
    tamper(f);
    std::string error;
    const auto bundle = f.Load(signer, &error);
    EXPECT_FALSE(bundle) << what;
    EXPECT_NE(error.find(reason), std::string::npos) << what << ": " << error;
    File::DeleteDirRecursively(root);
    ++n;
  };
  // The untampered bundles load.
  {
    const std::string root = TempDir();
    std::string error;
    EXPECT_TRUE(MakePPlus(root + "b/", signer).Load(signer, &error)) << error;
    EXPECT_TRUE(MakeBrawl(root + "c/", signer).Load(signer, &error)) << error;
    File::DeleteDirRecursively(root);
  }

  const auto flip = [](Flat& f, const std::string& name, size_t at) {
    std::string bytes = f.files[name];
    bytes[at] ^= 0x01;
    ASSERT_TRUE(File::WriteStringToFile(f.dir + name, bytes));  // same size, one byte changed
  };
  refused("a patches byte", [&](Flat& f) { flip(f, "PPLUS32.patches", 3); }, "doesn't match");
  refused("a mod.json byte", [&](Flat& f) { flip(f, "mod.json", f.files["mod.json"].find("600") + 2); },
          "doesn't match");
  refused("a launcher byte", [&](Flat& f) { flip(f, "Project+ Netplay Launcher.dol", 100); },
          "doesn't match");
  refused("a JIT seed byte", [&](Flat& f) { flip(f, "PPLUS32.jitwarm", 0); }, "doesn't match");
  refused("an SD image byte", [&](Flat& f) { flip(f, "sd.raw", 40000); }, "not the SD image");
  refused("an SD image of another size",
          [&](Flat& f) { ASSERT_TRUE(File::WriteStringToFile(f.dir + "sd.raw", "short")); },
          "bytes, not the");
  refused("a signature byte",
          [&](Flat& f) {
            std::string sig;
            File::ReadFileToString(f.dir + "mod.sig", sig);
            sig[100] = sig[100] == 'a' ? 'b' : 'a';
            File::WriteStringToFile(f.dir + "mod.sig", sig);
          },
          "doesn't match");
  refused("another key", [&](Flat& f) { f.Sign(stranger); }, "doesn't trust");
  refused("no signature", [&](Flat& f) { File::Delete(f.dir + "mod.sig"); }, "mod.sig is missing");
  refused("a malformed signature",
          [&](Flat& f) { File::WriteStringToFile(f.dir + "mod.sig", "signed, honest\n"); },
          "mod.sig is not");
  refused("a huge signature",
          [&](Flat& f) { File::WriteStringToFile(f.dir + "mod.sig", std::string(4096, 'a')); },
          "mod.sig is over");
  refused("an extra file", [&](Flat& f) { f.Put("RSBE01.ini", "[Profile]\n"); }, "doesn't name it");
  refused("a missing file", [&](Flat& f) { File::Delete(f.dir + "PPLUS32.jitwarm"); },
          "PPLUS32.jitwarm is missing");
  refused("a folder", [&](Flat& f) { File::CreateDir(f.dir + "Textures"); }, "not a file");
  refused("nine files",
          [&](Flat& f) {
            for (const char* name : {"a.patches", "b.patches", "c.patches", "d.patches", "e.patches"})
              f.Put(name, PATCHES);
          },
          "at most 8", false);
  refused("too many entries",
          [&](Flat& f) {
            for (int i = 0; i < 70; ++i)
              File::WriteStringToFile(f.dir + fmt::format(".hidden{}", i), "");
          },
          "entries");
#ifndef _WIN32  // (a symbolic link needs an elevated or developer-mode Windows)
  refused("a link",
          [&](Flat& f) {
            File::Delete(f.dir + "PPLUS32.patches");
            std::error_code ec;
            std::filesystem::create_symlink(f.dir + "PPLUS32.jitwarm", f.dir + "PPLUS32.patches", ec);
            ASSERT_FALSE(ec) << ec.message();
          },
          "is a link");
#endif
  refused("a name that climbs out",
          [&](Flat& f) {
            f.Put("mod.json", With(f.files["mod.json"], "patches", R"(["../PPLUS32.patches"])"));
            f.Sign(signer);
          },
          "patches: ../PPLUS32.patches");
  refused("a named folder",
          [&](Flat& f) {
            f.Put("mod.json", With(f.files["mod.json"], "jitwarm", R"("orca/jitwarm")"));
            f.Sign(signer);
          },
          "jitwarm: a character");
  refused("a huge patches file",
          [&](Flat& f) {
            f.Put("PPLUS32.patches", std::string((1 << 20) + 1, '#'));
            f.Sign(signer);
          },
          "PPLUS32.patches is over");
  refused("a huge launcher (signed)",
          [&](Flat& f) {
            File::IOFile big(f.dir + "Project+ Netplay Launcher.dol", "r+b");
            ASSERT_TRUE(big.Resize((u64{64} << 20) + 1));
          },
          "Project+ Netplay Launcher.dol is over");
  refused("a huge mod.json",
          [&](Flat& f) { f.Put("mod.json", f.files["mod.json"] + std::string(64 << 10, ' ')); },
          "mod.json is over");
  refused("a format 1 mod.json",
          [&](Flat& f) {
            f.Put("mod.json", R"({"format": 1, "engine": {"api": 1, "minOrca": "0.4.0"},
              "disc": {"id": "RSBE01", "revision": 2},
              "frameHook": {"address": "0x80017504", "word": "0x90170100"}})");
            f.Sign(signer);
          },
          "format 1");
  refused("patches that don't parse",
          [&](Flat& f) {
            f.Put("PPLUS32.patches", "this is not a patch\n");
            f.Sign(signer);
          },
          "PPLUS32.patches:");
  refused("no mod.json", [&](Flat& f) { File::Delete(f.dir + "mod.json"); }, "no mod.json");
  EXPECT_GE(n, 24);
}

// The key: mod mode over the signed lines and the card; a JIT seed change keeps it, anything that
// reaches the game moves it; never a legacy key.
TEST(OrcaBundle, KeyMovesWithTheBundleOnly)
{
  const Signer signer(1);
  const std::string root = TempDir();
  std::string error;
  const auto key_of = [&](const Flat& f) {
    const auto bundle = f.Load(signer, &error);
    EXPECT_TRUE(bundle) << error;
    auto profile = Orca::LoadFlatProfile(*bundle, &error);
    EXPECT_TRUE(profile) << error;
    Orca::SetActiveProfile(profile);
    const std::string parts = Orca::Online::CompatibilityKeyParts();
    Orca::SetActiveProfile(std::nullopt);
    return parts;
  };
  Flat p = MakePPlus(root + "p/", signer);
  const std::string parts = key_of(p);
  const auto bundle = p.Load(signer, &error);
  ASSERT_TRUE(bundle);
  const std::string disc = Orca::GetEnv("ORCA_DISC_SHA1");
  EXPECT_TRUE(parts.starts_with(fmt::format("orca2|{}|api={}|tree={}|sd={}|disc={}|",
                                            Common::GetScmRevGitStr(), Orca::Mod::ENGINE_API,
                                            bundle->tree.hash, bundle->sd_image_hash,
                                            disc.empty() ? "-" : disc)))
      << parts;
  p.Put("PPLUS32.jitwarm", "warmer");
  p.Sign(signer);
  EXPECT_EQ(key_of(p), parts);
  p.Put("PPLUS32.patches", "# Another patch file with no patches.\n");
  p.Sign(signer);
  EXPECT_NE(key_of(p), parts);
  // Legacy: "orca|..." (OrcaMod.LegacyKeyIsUnchanged pins the rest).
  const auto legacy = Orca::LoadProfileFrom(RepoPath("Data/Sys/"), "RSBE01", 2, &error);
  ASSERT_TRUE(legacy) << error;
  Orca::SetActiveProfile(legacy);
  EXPECT_TRUE(Orca::Online::CompatibilityKeyParts().starts_with("orca|"));
  Orca::SetActiveProfile(std::nullopt);
  File::DeleteDirRecursively(root);
}

TEST(OrcaBundle, FindsItsFolder)
{
  const ScopedEnv no_dir("ORCA_MOD_DIR", "");
  const std::string root = TempDir();
  const std::string old_user = File::GetUserPath(D_USER_IDX);
  File::SetUserPath(D_USER_IDX, root + "user/");
  {
    const ScopedEnv game("YOUGAME_GAME", "open-brawl");
    EXPECT_EQ(Orca::Mod::FlatBundleDir(nullptr), root + "user/../run/open-brawl/");
    const std::string launcher = root + "run/project-rollback/Project+ Netplay Launcher.dol";
    EXPECT_EQ(Orca::Mod::FlatBundleDir(&launcher), root + "run/project-rollback/");
    const ScopedEnv dir("ORCA_MOD_DIR", root + "elsewhere");
    EXPECT_EQ(Orca::Mod::FlatBundleDir(nullptr), root + "elsewhere/");
    EXPECT_EQ(Orca::Mod::FlatBundleDir(&launcher), root + "elsewhere/");
  }
  for (const char* bad : {"", "../x", "Open-Brawl", "a/b", "a b"})
  {
    const ScopedEnv game("YOUGAME_GAME", bad);
    EXPECT_EQ(Orca::Mod::FlatBundleDir(nullptr), "") << bad;
  }
  File::SetUserPath(D_USER_IDX, old_user);
  File::DeleteDirRecursively(root);
}

// What a session decides at boot: a good bundle is mod mode; a refused one, or none, is legacy.
TEST(OrcaBundle, ResolvesOrFallsBack)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv no_dir("ORCA_MOD_DIR", "");
  const ScopedEnv game("YOUGAME_GAME", "open-brawl");
  const Signer signer(1);
  const PublicKey key = signer.Key();
  Orca::Mod::TrustKeysForTests(std::span(&key, 1));
  const std::string root = TempDir();
  const std::string old_user = File::GetUserPath(D_USER_IDX);
  File::SetUserPath(D_USER_IDX, root + "user/");
  File::CreateFullPath(root + "user/");
  using Orca::Mod::BundleKind;

  // Nothing resolved yet: as before bundles.
  Orca::Mod::ResetResolvedBundle();
  EXPECT_FALSE(Orca::Mod::ResolvedKind());
  EXPECT_FALSE(Orca::Mod::Requested());

  // No folder, or one with no mod.json: legacy.
  Orca::Mod::ResolveBundle(nullptr);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::None);
  EXPECT_FALSE(Orca::Mod::Requested());

  // Brawl's run folder with a signed bundle: mod mode, from that folder.
  Orca::Mod::ResetResolvedBundle();
  Flat b = MakeBrawl(root + "run/open-brawl/", signer);
  Orca::Mod::ResolveBundle(nullptr);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Flat);
  EXPECT_TRUE(Orca::Mod::Requested());
  ASSERT_NE(Orca::Mod::ResolvedFlat(), nullptr);
  EXPECT_EQ(Orca::Mod::Dir(), root + "user/../run/open-brawl/");
  // Once per process: a change after the check isn't seen until the next process.
  b.Put("RSBE01.patches", "# changed\n");
  Orca::Mod::ResolveBundle(nullptr);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Flat);

  // The same folder tampered with, in a new process: refused, legacy.
  Orca::Mod::ResetResolvedBundle();
  Orca::Mod::ResolveBundle(nullptr);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::None);
  EXPECT_FALSE(Orca::Mod::Requested());
  EXPECT_EQ(Orca::Mod::ResolvedFlat(), nullptr);
  EXPECT_EQ(Orca::Mod::Dir(), "");

  // Project+: the launcher's own folder; the boot must run the bundle's launcher.
  Flat p = MakePPlus(root + "run/project-rollback/", signer);
  const std::string launcher = p.dir + "Project+ Netplay Launcher.dol";
  Orca::Mod::ResetResolvedBundle();
  Orca::Mod::ResolveBundle(&launcher);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Flat);
  const std::string other = p.dir + "sd.raw";
  Orca::Mod::ResetResolvedBundle();
  Orca::Mod::ResolveBundle(&other);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::None);
  // A launcher bundle found for a disc boot (ORCA_MOD_DIR pointing at it): refused.
  {
    const ScopedEnv dir("ORCA_MOD_DIR", p.dir);
    Orca::Mod::ResetResolvedBundle();
    Orca::Mod::ResolveBundle(nullptr);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::None);
  }
  // Signed by a key the build doesn't pin: refused.
  Orca::Mod::TrustKeysForTests({});
  Orca::Mod::ResetResolvedBundle();
  Orca::Mod::ResolveBundle(&launcher);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::None);
  Orca::Mod::TrustKeysForTests(std::span(&key, 1));

  // ORCA_MOD_DIR with a tree.json: mods phase 1's tree bundle, as before.
  {
    File::CreateFullPath(root + "tree/");
    File::WriteStringToFile(root + "tree/tree.json", "{}");
    const ScopedEnv dir("ORCA_MOD_DIR", root + "tree");
    Orca::Mod::ResetResolvedBundle();
    Orca::Mod::ResolveBundle(nullptr);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Tree);
    EXPECT_TRUE(Orca::Mod::Requested());
    EXPECT_EQ(Orca::Mod::Dir(), root + "tree/");
  }
  // Outside a session nothing is looked at.
  {
    const ScopedEnv no_session("ORCA_SESSION", "");
    Orca::Mod::ResetResolvedBundle();
    Orca::Mod::ResolveBundle(&launcher);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::None);
    EXPECT_FALSE(Orca::Mod::Requested());
  }
  Orca::Mod::ResetResolvedBundle();
  Orca::Mod::TrustKeysForTests({});
  File::SetUserPath(D_USER_IDX, old_user);
  File::DeleteDirRecursively(root);
}
