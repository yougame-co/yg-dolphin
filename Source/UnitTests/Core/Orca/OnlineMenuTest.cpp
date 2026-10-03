// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The Online menu (ORCA.md "Online menu"): the patch groups that open the game's own ONLINE page,
// Orca's texture pack, the menu text rewritten so nothing about going online is Nintendo's, and the
// reader that tells the app what the player picked.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/ScopeGuard.h"
#include "Core/Orca/UX/GamePatches.h"
#include "Core/Orca/UX/MenuText.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Rollback/OnlineMatch.h"
#include "VideoCommon/HiresTextures.h"

using namespace Orca::UX;

namespace
{
std::filesystem::path SysOrca()
{
  // The source tree's Data/Sys/Orca, found from this file (tests run from the build directory).
  return std::filesystem::path(__FILE__).parent_path() / "../../../../Data/Sys/Orca";
}

std::vector<GamePatch> Shipped(const char* name)
{
  std::string text;
  EXPECT_TRUE(File::ReadFileToString((SysOrca() / name).string(), text)) << name;
  std::string error;
  const auto patches = ParseGamePatches(text, &error);
  EXPECT_TRUE(patches.has_value()) << name << ": " << error;
  return patches.value_or(std::vector<GamePatch>{});
}

// The group (consecutive guarded lines) that starts at `address`; empty if none starts there.
std::vector<GamePatch> GroupAt(const std::vector<GamePatch>& patches, u32 address)
{
  const auto first = std::find_if(patches.begin(), patches.end(),
                                  [&](const GamePatch& p) { return p.address == address; });
  if (first == patches.end() || first->joins_previous)
    return {};
  std::vector<GamePatch> group{*first};
  for (auto it = first + 1; it != patches.end() && it->joins_previous; ++it)
    group.push_back(*it);
  return group;
}

bool AnyLineIn(const std::vector<GamePatch>& patches, u32 from, u32 to)
{
  return std::any_of(patches.begin(), patches.end(),
                     [&](const GamePatch& p) { return p.address >= from && p.address <= to; });
}

// A PowerPC b/bl at `at`: where it goes (nullopt: not an I-form branch, or absolute).
std::optional<u32> BranchTarget(u32 at, u32 word)
{
  if ((word >> 26) != 18 || (word & 2))
    return std::nullopt;
  s32 offset = static_cast<s32>(word & 0x03FFFFFC);
  if (offset & 0x02000000)
    offset -= 0x04000000;
  return at + static_cast<u32>(offset);
}

std::vector<u32> Values(const std::vector<GamePatch>& group)
{
  std::vector<u32> out;
  for (const GamePatch& p : group)
    out.push_back(p.value);
  return out;
}

// What memory holds before the group lands, word by word.
std::vector<u32> Originals(const std::vector<GamePatch>& group)
{
  std::vector<u32> out;
  for (const GamePatch& p : group)
    out.push_back(p.original.value_or(0xDEADBEEF));
  return out;
}

// The gates of one game's file. `module` is where its sora_menu_main sits relative to Brawl's
// (Project+'s own copy of the module: 0x1480 higher).
void ExpectOnlineGates(const std::vector<GamePatch>& patches, u32 module)
{
  for (const GamePatch& p : patches)
    EXPECT_TRUE(p.original.has_value() || p.address >= 0x90000000) << std::hex << p.address;
  // G2: A on the Online button enters its page like the other buttons (two copies): the cursor
  // load and compare are guards, the bne becomes b, same displacement.
  for (const u32 at : {0x81178A58u, 0x81178C30u})
  {
    SCOPED_TRACE(at);
    const auto g = GroupAt(patches, at + module);
    ASSERT_EQ(g.size(), 3u);
    EXPECT_EQ(Originals(g), (std::vector<u32>{0xA01D0042, 0x28000002, 0x4082007C}));
    EXPECT_EQ(Values(g), (std::vector<u32>{0xA01D0042, 0x28000002, 0x4800007C}));
    EXPECT_TRUE(GroupApplies(g, Originals(g)));
    EXPECT_FALSE(GroupApplies(g, Values(g)));
  }
  // G3: B on the ONLINE page goes back with no disconnect window (two copies of nine words):
  // the back animation, next proc 0, sub-state 3, return 1, at the function's end.
  for (const u32 at : {0x81179F48u, 0x8117A148u})
  {
    SCOPED_TRACE(at);
    const auto g = GroupAt(patches, at + module);
    ASSERT_EQ(g.size(), 9u);
    for (const GamePatch& p : g)
      EXPECT_TRUE(p.original.has_value());
    EXPECT_EQ(g[0].value, 0x7FA3EB78u);  // mr r3, r29
    EXPECT_EQ(g[1].value, 0x38800000u);  // li r4, 0
    EXPECT_EQ(BranchTarget(g[2].address, g[2].value), std::optional<u32>(0x806A5A58));
    EXPECT_EQ(g[2].value & 1, 1u);       // bl
    EXPECT_EQ(g[3].value, 0x38000000u);  // li r0, 0
    EXPECT_EQ(g[4].value, 0x901D0634u);  // stw r0, 0x634(r29): the top page
    EXPECT_EQ(g[5].value, 0x38000003u);  // li r0, 3
    EXPECT_EQ(g[6].value, 0x901D0664u);  // stw r0, 0x664(r29)
    EXPECT_EQ(g[7].value, 0x38600001u);  // li r3, 1
    EXPECT_EQ(BranchTarget(g[8].address, g[8].value), std::optional<u32>(0x8117A1D4 + module));
    EXPECT_EQ(g[8].value & 1, 0u);  // b, not bl
    // The original was the window's creation: a bl too, to the connection window.
    EXPECT_TRUE(BranchTarget(g[0].address, *g[0].original).has_value());
  }
  // The ONLINE page's Options (proc 28) and With Anyone's Spectator (code 29) never show: their
  // isEnableButton checks (0x8014FEDC) answer no.
  for (const auto& [at, words, calls] : std::vector<std::tuple<u32, size_t, std::vector<size_t>>>{
           {0x81179A04, 8, {1, 7}}, {0x81191D7C, 3, {1}}})
  {
    SCOPED_TRACE(at);
    const auto g = GroupAt(patches, at + module);
    ASSERT_EQ(g.size(), words);
    for (size_t i = 0; i < g.size(); ++i)
    {
      const bool call = std::find(calls.begin(), calls.end(), i) != calls.end();
      if (call)
      {
        EXPECT_EQ(BranchTarget(g[i].address, *g[i].original), std::optional<u32>(0x8014FEDC));
        EXPECT_EQ(g[i].value, 0x38600000u);  // li r3, 0
      }
      else
      {
        EXPECT_EQ(g[i].value, *g[i].original);  // a guard
      }
    }
  }
  // G4: sora_scene's exit table, the same in both games: every online sequence becomes local
  // Versus (sqVsMelee), code 28 untouched.
  {
    const auto g = GroupAt(patches, 0x80701A04);
    ASSERT_EQ(g.size(), 8u);
    EXPECT_EQ(Originals(g), (std::vector<u32>{0x806DC8C8, 0x806DC8C8, 0x806DC8C8, 0x806DC8C8,
                                              0x806DC940, 0x806DC8E0, 0x806DC8F8, 0x806DC910}));
    EXPECT_EQ(Values(g), (std::vector<u32>{0x806DC618, 0x806DC618, 0x806DC618, 0x806DC618,
                                           0x806DC940, 0x806DC618, 0x806DC618, 0x806DC618}));
  }
  // Nothing patches the connection's own code: the window is never created, and nothing else of
  // Nintendo's online code is reached.
  EXPECT_FALSE(AnyLineIn(patches, 0x800C9B44, 0x800C9D40));
}
}  // namespace

TEST(OrcaOnlineMenuGates, BrawlOpensTheOnlinePageWithNoConnection)
{
  const auto patches = Shipped("RSBE01.patches");
  // G1: the main menu's cursor table is the game's own again (the button can be selected).
  EXPECT_FALSE(AnyLineIn(patches, 0x811A4070, 0x811A40CC));
  ExpectOnlineGates(patches, 0);
}

TEST(OrcaOnlineMenuGates, ProjectPlusOpensTheOnlinePageWithNoConnection)
{
  const auto patches = Shipped("PPLUS32.patches");
  // Every Project+ line is guarded: a group lands only while all its originals are there.
  for (const GamePatch& p : patches)
    EXPECT_TRUE(p.original.has_value()) << std::hex << p.address;
  // Project+'s cursor table (0x811A54FC) was never patched: its button was always selectable.
  EXPECT_FALSE(AnyLineIn(patches, 0x811A54FC, 0x811A5558));
  ExpectOnlineGates(patches, 0x1480);
}

namespace
{
// A PNG's size from its IHDR chunk.
std::optional<std::pair<u32, u32>> PngSize(const std::string& path)
{
  File::IOFile f(path, "rb");
  u8 head[24] = {};
  if (!f.ReadBytes(head, sizeof(head)))
    return std::nullopt;
  static constexpr u8 SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (!std::equal(std::begin(SIGNATURE), std::end(SIGNATURE), head) ||
      std::string(reinterpret_cast<const char*>(head + 12), 4) != "IHDR")
  {
    return std::nullopt;
  }
  const auto be32 = [&](int i) {
    return u32(head[i]) << 24 | u32(head[i + 1]) << 16 | u32(head[i + 2]) << 8 | head[i + 3];
  };
  return std::pair{be32(16), be32(20)};
}

struct TextureName
{
  u32 width = 0, height = 0;
  std::string hash, format;
};

// Dolphin's name for a texture without mipmaps or a palette: tex1_<w>x<h>_<16 hex>_<format>.
std::optional<TextureName> ParseTextureName(const std::string& stem)
{
  TextureName n;
  char hash[17] = {}, format[4] = {};
  int used = 0;
  if (std::sscanf(stem.c_str(), "tex1_%ux%u_%16[0-9a-f]_%3[0-9]%n", &n.width, &n.height, hash,
                  format, &used) != 4 ||
      used != static_cast<int>(stem.size()) || std::string(hash).size() != 16 || n.width == 0 ||
      n.height == 0 || n.width > 1024 || n.height > 1024)
  {
    return std::nullopt;
  }
  n.hash = hash;
  n.format = format;
  return n;
}
}  // namespace

TEST(OrcaOnlineMenuTextures, TextureNamesParse)
{
  EXPECT_TRUE(ParseTextureName("tex1_127x96_fdd5334e5af7357c_2").has_value());
  for (const char* bad : {"tex1_127x96_fdd5334e5af7357_2", "tex1_127x96_m_fdd5334e5af7357c_2",
                          "tex1_127x96_fdd5334e5af7357C_2", "tex1_0x96_fdd5334e5af7357c_2",
                          "tex1_127x96_fdd5334e5af7357c_2_arb", "tex1_127x96_fdd5334e5af7357c_",
                          "tex2_127x96_fdd5334e5af7357c_2", "tex1_127x96_fdd5334e5af7357c_14x"})
  {
    EXPECT_FALSE(ParseTextureName(bad).has_value()) << bad;
  }
}

TEST(OrcaOnlineMenuTextures, EveryShippedTextureIsAnIntegerMultipleOfItsName)
{
  const std::filesystem::path root = SysOrca() / "Textures";
  // Each texture's size, and its mip levels' (<name>_mip<N>) by level.
  std::map<std::string, std::pair<u32, u32>> bases;
  std::map<std::string, std::map<u32, std::pair<u32, u32>>> levels;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
  {
    if (!entry.is_regular_file())
      continue;
    const std::string path = entry.path().string();
    SCOPED_TRACE(path);
    // Only PNGs, in a folder named for the game ID the game loads its textures as.
    EXPECT_EQ(entry.path().extension(), ".png");
    EXPECT_EQ(entry.path().parent_path().filename(), "RSBE01");
    std::string stem = entry.path().stem().string();
    const auto size = PngSize(path);
    ASSERT_TRUE(size.has_value());
    if (const size_t mip = stem.rfind("_mip"); mip != std::string::npos)
    {
      levels[stem.substr(0, mip)][static_cast<u32>(std::stoul(stem.substr(mip + 4)))] = *size;
      continue;
    }
    const auto name = ParseTextureName(stem);
    ASSERT_TRUE(name.has_value());
    // Formats without a palette (I4 0, IA4 2, RGB5A3 5, ...): their names carry no TLUT hash.
    EXPECT_TRUE(name->format != "8" && name->format != "9" && name->format != "10");
    const auto [w, h] = *size;
    EXPECT_EQ(w % name->width, 0u);
    EXPECT_EQ(h % name->height, 0u);
    EXPECT_EQ(w / name->width, h / name->height);  // one scale, the game's aspect
    EXPECT_GE(w / name->width, 1u);
    bases[stem] = *size;
  }
  // Brawl's PLAY ONLINE, ONLINE breadcrumb, CASUAL and RANKED; Project+'s CASUAL and RANKED.
  EXPECT_EQ(bases.size(), 6u);
  // Each has its mip levels, every one half the last (Dolphin drops a level of any other size),
  // down to the game's own size, so a label drawn small is never the 4x one read every fourth
  // texel. No level without its texture.
  for (const auto& [stem, size] : bases)
  {
    SCOPED_TRACE(stem);
    const auto name = ParseTextureName(stem);
    ASSERT_TRUE(name.has_value());
    auto [w, h] = size;
    u32 level = 0;
    for (const auto& [n, mip] : levels[stem])
    {
      EXPECT_EQ(n, ++level);
      w /= 2;
      h /= 2;
      EXPECT_EQ(mip, std::pair(w, h)) << "level " << n;
    }
    EXPECT_EQ(std::pair(w, h), std::pair(name->width, name->height));
  }
  EXPECT_EQ(levels.size(), bases.size());
}

TEST(OrcaOnlineMenuTextures, OrcasPackKeepsItsOwnNamesAndThePlayersPackTheRest)
{
  // A player's own pack for the game (<user>/Load/Textures/RSBE01): a Brawl HD pack that holds
  // two of the textures Orca replaces (the "NINTENDO WFC" title, the Wi-Fi button's label as an
  // _arb file in a subfolder) and one Orca doesn't touch.
  const std::string root = File::CreateTempDir();
  ASSERT_FALSE(root.empty());
  Common::ScopeGuard remove_root([&] { File::DeleteDirRecursively(root); });
  const std::string player = root + "/Load/Textures/RSBE01";
  const std::string player2 = root + "/Load/Textures/RSBE01-more";
  const std::string title = "tex1_120x16_a56825159146a723_0";
  const std::string button = "tex1_127x96_fdd5334e5af7357c_2";
  const std::string other = "tex1_64x64_0123456789abcdef_5";
  for (const std::string& path :
       {player + "/" + title + ".png", player + "/" + title + "_mip1.png",
        player + "/hd/" + button + "_arb.png", player + "/" + other + ".png",
        player + "/" + other + "_mip1.png", player + "/notes.png", player2 + "/" + other + ".dds"})
  {
    ASSERT_TRUE(File::CreateFullPath(path));
    ASSERT_TRUE(File::WriteStringToFile(path, "not decoded here"));
  }
  const std::string orca = (SysOrca() / "Textures" / "RSBE01").lexically_normal().string();
  const auto by_id = [](const std::vector<HiresTextureFile>& files) {
    std::map<std::string, HiresTextureFile> out;
    for (const HiresTextureFile& f : files)
      EXPECT_TRUE(out.emplace(f.id, f).second) << "two files for " << f.id;
    return out;
  };
  const auto under = [](const std::string& path, const std::string& directory) {
    return std::filesystem::path(path).lexically_normal().string().starts_with(
        std::filesystem::path(directory + "/").lexically_normal().string());
  };

  // In a session: Orca's six names are Orca's, wherever else they are; every other name is the
  // player's. Mip level files (<name>_mip<N>) load with their texture, never as one of their own.
  {
    const auto files = by_id(CollectHiresTextureFiles({player, player2}, {orca}));
    ASSERT_EQ(files.size(), 7u);
    for (const auto& [id, f] : files)
    {
      SCOPED_TRACE(id);
      EXPECT_EQ(f.orca_pack, id != other);
      EXPECT_TRUE(under(f.path, f.orca_pack ? orca : player));
      EXPECT_FALSE(f.has_arbitrary_mipmaps);
    }
    EXPECT_TRUE(files.contains(title) && files.contains(button));
    // The player's first directory still wins over its second (Dolphin's rule).
    EXPECT_EQ(std::filesystem::path(files.at(other).path).extension(), ".png");
  }
  // Outside a session Orca's pack isn't loaded: the player's pack has every name it holds.
  {
    const auto files = by_id(CollectHiresTextureFiles({player, player2}, {}));
    ASSERT_EQ(files.size(), 3u);
    for (const auto& [id, f] : files)
    {
      EXPECT_FALSE(f.orca_pack);
      EXPECT_TRUE(under(f.path, player)) << f.path;
    }
    EXPECT_TRUE(files.at(button).has_arbitrary_mipmaps);
    EXPECT_FALSE(files.at(title).has_arbitrary_mipmaps);
  }
  // A session with no pack of the player's: Orca's alone.
  EXPECT_EQ(CollectHiresTextureFiles({}, {orca}).size(), 6u);
}

namespace
{
using Where = MenuState::Where;
MenuState Menu()
{
  return {Where::Menu, 0};
}
MenuState Leaving(u32 code)
{
  return {Where::Leaving, code};
}
MenuState Other()
{
  return {Where::Other, 0};
}

// Feeds boundaries (first runs, alone, no resync unless given) and collects what is announced.
struct Feed
{
  OnlineMenuReader reader;
  u64 resyncs = 0;
  std::vector<OnlinePick> picks;
  void Step(const MenuState& s, bool alone = true, bool resimulating = false)
  {
    if (const auto pick = reader.Boundary(s, resimulating, alone, resyncs))
      picks.push_back(*pick);
  }
};
}  // namespace

TEST(OrcaOnlineMenuReader, EachPickIsAnnouncedOnceAsTheMenuLeaves)
{
  for (const auto& [code, pick] :
       std::vector<std::pair<u32, OnlinePick>>{{30, OnlinePick::Casual},
                                               {31, OnlinePick::Ranked},
                                               {24, OnlinePick::Friends},
                                               {25, OnlinePick::Friends},
                                               {26, OnlinePick::Friends},
                                               {27, OnlinePick::Friends}})
  {
    SCOPED_TRACE(code);
    Feed r;
    r.Step(Other());
    r.Step(Menu());
    r.Step(Menu());
    EXPECT_TRUE(r.picks.empty());
    // The code shows from the first boundary between scenes, and stays there for a few.
    r.Step(Leaving(code));
    r.Step(Leaving(code));
    r.Step(Leaving(code));
    r.Step(Other());  // the character select: the code reads 0 again
    r.Step(Other());
    EXPECT_EQ(r.picks, std::vector<OnlinePick>{pick});
  }
  EXPECT_EQ(MenuEventText(OnlinePick::Casual), "online casual local");
  EXPECT_EQ(MenuEventText(OnlinePick::Ranked), "online ranked local");
  EXPECT_EQ(MenuEventText(OnlinePick::Friends), "online friends");
}

TEST(OrcaOnlineMenuReader, OnlyOnlineExitsFromTheMenu)
{
  Feed r;
  r.Step(Menu());
  r.Step(Leaving(0));  // the code can show a boundary later
  r.Step(Leaving(30));
  r.Step(Other());
  ASSERT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Casual});
  r.picks.clear();
  // Group > Brawl (1), Spectator (29, never shown) and anything else: no pick.
  for (const u32 code : {1u, 2u, 23u, 28u, 29u, 32u, 0xFFFFFFFFu})
  {
    r.Step(Menu());
    r.Step(Leaving(code));
    r.Step(Other());
  }
  // A code seen between two other scenes was not the menu's.
  r.Step(Other());
  r.Step(Leaving(30));
  r.Step(Other());
  EXPECT_TRUE(r.picks.empty());
  // Back to the menu and out again: a new exit.
  r.Step(Menu());
  r.Step(Leaving(31));
  EXPECT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Ranked});
}

TEST(OrcaOnlineMenuReader, NeverOnReRunFrames)
{
  Feed r;
  r.Step(Menu());
  // A re-run of the exit frame (a rollback) says nothing and changes nothing...
  r.Step(Leaving(30), true, true);
  EXPECT_TRUE(r.picks.empty());
  // ...and a re-run that went elsewhere doesn't hide the first run that follows.
  r.Step(Other(), true, true);
  r.Step(Leaving(30));
  EXPECT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Casual});
  r.Step(Leaving(30), true, true);
  r.Step(Leaving(30));
  EXPECT_EQ(r.picks.size(), 1u);
}

TEST(OrcaOnlineMenuReader, APickMadeWhileNotAloneIsNeverAnnouncedLater)
{
  // The stale case: the pick happens while friends play (or one catches up); it is tracked, not
  // announced, and once the game is alone again nothing about it comes out.
  Feed r;
  r.Step(Menu(), false);
  r.Step(Leaving(30), false);
  r.Step(Leaving(30), true);  // alone again while the code still shows: still the same exit
  r.Step(Leaving(30), true);
  r.Step(Other(), true);
  EXPECT_TRUE(r.picks.empty());
  // A later pick while alone is announced.
  r.Step(Menu());
  r.Step(Leaving(25));
  EXPECT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Friends});
}

TEST(OrcaOnlineMenuReader, AloneMeansNoFriendIsAboutToLand)
{
  using Rollback::OnlineMatch::AloneAt;
  using Rollback::OnlineMatch::KEYFRAME_FRESH_FRAMES;
  EXPECT_EQ(KEYFRAME_FRESH_FRAMES, 30 * 60);  // 30 s
  // Solo, nothing of drop-in under way or waiting, no keyframe stored: alone.
  EXPECT_TRUE(AloneAt(5000, true, false, std::nullopt));
  // Not solo and idle (a friend plugged in or on the way, joining, catching up, a keyframe wanted
  // or being made).
  EXPECT_FALSE(AloneAt(5000, false, false, std::nullopt));
  // Drop-in work this boundary will take: a friend's arrival, a prepare-join, the app's join or
  // leave.
  EXPECT_FALSE(AloneAt(5000, true, true, std::nullopt));
  // A keyframe stored for an invite nobody took yet: a friend who arrives within 30 s of it
  // replays every frame since.
  EXPECT_FALSE(AloneAt(5000, true, false, 5000));
  EXPECT_FALSE(AloneAt(5000 + KEYFRAME_FRESH_FRAMES, true, false, 5000));
  // Older than that, an arrival gets a keyframe of its own, taken after this frame.
  EXPECT_TRUE(AloneAt(5000 + KEYFRAME_FRESH_FRAMES + 1, true, false, 5000));
  EXPECT_FALSE(AloneAt(5000 + KEYFRAME_FRESH_FRAMES + 1, true, true, 5000));
}

TEST(OrcaOnlineMenuReader, APickWhileAFriendIsAboutToLandIsNeverAnnounced)
{
  using Rollback::OnlineMatch::AloneAt;
  using Rollback::OnlineMatch::KEYFRAME_FRESH_FRAMES;
  {
    // The app invites a friend (prepare-join): the host stores a keyframe at frame 1000 and plays
    // on. The player picks Casual 10 s later: a friend who lands now replays that pick, so it is
    // never announced, not even once the keyframe is stale and the game alone again.
    Feed r;
    const std::optional<int> invite = 1000;
    int frame = 1600;
    const auto step = [&](const MenuState& s) {
      r.Step(s, AloneAt(frame, true, false, invite));
      frame += 50;
    };
    step(Menu());
    step(Leaving(30));
    step(Leaving(30));
    frame = 1000 + KEYFRAME_FRESH_FRAMES + 1;
    step(Leaving(30));
    step(Other());
    EXPECT_TRUE(r.picks.empty());
    // Back on the menu once the invite's keyframe is stale: the next pick is the player's.
    step(Menu());
    step(Leaving(31));
    EXPECT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Ranked});
  }
  {
    // A friend's arrival waits for the boundary the menu leaves at: the pick is the friend's too.
    Feed r;
    r.Step(Menu(), AloneAt(2000, true, false, std::nullopt));
    r.Step(Leaving(24), AloneAt(2001, true, true, std::nullopt));
    r.Step(Leaving(24), AloneAt(2002, false, false, std::nullopt));  // taken: a keyframe is wanted
    r.Step(Other(), AloneAt(2003, false, false, std::nullopt));
    EXPECT_TRUE(r.picks.empty());
  }
}

TEST(OrcaOnlineMenuReader, AResyncStartsOverWithoutAnnouncing)
{
  {
    // Seen on the menu during a session; the session ends (GoSolo) and the game's state is
    // what it is: an exit already under way is not news.
    Feed r;
    r.Step(Menu(), false);
    ++r.resyncs;
    r.Step(Leaving(30));
    r.Step(Leaving(30));
    r.Step(Other());
    EXPECT_TRUE(r.picks.empty());
  }
  {
    // A friend's keyframe loaded: the host's game sits on its menu, about to leave it. The first
    // boundary after the load is where the reader starts; what follows is new.
    Feed r;
    r.Step(Other(), false);
    ++r.resyncs;
    r.Step(Menu(), false);
    r.Step(Leaving(30), false);  // the joiner, catching up: not alone
    EXPECT_TRUE(r.picks.empty());
    ++r.resyncs;  // it leaves the host (BecomeSolo) and plays on alone
    r.Step(Other());
    r.Step(Menu());
    r.Step(Leaving(31));
    EXPECT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Ranked});
  }
  {
    // A resync that lands on the menu itself: the next exit counts.
    Feed r;
    r.Step(Other());
    ++r.resyncs;
    r.Step(Menu());
    r.Step(Leaving(24));
    EXPECT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Friends});
  }
  {
    // The very first boundary is a start too: a process that boots into an exit announces nothing.
    Feed r;
    r.Step(Leaving(30));
    r.Step(Other());
    EXPECT_TRUE(r.picks.empty());
  }
}

namespace
{
// Emulated memory as a map of bytes (big-endian words).
class FakeMemory final : public GuestMemory
{
public:
  bool Valid(u32 a) const override { return bytes.contains(a); }
  u8 Read8(u32 a) const override { return bytes.at(a); }
  u16 Read16(u32 a) const override { return static_cast<u16>(Read8(a) << 8 | Read8(a + 1)); }
  u32 Read32(u32 a) const override { return u32(Read16(a)) << 16 | Read16(a + 2); }
  void Write8(u32 a, u8 v) override
  {
    bytes[a] = v;
    ++writes;
  }
  void Write16(u32 a, u16 v) override
  {
    Write8(a, static_cast<u8>(v >> 8));
    Write8(a + 1, static_cast<u8>(v));
  }
  void Write32(u32 a, u32 v) override
  {
    Write16(a, static_cast<u16>(v >> 16));
    Write16(a + 2, static_cast<u16>(v));
  }
  void Fill(u32 a, u32 n)
  {
    for (u32 i = 0; i < n; ++i)
      bytes[a + i] = 0;
  }
  void Text(u32 a, std::string_view s)
  {
    for (size_t i = 0; i < s.size(); ++i)
      bytes[a + static_cast<u32>(i)] = static_cast<u8>(s[i]);
    bytes[a + static_cast<u32>(s.size())] = 0;
  }
  std::map<u32, u8> bytes;
  int writes = 0;
};

// Brawl's scene manager at 0x805A0060 -> 0x80900000; its scene at +4 -> 0x80910000, named at
// 0x80920000; the exit code at +0x284.
FakeMemory Scene(std::string_view name, u32 exit_code)
{
  FakeMemory m;
  m.Fill(0x805A0060, 4);
  m.Fill(0x80900000, 0x300);
  m.Fill(0x80910000, 4);
  m.Write32(0x805A0060, 0x80900000);
  m.Write32(0x80900004, 0x80910000);
  m.Write32(0x80910000, 0x80920000);
  m.Text(0x80920000, name);
  m.Write32(0x80900284, exit_code);
  m.writes = 0;
  return m;
}
}  // namespace

TEST(OrcaOnlineMenuReader, ReadsTheSceneAndTheExitCode)
{
  {
    const FakeMemory m = Scene("muMenuMain", 0);
    const MenuState s = ReadMenuState(m);
    EXPECT_EQ(s.where, Where::Menu);
    EXPECT_EQ(s.exit_code, 0u);
  }
  {
    const FakeMemory m = Scene("scMemoryChange", 30);
    const MenuState s = ReadMenuState(m);
    EXPECT_EQ(s.where, Where::Leaving);
    EXPECT_EQ(s.exit_code, 30u);
  }
  {
    const FakeMemory m = Scene("scSelctCharacter", 30);
    EXPECT_EQ(ReadMenuState(m).where, Where::Other);
    EXPECT_EQ(ReadMenuState(m).exit_code, 0u);  // only read on the menu or between scenes
  }
  {
    // Not mapped, or a name that never ends: nothing.
    FakeMemory m;
    EXPECT_EQ(ReadMenuState(m).where, Where::Other);
    FakeMemory n = Scene("muMenuMain", 0);
    for (u32 i = 0; i < 40; ++i)
      n.bytes[0x80920000 + i] = 'x';
    EXPECT_EQ(ReadMenuState(n).where, Where::Other);
    FakeMemory p = Scene("muMenuMain", 0);
    p.Write32(0x80900004, 0x80910001);  // a misaligned scene pointer
    EXPECT_EQ(ReadMenuState(p).where, Where::Other);
  }
  // Reading never writes.
  FakeMemory m = Scene("scMemoryChange", 30);
  (void)ReadMenuState(m);
  EXPECT_EQ(m.writes, 0);
}

namespace
{
// The main menu as MenuText.h finds it: Scene("muMenuMain") with its gfArchive* at +0xAC0, the
// archive object's third word pointing at the ARC image, whose MiscData 5 and 10 are message files
// (a u32 offset table, then single-byte text). Only the messages a test names are real; the rest
// are short placeholders.
constexpr u32 ARCHIVE_OBJECT = 0x81500000;
constexpr u32 IMAGE = 0x92000000;

struct ArcFile
{
  u16 index;
  std::vector<std::string> messages;
};

// Writes an ARC image at IMAGE: entry 100 (filler), then each file; returns where each file's data
// starts.
std::map<u16, u32> WriteArchive(FakeMemory& m, const std::vector<ArcFile>& files,
                                std::string_view name = "mu_menumain_en")
{
  m.Fill(IMAGE, 0x40);
  m.Write32(IMAGE, 0x41524300);  // "ARC\0"
  m.Write16(IMAGE + 6, static_cast<u16>(files.size() + 1));
  m.Text(IMAGE + 0x10, name);
  std::map<u16, u32> where;
  u32 entry = IMAGE + 0x40;
  auto add = [&](u16 index, const std::vector<u8>& data) {
    m.Fill(entry, 0x20);
    m.Write16(entry, 1);  // MiscData
    m.Write16(entry + 2, index);
    m.Write32(entry + 4, static_cast<u32>(data.size()));
    const u32 at = entry + 0x20;
    for (size_t i = 0; i < data.size(); ++i)
      m.bytes[at + static_cast<u32>(i)] = data[i];
    where[index] = at;
    entry = (at + static_cast<u32>(data.size()) + 31) & ~31u;
  };
  add(100, std::vector<u8>(0x40, 0xAA));
  for (const ArcFile& f : files)
  {
    std::vector<u8> data(4 * (f.messages.size() + 1), 0);
    u32 offset = static_cast<u32>(data.size());
    for (size_t k = 0; k <= f.messages.size(); ++k)
    {
      for (int b = 0; b < 4; ++b)
        data[4 * k + b] = static_cast<u8>(offset >> (24 - 8 * b));
      if (k < f.messages.size())
        offset += static_cast<u32>(f.messages[k].size());
    }
    for (const std::string& text : f.messages)
      data.insert(data.end(), text.begin(), text.end());
    add(f.index, data);
  }
  return where;
}

FakeMemory MainMenu()
{
  FakeMemory m = Scene("muMenuMain", 0);
  m.Fill(0x80910000, 0xAC4);
  m.Write32(0x80910000, 0x80920000);
  m.Write32(0x80910000 + 0xAC0, ARCHIVE_OBJECT);
  m.Fill(ARCHIVE_OBJECT, 0x80);
  m.Write32(ARCHIVE_OBJECT + 4, 0x92000004);  // not the image: misaligned
  m.Write32(ARCHIVE_OBJECT + 8, IMAGE);
  m.writes = 0;
  return m;
}

// The original of the first edit of `file`/`message` whose original starts with `prefix`.
const MenuTextEdit& Edit(u32 file, u32 message, std::string_view prefix)
{
  for (const MenuTextEdit& e : MenuTextEdits())
  {
    if (e.file == file && e.message == message &&
        e.original.substr(0, std::min(prefix.size(), e.original.size())) == prefix)
    {
      return e;
    }
  }
  ADD_FAILURE() << file << "." << message;
  return MenuTextEdits()[0];
}

std::string Bytes(const FakeMemory& m, u32 at, size_t n)
{
  std::string out;
  for (size_t i = 0; i < n; ++i)
    out.push_back(static_cast<char>(m.Read8(at + static_cast<u32>(i))));
  return out;
}

std::vector<std::string> Placeholders(size_t n)
{
  std::vector<std::string> out;
  for (size_t i = 0; i < n; ++i)
    out.push_back("x" + std::to_string(i) + std::string(1, '\x13'));
  return out;
}

constexpr char BRAWL_COLOUR[] = "\x12\x02\x0c"
                                "333\xff";
constexpr char PPLUS_COLOUR[] = "\x12\x02\x0c\xbf\xc7\xc4\xff";

// What the edits take out of the game's words.
constexpr const char* TAKEN_OUT[] = {
    "Nintendo WFC",   "(Discontinued)",   "(Project M not compatible!)",
    "(No names)",     "2-on-2",           "Register",
    "defeat enemies", "Home-Run Contest", "10 bosses",
    "(2 players)"};
}  // namespace

TEST(OrcaOnlineMenuText, EveryEditFitsItsSlotAndSaysNothingOfNintendo)
{
  ASSERT_FALSE(MenuTextEdits().empty());
  for (const MenuTextEdit& e : MenuTextEdits())
  {
    SCOPED_TRACE(std::string(e.text));
    EXPECT_LE(e.text.size(), e.original.size());
    EXPECT_NE(e.text, e.original);
    // Single-byte text: the same colour code first, the colour's end last, nothing else below 0x20.
    EXPECT_EQ(e.text.substr(0, 7), e.original.substr(0, 7));
    EXPECT_EQ(e.text.back(), '\x13');
    for (size_t i = 7; i + 1 < e.text.size(); ++i)
      EXPECT_GE(static_cast<u8>(e.text[i]), 0x20) << i;
    for (const char* word : {"Nintendo", "WFC", "Wiimmfi", "Wi-Fi", "Discontinued", "Project M"})
      EXPECT_EQ(e.text.find(word), std::string_view::npos) << word;
    // Each original is one of the game's own: it names what we take out (Nintendo's service, or a
    // mode, a roster or a promise the page no longer leads to).
    const bool names_it =
        std::any_of(std::begin(TAKEN_OUT), std::end(TAKEN_OUT), [&](const char* what) {
          return e.original.find(what) != std::string_view::npos;
        });
    EXPECT_TRUE(names_it);
    for (const char* what : TAKEN_OUT)
      EXPECT_EQ(e.text.find(what), std::string_view::npos) << what;
  }
}

TEST(OrcaOnlineMenuText, WithFriendsSaysWhereEachButtonGoes)
{
  // Every With Friends button leaves for the local Versus character select, where friends drop in
  // (2-4 players: Orca's four seats). Friend Roster (45), Team Multi-Man (47), Home-Run Contest
  // (48) and the Boss Battles text no button shows (49) are rewritten in both games; the Brawl /
  // FIGHT! button's own (46) already says just that.
  for (const char* colour : {BRAWL_COLOUR, PPLUS_COLOUR})
  {
    SCOPED_TRACE(colour == BRAWL_COLOUR ? "Brawl" : "Project+");
    std::vector<std::string> five = Placeholders(50);
    five[46] = std::string(colour) + "Take on your friends! (2-4 players)\x13";
    for (const u32 k : {45u, 47u, 48u, 49u})
    {
      const MenuTextEdit& e = Edit(5, k, colour);
      ASSERT_EQ(e.original.substr(0, 7), colour);
      five[k] = std::string(e.original);
      std::string lower(e.text);
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](char c) { return static_cast<char>(std::tolower(static_cast<u8>(c))); });
      EXPECT_NE(lower.find("friends"), std::string::npos) << k;
      EXPECT_NE(e.text.find("(2-4 players)"), std::string_view::npos) << k;
      // Each game's own word for a Versus match.
      EXPECT_NE(lower.find(colour == BRAWL_COLOUR ? "brawl" : "fight"), std::string::npos) << k;
    }
    FakeMemory m = MainMenu();
    const auto where = WriteArchive(m, {{5, five}});
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 4);
    const u32 file = where.at(5);
    for (const u32 k : {45u, 47u, 48u, 49u})
    {
      const MenuTextEdit& e = Edit(5, k, colour);
      EXPECT_EQ(Bytes(m, file + m.Read32(file + 4 * k), e.text.size()), e.text) << k;
    }
    EXPECT_EQ(Bytes(m, file + m.Read32(file + 4 * 46), five[46].size()), five[46]);
  }
}

TEST(OrcaOnlineMenuText, RewritesTheGamesOwnWordsInPlace)
{
  const MenuTextEdit& footer = Edit(5, 2, BRAWL_COLOUR);
  const MenuTextEdit& record = Edit(10, 48, "");
  std::vector<std::string> five = Placeholders(80);
  five[1] = std::string(BRAWL_COLOUR) + "Play this mode solo or challenge it cooperatively.\x13";
  five[2] = std::string(footer.original);
  std::vector<std::string> ten = Placeholders(50);
  ten[48] = std::string(record.original);
  FakeMemory m = MainMenu();
  const auto where = WriteArchive(m, {{5, five}, {9, Placeholders(3)}, {10, ten}});
  const u32 file5 = where.at(5), file10 = where.at(10);
  ASSERT_EQ(FindMenuArchive(m), IMAGE);
  EXPECT_EQ(FindArchiveFile(m, IMAGE, 5), file5);
  EXPECT_EQ(FindArchiveFile(m, IMAGE, 10), file10);
  EXPECT_EQ(FindArchiveFile(m, IMAGE, 11), 0u);
  const FakeMemory before = m;

  EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 2);
  // Message 2: the new words, then zeros to the end of the same slot; its neighbours untouched.
  const u32 at = file5 + m.Read32(file5 + 4 * 2);
  EXPECT_EQ(m.Read32(file5 + 4 * 3) - m.Read32(file5 + 4 * 2), footer.original.size());
  EXPECT_EQ(Bytes(m, at, footer.text.size()), footer.text);
  for (size_t i = footer.text.size(); i < footer.original.size(); ++i)
    EXPECT_EQ(m.Read8(at + static_cast<u32>(i)), 0) << i;
  const u32 at10 = file10 + m.Read32(file10 + 4 * 48);
  EXPECT_EQ(Bytes(m, at10, record.text.size()), record.text);
  size_t changed = 0;
  for (const auto& [address, value] : before.bytes)
    changed += m.bytes.at(address) != value;
  EXPECT_LE(changed, footer.original.size() + record.original.size());
  EXPECT_EQ(Bytes(m, file5 + m.Read32(file5 + 4), five[1].size()), five[1]);

  // Once rewritten, nothing matches any more: no writes at later boundaries.
  m.writes = 0;
  EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 0);
  EXPECT_EQ(m.writes, 0);
}

TEST(OrcaOnlineMenuText, WritesNothingUnlessItIsTheMenusOwnText)
{
  const MenuTextEdit& footer = Edit(5, 2, BRAWL_COLOUR);
  std::vector<std::string> five = Placeholders(4);
  five[2] = std::string(footer.original);
  {
    // A word of difference (another version of the game, or a mod's own text): left alone.
    std::vector<std::string> other = five;
    other[2][10] ^= 0x20;
    FakeMemory m = MainMenu();
    WriteArchive(m, {{5, other}});
    m.writes = 0;
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 0);
    EXPECT_EQ(m.writes, 0);
  }
  {
    // The same words in a slot of another size (the offset table says so): left alone.
    std::vector<std::string> longer = five;
    longer[2] += '\0';
    FakeMemory m = MainMenu();
    WriteArchive(m, {{5, longer}});
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 0);
  }
  {
    // Another archive in the menu's place, or no archive yet.
    FakeMemory m = MainMenu();
    WriteArchive(m, {{5, five}}, "mu_menumain_jp");
    EXPECT_EQ(FindMenuArchive(m), 0u);
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 0);
    FakeMemory n = MainMenu();
    EXPECT_EQ(ApplyMenuText(n, MenuTextEdits()), 0);
  }
  {
    // Not the main menu (the archive's memory may hold anything once the menu is gone).
    FakeMemory m = MainMenu();
    WriteArchive(m, {{5, five}});
    m.Text(0x80920000, "scSelctCharacter");
    m.writes = 0;
    EXPECT_EQ(FindMenuArchive(m), 0u);
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 0);
    EXPECT_EQ(m.writes, 0);
  }
  {
    // A file still loading: its offset table runs past what is there.
    FakeMemory m = MainMenu();
    const auto where = WriteArchive(m, {{5, five}});
    m.Write32(where.at(5) + 4 * 3, 0x7FFFFFF0);
    m.writes = 0;
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 0);
    EXPECT_EQ(m.writes, 0);
    // An entry whose size runs off the end of memory ends the walk.
    FakeMemory n = MainMenu();
    WriteArchive(n, {{5, five}});
    n.Write32(IMAGE + 0x40 + 4, 0xFFFFFF00);
    EXPECT_EQ(FindArchiveFile(n, IMAGE, 5), 0u);
  }
}
