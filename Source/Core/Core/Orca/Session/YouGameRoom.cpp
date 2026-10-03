// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/YouGameRoom.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <random>
#include <string_view>
#include <thread>

#include <curl/curl.h>
#include <fmt/format.h>
#include <picojson.h>

#include "Common/HttpRequest.h"
#include "Common/Logging/Log.h"
#include "Common/WebSocket.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Status.h"

namespace Orca::Net
{
namespace
{
// The rooms Worker's lobby protocol (rooms/src/index.ts, rooms/src/game-lobby.ts in the YouGame
// repo). Orca rooms are private game lobbies of up to four. The host plays from the moment it boots;
// a friend who joins the room plugs into the next free port (drop-in). No room match is started:
// a friends room keeps no results.
constexpr const char* CODE_ALPHABET = "abcdefghjkmnpqrstuvwxyz23456789";
// The room allows 120 messages a second per socket: packets go out at most every 10 ms (a stalled
// session asks to send far more often), the newest replacing any not yet sent.
constexpr auto SEND_GAP = std::chrono::milliseconds(10);
constexpr auto PING_EVERY = std::chrono::seconds(1);
constexpr auto HELLO_EVERY = std::chrono::seconds(1);
constexpr size_t MAX_INBOX = 1024;
// The server answers every ping (one a second): a connection that brings nothing back for this long
// is dead, even though sends still succeed into the socket's buffer (a network that went away
// without a reset, a laptop that slept). Not much less: a keyframe upload fills a home uplink, and
// pings wait behind it in the modem's buffer (a second or two on many links).
constexpr auto SERVER_SILENCE_LIMIT = std::chrono::seconds(8);
// A joiner whose room has no host (a stale invite, a host that left or restarted) gives up this
// long after its welcome; a host that is there says hello within about a second.
constexpr auto HOST_WAIT = std::chrono::seconds(8);
// How long a teardown waits for the app to take the emptied room (loopback, normally milliseconds).
constexpr auto MIRROR_END_TIMEOUT = std::chrono::seconds(1);

using Clock = std::chrono::steady_clock;

// Rooms come and go within one process (a host's reopened room, a join, a leave), each mirroring
// itself to the page on threads of its own: the page must only ever get the newest room's updates,
// never an older room's late one (its emptied room at teardown included). Each room has a
// generation; a post from a room older than the newest that posted is dropped.
std::atomic<u64> s_room_generation{0};
std::mutex s_mirror_mutex;
u64 s_mirror_newest = 0;  // under s_mirror_mutex

u64 NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string Env(const char* name)
{
  return Orca::GetEnv(name);
}

bool ValidCode(const std::string& code)
{
  return code.size() >= 6 && code.size() <= 12 &&
         std::all_of(code.begin(), code.end(),
                     [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

// A player name as it may reach game memory and line-based output: printable ASCII and whole UTF-8
// sequences only, at most 64 bytes, never a character cut in half.
std::string CleanName(const std::string& raw)
{
  std::string out;
  for (size_t i = 0; i < raw.size();)
  {
    const auto c = static_cast<unsigned char>(raw[i]);
    size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    bool ok = length != 0 && i + length <= raw.size() && (length > 1 || (c >= 0x20 && c != 0x7F));
    for (size_t k = 1; ok && k < length; ++k)
      ok = (static_cast<unsigned char>(raw[i + k]) & 0xC0) == 0x80;
    if (!ok)
    {
      ++i;
      continue;
    }
    if (out.size() + length > 64)
      break;
    out.append(raw, i, length);
    i += length;
  }
  return out;
}

picojson::value NamesToJson(const std::vector<KeyframeInfo::Name>& names)
{
  picojson::array out;
  for (const KeyframeInfo::Name& n : names)
  {
    out.push_back(picojson::value(picojson::array{picojson::value(static_cast<double>(n.seat)),
                                                  picojson::value(static_cast<double>(n.from)),
                                                  picojson::value(n.name)}));
  }
  return picojson::value(out);
}

// At most 16 entries (a few per seat: who held it, from when).
std::vector<KeyframeInfo::Name> NamesFromJson(const picojson::object& d)
{
  std::vector<KeyframeInfo::Name> names;
  const auto nm = d.find("nm");
  if (nm == d.end() || !nm->second.is<picojson::array>())
    return names;
  for (const auto& entry : nm->second.get<picojson::array>())
  {
    if (!entry.is<picojson::array>() || names.size() >= 16)
      continue;
    const auto& a = entry.get<picojson::array>();
    if (a.size() != 3 || !a[0].is<double>() || !a[1].is<double>() || !a[2].is<std::string>())
      continue;
    const double seat = a[0].get<double>(), from = a[1].get<double>();
    if (seat < 0 || seat >= MAX_SEATS || from < 0 || from > (1 << 30))
      continue;
    names.push_back({static_cast<int>(seat), static_cast<int>(from), CleanName(a[2].get<std::string>())});
  }
  return names;
}

std::string RandomString(const char* alphabet, size_t length)
{
  std::random_device random;
  const size_t n = std::char_traits<char>::length(alphabet);
  std::string out;
  for (size_t i = 0; i < length; ++i)
    out += alphabet[random() % n];
  return out;
}

std::string ToHex(const u8* data, size_t size)
{
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i)
  {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 15];
  }
  return out;
}

int HexDigit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

bool FromHex(const std::string& hex, std::vector<u8>* out)
{
  if (hex.size() % 2)
    return false;
  out->clear();
  for (size_t i = 0; i < hex.size(); i += 2)
  {
    const int high = HexDigit(hex[i]), low = HexDigit(hex[i + 1]);
    if (high < 0 || low < 0)
      return false;
    out->push_back(static_cast<u8>(high << 4 | low));
  }
  return true;
}

// A whole number in [lo, hi], or nullopt.
std::optional<int> Int(const picojson::object& o, const std::string& key, int lo, int hi)
{
  const auto it = o.find(key);
  if (it == o.end() || !it->second.is<double>())
    return std::nullopt;
  const double v = it->second.get<double>();
  if (!(v >= lo && v <= hi) || v != static_cast<double>(static_cast<int>(v)))
    return std::nullopt;
  return static_cast<int>(v);
}

// A telemetry number: clamped into [lo, hi] rather than refused, since a bad one should not end
// the match; nullopt when absent or not a number.
std::optional<int> ClampedInt(const picojson::object& o, const std::string& key, int lo, int hi)
{
  const auto it = o.find(key);
  if (it == o.end() || !it->second.is<double>())
    return std::nullopt;
  const double v = it->second.get<double>();
  if (std::isnan(v))
    return std::nullopt;
  return static_cast<int>(std::clamp(v, static_cast<double>(lo), static_cast<double>(hi)));
}

std::string Str(const picojson::object& o, const std::string& key)
{
  const auto it = o.find(key);
  return it != o.end() && it->second.is<std::string>() ? it->second.get<std::string>() : "";
}

constexpr int MAX_FRAME = 1 << 30;
// Telemetry bounds: re-simulated frames in one second, a lead in tenths of a frame, a round trip
// in milliseconds, stalls and hitches in a match. Far beyond anything honest (the session clamps
// further), small enough that no arithmetic on them overflows.
constexpr int MAX_RESIMULATED = 1 << 20;
constexpr int MAX_STALLS = 1 << 20;
constexpr int MAX_ADVANTAGE = 1 << 20;
constexpr int MAX_RTT = 1 << 20;
// Drop-in history in one packet (the session sends far less; CoalescePacket may join two).
constexpr size_t MAX_HISTORY_RUNS_IN_PACKET = 128;
constexpr size_t MAX_HISTORY_FRAMES_IN_PACKET = MAX_HISTORY_IN_PACKET;

// picojson has no nesting limit (deep arrays overflow the I/O thread's stack) and aborts on a number
// that overflows a double. Nothing YouGame sends nests deeply, uses an exponent or has more than 17
// digits in a row, so a message that does is refused before parsing.
bool SafeToParse(const std::string& text)
{
  int depth = 0, digits = 0;
  for (size_t i = 0; i < text.size(); ++i)
  {
    const char c = text[i];
    if (c == '"')
    {
      // Skip the string, escapes included.
      for (++i; i < text.size() && text[i] != '"'; ++i)
      {
        if (text[i] == '\\')
          ++i;
      }
      digits = 0;
      continue;
    }
    digits = c >= '0' && c <= '9' ? digits + 1 : 0;
    if (digits > 17)
      return false;
    if (c == '[' || c == '{')
    {
      if (++depth > 16)
        return false;
    }
    else if (c == ']' || c == '}')
    {
      --depth;
    }
    else if (text.compare(i, 4, "true") == 0)
    {
      i += 3;
    }
    else if (text.compare(i, 5, "false") == 0)
    {
      i += 4;
    }
    else if (c == 'e' || c == 'E')
    {
      return false;
    }
  }
  return true;
}

bool ParseJson(const std::string& text, picojson::value* value)
{
  return SafeToParse(text) && picojson::parse(*value, text).empty();
}

picojson::object PacketToJson(const Packet& packet)
{
  picojson::object d;
  d["k"] = picojson::value("p");
  d["s"] = picojson::value(static_cast<double>(packet.seat));
  d["f"] = picojson::value(static_cast<double>(packet.first_frame));
  std::string pads;
  for (const Pad& pad : packet.pads)
    pads += ToHex(pad.data(), pad.size());
  d["p"] = picojson::value(pads);
  picojson::array ack;
  for (int a : packet.ack)
    ack.emplace_back(static_cast<double>(a));
  d["a"] = picojson::value(ack);
  d["c"] = picojson::value(static_cast<double>(packet.current_frame));
  // Optional, so a peer built before adaptive delay and time sync still parses these packets.
  if (packet.resimulated >= 0)
    d["r"] = picojson::value(static_cast<double>(packet.resimulated));
  if (packet.advantage)
    d["v"] = picojson::value(static_cast<double>(*packet.advantage));
  if (packet.rtt)
    d["l"] = picojson::value(static_cast<double>(*packet.rtt));
  if (packet.stalls >= 0)
    d["n"] = picojson::value(static_cast<double>(packet.stalls));
  if (packet.hitches >= 0)
    d["j"] = picojson::value(static_cast<double>(packet.hitches));
  if (packet.checksum_frame)
  {
    d["x"] = picojson::value(static_cast<double>(*packet.checksum_frame));
    // A u64 does not survive a JSON number.
    u8 bytes[8];
    for (int i = 0; i < 8; ++i)
      bytes[i] = static_cast<u8>(packet.checksum >> (56 - 8 * i));
    d["h"] = picojson::value(ToHex(bytes, 8));
  }
  // Drop-in (all optional): the roster as [plug, unplug, live] per seat (-1: never), the history
  // acknowledgement, history as [count, pads hex] runs, and a leave request.
  if (packet.roster)
  {
    picojson::array roster;
    for (const SeatPlan& plan : *packet.roster)
    {
      picojson::array entry;
      for (int v : {plan.plug_from, plan.unplug_from, plan.live_from})
        entry.emplace_back(static_cast<double>(v == NEVER ? -1 : v));
      roster.emplace_back(entry);
    }
    d["ro"] = picojson::value(roster);
  }
  if (packet.history_ack >= 0)
    d["ha"] = picojson::value(static_cast<double>(packet.history_ack));
  if (packet.history_seat >= 0 && !packet.history.empty())
  {
    d["ht"] = picojson::value(static_cast<double>(packet.history_seat));
    d["hf"] = picojson::value(static_cast<double>(packet.history_first));
    picojson::array runs;
    for (size_t i = 0; i < packet.history.size();)
    {
      size_t n = 1;
      while (i + n < packet.history.size() && packet.history[i + n] == packet.history[i])
        ++n;
      std::string hex;
      for (const Pad& pad : packet.history[i])
        hex += ToHex(pad.data(), pad.size());
      picojson::array run;
      run.emplace_back(static_cast<double>(n));
      run.emplace_back(hex);
      runs.emplace_back(run);
      i += n;
    }
    d["hp"] = picojson::value(runs);
  }
  if (packet.leaving)
    d["lv"] = picojson::value(true);
  return d;
}

// Shape and range checks only; the session bounds frames against its own position.
bool PacketFromJson(const picojson::object& d, Packet* packet)
{
  if (Str(d, "k") != "p")
    return false;
  const auto seat = Int(d, "s", 0, MAX_SEATS - 1);
  const auto first = Int(d, "f", 0, MAX_FRAME);
  const auto current = Int(d, "c", 0, MAX_FRAME);
  if (!seat || !first || !current)
    return false;
  std::vector<u8> bytes;
  const std::string pads = Str(d, "p");
  if (pads.size() > MAX_PADS_IN_PACKET * sizeof(Pad) * 2 || !FromHex(pads, &bytes) ||
      bytes.size() % sizeof(Pad))
  {
    return false;
  }
  const auto ack = d.find("a");
  if (ack == d.end() || !ack->second.is<picojson::array>() ||
      ack->second.get<picojson::array>().size() != MAX_SEATS)
  {
    return false;
  }
  Packet out;
  out.seat = *seat;
  out.first_frame = *first;
  out.current_frame = *current;
  for (size_t i = 0; i < bytes.size(); i += sizeof(Pad))
  {
    Pad pad;
    std::copy_n(bytes.begin() + i, pad.size(), pad.begin());
    out.pads.push_back(pad);
  }
  const auto& acks = ack->second.get<picojson::array>();
  for (int s = 0; s < MAX_SEATS; ++s)
  {
    const picojson::value& v = acks[s];
    if (!v.is<double>() || v.get<double>() < -1 || v.get<double>() > MAX_FRAME ||
        v.get<double>() != static_cast<double>(static_cast<int>(v.get<double>())))
    {
      return false;
    }
    out.ack[s] = static_cast<int>(v.get<double>());
  }
  // Telemetry for adaptive delay and time sync: optional (older peers send none) and clamped.
  if (const auto resimulated = ClampedInt(d, "r", 0, MAX_RESIMULATED))
    out.resimulated = *resimulated;
  out.advantage = ClampedInt(d, "v", -MAX_ADVANTAGE, MAX_ADVANTAGE);
  out.rtt = ClampedInt(d, "l", 0, MAX_RTT);
  if (const auto stalls = ClampedInt(d, "n", 0, MAX_STALLS))
    out.stalls = *stalls;
  if (const auto hitches = ClampedInt(d, "j", 0, MAX_STALLS))
    out.hitches = *hitches;
  if (d.count("x"))
  {
    const auto frame = Int(d, "x", 0, MAX_FRAME);
    std::vector<u8> hash;
    if (!frame || !FromHex(Str(d, "h"), &hash) || hash.size() != 8)
      return false;
    out.checksum_frame = *frame;
    out.checksum = 0;
    for (u8 b : hash)
      out.checksum = out.checksum << 8 | b;
  }
  if (d.count("ro"))
  {
    const auto& ro = d.at("ro");
    if (!ro.is<picojson::array>() || ro.get<picojson::array>().size() != MAX_SEATS)
      return false;
    std::array<SeatPlan, MAX_SEATS> roster;
    for (int s = 0; s < MAX_SEATS; ++s)
    {
      const auto& entry = ro.get<picojson::array>()[s];
      if (!entry.is<picojson::array>() || entry.get<picojson::array>().size() != 3)
        return false;
      int values[3];
      for (int k = 0; k < 3; ++k)
      {
        const picojson::value& v = entry.get<picojson::array>()[k];
        if (!v.is<double>() || v.get<double>() < -1 || v.get<double>() > MAX_FRAME ||
            v.get<double>() != static_cast<double>(static_cast<int>(v.get<double>())))
        {
          return false;
        }
        values[k] = v.get<double>() < 0 ? NEVER : static_cast<int>(v.get<double>());
      }
      roster[s] = {values[0], values[1], values[2]};
    }
    out.roster = roster;
  }
  if (d.count("ha"))
  {
    const auto ha = Int(d, "ha", 0, MAX_FRAME);
    if (!ha)
      return false;
    out.history_ack = *ha;
  }
  if (d.count("hp"))
  {
    const auto seat_h = Int(d, "ht", 0, MAX_SEATS - 1);
    const auto first_h = Int(d, "hf", 0, MAX_FRAME);
    const auto& hp = d.at("hp");
    if (!seat_h || !first_h || !hp.is<picojson::array>() ||
        hp.get<picojson::array>().size() > MAX_HISTORY_RUNS_IN_PACKET)
    {
      return false;
    }
    out.history_seat = *seat_h;
    out.history_first = *first_h;
    for (const picojson::value& run : hp.get<picojson::array>())
    {
      if (!run.is<picojson::array>() || run.get<picojson::array>().size() != 2)
        return false;
      const auto& r = run.get<picojson::array>();
      std::vector<u8> run_bytes;
      if (!r[0].is<double>() || !r[1].is<std::string>() || r[0].get<double>() < 1 ||
          r[0].get<double>() > MAX_HISTORY_FRAMES_IN_PACKET ||
          !FromHex(r[1].get<std::string>(), &run_bytes) || run_bytes.size() != sizeof(Pads))
      {
        return false;
      }
      const int n = static_cast<int>(r[0].get<double>());
      if (out.history.size() + n > MAX_HISTORY_FRAMES_IN_PACKET)
        return false;
      Pads run_pads;
      for (int k = 0; k < MAX_SEATS; ++k)
        std::copy_n(run_bytes.begin() + k * sizeof(Pad), sizeof(Pad), run_pads[k].begin());
      out.history.insert(out.history.end(), n, run_pads);
    }
  }
  out.leaving = d.count("lv") && d.at("lv").evaluate_as_boolean();
  *packet = std::move(out);
  return true;
}

// The desktop app's bridge: exactly http://127.0.0.1:<port>, so the token goes nowhere else.
bool LoopbackBridge(std::string bridge)
{
  if (bridge.ends_with("/"))
    bridge.pop_back();
  constexpr std::string_view prefix = "http://127.0.0.1:";
  if (!bridge.starts_with(prefix))
    return false;
  const std::string_view port = std::string_view(bridge).substr(prefix.size());
  return !port.empty() && port.size() <= 5 &&
         std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Room tickets travel only over TLS, except to a rooms Worker on this machine (tests).
bool AcceptableRoomUrl(const std::string& url)
{
  return url.starts_with("https://") || url.starts_with("wss://") ||
         url.starts_with("http://127.0.0.1:") || url.starts_with("http://localhost:") ||
         url.starts_with("ws://127.0.0.1:") || url.starts_with("ws://localhost:");
}

std::string WebSocketUrl(std::string url)
{
  if (url.starts_with("https://"))
    url.replace(0, 8, "wss://");
  else if (url.starts_with("http://"))
    url.replace(0, 7, "ws://");
  return url;
}
}  // namespace

// Why the rooms Worker refused the WebSocket upgrade, for the player. The upgrade's body isn't
// readable here, only its status (rooms/src/index.ts, the room's join checks): 401 an expired
// ticket; 403 a room whose lobby key differs (another Orca build, disc or save, which for an
// Orca room is the only kind of room it can be refused for) or a host's kick; 409 a full room.
std::string RefusalCode(const std::string& connect_error)
{
  if (connect_error.find("(HTTP 403)") != std::string::npos)
    return "room_mismatch";
  if (connect_error.find("(HTTP 409)") != std::string::npos)
    return "room_full";
  return "network";
}

std::string RefusalReason(const std::string& connect_error)
{
  if (connect_error.find("(HTTP 403)") != std::string::npos)
  {
    return "The room didn't let you in: your friend's Orca, disc or save differs from yours (both "
           "of you: update YouGame and press Play again), or the host removed you";
  }
  if (connect_error.find("(HTTP 409)") != std::string::npos)
    return "That room is full";
  if (connect_error.find("(HTTP 401)") != std::string::npos)
    return "The invite expired; press Play again";
  if (connect_error.find("(HTTP 4") != std::string::npos)
    return "The room refused to let you in";
  return "Could not reach YouGame's rooms";
}

std::string EncodePacket(const Packet& packet)
{
  return picojson::value(PacketToJson(packet)).serialize();
}

bool DecodePacket(const std::string& json, Packet* packet)
{
  picojson::value value;
  if (!ParseJson(json, &value) || !value.is<picojson::object>())
    return false;
  return PacketFromJson(value.get<picojson::object>(), packet);
}

struct YouGameRoom::Impl
{
  RoomOptions options;
  std::thread thread;
  std::atomic<bool> stop{false};

  // Shared with the emulator thread.
  mutable std::mutex mutex;
  RoomState state = RoomState::Connecting;
  std::string status = "Connecting to YouGame...";
  std::string error_code;
  // Why this player leaves, told to the friend before going (Orca::Status code, e.g. "desync").
  std::string leave_reason;
  std::string code;
  int seat = -1;
  // Each seat's player name as the room's roster gives it (the same bytes on every machine; under
  // mutex).
  std::map<int, std::string> seat_names;
  int rtt = -1;
  // Counts pongs, so a caller can tell a new measurement from the same one read again.
  u32 rtt_sequence = 0;
  // The latest RTT_SAMPLES measurements, oldest first.
  std::deque<int> recent_rtts;
  std::vector<Packet> inbox;
  // Which connection sent each inbox packet: a player who left takes its unread packets with it
  // (the next player in that seat must not read them as its own).
  std::vector<std::string> inbox_from;
  std::optional<Packet> outgoing;

  // The I/O thread's own.
  Common::WebSocket ws;
  std::string me, host;
  int revision = -1;
  // Every other participant, by connection id.
  struct Peer
  {
    int slot = -1;
    std::string name;
    // It said hello with our compatibility key; it heard ours; it was reported as a player.
    bool heard = false;
    bool heard_us = false;
    bool announced = false;
    bool drop_in_host = false;
    bool sent_packet = false;
    std::optional<KeyframeInfo> keyframe;
    // Why it said it was leaving (its "bye"), for this side's own report.
    std::string bye;
  };
  std::map<std::string, Peer> peers;
  Clock::time_point last_send{}, last_ping{}, last_hello{};
  // Shared with the emulator thread (under mutex): what happened to other players, and the
  // keyframe the host offers each joining seat.
  std::vector<PeerEvent> events;
  std::map<int, KeyframeInfo> keyframe_offers;
  std::vector<std::pair<int, std::string>> pending_drops;
  std::optional<std::vector<KeyframeInfo::Name>> pending_names;
  // Whether this Orca joins someone else's game (RoomOptions::joining, or the app's invite), known
  // once the ticket is in (role_known, under mutex).
  bool joining = false;
  bool role_known = false;
  std::atomic<bool> offer_now{false};
  // The room as the page draws it (welcome, join, leave, lobby and start carry these).
  picojson::array room_participants, room_players;
  // This room's place among the process's rooms (s_room_generation): its page mirror never
  // overwrites a newer room's.
  const u64 generation = ++s_room_generation;
  // The I/O thread's: when the welcome came (a joiner waits HOST_WAIT for its host from then),
  // whether the game's host ever said hello here, and when the server last sent anything.
  std::optional<Clock::time_point> welcomed_at;
  bool host_seen = false;
  Clock::time_point last_heard{};

  // Under the desktop app: the room is mirrored to the YouGame page through the app's bridge
  // (mpRoom, the latest only), and the page's lobby commands come back on its event stream.
  bool bridged = false;
  std::string bridge_url, bridge_token;
  // How tickets were minted (set once, before role_known): the dev site and player, and the
  // keyframe store's address from the ticket reply (YouGame's "keyframes", else the default).
  std::string site, dev_player, keyframes_url;
  std::string current_ticket;  // under mutex
  std::thread bridge_thread, events_thread;
  std::atomic<bool> bridge_done{false};
  std::mutex bridge_mutex;
  std::condition_variable bridge_wake;
  std::optional<std::string> bridge_pending;
  std::string last_published;  // the I/O thread's
  bool mirror_closed = false;  // under s_mirror_mutex: the emptied room went out
  std::atomic<bool> leave_requested{false};
  std::atomic<bool> test_drop{false};
  std::vector<picojson::object> page_commands;  // guarded by bridge_mutex

  // Test-only network delay (Orca::TestNetDelayMs): messages wait in these queues, in order.
  struct Held
  {
    Clock::time_point due;
    std::string text;
  };
  std::optional<int> net_delay = TestNetDelayMs();
  int net_jitter = TestNetJitterMs();
  u32 net_count = 0;
  std::deque<Held> held_out, held_in;
  // Test-only spikes (Orca::TestNetSpikesConfig): the current or next one, from the room's start.
  std::optional<TestNetSpikes> net_spikes = TestNetSpikesConfig();
  const Clock::time_point net_opened = Clock::now();
  Clock::time_point spike_from = net_opened;
  Clock::time_point spike_to = net_opened;
  u32 spike_count = 0;

  // The end of the spike `now` falls in, or `now`.
  Clock::time_point RadioFree(Clock::time_point now)
  {
    if (!net_spikes)
      return now;
    const auto draw = [this](int lo, int hi) {
      const u32 x = ++spike_count * 2246822519u ^ 0x9e3779b9u;
      return lo + static_cast<int>((x ^ (x >> 15)) % static_cast<u32>(hi - lo + 1));
    };
    while (spike_to <= now)
    {
      spike_from = spike_to + std::chrono::milliseconds(draw(net_spikes->gap_min_ms,
                                                             net_spikes->gap_max_ms));
      spike_to = spike_from + std::chrono::milliseconds(draw(net_spikes->length_min_ms,
                                                             net_spikes->length_max_ms));
    }
    const auto since = std::chrono::duration_cast<std::chrono::seconds>(spike_from - net_opened);
    if (now < spike_from || since.count() < net_spikes->from_s || since.count() >= net_spikes->to_s)
      return now;
    return spike_to;
  }

  Clock::time_point HoldUntil(const std::deque<Held>& queue)
  {
    // A deterministic 0..jitter extra per message, after any spike; never earlier than the message
    // before it.
    const u32 extra = net_jitter ? (++net_count * 2654435761u) % static_cast<u32>(net_jitter + 1) : 0;
    const auto due = RadioFree(Clock::now()) +
                     std::chrono::milliseconds(*net_delay + static_cast<int>(extra));
    return queue.empty() ? due : std::max(due, queue.back().due);
  }

  explicit Impl(RoomOptions room_options) : options(std::move(room_options))
  {
    joining = options.joining;
    site = Env("ORCA_SITE");
    if (site.empty())
      site = "https://yougame.co";
    while (site.ends_with("/"))
      site.pop_back();
    thread = std::thread([this] { Run(); });
  }

  ~Impl()
  {
    stop = true;
    if (thread.joinable())
      thread.join();
  }

  RoomState State() const
  {
    std::lock_guard lock(mutex);
    return state;
  }

  void SetStatus(std::string line)
  {
    INFO_LOG_FMT(NETPLAY, "Orca room: {}", line);
    std::lock_guard lock(mutex);
    status = std::move(line);
  }

  // `app_code`: the desktop app's error code for this end (Orca::Status), empty for a normal leave.
  void End(std::string why, std::string app_code = "network")
  {
    WARN_LOG_FMT(NETPLAY, "Orca room ended: {}", why);
    std::lock_guard lock(mutex);
    if (state == RoomState::Ended)
      return;
    state = RoomState::Ended;
    status = std::move(why);
    error_code = std::move(app_code);
  }

  // `held`: through the test delay queue when one is configured (teardown messages skip it).
  bool SendJson(const picojson::object& message,
                std::chrono::milliseconds timeout = std::chrono::seconds(2), bool held = true)
  {
    if (held && net_delay)
    {
      held_out.push_back({HoldUntil(held_out), picojson::value(message).serialize()});
      return true;
    }
    if (ws.SendText(picojson::value(message).serialize(), timeout))
      return true;
    WARN_LOG_FMT(NETPLAY, "Orca room: send failed: {}", ws.Error());
    return false;
  }

  // The ticket for the room, and the room code, from the desktop app's bridge or (tests) the site.
  bool Ticket(std::string* url, std::string* ticket)
  {
    picojson::object lobby;
    lobby["version"] = picojson::value(1.0);
    lobby["minPlayers"] = picojson::value(2.0);
    lobby["maxLocalPlayers"] = picojson::value(1.0);
    lobby["compatibility"] = picojson::value(options.compatibility);

    const std::string bridge = Env("YOUGAME_BRIDGE"), token = Env("YOUGAME_TOKEN");
    const std::string dev = Env("ORCA_TEST_DEV_GAME");
    const bool use_bridge = !bridge.empty() && !token.empty();
    if (!use_bridge && dev.empty())
    {
      End("Start Orca from the YouGame app to play online");
      return false;
    }
    // The bridge is the desktop app on loopback; nothing else may receive the token.
    if (use_bridge && !LoopbackBridge(bridge))
    {
      End("Invalid YouGame app bridge address");
      return false;
    }
    if (use_bridge)
    {
      bridged = true;
      bridge_url = bridge.ends_with("/") ? bridge.substr(0, bridge.size() - 1) : bridge;
      bridge_token = token;
    }

    // Leave() must not wait out a slow request: the progress callback aborts it.
    Common::HttpRequest http(std::chrono::seconds(10),
                             [this](s64, s64, s64, s64) { return !stop.load(); });
    const auto post = [&](const std::string& target, const picojson::object& body,
                          const Common::HttpRequest::Headers& headers,
                          picojson::object* result) -> bool {
      const auto response = http.Post(target, picojson::value(body).serialize(), headers,
                                      Common::HttpRequest::AllowedReturnCodes::All);
      if (!response)
      {
        WARN_LOG_FMT(NETPLAY, "Orca room: no reply from {} (HTTP {})", target,
                     http.GetLastResponseCode());
        End(stop ? "You left the room" : "YouGame did not respond", stop ? "" : "network");
        return false;
      }
      picojson::value json;
      const std::string text(response->begin(), response->end());
      if (!ParseJson(text, &json) || !json.is<picojson::object>())
      {
        End(fmt::format("YouGame sent an unreadable reply (HTTP {})", http.GetLastResponseCode()));
        return false;
      }
      picojson::object o = json.get<picojson::object>();
      if (use_bridge)
      {
        // The bridge wraps SDK replies: {ok, result} or {ok: false, error}.
        if (!o.count("ok") || !o["ok"].evaluate_as_boolean() ||
            !o["result"].is<picojson::object>())
        {
          End(Str(o, "error").empty() ? "The YouGame app refused the request" : Str(o, "error"));
          return false;
        }
        // Copied out first: assigning a map from one of its own elements frees it mid-copy.
        picojson::object unwrapped = o["result"].get<picojson::object>();
        o = std::move(unwrapped);
      }
      else if (http.GetLastResponseCode() != 200)
      {
        End(Str(o, "error").empty() ? "YouGame refused the ticket" : Str(o, "error"));
        return false;
      }
      *result = std::move(o);
      return true;
    };

    std::string room = options.room_code;
    if (room.empty())
      room = Env("ORCA_TEST_ROOM");
    picojson::object reply;
    if (use_bridge)
    {
      const Common::HttpRequest::Headers headers{{"Authorization", "Bearer " + token},
                                                 {"Content-Type", "application/json"}};
      if (room.empty() && options.ask_app_for_room)
      {
        // An invite the player opened in the app names the room.
        picojson::object hello;
        hello["type"] = picojson::value("hello");
        if (!post(bridge + "/sdk", hello, headers, &reply))
          return false;
        room = Str(reply, "room");
        // The app opened an invite: this Orca joins the friend's game.
        if (!room.empty())
          joining = true;
      }
      if (room.empty())
        room = RandomString(CODE_ALPHABET, 8);
      if (!ValidCode(room))
      {
        End("Room codes are 6 to 12 lowercase letters and digits");
        return false;
      }
      picojson::object request;
      request["type"] = picojson::value("mpTicket");
      request["players"] = picojson::value(static_cast<double>(MAX_SEATS));
      request["mode"] = picojson::value(options.mode);
      request["ranked"] = picojson::value(false);
      request["room"] = picojson::value(room);
      request["lobby"] = picojson::value(lobby);
      if (!post(bridge + "/sdk", request, headers, &reply))
        return false;
    }
    else
    {
      if (room.empty())
        room = RandomString(CODE_ALPHABET, 8);
      if (!ValidCode(room))
      {
        End("Room codes are 6 to 12 lowercase letters and digits");
        return false;
      }
      std::string player = Env("ORCA_TEST_PLAYER");
      if (player.empty())
        player = "orca-" + RandomString("abcdefghijklmnopqrstuvwxyz0123456789", 15);
      dev_player = player;
      picojson::object request;
      request["dev"] = picojson::value(dev);
      request["player_id"] = picojson::value(player);
      request["name"] = picojson::value(options.player_name);
      request["players"] = picojson::value(static_cast<double>(MAX_SEATS));
      request["mode"] = picojson::value(options.mode);
      request["room"] = picojson::value(room);
      request["lobby"] = picojson::value(lobby);
      if (!post(site + "/api/multiplayer/ticket", request, {{"Content-Type", "application/json"}},
                &reply))
      {
        return false;
      }
    }
    *url = Str(reply, "url");
    *ticket = Str(reply, "ticket");
    if (url->empty() || ticket->empty() || !AcceptableRoomUrl(*url))
    {
      End("YouGame sent no usable room ticket");
      return false;
    }
    keyframes_url = KeyframesUrl(Str(reply, "keyframes"));
    std::lock_guard lock(mutex);
    code = room;
    current_ticket = *ticket;
    role_known = true;
    return true;
  }

  // The keyframe store's base: what YouGame's ticket reply names (only a URL on the site the
  // tickets come from: ORCA_SITE, else yougame.co), else the site's default.
  std::string KeyframesUrl(const std::string& named) const
  {
    if (!named.empty() && named.starts_with(site + "/"))
      return named.ends_with("/") ? named.substr(0, named.size() - 1) : named;
    return site + "/api/orca/keyframes";
  }

  // A fresh ticket for this room (tickets live five minutes; a keyframe goes out much later), for
  // the keyframe store. Any thread, once the room has its first ticket; no side effects.
  bool FreshTicket(bool fresh, std::string* ticket, std::string* store_url, std::string* error)
  {
    std::string room;
    {
      std::lock_guard lock(mutex);
      if (!role_known)
      {
        *error = "the room has no ticket yet";
        return false;
      }
      room = code;
      // YouGame's keyframe store takes a room's ticket for hours past its expiry: mint another
      // only when it refused this one.
      if (!fresh && !current_ticket.empty())
      {
        *ticket = current_ticket;
        *store_url = keyframes_url + "/" + room;
        return true;
      }
    }
    picojson::object lobby;
    lobby["version"] = picojson::value(1.0);
    lobby["minPlayers"] = picojson::value(2.0);
    lobby["maxLocalPlayers"] = picojson::value(1.0);
    lobby["compatibility"] = picojson::value(options.compatibility);
    picojson::object request;
    request["players"] = picojson::value(static_cast<double>(MAX_SEATS));
    request["mode"] = picojson::value(options.mode);
    request["room"] = picojson::value(room);
    request["lobby"] = picojson::value(lobby);
    Common::HttpRequest http(std::chrono::seconds(10));
    Common::HttpRequest::Response response;
    if (bridged)
    {
      request["type"] = picojson::value("mpTicket");
      request["ranked"] = picojson::value(false);
      response = http.Post(bridge_url + "/sdk", picojson::value(request).serialize(),
                           {{"Authorization", "Bearer " + bridge_token},
                            {"Content-Type", "application/json"}},
                           Common::HttpRequest::AllowedReturnCodes::All);
    }
    else
    {
      request["dev"] = picojson::value(Env("ORCA_TEST_DEV_GAME"));
      request["player_id"] = picojson::value(dev_player);
      request["name"] = picojson::value(options.player_name);
      response = http.Post(site + "/api/multiplayer/ticket", picojson::value(request).serialize(),
                           {{"Content-Type", "application/json"}},
                           Common::HttpRequest::AllowedReturnCodes::All);
    }
    picojson::value json;
    if (!response || !ParseJson(std::string(response->begin(), response->end()), &json) ||
        !json.is<picojson::object>())
    {
      *error = fmt::format("no ticket from YouGame (HTTP {})", http.GetLastResponseCode());
      return false;
    }
    picojson::object reply = json.get<picojson::object>();
    if (bridged)
    {
      if (!reply.count("ok") || !reply["ok"].evaluate_as_boolean() ||
          !reply["result"].is<picojson::object>())
      {
        *error = Str(reply, "error").empty() ? "the app refused a ticket" : Str(reply, "error");
        return false;
      }
      picojson::object unwrapped = reply["result"].get<picojson::object>();
      reply = std::move(unwrapped);
    }
    else if (http.GetLastResponseCode() != 200)
    {
      *error = Str(reply, "error").empty() ? "YouGame refused a ticket" : Str(reply, "error");
      return false;
    }
    *ticket = Str(reply, "ticket");
    if (ticket->empty())
    {
      *error = "YouGame sent no ticket";
      return false;
    }
    *store_url = KeyframesUrl(Str(reply, "keyframes")) + "/" + room;
    std::lock_guard lock(mutex);
    current_ticket = *ticket;
    return true;
  }

  void Run()
  {
    std::string url, ticket;
    if (!Ticket(&url, &ticket))
      return;
    if (stop)
    {
      End("You left the room", "");
      return;
    }
    std::string room;
    {
      std::lock_guard lock(mutex);
      room = code;
    }
    Common::HttpRequest escape;
    std::string error;
    if (!ws.Connect(WebSocketUrl(url) + "/room/" + room + "?ticket=" +
                        escape.EscapeComponent(ticket),
                    std::chrono::seconds(8), &error))
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: {}", error);
      End(RefusalReason(error), RefusalCode(error));
      return;
    }
    SetStatus(fmt::format("Room {}: waiting for your friend", room));
    last_heard = Clock::now();
    if (bridged)
    {
      bridge_thread = std::thread([this] { BridgeLoop(); });
      events_thread = std::thread([this] { EventsLoop(); });
    }

    if (net_delay)
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: TEST network delay {} ms + 0..{} ms each way", *net_delay,
                   net_jitter);
    }
    if (net_spikes)
    {
      WARN_LOG_FMT(NETPLAY,
                   "Orca room: TEST network spikes: dark both ways for {}-{} ms after every {}-{} "
                   "ms, from {} s to {} s",
                   net_spikes->length_min_ms, net_spikes->length_max_ms, net_spikes->gap_min_ms,
                   net_spikes->gap_max_ms, net_spikes->from_s, net_spikes->to_s);
    }
    // The connection failed under the player (it went quiet, or a test drops it): see below.
    bool dropped = false;
    while (!stop && State() != RoomState::Ended)
    {
      if (leave_requested)
      {
        End("You left the room", "");
        break;
      }
      if (test_drop)
      {
        WARN_LOG_FMT(NETPLAY, "Orca room: TEST dropping the connection");
        dropped = true;
        End("Lost the connection to the room (test drop)");
        break;
      }
      if (bridged)
        SendPageCommands();
      if (net_delay)
        FlushHeld();
      if (auto text = ws.ReceiveText(std::chrono::milliseconds(net_delay ? 1 : 2)))
      {
        last_heard = Clock::now();
        if (net_delay)
          held_in.push_back({HoldUntil(held_in), std::move(*text)});
        else
          OnMessage(*text);
        continue;
      }
      if (!ws.IsOpen())
      {
        // 4000: the server replaced this socket with a newer one for the same player (another run
        // of this game, on this account, in the same room). A host leaves the room to it rather
        // than take it back, which would replace that one in turn, and so on.
        const bool replaced = ws.CloseCode() == 4000;
        End(fmt::format("{} ({})",
                        replaced ? "Another game of yours took over this room" :
                                   "Lost the connection to the room",
                        ws.Error().empty() ? "no reason given" : ws.Error()),
            replaced && !joining ? "taken" : "network");
        break;
      }
      if (Clock::now() - last_heard > SERVER_SILENCE_LIMIT)
      {
        dropped = true;
        End(fmt::format("Lost the connection to the room (no answer for {} s)",
                        SERVER_SILENCE_LIMIT.count()));
        break;
      }
      Tick();
    }

    // The bridge threads wind down while the goodbyes go out. First the page learns the room is
    // gone (Orca left it), ahead of the next room's mirror if there is one (else that one stands,
    // and this is dropped: PostMirror).
    bridge_done = true;
    bridge_wake.notify_all();
    if (bridged)
      PublishEnded();
    std::string reason;
    {
      std::lock_guard lock(mutex);
      reason = leave_reason;
    }
    // A connection that failed under the player goes without a goodbye, and with a close the server
    // reads as a dropped connection: it holds the seat (and the host's role) for the player's next
    // connection, as it does for a real drop.
    if (dropped)
    {
      ws.Close(4001);
    }
    else
    {
      if (!reason.empty() && !peers.empty() && ws.IsOpen())
      {
        picojson::object d;
        d["k"] = picojson::value("bye");
        d["r"] = picojson::value(reason);
        picojson::object message;
        message["t"] = picojson::value("msg");
        message["d"] = picojson::value(d);
        SendJson(message, std::chrono::milliseconds(500), false);
      }
      if (ws.IsOpen())
      {
        picojson::object leave;
        leave["t"] = picojson::value("leave");
        SendJson(leave, std::chrono::milliseconds(500), false);
      }
      ws.Close();
    }
    StopBridge();
  }

  // The lobby the page shows: the same mpRoom a web game's SDK publishes (Player.tsx parses it).
  // `ended`: the room is gone (Orca left it, or lost it): its code with nobody in it, which the
  // page reads as Orca having left the room (Player's gameRoom, OrcaEmbed's onRoom), so a later
  // invite into the same room isn't taken for one Orca is already in.
  picojson::object RoomMirror(bool ended)
  {
    picojson::object lobby;
    lobby["version"] = picojson::value(1.0);
    lobby["minPlayers"] = picojson::value(2.0);
    lobby["maxLocalPlayers"] = picojson::value(1.0);
    lobby["compatibility"] = picojson::value(options.compatibility);
    picojson::object room;
    room["type"] = picojson::value("mpRoom");
    room["lifecycle"] = picojson::value("game");
    room["participants"] = picojson::value(ended ? picojson::array{} : room_participants);
    room["revision"] = picojson::value(static_cast<double>(revision));
    room["matchId"] = picojson::value();
    room["lobby"] = picojson::value(lobby);
    room["me"] = picojson::value(me);
    {
      std::lock_guard lock(mutex);
      room["code"] = picojson::value(code);
      room["ready"] =
          picojson::value(!ended && std::any_of(peers.begin(), peers.end(),
                                                [](const auto& p) { return p.second.announced; }));
    }
    room["queue"] = picojson::value("private");
    room["size"] = picojson::value(static_cast<double>(MAX_SEATS));
    room["host"] = ended ? picojson::value() : picojson::value(host);
    room["readyIds"] = picojson::value(picojson::array{});
    room["players"] = picojson::value(ended ? picojson::array{} : room_players);
    return room;
  }

  void Publish()
  {
    if (!bridged)
      return;
    std::string snapshot = picojson::value(RoomMirror(false)).serialize();
    if (snapshot == last_published)
      return;
    last_published = snapshot;
    {
      std::lock_guard lock(bridge_mutex);
      bridge_pending = std::move(snapshot);
    }
    bridge_wake.notify_all();
  }

  // At teardown, on the I/O thread: the emptied room, posted at once (the bridge thread is winding
  // down), for a room the page was shown.
  void PublishEnded()
  {
    if (last_published.empty())
      return;
    Common::HttpRequest http(MIRROR_END_TIMEOUT);
    if (!PostMirror(http, picojson::value(RoomMirror(true)).serialize(), true))
      WARN_LOG_FMT(NETPLAY, "Orca room: the app did not take the room's end");
  }

  // One mirror update to the app, unless a newer room has already posted its own (then the page has
  // that one, and this is stale) or this room already posted its end (`last`: this is the end).
  // True once the app took it, or when there was nothing to send.
  bool PostMirror(Common::HttpRequest& http, const std::string& body, bool last = false)
  {
    std::lock_guard lock(s_mirror_mutex);
    if (generation < s_mirror_newest || mirror_closed)
      return true;
    s_mirror_newest = generation;
    mirror_closed = last;
    const auto response = http.Post(
        bridge_url + "/sdk", body,
        {{"Authorization", "Bearer " + bridge_token}, {"Content-Type", "application/json"}},
        Common::HttpRequest::AllowedReturnCodes::All);
    picojson::value reply;
    return response && http.GetLastResponseCode() == 200 &&
           ParseJson(std::string(response->begin(), response->end()), &reply) &&
           reply.is<picojson::object>() && reply.get("ok").evaluate_as_boolean();
  }

  void SendPageCommands()
  {
    std::vector<picojson::object> commands;
    {
      std::lock_guard lock(bridge_mutex);
      commands.swap(page_commands);
    }
    for (const auto& command : commands)
      SendJson(command);
  }

  void BridgeLoop()
  {
    Common::HttpRequest http(std::chrono::seconds(5),
                             [this](s64, s64, s64, s64) { return !bridge_done.load(); });
    while (!bridge_done)
    {
      std::optional<std::string> body;
      {
        std::unique_lock lock(bridge_mutex);
        bridge_wake.wait_for(lock, std::chrono::milliseconds(200),
                             [this] { return bridge_done.load() || bridge_pending.has_value(); });
        body.swap(bridge_pending);
      }
      if (!body || bridge_done)
        continue;
      if (!PostMirror(http, *body))
      {
        WARN_LOG_FMT(NETPLAY, "Orca room: the app did not take the room update (HTTP {})",
                     http.GetLastResponseCode());
      }
    }
  }

  // One event from the app's stream: a lobby command the page sent this game.
  void OnPageEvent(const std::string& line)
  {
    picojson::value value;
    if (!ParseJson(line, &value) || !value.is<picojson::object>())
      return;
    const auto& e = value.get<picojson::object>();
    // Only this game's own page may command it.
    const std::string game = Env("YOUGAME_GAME");
    if (game.empty() || Str(e, "slug") != game)
      return;
    const std::string cmd = Str(e, "cmd");
    if (cmd == "leave")
    {
      // The lobby's Leave is the app's "leave": a joiner unplugs at an agreed frame and plays on, a
      // host gives its friends up and opens a new room (OnlineMatch's Leave, at a frame boundary).
      // Only an app that never turned "leave" on gets the old meaning: this room just ends.
      if (Orca::Status::Cap("leave"))
        Orca::Online::RequestLeave();
      else
        leave_requested = true;
    }
    else if (cmd == "kick" && !Str(e, "id").empty())
    {
      picojson::object kick;
      kick["t"] = picojson::value("kick");
      kick["id"] = picojson::value(Str(e, "id"));
      std::lock_guard lock(bridge_mutex);
      if (page_commands.size() < 16)
        page_commands.push_back(std::move(kick));
    }
    // ready, setting, local-add and local-remove don't apply: one controller per Orca, and the
    // host's Orca starts the match itself.
  }

  struct EventStream
  {
    Impl* impl;
    std::string pending;
  };

  static size_t OnEventData(char* data, size_t size, size_t count, void* user)
  {
    auto* stream = static_cast<EventStream*>(user);
    stream->pending.append(data, size * count);
    if (stream->pending.size() > 65536)
      return 0;  // aborts the transfer
    size_t newline;
    while ((newline = stream->pending.find('\n')) != std::string::npos)
    {
      std::string line = stream->pending.substr(0, newline);
      stream->pending.erase(0, newline + 1);
      if (line.ends_with("\r"))
        line.pop_back();
      // SSE: "data:" and an optional space.
      if (line.starts_with("data:"))
        stream->impl->OnPageEvent(line.substr(line.starts_with("data: ") ? 6 : 5));
    }
    return size * count;
  }

  static int OnEventProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
  {
    return static_cast<EventStream*>(user)->impl->bridge_done ? 1 : 0;
  }

  void EventsLoop()
  {
    // The stream stays open for the whole session; reconnect if the app restarts it.
    int backoff = 500;
    while (!bridge_done)
    {
      CURL* curl = curl_easy_init();
      if (!curl)
      {
        ERROR_LOG_FMT(NETPLAY, "Orca room: couldn't open the app's event stream");
        return;
      }
      EventStream stream{this, {}};
      curl_slist* headers =
          curl_slist_append(nullptr, ("Authorization: Bearer " + bridge_token).c_str());
      curl_easy_setopt(curl, CURLOPT_URL, (bridge_url + "/events").c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &OnEventData);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream);
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &OnEventProgress);
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stream);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
      const CURLcode result = curl_easy_perform(curl);
      long http_status = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      if (bridge_done)
        return;
      // An app without the stream, or one that refuses the token, won't change its mind.
      if (http_status == 401 || http_status == 403 || http_status == 404)
      {
        ERROR_LOG_FMT(NETPLAY, "Orca room: the app refused its event stream (HTTP {}); the page's "
                               "lobby commands won't reach this game",
                      http_status);
        return;
      }
      WARN_LOG_FMT(NETPLAY, "Orca room: the app's event stream ended ({}, HTTP {})",
                   curl_easy_strerror(result), http_status);
      backoff = std::min(backoff * 2, 10000);
      for (int waited = 0; waited < backoff && !bridge_done; waited += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  void StopBridge()
  {
    bridge_done = true;
    bridge_wake.notify_all();
    if (bridge_thread.joinable())
      bridge_thread.join();
    if (events_thread.joinable())
      events_thread.join();
  }

  void FlushHeld()
  {
    const auto now = Clock::now();
    while (!held_out.empty() && held_out.front().due <= now)
    {
      if (!ws.SendText(held_out.front().text, std::chrono::seconds(2)))
        WARN_LOG_FMT(NETPLAY, "Orca room: send failed: {}", ws.Error());
      held_out.pop_front();
    }
    while (!held_in.empty() && held_in.front().due <= now && State() != RoomState::Ended)
    {
      const std::string text = std::move(held_in.front().text);
      held_in.pop_front();
      OnMessage(text);
    }
  }

  void Tick()
  {
    const auto now = Clock::now();
    // A joiner in a room its friend's game isn't in (a stale invite, a host that left, restarted or
    // got here second) would wait for a keyframe that never comes: it gives up soon after its
    // welcome, unless the host said hello by then. From then on the host's leave says it all.
    if (joining && !host_seen && welcomed_at && now - *welcomed_at >= HOST_WAIT)
    {
      End("Your friend isn't in that game any more", "peer_left");
      return;
    }
    if (now - last_ping >= PING_EVERY)
    {
      last_ping = now;
      picojson::object ping;
      ping["t"] = picojson::value("ping");
      ping["at"] = picojson::value(static_cast<double>(NowMs()));
      {
        std::lock_guard lock(mutex);
        if (rtt >= 0)
          ping["rtt"] = picojson::value(static_cast<double>(rtt));
      }
      SendJson(ping);
    }

    // Players the host unplugged hear why first.
    std::vector<std::pair<int, std::string>> drops;
    {
      std::lock_guard lock(mutex);
      drops.swap(pending_drops);
    }
    std::optional<std::vector<KeyframeInfo::Name>> names;
    {
      std::lock_guard lock(mutex);
      names.swap(pending_names);
    }
    if (names)
    {
      picojson::object d;
      d["k"] = picojson::value("nm");
      d["nm"] = NamesToJson(*names);
      for (const auto& [id, peer] : peers)
      {
        if (peer.heard)
          SendTo(id, d);
      }
    }
    for (const auto& [slot, reason] : drops)
    {
      for (const auto& [id, peer] : peers)
      {
        if (peer.slot != slot)
          continue;
        picojson::object d;
        d["k"] = picojson::value("drop");
        d["r"] = picojson::value(reason);
        SendTo(id, d);
      }
    }

    // Say hello to every other Orca in the room until it answers, so neither side treats the other
    // as a player before both programs are up and agree on the build. Then the host offers each
    // newcomer its keyframe, again every second until the newcomer's first packet arrives.
    if (now - last_hello >= HELLO_EVERY || offer_now.exchange(false))
    {
      last_hello = now;
      std::map<int, KeyframeInfo> offers;
      {
        std::lock_guard lock(mutex);
        offers = keyframe_offers;
      }
      for (auto& [id, peer] : peers)
      {
        if (!peer.heard || !peer.heard_us)
        {
          picojson::object d;
          d["k"] = picojson::value("hello");
          d["c"] = picojson::value(options.compatibility);
          d["r"] = picojson::value(peer.heard);
          d["h"] = picojson::value(!joining);
          SendTo(id, d);
        }
        if (const auto offer = offers.find(peer.slot);
            offer != offers.end() && peer.heard && !peer.sent_packet)
        {
          picojson::object d;
          d["k"] = picojson::value("kf");
          d["f"] = picojson::value(static_cast<double>(offer->second.frame));
          d["id"] = picojson::value(offer->second.id);
          d["n"] = picojson::value(static_cast<double>(offer->second.size));
          d["x"] = picojson::value(offer->second.hash);
          d["key"] = picojson::value(offer->second.key);
          d["nm"] = NamesToJson(offer->second.names);
          SendTo(id, d);
        }
      }
    }

    if (now - last_send < SEND_GAP)
      return;
    std::optional<Packet> packet;
    {
      std::lock_guard lock(mutex);
      packet.swap(outgoing);
    }
    if (!packet)
      return;
    last_send = now;
    // One message to the whole room (the rooms Worker relays it to everyone else): every other
    // player gets the same inputs, and a room message limit counts it once.
    picojson::object message;
    message["t"] = picojson::value("msg");
    message["d"] = picojson::value(PacketToJson(*packet));
    SendJson(message);
  }

  void SendTo(const std::string& to, const picojson::object& d)
  {
    picojson::object message;
    message["t"] = picojson::value("msg");
    message["to"] = picojson::value(to);
    message["d"] = picojson::value(d);
    SendJson(message);
  }

  // A player left: its unread packets go too.
  void ForgetPackets(const std::string& id)
  {
    std::lock_guard lock(mutex);
    for (size_t i = inbox.size(); i-- > 0;)
    {
      if (inbox_from[i] == id)
      {
        inbox.erase(inbox.begin() + static_cast<std::ptrdiff_t>(i));
        inbox_from.erase(inbox_from.begin() + static_cast<std::ptrdiff_t>(i));
      }
    }
  }

  void Emit(PeerEvent event)
  {
    std::lock_guard lock(mutex);
    if (events.size() < 256)
      events.push_back(std::move(event));
  }

  // A connection that is out of the game while the room still lists it (gone away, or replaced by a
  // new socket): a player of the game is unplugged, its keyframe offer and unread packets go, and
  // whatever comes back on it is a stranger until it says hello again.
  void ResetPeer(std::map<std::string, Peer>::iterator p)
  {
    if (p->second.announced)
    {
      Emit({PeerEvent::Kind::Left,
            p->second.slot,
            p->second.drop_in_host,
            p->second.name,
            {},
            p->second.bye});
    }
    {
      std::lock_guard lock(mutex);
      keyframe_offers.erase(p->second.slot);
    }
    ForgetPackets(p->first);
    p->second = {};
    p->second.slot = -1;
  }

  // The lobby roster rides on welcome, join, leave, back, away and lobby messages.
  void Roster(const picojson::object& m)
  {
    if (m.count("host"))
      host = Str(m, "host");
    if (const auto players = m.find("players");
        players != m.end() && players->second.is<picojson::array>())
    {
      room_players = players->second.get<picojson::array>();
    }
    const auto it = m.find("participants");
    if (it == m.end() || !it->second.is<picojson::array>())
    {
      Publish();
      return;
    }
    room_participants = it->second.get<picojson::array>();
    if (const auto rev = Int(m, "revision", 0, MAX_FRAME))
      revision = *rev;
    std::map<std::string, std::pair<int, std::string>> present;
    std::map<int, std::string> names;
    for (const auto& p : room_participants)
    {
      if (!p.is<picojson::object>())
        continue;
      const auto& o = p.get<picojson::object>();
      const std::string id = Str(o, "connectionId");
      const auto slot = Int(o, "slot", 0, MAX_SEATS - 1);
      if (id.empty() || !slot)
        continue;
      names[*slot] = CleanName(Str(o, "name"));
      if (id == me)
      {
        std::lock_guard lock(mutex);
        seat = *slot;
        continue;
      }
      present[id] = {*slot, Str(o, "name")};
    }
    int my_seat;
    {
      std::lock_guard lock(mutex);
      seat_names = std::move(names);
      my_seat = seat;
    }
    // A hosting Orca holds the room's first seat and is the room's host: its game's port 1 is seat
    // 0, and joiners trust only the room's host. A friend who got here first (an invite sent before
    // this Orca reached its own room) holds both, so the room can't work for either: it ends, and
    // the game opens a fresh one (OnlineMatch), which the page then mirrors. "taken" never reaches
    // the app.
    if (!joining && !me.empty() && (my_seat != 0 || host != me))
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: this game's host got seat {} of a room hosted by {}",
                   my_seat + 1, host == me ? "it" : "someone else");
      End("Someone else was already hosting this room, so your game opened a new one", "taken");
      return;
    }
    // Who left: a player of the game is unplugged by the host; the host leaving ends a joiner's.
    for (auto p = peers.begin(); p != peers.end();)
    {
      if (present.contains(p->first))
      {
        ++p;
        continue;
      }
      if (p->second.announced)
      {
        Emit({PeerEvent::Kind::Left, p->second.slot, p->second.drop_in_host, p->second.name, {},
              p->second.bye});
      }
      {
        // An offer was for that player only: the next one in the seat gets its own keyframe.
        std::lock_guard lock(mutex);
        keyframe_offers.erase(p->second.slot);
      }
      ForgetPackets(p->first);
      p = peers.erase(p);
    }
    for (const auto& [id, info] : present)
    {
      auto [p, inserted] = peers.try_emplace(id);
      p->second.slot = info.first;
      p->second.name = info.second;
      if (inserted)
        last_hello = {};
    }
    std::string room;
    {
      std::lock_guard lock(mutex);
      room = code;
    }
    if (peers.empty())
      SetStatus(fmt::format("Room {}: waiting for your friend", room));
    Publish();
  }

  void OnMessage(const std::string& text)
  {
    picojson::value value;
    if (!ParseJson(text, &value) || !value.is<picojson::object>())
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: ignored an unreadable message ({} bytes)", text.size());
      return;
    }
    const auto& m = value.get<picojson::object>();
    const std::string t = Str(m, "t");

    if (t == "welcome")
    {
      me = Str(m, "me");
      if (Str(m, "lifecycle") != "game")
      {
        End("This room is not an Orca lobby", "room_mismatch");
        return;
      }
      // A room whose game is running is what a drop-in friend joins: no refusal.
      welcomed_at = Clock::now();
      Roster(m);
      std::lock_guard lock(mutex);
      NOTICE_LOG_FMT(NETPLAY, "Orca room: in room {} on seat {}, {}", code, seat + 1,
                     joining ? "joining" : "hosting");
      if (state == RoomState::Connecting)
        state = RoomState::Waiting;
    }
    else if (t == "join" || t == "back" || t == "leave" || t == "away" || t == "lobby")
    {
      // A dropped connection ("away") is out of the game at once: its controller unplugs. So is the
      // connection a player comes back on ("back", or "join" for an id still listed): the room
      // replaced a socket that died without it noticing, and behind the new one is another run of
      // Orca, which greets anew and needs its own keyframe.
      if (t == "away" || t == "back" || t == "join")
      {
        if (const auto player = m.find("player");
            player != m.end() && player->second.is<picojson::object>())
        {
          const std::string id = Str(player->second.get<picojson::object>(), "id");
          if (const auto p = peers.find(id); p != peers.end())
          {
            ResetPeer(p);
            if (t != "away")
              last_hello = {};  // greet it at once
          }
        }
      }
      Roster(m);
    }
    else if (t == "msg")
    {
      const auto p = peers.find(Str(m, "from"));
      if (p == peers.end())
        return;
      const auto d = m.find("d");
      if (d == m.end() || !d->second.is<picojson::object>())
        return;
      OnPeerMessage(p->first, p->second, d->second.get<picojson::object>());
    }
    else if (t == "pong")
    {
      if (const auto at = m.find("at"); at != m.end() && at->second.is<double>())
      {
        const double sent = at->second.get<double>();
        const double now = static_cast<double>(NowMs());
        if (sent <= now && now - sent < 60000)
        {
          std::lock_guard lock(mutex);
          rtt = static_cast<int>(now - sent);
          ++rtt_sequence;
          recent_rtts.push_back(rtt);
          if (recent_rtts.size() > static_cast<size_t>(RTT_SAMPLES))
            recent_rtts.pop_front();
        }
      }
    }
    else if (t == "command-error")
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: {} refused: {}", Str(m, "action"), Str(m, "error"));
    }
    else if (t == "error")
    {
      End(Str(m, "error").empty() ? "The room reported an error" : Str(m, "error"));
    }
    else if (t == "kicked")
    {
      End("You were removed from the room", "kicked");
    }
  }

  void OnPeerMessage(const std::string& id, Peer& peer, const picojson::object& d)
  {
    const std::string kind = Str(d, "k");
    if (kind == "bye")
    {
      // Only known reasons, so a peer can't put text in this player's report.
      if (const std::string reason = Str(d, "r"); reason == "desync" || reason == "stalled")
        peer.bye = reason;
      return;
    }
    if (kind == "drop")
    {
      // The host unplugged this player: its report says why, as the host's does.
      if (!peer.drop_in_host || !joining)
        return;
      const std::string reason = Str(d, "r");
      if (reason == "desync")
        End("Your games went out of sync, so you left your friend's game", "desync");
      else if (reason == "stalled")
        End("The connection with your friend stalled, so you left the game", "peer_left");
      else
        End("Your friend's game disconnected you", "network");
      return;
    }
    if (kind == "hello")
    {
      // A hello's "pf" (a float class, sent by 0.3.4 builds for Project+) is ignored: every Mac and
      // PC computes the same game now (Online.cpp CompatibilityKey), and a build that differs in
      // how has another compatibility key.
      if (Str(d, "c") != options.compatibility)
      {
        // Not a player of this game; a joiner can't play it at all.
        WARN_LOG_FMT(NETPLAY, "Orca room: {} runs a different Orca build or setup", id);
        if (joining && id == host)
          End("Your friend is running a different Orca build or setup", "room_mismatch");
        return;
      }
      const bool was_heard = peer.heard;
      peer.heard = true;
      peer.heard_us = peer.heard_us || (d.count("r") && d.at("r").evaluate_as_boolean());
      // The game's host says so, and is the room's host (the first in): no other player can take
      // a joiner's game over by claiming it.
      peer.drop_in_host = d.count("h") && d.at("h").evaluate_as_boolean() && id == host;
      host_seen = host_seen || peer.drop_in_host;
      if (!was_heard)
        last_hello = {};  // answer at once rather than on the next hello tick
      // A player of the game: a joiner at a seat other than the host's, or the host itself.
      if (!peer.announced && peer.slot >= 0)
      {
        peer.announced = true;
        std::string room;
        {
          std::lock_guard lock(mutex);
          room = code;
        }
        SetStatus(fmt::format("Room {}: {} is here", room, peer.name.empty() ? "a friend" : peer.name));
        Emit({PeerEvent::Kind::Arrived, peer.slot, peer.drop_in_host, peer.name, {}});
        // The page's lobby shows the room ready now, not at its next roster change.
        Publish();
      }
      return;
    }
    if (kind == "nm")
    {
      // Only from the host, to a player of its game.
      if (!peer.drop_in_host || !joining)
        return;
      KeyframeInfo info;
      info.names = NamesFromJson(d);
      Emit({PeerEvent::Kind::Names, peer.slot, true, peer.name, info});
      return;
    }
    if (kind == "kf")
    {
      // Only from the host, to a joiner.
      if (!peer.drop_in_host || !joining)
        return;
      KeyframeInfo info;
      const auto frame = Int(d, "f", 0, MAX_FRAME);
      // YouGame's store takes 128 MB at most.
      const auto size = Int(d, "n", 1, 128 << 20);
      info.id = Str(d, "id");
      info.hash = Str(d, "x");
      info.key = Str(d, "key");
      if (!frame || !size || info.id.empty() || info.id.size() > 128 || info.hash.size() > 64 ||
          info.key.size() != 64)
      {
        return;
      }
      info.frame = *frame;
      info.size = static_cast<u64>(*size);
      info.names = NamesFromJson(d);
      if (peer.keyframe && peer.keyframe->id == info.id)
        return;
      peer.keyframe = info;
      Emit({PeerEvent::Kind::Keyframe, peer.slot, true, peer.name, info});
      return;
    }
    if (kind != "p" || !peer.announced)
      return;
    Packet packet;
    if (!PacketFromJson(d, &packet) || packet.seat != peer.slot)
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: a malformed packet from seat {}", peer.slot + 1);
      return;
    }
    peer.sent_packet = true;
    std::lock_guard lock(mutex);
    // Nobody read the inbox for a while (a long stall on this side). Each packet repeats every
    // input the sender hasn't seen acknowledged, so the newer half says all the older half did.
    if (inbox.size() >= MAX_INBOX)
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: inbox full, dropping the {} oldest packets", MAX_INBOX / 2);
      inbox.erase(inbox.begin(), inbox.begin() + MAX_INBOX / 2);
      inbox_from.erase(inbox_from.begin(), inbox_from.begin() + MAX_INBOX / 2);
    }
    inbox.push_back(std::move(packet));
    inbox_from.push_back(id);
  }
};

YouGameRoom::YouGameRoom(RoomOptions options) : m_impl(std::make_unique<Impl>(std::move(options)))
{
}

YouGameRoom::~YouGameRoom() = default;

RoomState YouGameRoom::GetState() const
{
  return m_impl->State();
}

int YouGameRoom::Seat() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->seat;
}

std::optional<bool> YouGameRoom::Joining() const
{
  std::lock_guard lock(m_impl->mutex);
  if (!m_impl->role_known)
    return std::nullopt;
  return m_impl->joining;
}

std::vector<PeerEvent> YouGameRoom::TakePeerEvents()
{
  std::lock_guard lock(m_impl->mutex);
  return std::exchange(m_impl->events, {});
}

bool YouGameRoom::ArrivalPending() const
{
  std::lock_guard lock(m_impl->mutex);
  // As the host takes them (OnlineMatch's HostEvents): a friend on a seat after the host's.
  return std::ranges::any_of(m_impl->events, [](const PeerEvent& event) {
    return event.kind == PeerEvent::Kind::Arrived && event.seat > 0 && !event.host;
  });
}

bool YouGameRoom::FreshTicket(bool fresh, std::string* ticket, std::string* store_url,
                              std::string* error)
{
  return m_impl->FreshTicket(fresh, ticket, store_url, error);
}

void YouGameRoom::DropPeer(int seat, const std::string& reason)
{
  std::lock_guard lock(m_impl->mutex);
  if (m_impl->pending_drops.size() < 16)
    m_impl->pending_drops.emplace_back(seat, reason);
}

void YouGameRoom::BroadcastNames(const std::vector<KeyframeInfo::Name>& names)
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->pending_names = names;
  m_impl->offer_now = true;
}

void YouGameRoom::OfferKeyframe(int seat, const KeyframeInfo& info)
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->keyframe_offers[seat] = info;
  // Sent on the I/O thread's next turn, not its next hello tick: a friend waits on it.
  m_impl->offer_now = true;
}

std::string YouGameRoom::SeatName(int seat) const
{
  std::lock_guard lock(m_impl->mutex);
  const auto it = m_impl->seat_names.find(seat);
  return it == m_impl->seat_names.end() ? std::string() : it->second;
}

std::string YouGameRoom::Code() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->code;
}

std::string YouGameRoom::Status() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->status;
}

void YouGameRoom::SetLeaveReason(std::string reason)
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->leave_reason = std::move(reason);
}

std::string YouGameRoom::ErrorCode() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->state == RoomState::Ended ? m_impl->error_code : "";
}

int YouGameRoom::RoundTripMs(u32* sequence) const
{
  std::lock_guard lock(m_impl->mutex);
  if (sequence)
    *sequence = m_impl->rtt_sequence;
  return m_impl->rtt;
}

std::vector<int> YouGameRoom::RecentRoundTrips(u32* sequence) const
{
  std::lock_guard lock(m_impl->mutex);
  *sequence = m_impl->rtt_sequence;
  return {m_impl->recent_rtts.begin(), m_impl->recent_rtts.end()};
}

void CoalescePacket(std::optional<Packet>* outgoing, const Packet& packet)
{
  std::optional<Packet>& pending = *outgoing;
  if (!pending)
  {
    pending = packet;
    return;
  }
  // The newer packet replaces one not sent yet. Keep what only the older one carried: inputs
  // before the newer one's first frame (both come from the same local history), and its checksum.
  Packet merged = packet;
  const int old_last = pending->first_frame + static_cast<int>(pending->pads.size()) - 1;
  if (pending->first_frame < merged.first_frame && old_last >= merged.first_frame - 1)
  {
    const int extra = std::min(merged.first_frame - pending->first_frame,
                               MAX_PADS_IN_PACKET - static_cast<int>(merged.pads.size()));
    if (extra > 0)
    {
      const auto from = pending->pads.begin() + (merged.first_frame - extra - pending->first_frame);
      merged.pads.insert(merged.pads.begin(), from, from + extra);
      merged.first_frame -= extra;
    }
  }
  if (pending->checksum_frame && !merged.checksum_frame)
  {
    merged.checksum_frame = pending->checksum_frame;
    merged.checksum = pending->checksum;
  }
  // Drop-in history: the older packet's goes first when the newer one continues it (or has none);
  // a gap is sent again once the joiner's acknowledgement stalls.
  if (!pending->history.empty())
  {
    if (merged.history.empty())
    {
      merged.history_seat = pending->history_seat;
      merged.history_first = pending->history_first;
      merged.history = pending->history;
    }
    else if (merged.history_seat == pending->history_seat &&
             pending->history_first + static_cast<int>(pending->history.size()) ==
                 merged.history_first &&
             pending->history.size() + merged.history.size() <= MAX_HISTORY_FRAMES_IN_PACKET)
    {
      std::vector<Pads> joined = pending->history;
      joined.insert(joined.end(), merged.history.begin(), merged.history.end());
      merged.history = std::move(joined);
      merged.history_first = pending->history_first;
    }
  }
  merged.leaving = merged.leaving || pending->leaving;
  pending = std::move(merged);
}

void YouGameRoom::TestDropConnection()
{
  m_impl->test_drop = true;
}

void YouGameRoom::Send(const Packet& packet)
{
  std::lock_guard lock(m_impl->mutex);
  CoalescePacket(&m_impl->outgoing, packet);
}

std::vector<Packet> YouGameRoom::Receive()
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->inbox_from.clear();
  return std::exchange(m_impl->inbox, {});
}

bool YouGameRoom::Connected() const
{
  return GetState() == RoomState::Waiting;
}

void YouGameRoom::Leave()
{
  m_impl->stop = true;
  if (m_impl->thread.joinable())
    m_impl->thread.join();
  m_impl->End("You left the room", "");
}
}  // namespace Orca::Net
