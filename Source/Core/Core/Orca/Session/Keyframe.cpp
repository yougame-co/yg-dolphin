// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/Keyframe.h"

#include <algorithm>
#include <cstring>

#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>

#include <curl/curl.h>
#include <fmt/format.h>
#include <picojson.h>
#include <xxh3.h>
#include <zstd.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"

namespace Orca::Net
{
namespace
{
// OKF2: NAND entries are files (0), folders (1) or references (2: a hash, no data).
constexpr char MAGIC[4] = {'O', 'K', 'F', '2'};
// Long-distance matching over a 128 MB window: RAM and the NAND's /tmp share data far apart.
constexpr int WINDOW_LOG = 27;
constexpr int LEVEL = 3;
// No section of a keyframe is near this (MEM2 is 64 MB; a Wii NAND is 512 MB at most).
constexpr u64 MAX_SECTION = 512ull << 20;
constexpr u64 MAX_RAW = 1ull << 30;
constexpr u32 MAX_NAND_ENTRIES = 65536;
// The site takes 128 MB at most (ORCA.md, "Keyframe store").
constexpr u64 MAX_KEYFRAME_BYTES = 128ull << 20;

class Writer
{
public:
  void U32(u32 v) { Bytes(&v, sizeof(v)); }
  void U64(u64 v) { Bytes(&v, sizeof(v)); }
  void Bytes(const void* data, size_t size)
  {
    const auto* p = static_cast<const u8*>(data);
    out.insert(out.end(), p, p + size);
  }
  void Blob(const std::vector<u8>& data)
  {
    U64(data.size());
    Bytes(data.data(), data.size());
  }
  std::vector<u8> out;
};

class Reader
{
public:
  explicit Reader(const std::vector<u8>& data) : m_data(data) {}
  bool Bytes(void* out, size_t size)
  {
    if (size > m_data.size() - m_pos)
      return false;
    std::memcpy(out, m_data.data() + m_pos, size);
    m_pos += size;
    return true;
  }
  bool U32(u32* v) { return Bytes(v, sizeof(*v)); }
  bool U64(u64* v) { return Bytes(v, sizeof(*v)); }
  bool Blob(std::vector<u8>* out)
  {
    u64 size;
    if (!U64(&size) || size > MAX_SECTION || size > m_data.size() - m_pos)
      return false;
    out->assign(m_data.begin() + m_pos, m_data.begin() + m_pos + size);
    m_pos += size;
    return true;
  }
  bool AtEnd() const { return m_pos == m_data.size(); }

private:
  const std::vector<u8>& m_data;
  size_t m_pos = 0;
};

void Collect(const File::FSTEntry& entry, const std::string& prefix,
             std::vector<NandEntry>* entries, bool* ok)
{
  std::vector<const File::FSTEntry*> children;
  for (const auto& child : entry.children)
    children.push_back(&child);
  std::sort(children.begin(), children.end(),
            [](const auto* a, const auto* b) { return a->virtualName < b->virtualName; });
  for (const File::FSTEntry* child : children)
  {
    NandEntry out;
    out.path = prefix.empty() ? child->virtualName : prefix + "/" + child->virtualName;
    out.directory = child->isDirectory;
    if (!out.directory)
    {
      std::string data;
      if (!File::ReadFileToString(child->physicalName, data))
        *ok = false;
      out.data.assign(data.begin(), data.end());
    }
    const std::string path = out.path;
    entries->push_back(std::move(out));
    if (child->isDirectory)
      Collect(*child, path, entries, ok);
  }
}

bool SafeRelative(const std::string& path)
{
  if (path.empty() || path.size() > 1024 || path.front() == '/' || path.find('\\') != std::string::npos ||
      path.find('\0') != std::string::npos)
  {
    return false;
  }
  size_t start = 0;
  while (start <= path.size())
  {
    const size_t end = std::min(path.find('/', start), path.size());
    const std::string_view part(path.data() + start, end - start);
    if (part.empty() || part == "." || part == "..")
      return false;
    start = end + 1;
  }
  return true;
}
}  // namespace

bool ReadNandTree(const std::string& root, std::vector<NandEntry>* entries)
{
  entries->clear();
  if (!File::IsDirectory(root))
    return false;
  bool ok = true;
  Collect(File::ScanDirectoryTree(root, true), "", entries, &ok);
  return ok;
}

bool ReplaceNandTree(const std::string& root, const std::vector<NandEntry>& entries)
{
  if (root.empty())
    return false;
  for (const NandEntry& entry : entries)
  {
    if (!SafeRelative(entry.path))
      return false;
  }
  if (File::IsDirectory(root) && !File::DeleteDirRecursively(root))
    return false;
  if (!File::CreateFullPath(root + "/"))
    return false;
  for (const NandEntry& entry : entries)
  {
    const std::string path = root + "/" + entry.path;
    if (entry.directory)
    {
      if (!File::CreateFullPath(path + "/"))
        return false;
      continue;
    }
    File::CreateFullPath(path);
    File::IOFile file(path, "wb");
    if (!file || !file.WriteBytes(entry.data.data(), entry.data.size()))
      return false;
  }
  return true;
}

std::vector<u8> PackKeyframe(int frame, const Rollback::MachineImage& image,
                             const std::vector<NandEntry>& nand)
{
  Writer w;
  w.Bytes(MAGIC, sizeof(MAGIC));
  w.U32(static_cast<u32>(frame));
  w.Blob(image.state);
  w.Blob(image.mem1);
  w.Blob(image.mem2);
  w.Blob(image.l1_cache);
  w.U32(static_cast<u32>(nand.size()));
  for (const NandEntry& entry : nand)
  {
    w.U32(static_cast<u32>(entry.path.size()));
    w.Bytes(entry.path.data(), entry.path.size());
    if (entry.reference)
    {
      w.U32(2);
      w.U64(entry.hash);
      continue;
    }
    w.U32(entry.directory ? 1 : 0);
    w.Blob(entry.data);
  }

  ZSTD_CCtx* cctx = ZSTD_createCCtx();
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, LEVEL);
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1);
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, WINDOW_LOG);
  // Uses every core when this zstd was built with threads; one otherwise.
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 4);
  std::vector<u8> out(ZSTD_compressBound(w.out.size()));
  const size_t size = ZSTD_compress2(cctx, out.data(), out.size(), w.out.data(), w.out.size());
  ZSTD_freeCCtx(cctx);
  if (ZSTD_isError(size))
  {
    ERROR_LOG_FMT(NETPLAY, "Orca keyframe: compression failed: {}", ZSTD_getErrorName(size));
    return {};
  }
  out.resize(size);
  return out;
}

bool UnpackKeyframe(const std::vector<u8>& blob, int* frame, Rollback::MachineImage* image,
                    std::vector<NandEntry>* nand)
{
  const unsigned long long raw_size = ZSTD_getFrameContentSize(blob.data(), blob.size());
  if (raw_size == ZSTD_CONTENTSIZE_ERROR || raw_size == ZSTD_CONTENTSIZE_UNKNOWN ||
      raw_size > MAX_RAW)
  {
    return false;
  }
  std::vector<u8> raw(raw_size);
  ZSTD_DCtx* dctx = ZSTD_createDCtx();
  ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, WINDOW_LOG);
  const size_t size = ZSTD_decompressDCtx(dctx, raw.data(), raw.size(), blob.data(), blob.size());
  ZSTD_freeDCtx(dctx);
  if (ZSTD_isError(size) || size != raw.size())
    return false;

  Reader r(raw);
  char magic[4];
  u32 frame_value, count;
  if (!r.Bytes(magic, sizeof(magic)) || std::memcmp(magic, MAGIC, sizeof(MAGIC)) != 0 ||
      !r.U32(&frame_value) || frame_value > (1u << 30) || !r.Blob(&image->state) ||
      !r.Blob(&image->mem1) || !r.Blob(&image->mem2) || !r.Blob(&image->l1_cache) ||
      !r.U32(&count) || count > MAX_NAND_ENTRIES)
  {
    return false;
  }
  nand->clear();
  for (u32 i = 0; i < count; ++i)
  {
    u32 path_size, kind;
    NandEntry entry;
    if (!r.U32(&path_size) || path_size > 1024)
      return false;
    entry.path.resize(path_size);
    if (!r.Bytes(entry.path.data(), path_size) || !r.U32(&kind) || kind > 2 ||
        !SafeRelative(entry.path))
    {
      return false;
    }
    if (kind == 2)
    {
      entry.reference = true;
      if (!r.U64(&entry.hash))
        return false;
    }
    else if (!r.Blob(&entry.data))
    {
      return false;
    }
    entry.directory = kind == 1;
    nand->push_back(std::move(entry));
  }
  *frame = static_cast<int>(frame_value);
  return r.AtEnd();
}

u64 NandHash(const std::vector<u8>& data)
{
  return XXH3_64bits(data.data(), data.size());
}

std::map<std::string, u64> HashNandTree(const std::string& root)
{
  std::map<std::string, u64> hashes;
  std::vector<NandEntry> entries;
  if (!ReadNandTree(root, &entries))
    return hashes;
  for (const NandEntry& entry : entries)
  {
    if (!entry.directory)
      hashes[entry.path] = NandHash(entry.data);
  }
  return hashes;
}

bool ResolveNandReferences(const std::string& root, std::vector<NandEntry>* entries,
                           std::string* error)
{
  for (NandEntry& entry : *entries)
  {
    if (!entry.reference)
      continue;
    std::string data;
    if (!File::ReadFileToString(root + "/" + entry.path, data))
    {
      *error = entry.path + " is missing";
      return false;
    }
    entry.data.assign(data.begin(), data.end());
    if (NandHash(entry.data) != entry.hash)
    {
      *error = entry.path + " differs";
      return false;
    }
    entry.reference = false;
  }
  return true;
}

std::string KeyframeHash(const std::vector<u8>& blob)
{
  return fmt::format("{:016x}", XXH3_64bits(blob.data(), blob.size()));
}

namespace
{
// A folder both Orcas on one machine use (tests): the host writes <id>.okf, the friend reads it.
class LocalKeyframeStore final : public KeyframeStore
{
public:
  explicit LocalKeyframeStore(std::string dir) : m_dir(std::move(dir))
  {
    File::CreateFullPath(m_dir + "/");
  }
  bool Put(const KeyframeInfo& info, const std::vector<u8>& data, std::string* error) override
  {
    const std::string path = Path(info.id);
    const std::string temp = path + ".part";
    File::IOFile file(temp, "wb");
    if (!file || !file.WriteBytes(data.data(), data.size()) || !file.Close() ||
        !File::Rename(temp, path))
    {
      *error = "could not write " + path;
      return false;
    }
    return true;
  }
  std::optional<std::vector<u8>> Get(const KeyframeInfo& info,
                                     const std::function<bool(u64, u64)>& progress,
                                     std::string* error) override
  {
    File::IOFile file(Path(info.id), "rb");
    if (!file || file.GetSize() != info.size)
    {
      *error = "no keyframe " + info.id;
      return std::nullopt;
    }
    std::vector<u8> data(info.size);
    constexpr u64 CHUNK = 4 << 20;
    for (u64 done = 0; done < info.size;)
    {
      const u64 n = std::min(CHUNK, info.size - done);
      if (!file.ReadBytes(data.data() + done, n))
      {
        *error = "could not read keyframe " + info.id;
        return std::nullopt;
      }
      done += n;
      if (!progress(done, info.size))
      {
        *error = "cancelled";
        return std::nullopt;
      }
    }
    return data;
  }
  void Delete(const std::string& id) override
  {
    if (Orca::GetEnv("ORCA_TEST_KEEP_KEYFRAMES") != "1")
      File::Delete(Path(id));
  }

private:
  std::string Path(const std::string& id) const { return m_dir + "/" + id + ".okf"; }
  std::string m_dir;
};

// YouGame's keyframe endpoint (ORCA.md, "Keyframe store"): PUT, GET and DELETE on
// <store_url>/<id> with "Authorization: Ticket <room ticket>". A body too large for the site
// (HTTP 413) goes straight to storage through a presigned URL from POST <store_url>/<id>/upload.
// Network errors, 429 and 5xx are retried with backoff; other refusals are final.
class HttpKeyframeStore final : public KeyframeStore
{
public:
  explicit HttpKeyframeStore(TicketSource tickets) : m_tickets(std::move(tickets)) {}
  ~HttpKeyframeStore() override
  {
    std::lock_guard lock(m_deletes_mutex);
    for (std::thread& t : m_deletes)
      t.join();
  }

  bool Put(const KeyframeInfo& info, const std::vector<u8>& data, std::string* error) override
  {
    return WithRetries(error, [&](const std::string& ticket, const std::string& base,
                                  Result* result) {
      const std::vector<std::string> headers = {
          "Authorization: Ticket " + ticket, "Content-Type: application/octet-stream",
          fmt::format("X-Orca-Frame: {}", info.frame), "X-Orca-Hash: " + info.hash};
      *result = Request("PUT", base + "/" + info.id, headers, &data, nullptr, nullptr);
      if (result->status != 413)
        return;
      // Too large for the site's request limit: straight to storage.
      std::vector<u8> reply;
      static const std::vector<u8> no_body;
      *result = Request("POST", base + "/" + info.id + "/upload",
                        {"Authorization: Ticket " + ticket,
                         fmt::format("X-Orca-Size: {}", data.size()),
                         fmt::format("X-Orca-Frame: {}", info.frame), "X-Orca-Hash: " + info.hash},
                        &no_body, &reply, nullptr);
      if (result->status != 200)
        return;
      const std::string url = PresignedUrl(reply);
      if (url.empty())
      {
        result->status = 0;
        result->error = "no upload address in YouGame's reply";
        result->final = true;
        return;
      }
      *result = Request("PUT", url, {"Content-Type: application/octet-stream"}, &data, nullptr,
                        nullptr);
    });
  }

  std::optional<std::vector<u8>> Get(const KeyframeInfo& info,
                                     const std::function<bool(u64, u64)>& progress,
                                     std::string* error) override
  {
    std::vector<u8> body;
    const bool ok = WithRetries(error, [&](const std::string& ticket, const std::string& base,
                                           Result* result) {
      body.clear();
      body.reserve(info.size);
      *result = Request("GET", base + "/" + info.id, {"Authorization: Ticket " + ticket}, nullptr,
                        &body, [&](u64 done, u64) { return progress(done, info.size); });
    });
    if (!ok)
      return std::nullopt;
    if (body.size() != info.size)
    {
      *error = fmt::format("the keyframe arrived with {} bytes of {}", body.size(), info.size);
      return std::nullopt;
    }
    return body;
  }

  void Delete(const std::string& id) override
  {
    // Best effort, off the caller's thread (the CPU thread calls this right after a load); the
    // site's cleanup rule catches what this misses.
    std::lock_guard lock(m_deletes_mutex);
    m_deletes.emplace_back([this, id] {
      std::string error;
      WithRetries(&error, [&](const std::string& ticket, const std::string& base, Result* result) {
        *result = Request("DELETE", base + "/" + id, {"Authorization: Ticket " + ticket}, nullptr,
                          nullptr, nullptr);
        if (result->status == 404)
          result->status = 204;  // already gone
      });
      if (!error.empty())
        WARN_LOG_FMT(NETPLAY, "Orca keyframe: deleting {} failed: {}", id, error);
    });
  }

private:
  struct Result
  {
    long status = 0;  // 0: no HTTP response
    std::string error;
    bool final = false;  // not worth retrying
  };

  static constexpr int ATTEMPTS = 4;

  bool WithRetries(std::string* error,
                   const std::function<void(const std::string&, const std::string&, Result*)>& op)
  {
    for (int attempt = 0; attempt < ATTEMPTS; ++attempt)
    {
      if (attempt > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(500 << (attempt - 1)));
      std::string ticket, base, ticket_error;
      if (!m_tickets || !m_tickets(&ticket, &base, &ticket_error))
      {
        *error = "no ticket: " + ticket_error;
        continue;
      }
      Result result;
      op(ticket, base, &result);
      if (result.status >= 200 && result.status < 300)
      {
        error->clear();
        return true;
      }
      *error = result.status ? fmt::format("HTTP {}{}{}", result.status,
                                           result.error.empty() ? "" : ": ", result.error) :
                               result.error;
      // A refusal (bad ticket, wrong room, too large, gone) stays a refusal; 429 and 5xx and the
      // network may pass. A 401 gets one more try with a new ticket.
      const bool retry = !result.final &&
                         (result.status == 0 || result.status == 429 || result.status >= 500 ||
                          (result.status == 401 && attempt == 0));
      if (!retry)
        return false;
      WARN_LOG_FMT(NETPLAY, "Orca keyframe: {}; trying again", *error);
    }
    return false;
  }

  static std::string PresignedUrl(const std::vector<u8>& reply)
  {
    picojson::value json;
    const std::string text(reply.begin(), reply.end());
    if (!picojson::parse(json, text).empty() || !json.is<picojson::object>())
      return {};
    const auto& o = json.get<picojson::object>();
    const auto it = o.find("url");
    if (it == o.end() || !it->second.is<std::string>())
      return {};
    const std::string& url = it->second.get<std::string>();
    return url.starts_with("https://") || url.starts_with("http://127.0.0.1:") ? url : "";
  }

  struct Transfer
  {
    const std::vector<u8>* upload = nullptr;
    size_t sent = 0;
    std::vector<u8>* download = nullptr;
    u64 limit = 0;
    std::function<bool(u64, u64)> progress;
  };

  static size_t OnRead(char* buffer, size_t size, size_t count, void* user)
  {
    auto* t = static_cast<Transfer*>(user);
    const size_t n = std::min(size * count, t->upload->size() - t->sent);
    std::memcpy(buffer, t->upload->data() + t->sent, n);
    t->sent += n;
    return n;
  }

  static size_t OnWrite(char* data, size_t size, size_t count, void* user)
  {
    auto* t = static_cast<Transfer*>(user);
    const size_t n = size * count;
    if (!t->download || t->download->size() + n > t->limit)
      return 0;  // aborts: more than any keyframe
    t->download->insert(t->download->end(), data, data + n);
    if (t->progress && !t->progress(t->download->size(), 0))
      return 0;
    return n;
  }

  static Result Request(const char* method, const std::string& url,
                        const std::vector<std::string>& headers, const std::vector<u8>* upload,
                        std::vector<u8>* download, std::function<bool(u64, u64)> progress)
  {
    Result result;
    CURL* curl = curl_easy_init();
    if (!curl)
    {
      result.error = "no HTTP client";
      return result;
    }
    curl_slist* list = nullptr;
    for (const std::string& h : headers)
      list = curl_slist_append(list, h.c_str());
    std::vector<u8> error_body;
    Transfer t;
    t.upload = upload;
    t.download = download ? download : &error_body;
    t.limit = download ? MAX_KEYFRAME_BYTES : 64 * 1024;
    t.progress = std::move(progress);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    // A transfer may take minutes on a slow link, but not stand still for 20 s.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &OnWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    if (std::string_view(method) == "PUT")
    {
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
      curl_easy_setopt(curl, CURLOPT_READFUNCTION, &OnRead);
      curl_easy_setopt(curl, CURLOPT_READDATA, &t);
      curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                       static_cast<curl_off_t>(upload ? upload->size() : 0));
    }
    else if (std::string_view(method) == "POST")
    {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    }
    else if (std::string_view(method) != "GET")
    {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }
    const CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK)
    {
      result.error = curl_easy_strerror(code);
      // An abort from progress (the player gave up) is not worth repeating.
      result.final = code == CURLE_WRITE_ERROR || code == CURLE_ABORTED_BY_CALLBACK;
      if (result.status >= 200 && result.status < 300)
        result.status = 0;
    }
    else if (!(result.status >= 200 && result.status < 300) && !error_body.empty())
    {
      // YouGame's refusals are {"error": "..."}: keep the sentence for the log.
      result.error.assign(error_body.begin(),
                          error_body.begin() + std::min<size_t>(error_body.size(), 200));
    }
    return result;
  }

  TicketSource m_tickets;
  std::mutex m_deletes_mutex;
  std::vector<std::thread> m_deletes;
};
}  // namespace

std::unique_ptr<KeyframeStore> MakeHttpKeyframeStore(TicketSource tickets)
{
  return std::make_unique<HttpKeyframeStore>(std::move(tickets));
}

std::unique_ptr<KeyframeStore> MakeKeyframeStore(TicketSource tickets)
{
  const std::string dir = Orca::GetEnv("ORCA_TEST_KEYFRAME_DIR");
  if (!dir.empty())
    return std::make_unique<LocalKeyframeStore>(dir);
  return MakeHttpKeyframeStore(std::move(tickets));
}
}  // namespace Orca::Net
