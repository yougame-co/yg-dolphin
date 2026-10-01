// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/Keyframe.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>
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
constexpr char MAGIC[4] = {'O', 'K', 'F', '1'};
// Long-distance matching over a 128 MB window: RAM and the NAND's /tmp share data far apart.
constexpr int WINDOW_LOG = 27;
constexpr int LEVEL = 3;
// No section of a keyframe is near this (MEM2 is 64 MB; a Wii NAND is 512 MB at most).
constexpr u64 MAX_SECTION = 512ull << 20;
constexpr u64 MAX_RAW = 1ull << 30;
constexpr u32 MAX_NAND_ENTRIES = 65536;

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
    u32 path_size, directory;
    NandEntry entry;
    if (!r.U32(&path_size) || path_size > 1024)
      return false;
    entry.path.resize(path_size);
    if (!r.Bytes(entry.path.data(), path_size) || !r.U32(&directory) || !r.Blob(&entry.data) ||
        !SafeRelative(entry.path))
    {
      return false;
    }
    entry.directory = directory != 0;
    nand->push_back(std::move(entry));
  }
  *frame = static_cast<int>(frame_value);
  return r.AtEnd();
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
  bool Put(const std::string& id, const std::vector<u8>& data, std::string* error) override
  {
    const std::string path = Path(id);
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
  std::optional<std::vector<u8>> Get(const std::string& id, u64 size,
                                     const std::function<bool(u64, u64)>& progress,
                                     std::string* error) override
  {
    File::IOFile file(Path(id), "rb");
    if (!file || file.GetSize() != size)
    {
      *error = "no keyframe " + id;
      return std::nullopt;
    }
    std::vector<u8> data(size);
    constexpr u64 CHUNK = 4 << 20;
    for (u64 done = 0; done < size;)
    {
      const u64 n = std::min(CHUNK, size - done);
      if (!file.ReadBytes(data.data() + done, n))
      {
        *error = "could not read keyframe " + id;
        return std::nullopt;
      }
      done += n;
      if (!progress(done, size))
      {
        *error = "cancelled";
        return std::nullopt;
      }
    }
    return data;
  }
  void Delete(const std::string& id) override { File::Delete(Path(id)); }

private:
  std::string Path(const std::string& id) const { return m_dir + "/" + id + ".okf"; }
  std::string m_dir;
};
}  // namespace

std::unique_ptr<KeyframeStore> MakeKeyframeStore()
{
  const std::string dir = Orca::GetEnv("ORCA_TEST_KEYFRAME_DIR");
  if (!dir.empty())
    return std::make_unique<LocalKeyframeStore>(dir);
  return nullptr;
}
}  // namespace Orca::Net
