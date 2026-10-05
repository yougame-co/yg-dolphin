// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Mod.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <set>
#include <system_error>

#include <fmt/format.h>
#include <mbedtls/sha256.h>
#include <picojson.h>

#include "Common/FatFsUtil.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/GamePatches.h"
#include "Core/PowerPC/MMU.h"
#include "DiscIO/Volume.h"

namespace Orca::Mod
{
namespace
{
constexpr auto FORCED_ALLOWLIST = std::to_array<std::string_view>({
    // A later pinned clock than the universal one (Project+ needs 2025-01-01: PPLUS32.ini).
    "Dolphin.Core.CustomRTCValue",
});
constexpr auto OFFERABLE_CAPS = std::to_array<std::string_view>({"results", "locks", "queue2"});

bool IsLowerHex(std::string_view s, size_t length)
{
  return s.size() == length && std::ranges::all_of(s, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::string ToLowerAscii(std::string_view s)
{
  std::string out(s);
  for (char& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// A JSON integer (picojson keeps numbers as doubles: exact up to 2^53) within [min, max].
std::optional<u64> Integer(const picojson::value& v, u64 min, u64 max)
{
  if (!v.is<double>())
    return std::nullopt;
  const double d = v.get<double>();
  if (!std::isfinite(d) || d != std::floor(d) || d < static_cast<double>(min) ||
      d > static_cast<double>(max))
  {
    return std::nullopt;
  }
  return static_cast<u64>(d);
}

// "0x" and 1-8 hex digits.
std::optional<u32> HexWord(const picojson::value& v)
{
  if (!v.is<std::string>())
    return std::nullopt;
  const std::string& s = v.get<std::string>();
  if (s.size() < 3 || s.size() > 10 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X') ||
      !std::all_of(s.begin() + 2, s.end(), [](unsigned char c) { return std::isxdigit(c); }))
  {
    return std::nullopt;
  }
  return static_cast<u32>(std::stoul(s.substr(2), nullptr, 16));
}

bool InMem1Word(u32 address)
{
  return address >= 0x80000000 && address <= 0x817FFFFC && address % 4 == 0;
}

// The object's keys are exactly among `allowed` (nullopt) or the first that isn't.
std::optional<std::string> UnknownKey(const picojson::object& o,
                                      std::initializer_list<std::string_view> allowed)
{
  for (const auto& [key, value] : o)
  {
    if (std::ranges::find(allowed, key) == allowed.end())
      return key;
  }
  return std::nullopt;
}

bool IsVersion(std::string_view s)
{
  // <major>.<minor>.<patch>, decimal.
  int dots = 0;
  bool digit = false;
  for (char c : s)
  {
    if (c == '.')
    {
      if (!digit)
        return false;
      ++dots;
      digit = false;
    }
    else if (c >= '0' && c <= '9')
    {
      digit = true;
    }
    else
    {
      return false;
    }
  }
  return dots == 2 && digit && s.size() <= 20;
}
}  // namespace

// --- The tree ----------------------------------------------------------------------------------

const TreeFile* Tree::Find(std::string_view path) const
{
  const auto it = std::ranges::lower_bound(files, path, {}, &TreeFile::path);
  return it != files.end() && it->path == path ? &*it : nullptr;
}

std::vector<const TreeFile*> Tree::Under(std::string_view folder) const
{
  std::vector<const TreeFile*> out;
  for (const TreeFile& f : files)
  {
    if (f.path.starts_with(folder))
      out.push_back(&f);
  }
  return out;
}

std::optional<std::string> PathProblem(std::string_view path)
{
  if (path.empty())
    return "an empty path";
  if (path.size() > MAX_PATH_BYTES)
    return fmt::format("longer than {} bytes", MAX_PATH_BYTES);
  for (const char c : path)
  {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7e || std::string_view("<>:\"\\|?*").find(c) != std::string_view::npos)
      return "a character outside printable ASCII, or one of <>:\"\\|?*";
  }
  size_t start = 0;
  while (true)
  {
    const size_t slash = path.find('/', start);
    const std::string_view part =
        path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (part.empty())
      return "an empty component (a leading, trailing or doubled '/')";
    if (part.front() == '.')
      return "a component that starts with a dot";
    if (part.back() == '.' || part.back() == ' ')
      return "a component that ends in a dot or a space";
    // Windows device names, with or without an extension.
    std::string base = ToLowerAscii(part.substr(0, part.find('.')));
    while (!base.empty() && base.back() == ' ')
      base.pop_back();
    static constexpr std::array<std::string_view, 4> devices{"con", "prn", "aux", "nul"};
    if (std::ranges::find(devices, base) != devices.end() ||
        (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) &&
         base[3] >= '0' && base[3] <= '9'))
    {
      return "a Windows device name";
    }
    if (slash == std::string_view::npos)
      break;
    start = slash + 1;
  }
  return std::nullopt;
}

bool HostOnly(std::string_view path)
{
  return path == "orca/jitwarm" || path.starts_with("orca/test/");
}

std::string Sha256Hex(std::string_view data)
{
  std::array<u8, 32> digest{};
  mbedtls_sha256_ret(reinterpret_cast<const u8*>(data.data()), data.size(), digest.data(), 0);
  std::string hex;
  hex.reserve(64);
  for (const u8 b : digest)
    hex += fmt::format("{:02x}", b);
  return hex;
}

std::string TreeHash(std::span<const TreeFile> files)
{
  std::string text;
  for (const TreeFile& f : files)
  {
    if (!HostOnly(f.path))
      text += fmt::format("{}\t{}\t{}\n", f.path, f.sha256, f.bytes);
  }
  return Sha256Hex(text);
}

std::string SdTreeHash(const Tree& tree, u64 bytes)
{
  std::string text = fmt::format("bytes={}\n", bytes);
  for (const TreeFile* f : tree.Under("sd/"))
    text += fmt::format("{}\t{}\t{}\n", f->path, f->sha256, f->bytes);
  return Sha256Hex(text);
}

std::optional<Tree> ParseTree(std::string_view json, std::string* error)
{
  const auto fail = [error](std::string why) {
    *error = "tree.json: " + std::move(why);
    return std::nullopt;
  };
  picojson::value root;
  const std::string parse_error = picojson::parse(root, std::string(json));
  if (!parse_error.empty())
    return fail(parse_error);
  if (!root.is<picojson::object>())
    return fail("not an object");
  const picojson::object& o = root.get<picojson::object>();
  if (const auto key = UnknownKey(o, {"tree", "files"}))
    return fail(fmt::format("unknown key {}", *key));
  if (!o.contains("tree") || !o.at("tree").is<std::string>() ||
      !IsLowerHex(o.at("tree").get<std::string>(), 64))
  {
    return fail("\"tree\" must be 64 lowercase hex digits");
  }
  if (!o.contains("files") || !o.at("files").is<picojson::array>())
    return fail("\"files\" must be an array");
  const picojson::array& list = o.at("files").get<picojson::array>();
  if (list.size() > MAX_FILES)
    return fail(fmt::format("more than {} files", MAX_FILES));

  Tree tree;
  tree.hash = o.at("tree").get<std::string>();
  std::set<std::string> lower;
  for (const picojson::value& item : list)
  {
    if (!item.is<picojson::object>())
      return fail("a file that is not an object");
    const picojson::object& f = item.get<picojson::object>();
    if (const auto key = UnknownKey(f, {"path", "sha256", "bytes"}))
      return fail(fmt::format("unknown key {} in a file", *key));
    if (!f.contains("path") || !f.at("path").is<std::string>())
      return fail("a file without a path");
    TreeFile file;
    file.path = f.at("path").get<std::string>();
    if (const auto problem = PathProblem(file.path))
      return fail(fmt::format("{}: {}", file.path, *problem));
    if (file.path == "tree.json")
      return fail("tree.json lists itself");
    if (!f.contains("sha256") || !f.at("sha256").is<std::string>() ||
        !IsLowerHex(f.at("sha256").get<std::string>(), 64))
    {
      return fail(fmt::format("{}: sha256 must be 64 lowercase hex digits", file.path));
    }
    file.sha256 = f.at("sha256").get<std::string>();
    const auto bytes = f.contains("bytes") ? Integer(f.at("bytes"), 0, MAX_FILE_BYTES) : std::nullopt;
    if (!bytes)
      return fail(fmt::format("{}: bytes must be an integer from 0 to {}", file.path, MAX_FILE_BYTES));
    file.bytes = *bytes;
    if (!tree.files.empty() && !(tree.files.back().path < file.path))
      return fail(fmt::format("{} is out of order or listed twice (sorted by path)", file.path));
    if (!lower.insert(ToLowerAscii(file.path)).second)
      return fail(fmt::format("{} differs from another path only in case", file.path));
    tree.files.push_back(std::move(file));
  }
  // Never both a file and a folder (also without case).
  for (const TreeFile& f : tree.files)
  {
    const std::string l = ToLowerAscii(f.path);
    for (size_t slash = l.find('/'); slash != std::string::npos; slash = l.find('/', slash + 1))
    {
      if (lower.contains(l.substr(0, slash)))
        return fail(fmt::format("{} is inside a path that is also a file", f.path));
    }
  }
  if (TreeHash(tree.files) != tree.hash)
    return fail("\"tree\" is not the hash of the files listed");
  return tree;
}

// --- mod.json ----------------------------------------------------------------------------------

std::optional<Builtin> BuiltinFromName(std::string_view name)
{
  if (name == "brawl-rev2")
    return Builtin::BrawlRev2;
  if (name == "brawl-rev2-pplus")
    return Builtin::BrawlRev2PPlus;
  return std::nullopt;
}

std::string_view BuiltinName(Builtin builtin)
{
  switch (builtin)
  {
  case Builtin::BrawlRev2:
    return "brawl-rev2";
  case Builtin::BrawlRev2PPlus:
    return "brawl-rev2-pplus";
  default:
    return "none";
  }
}

std::span<const std::string_view> ForcedAllowlist()
{
  return FORCED_ALLOWLIST;
}

std::span<const std::string_view> OfferableCaps()
{
  return OFFERABLE_CAPS;
}

u64 DiscTitleId(std::string_view disc_id)
{
  u64 code = 0;
  for (size_t i = 0; i < 4 && i < disc_id.size(); ++i)
    code = (code << 8) | static_cast<u8>(disc_id[i]);
  return (u64{0x00010000} << 32) | code;
}

std::optional<Manifest> ParseManifest(std::string_view json, std::string* error)
{
  const auto fail = [error](std::string why) {
    *error = "mod.json: " + std::move(why);
    return std::nullopt;
  };
  picojson::value root;
  const std::string parse_error = picojson::parse(root, std::string(json));
  if (!parse_error.empty())
    return fail(parse_error);
  if (!root.is<picojson::object>())
    return fail("not an object");
  const picojson::object& o = root.get<picojson::object>();
  if (const auto key = UnknownKey(
          o, {"format", "engine", "title", "disc", "launcher", "sd", "frameHook", "bootNandFrame",
              "firstKeyframeFrame", "jitClearFrame", "keepGameCode", "saveTitle", "forced",
              "builtin", "rulesets", "offers"}))
  {
    return fail(fmt::format("unknown key {}", *key));
  }
  const auto get = [&o](const char* key) -> const picojson::value* {
    const auto it = o.find(key);
    return it == o.end() ? nullptr : &it->second;
  };

  Manifest m;
  // format
  if (const picojson::value* v = get("format"); !v || Integer(*v, 1, 1) != 1u)
    return fail("\"format\" must be 1");
  m.format = 1;

  // engine
  {
    const picojson::value* v = get("engine");
    if (!v || !v->is<picojson::object>())
      return fail("\"engine\" must be an object");
    const picojson::object& e = v->get<picojson::object>();
    if (const auto key = UnknownKey(e, {"api", "minOrca"}))
      return fail(fmt::format("unknown key engine.{}", *key));
    const auto api = e.contains("api") ? Integer(e.at("api"), 1, 1000) : std::nullopt;
    if (!api)
      return fail("engine.api must be an integer from 1");
    if (*api > static_cast<u64>(ENGINE_API))
    {
      return fail(fmt::format("this mod needs engine API {}; this Orca has {}. Update Orca.", *api,
                              ENGINE_API));
    }
    m.engine_api = static_cast<int>(*api);
    if (!e.contains("minOrca") || !e.at("minOrca").is<std::string>() ||
        !IsVersion(e.at("minOrca").get<std::string>()))
    {
      return fail("engine.minOrca must be a version (major.minor.patch)");
    }
    m.min_orca = e.at("minOrca").get<std::string>();
  }

  // title
  if (const picojson::value* v = get("title"))
  {
    if (!v->is<std::string>())
      return fail("\"title\" must be a string");
    const std::string& t = v->get<std::string>();
    if (t.empty() || t.size() > 48 ||
        !std::ranges::all_of(t, [](unsigned char c) { return c >= 0x20 && c < 0x7f; }))
    {
      return fail("\"title\" must be 1-48 printable ASCII characters");
    }
    m.title = t;
  }

  // disc
  {
    const picojson::value* v = get("disc");
    if (!v || !v->is<picojson::object>())
      return fail("\"disc\" must be an object");
    const picojson::object& d = v->get<picojson::object>();
    if (const auto key = UnknownKey(d, {"id", "revision"}))
      return fail(fmt::format("unknown key disc.{}", *key));
    if (!d.contains("id") || !d.at("id").is<std::string>())
      return fail("disc.id must be a game ID");
    const std::string& id = d.at("id").get<std::string>();
    if (id.size() != 6 || !std::ranges::all_of(id, [](unsigned char c) {
          return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        }))
    {
      return fail("disc.id must be 6 characters A-Z 0-9");
    }
    const auto revision = d.contains("revision") ? Integer(d.at("revision"), 0, 255) : std::nullopt;
    if (!revision)
      return fail("disc.revision must be an integer from 0 to 255");
    m.disc_id = id;
    m.disc_revision = static_cast<u16>(*revision);
  }

  // launcher
  if (const picojson::value* v = get("launcher"))
  {
    if (!v->is<std::string>())
      return fail("\"launcher\" must be a file name");
    const std::string& l = v->get<std::string>();
    if (PathProblem(l) || l.find('/') != std::string::npos || l == "mod.json" ||
        l == "tree.json")
    {
      return fail("\"launcher\" must be a file name at the bundle's root");
    }
    const std::string lower = ToLowerAscii(l);
    if (!lower.ends_with(".dol") && !lower.ends_with(".elf"))
      return fail("\"launcher\" must be a .dol or .elf");
    m.launcher = l;
  }

  // sd
  if (const picojson::value* v = get("sd"))
  {
    if (!v->is<picojson::object>())
      return fail("\"sd\" must be an object");
    const picojson::object& s = v->get<picojson::object>();
    if (const auto key = UnknownKey(s, {"bytes"}))
      return fail(fmt::format("unknown key sd.{}", *key));
    const auto bytes =
        s.contains("bytes") ? Integer(s.at("bytes"), SD_MIN_BYTES, SD_MAX_BYTES) : std::nullopt;
    if (!bytes || *bytes % SD_ALIGN != 0)
    {
      return fail(fmt::format("sd.bytes must be a multiple of {} from {} to {} (an SDSC card)",
                              SD_ALIGN, SD_MIN_BYTES, SD_MAX_BYTES));
    }
    m.sd_bytes = *bytes;
  }

  // frameHook
  {
    const picojson::value* v = get("frameHook");
    if (!v || !v->is<picojson::object>())
      return fail("\"frameHook\" must be an object");
    const picojson::object& h = v->get<picojson::object>();
    if (const auto key = UnknownKey(h, {"address", "word"}))
      return fail(fmt::format("unknown key frameHook.{}", *key));
    const auto address = h.contains("address") ? HexWord(h.at("address")) : std::nullopt;
    const auto word = h.contains("word") ? HexWord(h.at("word")) : std::nullopt;
    if (!address || !InMem1Word(*address))
      return fail("frameHook.address must be a hex word address in MEM1 (\"0x80017504\")");
    if (!word)
      return fail("frameHook.word must be a hex word (\"0x90170100\")");
    m.frame_hook = *address;
    m.frame_hook_word = *word;
  }

  // Frames
  const auto frame = [&](const char* key, u64 min, u64 max) -> std::optional<std::optional<u32>> {
    const picojson::value* v = get(key);
    if (!v)
      return std::optional<u32>{};
    const auto n = Integer(*v, min, max);
    if (!n)
      return std::nullopt;
    return std::optional<u32>(static_cast<u32>(*n));
  };
  const auto boot_nand = frame("bootNandFrame", 1, 1'000'000);
  if (!boot_nand)
    return fail("bootNandFrame must be an integer from 1 to 1000000");
  m.boot_nand_frame = *boot_nand;
  const auto first_keyframe = frame("firstKeyframeFrame", 60, 36'000);
  if (!first_keyframe)
    return fail("firstKeyframeFrame must be an integer from 60 to 36000");
  m.first_keyframe_frame = first_keyframe->value_or(DEFAULT_FIRST_KEYFRAME_FRAME);
  const auto jit_clear = frame("jitClearFrame", 0, 1'000'000);
  if (!jit_clear)
    return fail("jitClearFrame must be an integer from 0 to 1000000");
  m.jit_clear_frame = *jit_clear;

  // keepGameCode
  if (const picojson::value* v = get("keepGameCode"))
  {
    if (!v->is<picojson::array>() || v->get<picojson::array>().size() > 64)
      return fail("\"keepGameCode\" must be an array of at most 64 entries");
    for (const picojson::value& item : v->get<picojson::array>())
    {
      const auto* triple = item.is<picojson::array>() ? &item.get<picojson::array>() : nullptr;
      const auto address = triple && triple->size() == 3 ? HexWord((*triple)[0]) : std::nullopt;
      const auto game = triple && triple->size() == 3 ? HexWord((*triple)[1]) : std::nullopt;
      const auto loader = triple && triple->size() == 3 ? HexWord((*triple)[2]) : std::nullopt;
      if (!address || !game || !loader)
        return fail("keepGameCode entries must be [address, the game's word, the mod's word], hex");
      if (!InMem1Word(*address))
        return fail("keepGameCode's address must be a word in MEM1 (0x80000000-0x817FFFFC)");
      if (*game == *loader)
        return fail("keepGameCode's two words are the same");
      if (std::ranges::any_of(m.keep_game_code,
                              [&](const KeptWord& k) { return k.address == *address; }))
      {
        return fail(fmt::format("keepGameCode {:#010x} twice", *address));
      }
      m.keep_game_code.push_back({*address, *game, *loader});
    }
  }

  // saveTitle: only the disc's own.
  if (const picojson::value* v = get("saveTitle"))
  {
    u64 title = 0;
    if (!v->is<std::string>() || !v->get<std::string>().starts_with("0x") ||
        v->get<std::string>().size() != 18 || !TryParse(v->get<std::string>().substr(2), &title, 16))
    {
      return fail("saveTitle must be a 16-digit hex title ID (\"0x0001000052534245\")");
    }
    if (title != DiscTitleId(m.disc_id))
      return fail("saveTitle may name only the disc's own title");
    m.save_title = title;
  }

  // forced
  if (const picojson::value* v = get("forced"))
  {
    if (!v->is<picojson::object>())
      return fail("\"forced\" must be an object");
    for (const auto& [key, value] : v->get<picojson::object>())
    {
      if (std::ranges::find(FORCED_ALLOWLIST, key) == FORCED_ALLOWLIST.end())
        return fail(fmt::format("forced.{} is not a setting a mod may force", key));
      // Every allowed key so far is a 32-bit unsigned integer.
      const auto n = Integer(value, 0, 0xFFFFFFFFu);
      if (!n)
        return fail(fmt::format("forced.{} must be an integer from 0 to 4294967295", key));
      m.forced.emplace_back(key, fmt::format("{}", *n));
    }
  }

  // builtin
  if (const picojson::value* v = get("builtin"))
  {
    const auto builtin = v->is<std::string>() ? BuiltinFromName(v->get<std::string>()) : std::nullopt;
    if (!builtin)
      return fail("\"builtin\" must be \"brawl-rev2\" or \"brawl-rev2-pplus\"");
    if (m.disc_id != "RSBE01" || m.disc_revision != 2)
      return fail(fmt::format("builtin {} runs on RSBE01 revision 2 only", BuiltinName(*builtin)));
    m.builtin = *builtin;
  }

  // rulesets
  if (const picojson::value* v = get("rulesets"))
  {
    if (!v->is<picojson::array>() || v->get<picojson::array>().size() > 16)
      return fail("\"rulesets\" must be an array of at most 16 names");
    for (const picojson::value& item : v->get<picojson::array>())
    {
      const std::string* name = item.is<std::string>() ? &item.get<std::string>() : nullptr;
      if (!name || name->empty() || name->size() > 32 || (*name)[0] == '-' ||
          !std::ranges::all_of(*name, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
          }))
      {
        return fail("rulesets must be names of 1-32 characters a-z 0-9 -");
      }
      if (std::ranges::find(m.rulesets, *name) != m.rulesets.end())
        return fail(fmt::format("ruleset {} twice", *name));
      m.rulesets.push_back(*name);
    }
  }

  // offers
  if (const picojson::value* v = get("offers"))
  {
    if (!v->is<picojson::array>())
      return fail("\"offers\" must be an array");
    for (const picojson::value& item : v->get<picojson::array>())
    {
      const std::string* cap = item.is<std::string>() ? &item.get<std::string>() : nullptr;
      if (!cap || std::ranges::find(OFFERABLE_CAPS, *cap) == OFFERABLE_CAPS.end())
        return fail("offers must be among \"results\", \"locks\", \"queue2\"");
      if (std::ranges::find(m.offers, *cap) != m.offers.end())
        return fail(fmt::format("offer {} twice", *cap));
      m.offers.push_back(*cap);
    }
    if (!m.offers.empty() && m.builtin == Builtin::None)
      return fail("a mod with no builtin has no logic to offer caps with");
  }
  return m;
}

// --- The session -------------------------------------------------------------------------------

bool Requested()
{
  return SessionActive() && !GetEnv("ORCA_MOD_DIR").empty();
}

std::string Dir()
{
  std::string dir = GetEnv("ORCA_MOD_DIR");
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
    dir += '/';
  return dir;
}

const ModInfo* ActiveMod()
{
  const Profile* profile = ActiveProfile();
  return profile && profile->mod ? &*profile->mod : nullptr;
}

u32 FirstKeyframeFrame()
{
  const ModInfo* mod = ActiveMod();
  return mod ? mod->manifest.first_keyframe_frame : DEFAULT_FIRST_KEYFRAME_FRAME;
}

namespace
{
// Keeps the newest `keep` cached images (by mtime) in `dir`, and drops the rest with their notes.
void PruneSdCache(const std::string& dir, size_t keep)
{
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<std::pair<fs::file_time_type, fs::path>> images;
  for (const auto& entry : fs::directory_iterator(StringToPath(dir), ec))
  {
    const std::string name = PathToString(entry.path().filename());
    // <64 hex>-<builder>.raw
    if (name.size() < 70 || !IsLowerHex(std::string_view(name).substr(0, 64), 64) ||
        name[64] != '-' || !name.ends_with(".raw"))
    {
      continue;
    }
    images.emplace_back(entry.last_write_time(ec), entry.path());
  }
  std::ranges::sort(images, std::greater{});
  for (size_t i = keep; i < images.size(); ++i)
  {
    fs::remove(images[i].second, ec);
    fs::path note = images[i].second;
    note += ".xxh3";
    fs::remove(note, ec);
    NOTICE_LOG_FMT(BOOT, "Orca mod: dropped the cached SD image {}", PathToString(images[i].second));
  }
}
}  // namespace

bool BuildSdImage(std::span<const SdSource> files, u64 bytes, const std::string& out_path,
                  std::string* error)
{
  std::vector<Common::SDImageFile> list;
  list.reserve(files.size());
  for (const SdSource& f : files)
    list.push_back({f.image_path, f.source_path, f.bytes});
  return Common::BuildSDImageFromFiles(list, bytes, out_path, error);
}

std::optional<SdImage> PrepareSdImage(const ModInfo& mod, const std::string& cache_dir,
                                      std::string* error)
{
  if (!mod.manifest.sd_bytes)
  {
    *error = "Orca mod: this mod has no SD card";
    return std::nullopt;
  }
  if (cache_dir.empty())
  {
    *error = "Orca mod: a mod with an SD card needs ORCA_SD_CACHE (where its image is built)";
    return std::nullopt;
  }
  const u64 bytes = *mod.manifest.sd_bytes;
  std::string dir = cache_dir;
  if (dir.back() != '/' && dir.back() != '\\')
    dir += '/';
  if (!File::CreateFullPath(dir))
  {
    *error = fmt::format("Orca mod: can't make the SD cache folder {}", dir);
    return std::nullopt;
  }
  SdImage image;
  image.path = fmt::format("{}{}-{}.raw", dir, SdTreeHash(mod.tree, bytes), SD_BUILDER);
  const std::string note = image.path + ".xxh3";

  // A cached image is reused while it hashes as when it was built (a damaged one is built again).
  if (File::Exists(image.path) && File::GetSize(image.path) == bytes)
  {
    std::string recorded;
    File::ReadFileToString(note, recorded);
    const std::string hash = HashFile(image.path);
    if (!hash.empty() && hash == StripWhitespace(recorded))
      image.hash = hash;
    else
      NOTICE_LOG_FMT(BOOT, "Orca mod: the cached SD image {} is damaged: building it again",
                     image.path);
  }
  if (image.hash.empty())
  {
    std::vector<SdSource> files;
    for (const TreeFile* f : mod.tree.Under("sd/"))
    {
      files.push_back({f->path.substr(3), mod.dir + f->path, f->bytes});
      // The tree's sizes were checked at load; checked again here, as the bytes go in.
      if (File::GetSize(files.back().source_path) != f->bytes)
      {
        *error = fmt::format("Orca mod: {} is not the size tree.json names", f->path);
        return std::nullopt;
      }
    }
    const auto start = std::chrono::steady_clock::now();
    std::string why;
    File::Delete(note, File::IfAbsentBehavior::NoConsoleWarning);
    if (!BuildSdImage(files, bytes, image.path, &why))
    {
      *error = fmt::format("Orca mod: couldn't build the SD card: {}", why);
      return std::nullopt;
    }
    image.hash = HashFile(image.path);
    if (image.hash.empty() || !File::WriteStringToFile(note + ".tmp", image.hash) ||
        !File::Rename(note + ".tmp", note))
    {
      *error = fmt::format("Orca mod: couldn't hash or record the SD image {}", image.path);
      return std::nullopt;
    }
    image.built = true;
    NOTICE_LOG_FMT(BOOT, "Orca mod: built the SD card ({} files, {} bytes) in {} ms: {}",
                   files.size(), bytes,
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count(),
                   image.path);
  }
  // Newest first by use: this one is the newest.
  std::error_code ec;
  std::filesystem::last_write_time(StringToPath(image.path),
                                   std::filesystem::file_time_type::clock::now(), ec);
  PruneSdCache(dir, 2);
  return image;
}

// --- Builtins and their guards -----------------------------------------------------------------

namespace
{
// Brawl rev 2's executable (main.dol, NTSC-U), as every builtin module reads it: the frame boundary,
// the small-data base (r13) and the code that loads the globals the modules follow through it (the
// scene manager, g_GameGlobal, the pad system), and the message buffer's vtable (NativeText). Words of
// the DOL's text and data, never written while the game runs: the same in vanilla and Project+ at
// every frame boundary from the hook's first (picked from RAM at frame 1,000 of both, 2026-10-05).
constexpr auto BRAWL_REV2_GUARDS = std::to_array<GuardWord>({
    {0x80017504, 0x90170100},  // the frame loop's end: stw r0,0x100(r23)
    {0x8000422C, 0x3DA0805A},  // __init_registers: lis r13,0x805A
    {0x80004230, 0x61AD4420},  // ori r13,r13,0x4420
    {0x8002D018, 0x806DBC40},  // gfSceneManager::getInstance: lwz r3,-0x43C0(r13) (0x805A0060)
    {0x8000D8F4, 0x806DBCC0},  // lwz r3,-0x4340(r13): g_GameGlobal (0x805A00E0)
    {0x80017538, 0x83EDBC20},  // the frame loop: lwz r31,-0x43E0(r13): gfPadSystem (0x805A0040)
    {0x804540E8, 0x80069820},  // the message buffer's vtable (0x804540E0), +8
    {0x804540EC, 0x8004755C},  // and +C
});
// Project+ v3.2 adds its codeset's code that reads and writes the stage select data RankedPPlus and
// the online rules use (Source/Netplay/Net-Random.asm: the data at 0x8042C4E8, its page index at
// 0x8042C821, the current page at 0x80496000). A codeset built with those routines elsewhere loses
// the builtin (phase 3's labels name them instead).
constexpr auto PPLUS_GUARDS = std::to_array<GuardWord>({
    {0x80553594, 0x3CA08042},  // lis r5,0x8042
    {0x80553598, 0x60A5C4E8},  // ori r5,r5,0xC4E8: the stage select data
    {0x80554320, 0x3C608042},  // lis r3,0x8042
    {0x80554324, 0x6063C821},  // ori r3,r3,0xC821: its page index
    {0x805518D0, 0x3E008049},  // lis r16,0x8049
    {0x805518D4, 0x62106000},  // ori r16,r16,0x6000: the current page
});
// Vanilla's own: Brawl's v-sync call, which Project+'s codes replace (PPLUS32 keepGameCode), so the
// vanilla modules (BrawlStages' stage flow, the rules screen) never run on Project+.
constexpr auto VANILLA_GUARDS = std::to_array<GuardWord>({
    {0x80023B88, 0x4182FF7C},  // beq- (Project+: nop)
});
template <size_t A, size_t B>
constexpr std::array<GuardWord, A + B> Join(const std::array<GuardWord, A>& a,
                                            const std::array<GuardWord, B>& b)
{
  std::array<GuardWord, A + B> all{};
  std::ranges::copy(a, all.begin());
  std::ranges::copy(b, all.begin() + A);
  return all;
}
constexpr auto VANILLA_BRAWL_GUARDS = Join(BRAWL_REV2_GUARDS, VANILLA_GUARDS);
constexpr auto BRAWL_REV2_PPLUS_GUARDS = Join(BRAWL_REV2_GUARDS, PPLUS_GUARDS);
}  // namespace

std::span<const GuardWord> BuiltinGuards(Builtin builtin)
{
  switch (builtin)
  {
  case Builtin::BrawlRev2:
    return VANILLA_BRAWL_GUARDS;
  case Builtin::BrawlRev2PPlus:
    return BRAWL_REV2_PPLUS_GUARDS;
  default:
    return {};
  }
}

bool GuardsHold(Builtin builtin, const std::function<std::optional<u32>(u32)>& read)
{
  if (builtin == Builtin::None)
    return false;
  for (const GuardWord& g : BuiltinGuards(builtin))
  {
    if (read(g.address) != g.word)
      return false;
  }
  return true;
}

namespace
{
// -1: not checked since boot (the game's code isn't live yet), 0: failed, 1: held.
std::atomic<int> s_guards{-1};
std::atomic<bool> s_guards_failed{false};
const ModInfo* s_guards_for = nullptr;
}  // namespace

void RefreshGuards(const Core::CPUThreadGuard& guard)
{
  const Profile* profile = ActiveProfile();
  const ModInfo* mod = profile && profile->mod ? &*profile->mod : nullptr;
  if (mod != s_guards_for)
  {
    s_guards_for = mod;
    s_guards = -1;
    s_guards_failed = false;
  }
  if (!mod || mod->manifest.builtin == Builtin::None)
    return;
  // Before the game's code is live (a launcher still running), nothing to check.
  if (!FrameHookLive(guard, *profile))
    return;
  const bool held = GuardsHold(mod->manifest.builtin, [&guard](u32 address) -> std::optional<u32> {
    if (!PowerPC::MMU::HostIsRAMAddress(guard, address))
      return std::nullopt;
    return PowerPC::MMU::HostRead<u32>(guard, address);
  });
  const int now = held ? 1 : 0;
  if (s_guards.exchange(now) != now)
  {
    if (held)
    {
      NOTICE_LOG_FMT(BOOT, "Orca mod: builtin {} on (its guard words hold)",
                     BuiltinName(mod->manifest.builtin));
    }
    else
    {
      for (const GuardWord& g : BuiltinGuards(mod->manifest.builtin))
      {
        const u32 word = PowerPC::MMU::HostIsRAMAddress(guard, g.address) ?
                             PowerPC::MMU::HostRead<u32>(guard, g.address) :
                             0;
        if (word != g.word)
        {
          WARN_LOG_FMT(BOOT,
                       "Orca mod: builtin {} off: {:08x} holds {:08x}, not {:08x} (this mod's "
                       "code moved what the builtin's modules rely on)",
                       BuiltinName(mod->manifest.builtin), g.address, word, g.word);
          break;
        }
      }
    }
  }
  if (!held)
    s_guards_failed = true;
}

bool BuiltinOn(Builtin builtin)
{
  const ModInfo* mod = ActiveMod();
  return mod && builtin != Builtin::None && mod->manifest.builtin == builtin &&
         mod == s_guards_for && s_guards == 1;
}

Builtin DeclaredBuiltin()
{
  const ModInfo* mod = ActiveMod();
  return mod ? mod->manifest.builtin : Builtin::None;
}

bool GuardsFailed()
{
  return ActiveMod() == s_guards_for && s_guards_failed;
}

bool Offers(std::string_view cap)
{
  const ModInfo* mod = ActiveMod();
  // Both builtins carry results, the online rules' locks and the queue's character select.
  return mod && mod->manifest.builtin != Builtin::None &&
         std::ranges::find(mod->manifest.offers, cap) != mod->manifest.offers.end();
}
}  // namespace Orca::Mod

// --- The profile -------------------------------------------------------------------------------

namespace Orca
{
std::optional<Profile> LoadModProfile(const std::string& dir_in, std::string* error)
{
  std::string dir = dir_in;
  if (dir.empty())
  {
    *error = "Orca mod: no mod folder";
    return std::nullopt;
  }
  if (dir.back() != '/' && dir.back() != '\\')
    dir += '/';
  std::string text;
  if (!File::ReadFileToString(dir + "tree.json", text))
  {
    *error = fmt::format("Orca mod: {}tree.json can't be read", dir);
    return std::nullopt;
  }
  std::optional<Mod::Tree> tree = Mod::ParseTree(text, error);
  if (!tree)
    return std::nullopt;
  // Every file there, at the size tree.json names (its content is the app's sha256 check at
  // download, and for the SD card the built image's hash in the key).
  for (const Mod::TreeFile& f : tree->files)
  {
    const std::string path = dir + f.path;
    if (!File::Exists(path) || File::IsDirectory(path) || File::GetSize(path) != f.bytes)
    {
      *error = fmt::format("Orca mod: {} is missing or not the size tree.json names ({} bytes)",
                           f.path, f.bytes);
      return std::nullopt;
    }
  }
  if (!tree->Find("mod.json") || !File::ReadFileToString(dir + "mod.json", text))
  {
    *error = "Orca mod: the bundle has no mod.json";
    return std::nullopt;
  }
  std::optional<Mod::Manifest> manifest = Mod::ParseManifest(text, error);
  if (!manifest)
    return std::nullopt;
  const Mod::TreeFile* launcher =
      manifest->launcher.empty() ? nullptr : tree->Find(manifest->launcher);
  if (!manifest->launcher.empty() && !launcher)
  {
    *error = fmt::format("Orca mod: mod.json's launcher {} is not in the bundle", manifest->launcher);
    return std::nullopt;
  }

  // Game patches (orca/patches/<name>.patches): every file must parse, or the session is refused
  // here rather than running without them.
  for (const Mod::TreeFile* f : tree->Under("orca/patches/"))
  {
    if (!f->path.ends_with(".patches") ||
        f->path.find('/', sizeof("orca/patches/") - 1) != std::string::npos)
    {
      continue;
    }
    std::string patch_text, why;
    if (!File::ReadFileToString(dir + f->path, patch_text) ||
        !UX::ParseGamePatches(patch_text, &why))
    {
      *error = fmt::format("Orca mod: {}: {}", f->path, why.empty() ? "can't be read" : why);
      return std::nullopt;
    }
  }

  Profile profile;
  // The room's mode and the boot log name it as the legacy profile its builtin replaces, so a
  // bundle of Brawl or Project+ plays in the same modes as before.
  switch (manifest->builtin)
  {
  case Mod::Builtin::BrawlRev2:
    profile.game_id = "RSBE01";
    break;
  case Mod::Builtin::BrawlRev2PPlus:
    profile.game_id = "PPLUS32";
    break;
  default:
    profile.game_id = manifest->disc_id;
    break;
  }
  profile.title = manifest->title;
  profile.revision = manifest->disc_revision;
  profile.frame_hook = manifest->frame_hook;
  profile.frame_hook_word = manifest->frame_hook_word;
  profile.boot_nand_frame = manifest->boot_nand_frame;
  profile.jit_clear_frame = manifest->jit_clear_frame;
  for (const Mod::KeptWord& k : manifest->keep_game_code)
    profile.kept_code.push_back({k.address, k.game, k.loader});
  profile.save_title = manifest->save_title;
  // The card is in exactly when mod.json has one (never a forced key of its own).
  if (manifest->sd_bytes)
    profile.forced.emplace_back(Config::Location{Config::System::Main, "Core", "WiiSDCard"}, "True");
  for (const auto& [key, value] : manifest->forced)
  {
    const size_t dot1 = key.find('.');
    const size_t dot2 = key.find('.', dot1 + 1);
    const std::optional<Config::System> system = Config::GetSystemFromName(key.substr(0, dot1));
    if (!system)
    {
      *error = fmt::format("Orca mod: unknown system in {}", key);
      return std::nullopt;
    }
    profile.forced.emplace_back(
        Config::Location{*system, key.substr(dot1 + 1, dot2 - dot1 - 1), key.substr(dot2 + 1)},
        value);
  }
  if (launcher)
  {
    profile.launcher_hash = launcher->sha256;
    profile.disc = manifest->disc_id;
  }
  if (manifest->sd_bytes)
    profile.sd_image = "sd/";
  profile.mod = Mod::ModInfo{dir, std::move(*tree), std::move(*manifest), {}};
  return profile;
}
}  // namespace Orca

namespace Orca
{
bool PrepareModBoot(Profile* profile, const std::string* executable_path,
                    const std::string& default_disc, const std::string& boot_game_id,
                    u16 boot_revision, std::string* error)
{
  Mod::ModInfo& mod = *profile->mod;
  const Mod::Manifest& m = mod.manifest;
  if (executable_path)
  {
    if (m.launcher.empty())
    {
      *error = "Orca mod: this mod boots its disc, not a loader";
      return false;
    }
    // Exactly the bundle's launcher (the app lays it out at <run folder>/<launcher>).
    std::error_code ec;
    if (!std::filesystem::equivalent(StringToPath(*executable_path),
                                     StringToPath(mod.dir + m.launcher), ec))
    {
      *error = fmt::format("Orca mod: {} is not this mod's launcher ({})", *executable_path,
                           m.launcher);
      return false;
    }
    const std::unique_ptr<DiscIO::Volume> disc =
        default_disc.empty() ? nullptr : DiscIO::CreateVolume(default_disc);
    if (!disc)
    {
      *error = fmt::format("Orca mod: the launcher boots {} from the drive, and no disc is set",
                           m.disc_id);
      return false;
    }
    const u16 revision = disc->GetRevision().value_or(0);
    if (disc->GetGameID() != m.disc_id || revision != m.disc_revision)
    {
      *error = fmt::format("Orca mod: this mod needs {} revision {}; the disc is {} revision {}",
                           m.disc_id, m.disc_revision, disc->GetGameID(), revision);
      return false;
    }
  }
  else
  {
    if (!m.launcher.empty())
    {
      *error = "Orca mod: this mod boots through its launcher, not the disc";
      return false;
    }
    if (boot_game_id != m.disc_id || boot_revision != m.disc_revision)
    {
      *error = fmt::format("Orca mod: this mod needs {} revision {}; the disc is {} revision {}",
                           m.disc_id, m.disc_revision, boot_game_id, boot_revision);
      return false;
    }
  }
  if (m.sd_bytes)
  {
    const std::optional<Mod::SdImage> image =
        Mod::PrepareSdImage(mod, GetEnv("ORCA_SD_CACHE"), error);
    if (!image)
      return false;
    // The session's config layer points the SD card at this image, read-only (writes and folder
    // sync are forced off), as for a legacy launcher profile's sd.raw.
    profile->sd_image_path = image->path;
    profile->sd_image_hash = image->hash;
    mod.sd_image_hash = image->hash;
  }
  return true;
}
}  // namespace Orca
