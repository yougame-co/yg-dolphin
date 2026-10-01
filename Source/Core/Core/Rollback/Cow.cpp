// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Cow.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include <xxh3.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/MemTools.h"
#include "Core/Rollback/UndoLog.h"
#include "Core/System.h"

namespace Rollback::Cow
{
namespace
{
// The granularity a load compares, restores and reports changed RAM in (Rollback.cpp invalidates
// the JIT per changed block).
constexpr std::size_t RESTORE_BLOCK = 4096;
constexpr u32 MEM2_PHYSICAL = 0x10000000u;
constexpr std::size_t MAX_VIEWS = 64;
// The widest single store a host CPU makes (AVX-512, arm64 DC ZVA).
constexpr std::size_t MAX_ACCESS = 64;
// Free pre-image buffers kept ready at each snapshot, so a fault rarely allocates.
constexpr std::size_t RESERVE_PAGES = 512;

// Hot pages. A fault costs far more than copying a page (Apple M-series: ~12 us against under 1 us
// to compare and copy 16 KB), and a game writes much the same pages every frame. So a page written
// since the previous snapshot can stay writable through the next one, its pre-image copied into the
// new log by the snapshot itself: a page that is in the newest log needs no fault. Its heat says
// for how many more snapshots: set when a fault records it, set higher while its bytes keep
// changing from one snapshot to the next, counting down while they don't. At zero it is read-only
// again and its next write faults. Brawl's fight frames on an M4 Pro: ~450 hot pages copied
// (~7 MB) and ~4 faults per snapshot; 4 and 16 gave ~50 faults and 2.9 ms frames against 2.6 ms.
// YG_COW_FAULT_HEAT and YG_COW_HEAT override them (0-255), to tune another machine.
int EnvHeat(const char* name, int fallback)
{
  const char* value = std::getenv(name);
  return value && *value ? std::clamp(std::atoi(value), 0, 255) : fallback;
}
const u8 HEAT_AFTER_FAULT = static_cast<u8>(EnvHeat("YG_COW_FAULT_HEAT", 64));
const u8 HEAT_WHILE_CHANGING = static_cast<u8>(EnvHeat("YG_COW_HEAT", 255));

struct ViewInfo
{
  u8* base;
  u8* end;
  std::size_t first_page;
};

struct Tracker
{
  std::mutex lock;
  const void* owner = nullptr;
  std::size_t page_size = 0;
  std::vector<Area> areas;
  std::vector<std::size_t> area_first_page;  // global index of each area's first page
  std::vector<ViewInfo> views;               // writeable views only
  std::unique_ptr<UndoLog> log;
  // Per page: bit v set when the page is writable in views[v]. Only pages in the newest log can be.
  std::vector<u64> open_in;
  std::vector<u32> open_pages;  // pages whose open_in is nonzero
  // Per page: how many more snapshots it stays writable (above), and whether a fault recorded it
  // since the newest snapshot (rather than the snapshot copying it up front).
  std::vector<u8> heat;
  std::vector<u8> fault_recorded;
  Counters counters;
  // Snapshot ids are unique across arms, so a ring's stale id never names another ring's snapshot.
  u64 next_id = 1;
};

// Never destroyed: a fault (or a ring's static destructor) may still look at it at exit.
Tracker& T()
{
  static Tracker* const tracker = new Tracker;
  return *tracker;
}

// HandleFault's lock-free early out.
std::atomic<bool> s_armed{false};

std::size_t HostPageSize()
{
#ifdef _WIN32
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwPageSize;
#else
  return static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
}

bool Protect(u8* address, std::size_t size, bool writable)
{
#ifdef _WIN32
  DWORD old_protect;
  if (VirtualProtect(address, size, writable ? PAGE_READWRITE : PAGE_READONLY, &old_protect))
    return true;
#else
  if (mprotect(address, size, writable ? (PROT_READ | PROT_WRITE) : PROT_READ) == 0)
    return true;
#endif
  ERROR_LOG_FMT(MEMMAP, "Rollback COW: could not make {} bytes at {} {}", size, fmt::ptr(address),
                writable ? "writable" : "read-only");
  return false;
}

u8* AliasOf(const Tracker& t, std::size_t page)
{
  const std::size_t area = static_cast<std::size_t>(
      std::upper_bound(t.area_first_page.begin(), t.area_first_page.end(), page) -
      t.area_first_page.begin() - 1);
  return t.areas[area].alias + (page - t.area_first_page[area]) * t.page_size;
}

u32 PhysicalOf(const Tracker& t, std::size_t page)
{
  const std::size_t area = static_cast<std::size_t>(
      std::upper_bound(t.area_first_page.begin(), t.area_first_page.end(), page) -
      t.area_first_page.begin() - 1);
  return t.areas[area].physical_address +
         static_cast<u32>((page - t.area_first_page[area]) * t.page_size);
}

// The writeable views, cut to the tracked areas, as page ranges. False if one is not page-aligned
// or there are too many.
bool BuildViews(const Tracker& t, const std::vector<View>& views, std::vector<ViewInfo>* out)
{
  out->clear();
  for (const View& view : views)
  {
    if (!view.writeable || !view.base)
      continue;
    for (std::size_t a = 0; a < t.areas.size(); ++a)
    {
      const Area& area = t.areas[a];
      const u64 start = std::max<u64>(view.physical_address, area.physical_address);
      const u64 end = std::min<u64>(u64{view.physical_address} + view.size,
                                    u64{area.physical_address} + area.size);
      if (start >= end)
        continue;
      u8* const base = view.base + (start - view.physical_address);
      if ((start - area.physical_address) % t.page_size != 0 ||
          (end - start) % t.page_size != 0 || reinterpret_cast<uintptr_t>(base) % t.page_size != 0)
      {
        ERROR_LOG_FMT(MEMMAP, "Rollback COW: a view of {:08x}..{:08x} is not page-aligned", start,
                      end);
        return false;
      }
      out->push_back(ViewInfo{base, base + (end - start),
                              t.area_first_page[a] + (start - area.physical_address) / t.page_size});
    }
  }
  if (out->size() > MAX_VIEWS)
  {
    ERROR_LOG_FMT(MEMMAP, "Rollback COW: {} views of guest RAM, at most {} supported", out->size(),
                  MAX_VIEWS);
    return false;
  }
  return true;
}

// Every page read-only in every view (after Arm or a mapping change).
bool ProtectAll(Tracker& t)
{
  bool ok = true;
  for (const ViewInfo& view : t.views)
    ok &= Protect(view.base, static_cast<std::size_t>(view.end - view.base), false);
  std::fill(t.open_in.begin(), t.open_in.end(), 0);
  t.open_pages.clear();
  return ok;
}

// `pages` (open ones, sorted) read-only again in every view where a write opened them. Runs of
// neighbouring pages go in one call per view.
void Reprotect(Tracker& t, const std::vector<u32>& pages)
{
  for (std::size_t v = 0; v < t.views.size(); ++v)
  {
    const u64 bit = u64{1} << v;
    const ViewInfo& view = t.views[v];
    std::size_t i = 0;
    while (i < pages.size())
    {
      if (!(t.open_in[pages[i]] & bit))
      {
        ++i;
        continue;
      }
      const std::size_t first = pages[i];
      std::size_t last = first;
      ++i;
      while (i < pages.size() && pages[i] == last + 1 && (t.open_in[pages[i]] & bit))
      {
        last = pages[i];
        ++i;
      }
      Protect(view.base + (first - view.first_page) * t.page_size,
              (last - first + 1) * t.page_size, false);
    }
  }
  for (const u32 page : pages)
    t.open_in[page] = 0;
  t.counters.pages_reprotected += pages.size();
}

// At a snapshot, or a load (which makes an older snapshot the newest again), before the newest log
// becomes that snapshot's own: which open pages stay hot (writable) and which are read-only again.
// `measure`: the newest log still holds what each page had at the previous snapshot, so whether
// its bytes changed since can be seen (not after a load, which dropped those pre-images).
void SettleOpenPages(Tracker& t, bool measure)
{
  std::vector<u32> hot, cold;
  for (const u32 page : t.open_pages)
  {
    // A page the newest log copied up front stays hot while it keeps changing. One a fault
    // recorded keeps the heat the fault gave it.
    if (measure && !t.fault_recorded[page])
    {
      const u8* const pre_image = t.log->NewestPreImage(page);
      const bool changed =
          pre_image && std::memcmp(pre_image, AliasOf(t, page), t.page_size) != 0;
      t.heat[page] =
          changed ? HEAT_WHILE_CHANGING : static_cast<u8>(t.heat[page] > 0 ? t.heat[page] - 1 : 0);
    }
    t.fault_recorded[page] = 0;
    (t.heat[page] > 0 ? hot : cold).push_back(page);
  }
  std::sort(cold.begin(), cold.end());
  Reprotect(t, cold);
  t.open_pages = std::move(hot);
}

// The hot pages' pre-images into the newest log, now (they stay writable: no fault will).
void CopyHotPages(Tracker& t)
{
  for (const u32 page : t.open_pages)
  {
    if (t.log->Record(page, AliasOf(t, page)))
      ++t.counters.pages_copied;
  }
}

// A write to `page` through view `v` is about to happen: keep the page's pre-image for the newest
// snapshot (first write since it) and make it writable in that view. False if it already is (the
// fault is someone else's).
bool OpenPage(Tracker& t, std::size_t v, std::size_t page)
{
  const u64 bit = u64{1} << v;
  if (t.open_in[page] & bit)
    return false;
  if (t.log->Record(page, AliasOf(t, page)))
  {
    ++t.counters.pages_recorded;
    t.fault_recorded[page] = 1;
    t.heat[page] = std::max(t.heat[page], HEAT_AFTER_FAULT);
  }
  const ViewInfo& view = t.views[v];
  if (!Protect(view.base + (page - view.first_page) * t.page_size, t.page_size, true))
    return false;
  if (t.open_in[page] == 0)
    t.open_pages.push_back(static_cast<u32>(page));
  t.open_in[page] |= bit;
  return true;
}

// Every view writable again.
void UnprotectAll(Tracker& t)
{
  for (const ViewInfo& view : t.views)
    Protect(view.base, static_cast<std::size_t>(view.end - view.base), true);
}

void ResetLocked(Tracker& t)
{
  s_armed.store(false, std::memory_order_release);
  if (t.log)
    t.next_id = t.log->NextId();
  t.owner = nullptr;
  t.views.clear();
  t.areas.clear();
  t.area_first_page.clear();
  t.open_in.clear();
  t.open_pages.clear();
  t.heat.clear();
  t.fault_recorded.clear();
  t.log.reset();
}
}  // namespace

std::size_t PageSize()
{
  static const std::size_t page_size = HostPageSize();
  return page_size;
}

bool Arm(const void* owner, const std::vector<Area>& areas, const std::vector<View>& views)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (t.owner)
    return t.owner == owner;
  if (!EMM::IsExceptionHandlerSupported() || areas.empty())
    return false;

  t.page_size = PageSize();
  std::size_t pages = 0;
  for (const Area& area : areas)
  {
    if (!area.alias || area.size % t.page_size != 0 || area.physical_address % t.page_size != 0)
    {
      ResetLocked(t);
      return false;
    }
    t.area_first_page.push_back(pages);
    pages += area.size / t.page_size;
  }
  t.areas = areas;
  if (!BuildViews(t, views, &t.views))
  {
    ResetLocked(t);
    return false;
  }
  t.log = std::make_unique<UndoLog>(t.page_size, pages, t.next_id);
  t.log->Reserve(RESERVE_PAGES);
  t.open_in.assign(pages, 0);
  t.open_pages.reserve(pages);
  t.heat.assign(pages, 0);
  t.fault_recorded.assign(pages, 0);
  t.owner = owner;
  // Faults on any thread must be handled, and seen as tracked, before a page is read-only anywhere.
  EMM::InstallCowFallbackHandler();
  s_armed.store(true, std::memory_order_release);
  if (!ProtectAll(t))
  {
    UnprotectAll(t);
    ResetLocked(t);
    return false;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Rollback: copy-on-write snapshots over {} KB pages, {} views",
                 t.page_size / 1024, t.views.size());
  return true;
}

bool ArmForSystem(Core::System& system, const void* owner)
{
  auto& memory = system.GetMemory();
  std::vector<Area> areas;
  if (u8* alias = memory.GetRollbackAlias(false))
    areas.push_back(Area{alias, 0, memory.GetRamSize()});
  else
    return false;
  // Page-table mappings (MMU emulation, off in sessions) come and go per guest page: far too many
  // views, each change re-protecting all of RAM.
  if (memory.HasPageTableMappings())
    return false;
  if (memory.GetEXRAM())
  {
    u8* alias = memory.GetRollbackAlias(true);
    if (!alias)
      return false;
    areas.push_back(Area{alias, MEM2_PHYSICAL, memory.GetExRamSize()});
  }
  std::vector<View> views;
  for (const auto& view : memory.GetGuestRamViews())
    views.push_back(View{view.base, view.physical_address, view.size, view.writeable});
  return Arm(owner, areas, views);
}

void Disarm(const void* owner)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.owner || t.owner != owner)
    return;
  UnprotectAll(t);
  ResetLocked(t);
}

bool IsArmedFor(const void* owner)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  return t.owner && t.owner == owner;
}

u64 Snapshot()
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log)
    return 0;
  t.log->Reserve(t.open_pages.size() + RESERVE_PAGES);
  // Pages written since the previous snapshot: read-only again, or kept writable with their
  // pre-image copied into the new log.
  SettleOpenPages(t, true);
  const u64 id = t.log->Open();
  CopyHotPages(t);
  return id;
}

bool Has(u64 id)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  return t.log && t.log->Has(id);
}

bool Restore(u64 id, const std::function<void(u32 physical_address, u32 length)>& changed)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log || !t.log->Has(id))
    return false;
  // Through the private mapping: whether a page is protected anywhere does not matter here.
  t.log->ForEachPreImage(id, [&](std::size_t page, const u8* pre_image) {
    u8* const live = AliasOf(t, page);
    const u32 physical = PhysicalOf(t, page);
    for (std::size_t offset = 0; offset < t.page_size; offset += RESTORE_BLOCK)
    {
      if (std::memcmp(live + offset, pre_image + offset, RESTORE_BLOCK) == 0)
        continue;
      std::memcpy(live + offset, pre_image + offset, RESTORE_BLOCK);
      changed(physical + static_cast<u32>(offset), static_cast<u32>(RESTORE_BLOCK));
    }
  });
  t.log->RewindTo(id);
  // RAM is snapshot `id` again and its log is empty: the hot pages are copied into it (their bytes
  // are what `id` holds) and the rest are read-only in every view.
  SettleOpenPages(t, false);
  CopyHotPages(t);
  return true;
}

void Drop(u64 id)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (t.log)
    t.log->Drop(id);
}

std::optional<u64> Checksum(u64 id)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log || !t.log->Has(id))
    return std::nullopt;
  std::vector<const u8*> pre_images(t.log->PageCount(), nullptr);
  t.log->ForEachPreImage(id, [&](std::size_t page, const u8* data) { pre_images[page] = data; });

  // The same bytes, in the same order, as Rollback::RamChecksum over full copies of MEM1 and MEM2.
  XXH3_state_t* const state = XXH3_createState();
  XXH3_64bits_reset(state);
  for (std::size_t a = 0; a < t.areas.size(); ++a)
  {
    const std::size_t first = t.area_first_page[a];
    const std::size_t count = t.areas[a].size / t.page_size;
    std::size_t run = 0;  // live pages not yet hashed
    for (std::size_t i = 0; i <= count; ++i)
    {
      const u8* const pre_image = i < count ? pre_images[first + i] : nullptr;
      if (i < count && !pre_image)
      {
        ++run;
        continue;
      }
      if (run > 0)
      {
        XXH3_64bits_update(state, t.areas[a].alias + (i - run) * t.page_size, run * t.page_size);
        run = 0;
      }
      if (pre_image)
        XXH3_64bits_update(state, pre_image, t.page_size);
    }
  }
  const u64 hash = XXH3_64bits_digest(state);
  XXH3_freeState(state);
  return hash;
}

bool HandleFault(uintptr_t address)
{
  if (!s_armed.load(std::memory_order_acquire))
    return false;
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  // Tracking stopped while this fault waited for the lock; every page was made writable first, so
  // the write can run again (a fault that wasn't ours comes back and finds tracking off).
  if (!t.log)
    return true;
  u8* const at = reinterpret_cast<u8*>(address);
  for (std::size_t v = 0; v < t.views.size(); ++v)
  {
    const ViewInfo& view = t.views[v];
    if (at < view.base || at >= view.end)
      continue;
    const std::size_t offset = static_cast<std::size_t>(at - view.base);
    const std::size_t index = offset / t.page_size;
    const std::size_t in_page = offset % t.page_size;
    const std::size_t last = static_cast<std::size_t>(view.end - view.base) / t.page_size - 1;
    // A store that straddles two pages can report an address in either one (arm64 allows any
    // address the access touched): when this page is writable already, a protected neighbour
    // within one access of it is what faulted.
    if (OpenPage(t, v, view.first_page + index) ||
        (in_page >= t.page_size - MAX_ACCESS && index < last &&
         OpenPage(t, v, view.first_page + index + 1)) ||
        (in_page < MAX_ACCESS && index > 0 && OpenPage(t, v, view.first_page + index - 1)))
    {
      ++t.counters.faults;
      return true;
    }
    // Writable here already and no protected neighbour: another thread opened the page while this
    // fault waited for the lock, and the write can run again. Unless the access may reach past the
    // view's ends, where an unmapped neighbour is someone else's fault (fastmem).
    return (t.open_in[view.first_page + index] & (u64{1} << v)) &&
           offset >= MAX_ACCESS &&
           offset + MAX_ACCESS <= static_cast<std::size_t>(view.end - view.base);
  }
  return false;
}

void PrepareHostWrite(const void* ptr, std::size_t size)
{
  if (!s_armed.load(std::memory_order_acquire) || size == 0)
    return;
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log)
    return;
  const u8* const start = static_cast<const u8*>(ptr);
  for (std::size_t v = 0; v < t.views.size(); ++v)
  {
    const ViewInfo& view = t.views[v];
    if (start < view.base || start >= view.end)
      continue;
    const u8* const end = std::min<const u8*>(start + size, view.end);
    const std::size_t first = static_cast<std::size_t>(start - view.base) / t.page_size;
    const std::size_t last = static_cast<std::size_t>(end - 1 - view.base) / t.page_size;
    for (std::size_t page = first; page <= last; ++page)
      OpenPage(t, v, view.first_page + page);
    return;
  }
}

void OnMappingsChanged(Core::System& system)
{
  if (!s_armed.load(std::memory_order_acquire))
    return;
  std::vector<View> views;
  for (const auto& view : system.GetMemory().GetGuestRamViews())
    views.push_back(View{view.base, view.physical_address, view.size, view.writeable});
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log)
    return;
  std::vector<ViewInfo> rebuilt;
  const bool built = BuildViews(t, views, &rebuilt);
  const auto same = [](const ViewInfo& a, const ViewInfo& b) {
    return a.base == b.base && a.end == b.end && a.first_page == b.first_page;
  };
  if (built && std::equal(rebuilt.begin(), rebuilt.end(), t.views.begin(), t.views.end(), same))
    return;
  ++t.counters.remaps;
  // New views start writable. Protect every page again in all of them: pages already in the newest
  // log keep their pre-image and only need making writable again when next written.
  t.views = std::move(rebuilt);
  if (!built || !ProtectAll(t))
  {
    // Untracked writes could follow: no snapshot can be trusted any more.
    ERROR_LOG_FMT(ROLLBACK, "Rollback: guest RAM mappings changed beyond what copy-on-write "
                            "snapshots track; every snapshot is dropped");
    // Every current mapping writable (views that are gone are not touched).
    for (const View& view : views)
    {
      if (view.writeable && view.base)
        Protect(view.base, view.size, true);
    }
    ResetLocked(t);
  }
}

void StopTracking()
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  // The mappings still exist: writable again, for whatever writes RAM until they go.
  UnprotectAll(t);
  ResetLocked(t);
}

Counters GetCounters()
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  return t.counters;
}
}  // namespace Rollback::Cow
