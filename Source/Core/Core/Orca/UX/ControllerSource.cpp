// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/ControllerSource.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <fmt/format.h>

#include "Common/HttpRequest.h"
#include "Common/Logging/Log.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/UX/Controllers.h"
#include "InputCommon/ControlReference/ControlReference.h"

namespace Orca::UX
{
namespace
{
using Clock = std::chrono::steady_clock;

// After the stream ends, its last snapshot goes neutral (stale) and, this long after, Orca falls
// back to Dolphin's own mapping until the stream is back.
constexpr auto DETACHED_FALLBACK = std::chrono::seconds(1);
constexpr size_t TIMING_SAMPLES = 4096;
// The input age's log line covers the frames since the last one, at most this often.
constexpr auto AGE_LOG_PERIOD = std::chrono::seconds(10);
// A snapshot's hop through the app is timed against the fastest over this long (Stream::Cached).
constexpr auto HOP_FLOOR_PERIOD = std::chrono::seconds(1);

double EpochMs()
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// The desktop app's bridge only: http://127.0.0.1:<port>, so the token goes nowhere else.
bool LoopbackBridge(std::string_view bridge)
{
  constexpr std::string_view prefix = "http://127.0.0.1:";
  if (bridge.ends_with("/"))
    bridge.remove_suffix(1);
  if (!bridge.starts_with(prefix))
    return false;
  const std::string_view port = bridge.substr(prefix.size());
  return !port.empty() && port.size() <= 5 &&
         std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string Env(const char* name)
{
  const char* v = std::getenv(name);
  return v ? v : "";
}

// The newest TIMING_SAMPLES values of one measure.
class Samples
{
public:
  void Add(double value)
  {
    if (m_values.size() < TIMING_SAMPLES)
      m_values.push_back(value);
    else
      m_values[m_next++ % TIMING_SAMPLES] = value;
  }
  void Clear()
  {
    m_values.clear();
    m_next = 0;
  }
  const std::vector<double>& Values() const { return m_values; }

private:
  std::vector<double> m_values;
  size_t m_next = 0;
};

struct Percentiles
{
  double p50 = 0, p95 = 0, p99 = 0, max = 0;
};

// Sorts its own copy: callers copy under their lock and call this outside it, so the stream thread
// and the CPU thread never wait on a sort.
Percentiles Summarize(std::vector<double> values)
{
  Percentiles p;
  if (values.empty())
    return p;
  std::sort(values.begin(), values.end());
  const auto at = [&values](double q) {
    return values[std::min(values.size() - 1, static_cast<size_t>(q * values.size()))];
  };
  p.p50 = at(0.50);
  p.p95 = at(0.95);
  p.p99 = at(0.99);
  p.max = values.back();
  return p;
}

// The input age's numbers over some frames (ControllerSource.h), and their log line.
struct AgeWindow
{
  Clock::time_point start{};
  u64 frames = 0;
  u64 reports = 0;
  std::vector<double> age, helper, cache, hop;

  // `hop_ms` < 0: none for this read.
  void Add(double helper_ms, double cache_ms, double hop_ms)
  {
    if (age.size() >= TIMING_SAMPLES)
      return;
    age.push_back(helper_ms + cache_ms);
    helper.push_back(helper_ms);
    cache.push_back(cache_ms);
    if (hop_ms >= 0)
      hop.push_back(hop_ms);
  }

  std::string Describe(std::string_view over) const
  {
    std::string line = fmt::format("Orca: controller input age {}: {} frames read the adapter, {} "
                                   "with a new pad",
                                   over, frames, age.size());
    if (!age.empty())
    {
      double sum = 0;
      for (const double a : age)
        sum += a;
      const Percentiles p = Summarize(age);
      line += fmt::format("; newest report at the read: mean {:.2f}, p50 {:.1f}, p95 {:.1f}, p99 "
                          "{:.1f}, max {:.1f} ms (p50 helper {:.2f}, in Orca's cache {:.2f})",
                          sum / age.size(), p.p50, p.p95, p.p99, p.max, Summarize(helper).p50,
                          Summarize(cache).p50);
    }
    if (!hop.empty())
    {
      const Percentiles h = Summarize(hop);
      line += fmt::format("; the hop through the app, not in the age, above its fastest in the "
                          "last second: p50 {:.2f}, p95 {:.2f}, max {:.2f} ms",
                          h.p50, h.p95, h.max);
    }
    if (frames > 0)
      line += fmt::format("; {:.2f} reports per frame", static_cast<double>(reports) / frames);
    return line;
  }
};

class Stream
{
public:
  Stream(std::string bridge, std::string token)
      : m_url((bridge.ends_with("/") ? bridge.substr(0, bridge.size() - 1) : bridge) +
              "/controllers/events"),
        m_token(std::move(token))
  {
    m_thread = std::thread([this] { Run(); });
  }
  ~Stream()
  {
    m_done = true;
    if (m_thread.joinable())
      m_thread.join();
  }
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  struct Cached
  {
    std::shared_ptr<const Snapshot> snapshot;
    double age_ms = 0;  // since this process took it in (its own steady clock)
    // Its hop through the app, from sent_at to this process taking it in, above the fastest such
    // hop of the last HOP_FLOOR_PERIOD; -1 for a snapshot without an adapter report. The hop's own
    // length can't be had: its ends are the helper's clock (its wall clock read once at start, then
    // its own steady clock) and this process's wall clock, which drift apart (a few ppm) and step
    // (NTP, w32time). Over a second their drift is microseconds and their offset cancels, so this
    // is the hop's jitter: how much later than its best this one came.
    double hop_ms = -1;
  };

  // The newest snapshot; nullopt when there is none to use (never attached, or detached for longer
  // than DETACHED_FALLBACK).
  std::optional<Cached> Latest() const
  {
    std::lock_guard lk(m_lock);
    const auto now = Clock::now();
    if (!m_latest || (!m_attached && now - m_detached_at > DETACHED_FALLBACK))
      return std::nullopt;
    return Cached{m_latest, std::chrono::duration<double, std::milli>(now - m_arrived).count(),
                  m_latest_hop};
  }

  // A frame's own read (LocalPad(true)) took `choice` (nullptr: no pad from the app) from `cached`:
  // the input age at the boundary (ControllerSource.h). CPU thread. Returns the log line when one
  // is due, for the caller to log outside the locks.
  std::string NoteFrame(const Cached& cached, const LocalChoice* choice)
  {
    const auto now = Clock::now();
    std::lock_guard lk(m_lock);
    const u64 reports = m_reports - m_reports_at_frame;
    m_reports_at_frame = m_reports;
    const Snapshot& snapshot = *cached.snapshot;
    // Only an adapter port's reports carry the helper's receive time. A read of anything else, or
    // a neutral one (stale, suspended), says nothing; the read after it starts afresh.
    if (!choice || choice->source != PadSource::Adapter || choice->neutral ||
        snapshot.received_at <= 0)
    {
      m_last_pad.reset();
      return {};
    }
    if (m_window.frames == 0)
      m_window.start = now;
    ++m_input.frames;
    m_input.reports += reports;
    ++m_window.frames;
    m_window.reports += reports;
    const Orca::Net::Pad pad = Orca::Net::EncodePad(choice->pad);
    if (m_last_pad && *m_last_pad != pad)
    {
      // Two spans, each on one clock, never one process's time against another's: the helper's
      // (sent_at - received_at: its steady clock, plus the app's own monotonic clock for a snapshot
      // it held) and the wait in this process's cache (its steady clock). The hop between them is
      // left out (Cached::hop_ms) and reported beside the age.
      const double helper = std::max(0.0, snapshot.sent_at - snapshot.received_at);
      const double cache = cached.age_ms;
      ++m_input.changes;
      m_input.age_ms += helper + cache;
      m_age.Add(helper + cache);
      m_helper.Add(helper);
      m_cache.Add(cache);
      if (cached.hop_ms >= 0)
        m_hop.Add(cached.hop_ms);
      m_window.Add(helper, cache, cached.hop_ms);
    }
    m_last_pad = pad;
    if (now - m_window.start < AGE_LOG_PERIOD)
      return {};
    const std::string line = m_window.Describe(fmt::format(
        "over {:.0f} s", std::chrono::duration<double>(now - m_window.start).count()));
    m_window = {};
    return line;
  }

  Orca::Events::InputAge Input() const
  {
    std::lock_guard lk(m_lock);
    return m_input;
  }

  // The whole stream's input age, for the log when it stops (empty: no frame read the adapter).
  std::string RunSummary() const
  {
    AgeWindow run;
    {
      std::lock_guard lk(m_lock);
      run.frames = m_input.frames;
      run.reports = m_input.reports;
      run.age = m_age.Values();
      run.helper = m_helper.Values();
      run.cache = m_cache.Values();
      run.hop = m_hop.Values();
    }
    if (run.frames == 0)
      return {};
    return run.Describe(run.age.size() < TIMING_SAMPLES ? "over the run" :
                                                          "over the run (newest 4096 changes)");
  }

  ControllerTiming Timing() const
  {
    ControllerTiming t;
    std::vector<double> transport, age, helper, cache, hop;
    {
      std::lock_guard lk(m_lock);
      t = m_timing;
      t.attached = m_attached;
      t.input = m_input;
      transport = m_samples.Values();
      age = m_age.Values();
      helper = m_helper.Values();
      cache = m_cache.Values();
      hop = m_hop.Values();
    }
    t.samples = transport.size();
    const Percentiles tp = Summarize(std::move(transport));
    t.p50_ms = tp.p50;
    t.p95_ms = tp.p95;
    t.p99_ms = tp.p99;
    t.max_ms = tp.max;
    t.age_samples = age.size();
    const Percentiles ap = Summarize(std::move(age));
    t.age_p50_ms = ap.p50;
    t.age_p95_ms = ap.p95;
    t.age_p99_ms = ap.p99;
    t.age_max_ms = ap.max;
    t.helper_p50_ms = Summarize(std::move(helper)).p50;
    t.cache_p50_ms = Summarize(std::move(cache)).p50;
    t.hop_samples = hop.size();
    const Percentiles hp = Summarize(std::move(hop));
    t.hop_p50_ms = hp.p50;
    t.hop_p95_ms = hp.p95;
    return t;
  }

  void ResetTiming()
  {
    std::lock_guard lk(m_lock);
    m_timing = {};
    m_samples.Clear();
    m_input = {};
    m_age.Clear();
    m_helper.Clear();
    m_cache.Clear();
    m_hop.Clear();
    m_window = {};
    m_last_pad.reset();
    m_reports_at_frame = m_reports;
  }

private:
  static size_t OnData(char* data, size_t size, size_t count, void* user)
  {
    auto* self = static_cast<Stream*>(user);
    if (self->m_done)
      return 0;  // aborts the transfer
    // SSE lines end in LF, CRLF or CR (the app sends LF); a CR never appears inside the JSON, so
    // dropping CRs and treating a lone CR as LF reads all three alike.
    for (size_t i = 0; i < size * count; ++i)
    {
      const char c = data[i];
      if (c == '\r')
      {
        self->m_after_cr = true;
        self->m_pending.push_back('\n');
        continue;
      }
      if (c == '\n' && self->m_after_cr)
      {
        self->m_after_cr = false;
        continue;  // the LF of a CRLF
      }
      self->m_after_cr = false;
      self->m_pending.push_back(c);
    }
    if (self->m_pending.size() > 256 * 1024)
      return 0;
    // An event ends with a blank line. Its `data:` lines are joined with LF; comment lines
    // (`: ping`, the app's keepalive) and other fields are ignored.
    size_t end;
    while ((end = self->m_pending.find("\n\n")) != std::string::npos)
    {
      std::string_view block(self->m_pending.data(), end);
      std::string payload;
      bool has_data = false;
      while (!block.empty())
      {
        const size_t nl = block.find('\n');
        const std::string_view line = block.substr(0, nl);
        block = nl == std::string_view::npos ? std::string_view{} : block.substr(nl + 1);
        if (!line.starts_with("data:"))
          continue;
        if (has_data)
          payload.push_back('\n');
        payload.append(line.substr(line.starts_with("data: ") ? 6 : 5));
        has_data = true;
      }
      if (has_data)
        self->OnEvent(payload);
      self->m_pending.erase(0, end + 2);
    }
    return size * count;
  }

  static int OnProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
  {
    return static_cast<Stream*>(user)->m_done ? 1 : 0;
  }

  void OnEvent(std::string_view json)
  {
    std::optional<Snapshot> parsed = ParseSnapshotEvent(json);
    const auto arrived = Clock::now();
    const double arrived_epoch = EpochMs();
    std::lock_guard lk(m_lock);
    if (!parsed)
    {
      ++m_timing.refused;
      return;
    }
    // Sequence numbers count within one helper run (a restarted helper, or the app's own empty
    // snapshot after a failure, starts again): order only within a session.
    // Sequence 0 is the app's own snapshot (an idle or failed helper), never stale.
    if (m_latest && m_latest->session == parsed->session && parsed->sequence != 0 &&
        parsed->sequence < m_latest->sequence)
    {
      return;
    }
    if (parsed->received_at > 0 && parsed->received_at != m_last_report)
    {
      m_last_report = parsed->received_at;
      ++m_reports;
      m_samples.Add(arrived_epoch - parsed->received_at);
    }
    m_latest_hop =
        parsed->received_at > 0 ? HopAboveFloor(arrived, arrived_epoch - parsed->sent_at) : -1;
    ++m_timing.snapshots;
    m_latest = std::make_shared<const Snapshot>(std::move(*parsed));
    m_arrived = arrived;
    m_attached = true;
  }

  // `trip`: a snapshot's arrival here less its send, across the two clocks (Cached::hop_ms). Only
  // adapter snapshots count: their sent_at is always on the helper's clock (the app stamps its own
  // snapshots with its own). A clock step that lengthens the trips shows in full until the floor
  // from before it is a second old; one that shortens them is the new floor at once. Under m_lock.
  double HopAboveFloor(Clock::time_point arrived, double trip)
  {
    while (!m_hop_floor.empty() && m_hop_floor.back().second >= trip)
      m_hop_floor.pop_back();
    m_hop_floor.emplace_back(arrived, trip);
    while (arrived - m_hop_floor.front().first > HOP_FLOOR_PERIOD)
      m_hop_floor.pop_front();
    return trip - m_hop_floor.front().second;
  }

  void Detach()
  {
    std::lock_guard lk(m_lock);
    if (m_attached)
      m_detached_at = Clock::now();
    m_attached = false;
  }

  void Run()
  {
    int backoff_ms = 500;
    while (!m_done)
    {
      CURL* curl = curl_easy_init();
      if (!curl)
        return;
      m_pending.clear();
      m_after_cr = false;
      curl_slist* headers = curl_slist_append(nullptr, ("Authorization: Bearer " + m_token).c_str());
      curl_easy_setopt(curl, CURLOPT_URL, m_url.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &OnData);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &OnProgress);
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
      // Loopback: no proxy, and Nagle off so a small event is never held back.
      curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
      curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
      // A hung bridge: nothing at all for 30 s (a running helper sends every 100 ms; a failed or
      // missing one sends nothing, and reconnecting faster would only respawn it).
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
      const CURLcode result = curl_easy_perform(curl);
      long status = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      Detach();
      if (m_done)
        return;
      // A bridge that refuses the token, or has no such route, won't change its mind.
      if (status == 401 || status == 403 || status == 404)
      {
        ERROR_LOG_FMT(CONTROLLERINTERFACE,
                      "Orca: the YouGame app refused its controller stream (HTTP {}); using "
                      "Dolphin's controller mapping",
                      status);
        return;
      }
      int wait_ms;
      if (status == 409)
        wait_ms = 1000;  // the previous stream is still closing, or another game holds it
      else if (status == 503)
        wait_ms = 5000;  // the app has no controller helper
      else if (status == 200)
        wait_ms = backoff_ms = 500;  // the app restarted the stream
      else
        wait_ms = backoff_ms = std::min(backoff_ms * 2, 5000);
      // Once per change, not once a second while another game holds the stream.
      if (status != m_last_status || result != m_last_result)
      {
        WARN_LOG_FMT(CONTROLLERINTERFACE, "Orca: controller stream ended ({}, HTTP {})",
                     curl_easy_strerror(result), status);
        m_last_status = status;
        m_last_result = result;
      }
      for (int waited = 0; waited < wait_ms && !m_done; waited += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  const std::string m_url;
  const std::string m_token;
  std::atomic<bool> m_done{false};
  std::string m_pending;  // stream thread only
  bool m_after_cr = false;
  long m_last_status = -1;
  CURLcode m_last_result = CURLE_OK;

  mutable std::mutex m_lock;
  std::shared_ptr<const Snapshot> m_latest;
  Clock::time_point m_arrived{};
  double m_latest_hop = -1;  // m_latest's Cached::hop_ms
  // The adapter snapshots' trips of the last HOP_FLOOR_PERIOD that a later one hasn't beaten, in
  // arrival order: the front is the fastest (HopAboveFloor).
  std::deque<std::pair<Clock::time_point, double>> m_hop_floor;
  Clock::time_point m_detached_at{};
  bool m_attached = false;
  double m_last_report = 0;
  ControllerTiming m_timing;
  Samples m_samples;  // the transport's
  // Adapter reports taken in (distinct received_at), and the count at the last frame's read.
  u64 m_reports = 0;
  u64 m_reports_at_frame = 0;
  // The input age: totals, the newest changes (and their hops), the log line's frames, and the last
  // frame's pad.
  Orca::Events::InputAge m_input;
  Samples m_age, m_helper, m_cache, m_hop;
  AgeWindow m_window;
  std::optional<Orca::Net::Pad> m_last_pad;

  std::thread m_thread;  // last: starts after everything above exists
};

std::mutex s_stream_lock;
std::unique_ptr<Stream> s_stream;

// Swaps the stream out and joins it outside the lock. `log`: the run's input age goes to the log
// first (not at static destruction, when the log may be gone).
void StopStream(bool log)
{
  std::unique_ptr<Stream> stream;
  {
    std::lock_guard lk(s_stream_lock);
    stream.swap(s_stream);
  }
  const std::string summary = stream && log ? stream->RunSummary() : std::string();
  stream.reset();
  if (!summary.empty())
    NOTICE_LOG_FMT(ROLLBACK, "{}", summary);
}

// Joins the stream thread at exit.
struct Teardown
{
  ~Teardown() { StopStream(false); }
} s_teardown;

// The pad from the app's newest snapshot; with `for_frame`, timed (Stream::NoteFrame).
std::optional<LocalChoice> Choose(bool for_frame)
{
  std::optional<LocalChoice> choice;
  std::string age_line;
  {
    // Held through the choice (pure and quick) so the stream can't go while it is noted.
    std::lock_guard lk(s_stream_lock);
    if (!s_stream)
      return std::nullopt;
    const std::optional<Stream::Cached> cached = s_stream->Latest();
    if (!cached)
      return std::nullopt;
    choice = ChooseLocalPad(*cached->snapshot, cached->age_ms);
    if (for_frame)
      age_line = s_stream->NoteFrame(*cached, choice ? &*choice : nullptr);
  }
  if (!age_line.empty())
    NOTICE_LOG_FMT(ROLLBACK, "{}", age_line);
  return choice;
}
}  // namespace

void StartControllerStream(const std::string& bridge, const std::string& token)
{
  std::unique_ptr<Stream> next;
  if (LoopbackBridge(bridge) && !token.empty())
  {
    // libcurl's global init, through Common::HttpRequest's own once-only path (the room client and
    // the keyframe store use it too), before the stream thread's first curl_easy_init.
    { Common::HttpRequest init; }
    next = std::make_unique<Stream>(bridge, token);
  }
  {
    std::lock_guard lk(s_stream_lock);
    next.swap(s_stream);
  }
  next.reset();  // the old stream joins outside the lock
}

void StopControllerStream()
{
  StopStream(true);
}

std::optional<GCPadStatus> LatestPad()
{
  if (const auto choice = Choose(false))
    return choice->pad;
  return std::nullopt;
}

void StartControllerStreamFromEnvironment()
{
  const std::string bridge = Env("YOUGAME_BRIDGE"), token = Env("YOUGAME_TOKEN");
  if (bridge.empty() || token.empty())
    return;
  if (!LoopbackBridge(bridge))
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "Orca: YOUGAME_BRIDGE is not a loopback address");
    return;
  }
  StartControllerStream(bridge, token);
}

std::optional<GCPadStatus> LocalPad(bool for_frame)
{
  // Timed before the focus gate: the gate decides what the game gets, not how fresh it was.
  const auto choice = Choose(for_frame);
  if (!choice)
    return std::nullopt;
  // The same focus rule as the keyboard (Dolphin's input gate, set on the CPU thread each field).
  if (!ControlReference::GetInputGate())
    return NeutralPad();
  return choice->pad;
}

ControllerTiming GetControllerTiming()
{
  std::lock_guard lk(s_stream_lock);
  return s_stream ? s_stream->Timing() : ControllerTiming{};
}

void ResetControllerTiming()
{
  std::lock_guard lk(s_stream_lock);
  if (s_stream)
    s_stream->ResetTiming();
}

Orca::Events::InputAge GetInputAge()
{
  std::lock_guard lk(s_stream_lock);
  return s_stream ? s_stream->Input() : Orca::Events::InputAge{};
}
}  // namespace Orca::UX
