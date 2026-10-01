// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Undo logs for copy-on-write rollback snapshots (Rollback/Cow.h): for each snapshot, what its pages
// held when it was taken, recorded just before the first write to each page after it.
//
// Logs are kept oldest first. Log i holds the pre-images of the pages first written between
// snapshot i and snapshot i + 1 (or now, for the newest). Memory as it was at snapshot s is the live
// memory with every page that has a pre-image in logs s..newest replaced by the pre-image from the
// oldest of those logs: a page in log i was not written between snapshot s and snapshot i, so what
// it held at i is what it held at s. A page in no log since s has not been written since s.
//
// No platform code: the caller reports first writes (Cow.cpp from its page faults, the unit tests
// explicitly) and puts pre-images back itself. Not thread-safe: Cow.cpp holds its lock around it.

#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <vector>

#include "Common/CommonTypes.h"

namespace Rollback
{
class UndoLog
{
public:
  // Ids start at `first_id` (nonzero), so a later log can continue where an earlier one stopped.
  UndoLog(std::size_t page_size, std::size_t page_count, u64 first_id = 1);
  UndoLog(const UndoLog&) = delete;
  UndoLog& operator=(const UndoLog&) = delete;

  std::size_t PageSize() const { return m_page_size; }
  std::size_t PageCount() const { return m_page_count; }

  // Opens an empty log for a snapshot taken now, which becomes the newest. Returns its id: ids only
  // grow and are never 0.
  u64 Open();
  bool Has(u64 id) const;
  // The newest snapshot's id, or 0 when there is none.
  u64 Newest() const { return m_logs.empty() ? 0 : m_logs.back().id; }
  std::size_t SnapshotCount() const { return m_logs.size(); }
  // The id the next Open returns.
  u64 NextId() const { return m_next_id; }

  // Whether `page` already has a pre-image in the newest log: a write to it needs no record.
  bool LoggedInNewest(std::size_t page) const;
  // That pre-image, or null.
  const u8* NewestPreImage(std::size_t page) const
  {
    return LoggedInNewest(page) ? m_recorded_data[page] : nullptr;
  }
  // Records `contents` (PageSize() bytes) as the pre-image of `page` in the newest log, unless it has
  // one there already. Returns whether it recorded. Does nothing without a snapshot.
  bool Record(std::size_t page, const u8* contents);
  // The pages with a pre-image in the newest log, in the order they were first written.
  std::vector<u32> NewestPages() const;

  // Calls f(page, pre_image) once for every page with a pre-image in log `id` or a newer one, with
  // what the page held at snapshot `id`. Pages it does not name have not changed since. Nothing if
  // `id` is not held.
  template <typename F>
  void ForEachPreImage(u64 id, F&& f) const
  {
    const auto first = FindLog(id);
    if (first == m_logs.end())
      return;
    const u32 mark = NextMark();
    for (auto log = first; log != m_logs.end(); ++log)
    {
      for (const Entry& entry : log->entries)
      {
        if (m_scratch[entry.page] == mark)
          continue;
        m_scratch[entry.page] = mark;
        f(static_cast<std::size_t>(entry.page), static_cast<const u8*>(entry.data));
      }
    }
  }

  // After memory was put back to snapshot `id`: drops every newer log and empties `id`'s own, which
  // is the newest again. Returns false (and changes nothing) if `id` is not held.
  bool RewindTo(u64 id);
  // Forgets snapshot `id`. Its log merges into the next older one (the pre-images that one lacks
  // move over: what a page held at `id` is what it held at the older snapshot when it was not
  // written in between), or is dropped when `id` is the oldest.
  void Drop(u64 id);
  void Clear();

  // Pre-images held, over every log.
  std::size_t PagesHeld() const { return m_pages_held; }
  // Keeps at least `pages` free page buffers, so recording (in a fault handler) rarely allocates.
  void Reserve(std::size_t pages);

private:
  struct Entry
  {
    u32 page;
    u8* data;
  };
  struct Log
  {
    u64 id;
    std::vector<Entry> entries;
  };

  std::deque<Log>::iterator FindLog(u64 id);
  std::deque<Log>::const_iterator FindLog(u64 id) const;
  u32 NextMark() const;
  u8* TakeBuffer();
  void FreeEntries(Log* log);

  std::size_t m_page_size;
  std::size_t m_page_count;
  std::deque<Log> m_logs;
  u64 m_next_id = 1;
  // Per page: the id of the newest log that recorded it (it is in the newest log when that matches),
  // and the pre-image it recorded.
  std::vector<u64> m_recorded_in;
  std::vector<u8*> m_recorded_data;
  // Per page scratch marks for one pass over several logs.
  mutable std::vector<u32> m_scratch;
  mutable u32 m_mark = 0;
  std::size_t m_pages_held = 0;
  // Page buffers come from blocks of BLOCK_PAGES, recycled through the free list.
  std::vector<std::unique_ptr<u8[]>> m_blocks;
  std::vector<u8*> m_free;
};
}  // namespace Rollback
