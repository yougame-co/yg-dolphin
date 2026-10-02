// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/MenuText.h"

#include <array>
#include <string_view>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/NameTags.h"

namespace Orca::UX
{
namespace
{
using namespace std::string_view_literals;

// The message files' colour codes: 0x12 0x02 0x0C r g b a, then 0x13 to end the colour (a hex
// escape takes every hex digit after it, hence the split before "333").
// clang-format off
#define BRAWL "\x12\x02\x0c" "333\xff"
#define PPLUS "\x12\x02\x0c\xbf\xc7\xc4\xff"
#define RECORD "\x12\x02\x0c\xc0\xc0\xc0\xff"
#define END "\x13"
// clang-format on

// Read from the disc's and Project+'s mu_menumain archives (MiscData 5 and 10), each original
// checked byte for byte there. Grouped by file (one archive walk per file per boundary).
constexpr std::array<MenuTextEdit, 9> EDITS{{
    // The main menu's footer under the Online button. Brawl: 'Play different modes while
    // connected to Nintendo WFC.'; Project+: 'Play different modes online. (Discontinued)'.
    {5, 2, BRAWL "Play different modes while connected to " BRAWL "Nintendo WFC." END END ""sv,
     BRAWL "Play different modes online." END ""sv},
    {5, 2, PPLUS "Play different modes online. (Discontinued)" END ""sv,
     PPLUS "Play different modes online." END ""sv},
    // The ONLINE page's With Anyone (Project+ only: 'Play against random people! (Project M not
    // compatible!)'; Brawl's names no one).
    {5, 19, PPLUS "Play against random people! (Project M not compatible!)" END ""sv,
     PPLUS "Play against random people!" END ""sv},
    // With Anyone's first button, now Casual: 'Go online for a quick brawl (fight) with someone,
    // somewhere. (No names)'.
    {5, 51, BRAWL "Go online for a quick brawl with someone, somewhere. (No names)" END ""sv,
     BRAWL "Go online for a quick brawl with someone, somewhere." END ""sv},
    {5, 51, PPLUS "Go online for a quick fight with someone, somewhere. (No names)" END ""sv,
     PPLUS "Go online for a quick fight with someone, somewhere." END ""sv},
    // Its second, now Ranked: 'Form a team with someone, somewhere for a 2-on-2 battle!'.
    {5, 52, BRAWL "Form a team with someone, somewhere for a 2-on-2 battle!" END ""sv,
     BRAWL "Brawl someone at your level for a higher rank!" END ""sv},
    {5, 52, PPLUS "Form a team with someone, somewhere for a 2-on-2 battle!" END ""sv,
     PPLUS "Fight someone at your level for a higher rank!" END ""sv},
    // Data > Records (Brawl): 'View group records (excludes Nintendo WFC brawls).', a colour per
    // piece.
    {5, 78,
     BRAWL "View" BRAWL " " END BRAWL "g" END BRAWL "roup " END BRAWL "r" END BRAWL
           "ecords (" END BRAWL "e" END BRAWL "xcludes " END BRAWL "Nintendo WFC" END BRAWL
           " " END BRAWL "b" END BRAWL "rawls)" END BRAWL "." END END ""sv,
     BRAWL "View group records (excludes online brawls)." END ""sv},
    // A record's description, both games: 'Total vs. matches, including Nintendo WFC matches.'
    {10, 48, RECORD "Total vs. matches, including Nintendo WFC matches." END ""sv,
     RECORD "Total vs. matches, including online matches." END ""sv},
}};

#undef BRAWL
#undef PPLUS
#undef RECORD
#undef END

constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER_SCENE = 0x4;
constexpr u32 MENU_ARCHIVE = 0xAC0;      // muMenuMain's gfArchive*
constexpr u32 ARCHIVE_WORDS = 0x80 / 4;  // sizeof(gfArchive): one of its words is the image
constexpr u32 ARC_MAGIC = 0x41524300;    // "ARC\0"
constexpr u32 ARC_COUNT = 0x6;           // u16 entry count
constexpr u32 ARC_NAME = 0x10;
constexpr u32 ARC_FIRST = 0x40;
constexpr u32 ENTRY_HEADER = 0x20;  // u16 type, u16 index, u32 size, ...; the data follows
constexpr u16 TYPE_MISC = 1;
constexpr u32 MAX_ENTRIES = 64;
constexpr u32 MAX_MESSAGES = 1024;

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

bool StringAt(const GuestMemory& m, u32 at, std::string_view want)
{
  for (size_t i = 0; i <= want.size(); ++i)
  {
    const u32 a = at + static_cast<u32>(i);
    if (!m.Valid(a) || m.Read8(a) != (i < want.size() ? static_cast<u8>(want[i]) : 0))
      return false;
  }
  return true;
}
}  // namespace

std::span<const MenuTextEdit> MenuTextEdits()
{
  return EDITS;
}

u32 FindMenuArchive(const GuestMemory& m)
{
  if (!Pointer(m, SCENE_MANAGER))
    return 0;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_SCENE))
    return 0;
  const u32 scene = m.Read32(manager + MANAGER_SCENE);
  if (!Pointer(m, scene) || !Pointer(m, scene + MENU_ARCHIVE) ||
      !StringAt(m, m.Read32(scene), "muMenuMain"))
  {
    return 0;
  }
  const u32 archive = m.Read32(scene + MENU_ARCHIVE);
  if (!Pointer(m, archive) || !m.Valid(archive + ARCHIVE_WORDS * 4 - 1))
    return 0;
  // The archive object keeps its file image: the word that points at the menu's ARC.
  for (u32 i = 0; i < ARCHIVE_WORDS; ++i)
  {
    const u32 image = m.Read32(archive + 4 * i);
    if (image % 32 == 0 && Pointer(m, image) && m.Valid(image + ARC_FIRST - 1) &&
        m.Read32(image) == ARC_MAGIC && StringAt(m, image + ARC_NAME, "mu_menumain_en"))
    {
      return image;
    }
  }
  return 0;
}

u32 FindArchiveFile(const GuestMemory& m, u32 archive, u32 file)
{
  if (!Pointer(m, archive) || !m.Valid(archive + ARC_FIRST - 1))
    return 0;
  const u32 count = m.Read16(archive + ARC_COUNT);
  u32 entry = archive + ARC_FIRST;
  for (u32 i = 0; i < count && i < MAX_ENTRIES; ++i)
  {
    if (!Pointer(m, entry) || !m.Valid(entry + ENTRY_HEADER - 1))
      return 0;
    const u16 type = m.Read16(entry);
    const u16 index = m.Read16(entry + 2);
    const u32 size = m.Read32(entry + 4);
    const u32 data = entry + ENTRY_HEADER;
    if (type == TYPE_MISC && index == file)
      return m.Valid(data) && (size == 0 || m.Valid(data + size - 1)) ? data : 0;
    // Entries are 32-byte aligned.
    const u64 next = (static_cast<u64>(data) + size + 31) & ~u64{31};
    if (next > 0xFFFFFFFFull)
      return 0;
    entry = static_cast<u32>(next);
  }
  return 0;
}

int ApplyMenuText(GuestMemory& m, std::span<const MenuTextEdit> edits)
{
  const u32 archive = FindMenuArchive(m);
  if (!archive)
    return 0;
  int written = 0;
  u32 file_for = ~0u, messages = 0;
  for (const MenuTextEdit& e : edits)
  {
    if (e.file != file_for)
    {
      file_for = e.file;
      messages = FindArchiveFile(m, archive, e.file);
    }
    if (!messages || !Pointer(m, messages))
      continue;
    // A table of u32 offsets; the first one is where the text starts, so it says how many.
    const u32 count = m.Read32(messages) / 4;
    if (count < 2 || count > MAX_MESSAGES || e.message + 1 >= count ||
        !m.Valid(messages + 4 * (e.message + 1) + 3))
    {
      continue;
    }
    const u32 from = m.Read32(messages + 4 * e.message);
    const u32 to = m.Read32(messages + 4 * (e.message + 1));
    if (to < from || to - from != e.original.size() || e.text.size() > e.original.size() ||
        !m.Valid(messages + to - 1))
    {
      continue;
    }
    const u32 at = messages + from;
    bool same = true;
    for (size_t i = 0; i < e.original.size() && same; ++i)
      same = m.Read8(at + static_cast<u32>(i)) == static_cast<u8>(e.original[i]);
    if (!same)
      continue;
    for (size_t i = 0; i < e.original.size(); ++i)
      m.Write8(at + static_cast<u32>(i), i < e.text.size() ? static_cast<u8>(e.text[i]) : 0);
    ++written;
  }
  return written;
}

void WriteMenuText(const Core::CPUThreadGuard& guard)
{
  // Brawl rev 2 itself or a launcher profile booting it (Project+).
  const Orca::Profile* profile = Orca::ActiveProfile();
  if (!profile || profile->revision != 2 ||
      (profile->IsLauncher() ? profile->disc != "RSBE01" : profile->game_id != "RSBE01"))
  {
    return;
  }
  GuardMemory memory(guard);
  if (const int n = ApplyMenuText(memory, MenuTextEdits()))
    INFO_LOG_FMT(ROLLBACK, "Orca: {} main menu text(s) rewritten", n);
}
}  // namespace Orca::UX
