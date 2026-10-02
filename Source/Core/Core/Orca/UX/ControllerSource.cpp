// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/ControllerSource.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "Common/Logging/Log.h"
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

  // The newest snapshot and its age here; nullptr when there is none to use (never attached, or
  // detached for longer than DETACHED_FALLBACK).
  std::shared_ptr<const Snapshot> Latest(double* age_ms) const
  {
    std::lock_guard lk(m_lock);
    const auto now = Clock::now();
    if (!m_latest || (!m_attached && now - m_detached_at > DETACHED_FALLBACK))
      return nullptr;
    *age_ms = std::chrono::duration<double, std::milli>(now - m_arrived).count();
    return m_latest;
  }

  ControllerTiming Timing() const
  {
    ControllerTiming t;
    std::vector<double> sorted;
    {
      std::lock_guard lk(m_lock);
      t = m_timing;
      t.attached = m_attached;
      sorted = m_samples;
    }
    // Sorted outside the lock: the stream thread and the CPU thread never wait on it.
    t.samples = sorted.size();
    if (!sorted.empty())
    {
      std::sort(sorted.begin(), sorted.end());
      const auto at = [&sorted](double q) {
        return sorted[std::min(sorted.size() - 1, static_cast<size_t>(q * sorted.size()))];
      };
      t.p50_ms = at(0.50);
      t.p95_ms = at(0.95);
      t.p99_ms = at(0.99);
      t.max_ms = sorted.back();
    }
    return t;
  }

  void ResetTiming()
  {
    std::lock_guard lk(m_lock);
    m_timing = {};
    m_samples.clear();
    m_next_sample = 0;
  }

private:
  static size_t OnData(char* data, size_t size, size_t count, void* user)
  {
    auto* self = static_cast<Stream*>(user);
    if (self->m_done)
      return 0;  // aborts the transfer
    self->m_pending.append(data, size * count);
    if (self->m_pending.size() > 256 * 1024)
      return 0;
    // SSE: events end with a blank line; each data line is `data: <json>`.
    size_t end;
    while ((end = self->m_pending.find("\n\n")) != std::string::npos)
    {
      const std::string_view event(self->m_pending.data(), end);
      if (event.starts_with("data:"))
        self->OnEvent(event.substr(event.starts_with("data: ") ? 6 : 5));
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
    if (m_latest && m_latest->session == parsed->session && parsed->sequence < m_latest->sequence)
      return;
    if (parsed->received_at > 0 && parsed->received_at != m_last_report)
    {
      m_last_report = parsed->received_at;
      const double latency = arrived_epoch - parsed->received_at;
      if (m_samples.size() < TIMING_SAMPLES)
        m_samples.push_back(latency);
      else
        m_samples[m_next_sample++ % TIMING_SAMPLES] = latency;
    }
    ++m_timing.snapshots;
    m_latest = std::make_shared<const Snapshot>(std::move(*parsed));
    m_arrived = arrived;
    m_attached = true;
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
      // The app sends at least every 100 ms: a stream silent for 3 s is dead (a hung bridge).
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 3L);
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
  long m_last_status = -1;
  CURLcode m_last_result = CURLE_OK;

  mutable std::mutex m_lock;
  std::shared_ptr<const Snapshot> m_latest;
  Clock::time_point m_arrived{};
  Clock::time_point m_detached_at{};
  bool m_attached = false;
  double m_last_report = 0;
  ControllerTiming m_timing;
  std::vector<double> m_samples;
  size_t m_next_sample = 0;

  std::thread m_thread;  // last: starts after everything above exists
};

std::mutex s_stream_lock;
std::unique_ptr<Stream> s_stream;
std::once_flag s_from_env;

// Joins the stream thread at exit.
struct Teardown
{
  ~Teardown() { StopControllerStream(); }
} s_teardown;

std::optional<LocalChoice> Choose()
{
  std::shared_ptr<const Snapshot> snapshot;
  double age_ms = 0;
  {
    std::lock_guard lk(s_stream_lock);
    if (!s_stream)
      return std::nullopt;
    snapshot = s_stream->Latest(&age_ms);
  }
  if (!snapshot)
    return std::nullopt;
  return ChooseLocalPad(*snapshot, age_ms);
}
}  // namespace

void StartControllerStream(const std::string& bridge, const std::string& token)
{
  std::unique_ptr<Stream> next;
  if (LoopbackBridge(bridge) && !token.empty())
  {
    static std::once_flag s_curl;
    std::call_once(s_curl, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
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
  std::unique_ptr<Stream> stream;
  {
    std::lock_guard lk(s_stream_lock);
    stream.swap(s_stream);
  }
  stream.reset();  // joins outside the lock
}

std::optional<GCPadStatus> LatestPad()
{
  if (const auto choice = Choose())
    return choice->pad;
  return std::nullopt;
}

std::optional<GCPadStatus> LocalPad()
{
  std::call_once(s_from_env, [] {
    const std::string bridge = Env("YOUGAME_BRIDGE"), token = Env("YOUGAME_TOKEN");
    if (bridge.empty() || token.empty())
      return;
    if (!LoopbackBridge(bridge))
    {
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "Orca: YOUGAME_BRIDGE is not a loopback address");
      return;
    }
    StartControllerStream(bridge, token);
  });
  const auto choice = Choose();
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
}  // namespace Orca::UX
