// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/YouGameRoom.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <random>
#include <string_view>
#include <thread>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/HttpRequest.h"
#include "Common/Logging/Log.h"
#include "Common/WebSocket.h"
#include "Core/Orca/Profile.h"

namespace Orca::Net
{
namespace
{
// The rooms Worker's lobby protocol (rooms/src/index.ts, rooms/src/game-lobby.ts in the YouGame
// repo). Orca rooms are private two-player game lobbies; the host's Orca starts the match.
constexpr const char* CODE_ALPHABET = "abcdefghjkmnpqrstuvwxyz23456789";
// The room allows 120 messages a second per socket: packets go out at most every 10 ms (a stalled
// session asks to send far more often), the newest replacing any not yet sent.
constexpr auto SEND_GAP = std::chrono::milliseconds(10);
constexpr auto PING_EVERY = std::chrono::seconds(1);
constexpr auto HELLO_EVERY = std::chrono::seconds(1);
constexpr auto BEGIN_RETRY = std::chrono::seconds(3);
constexpr size_t MAX_INBOX = 1024;

using Clock = std::chrono::steady_clock;

u64 NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string Env(const char* name)
{
  const char* value = std::getenv(name);
  return value ? value : "";
}

bool ValidCode(const std::string& code)
{
  return code.size() >= 6 && code.size() <= 12 &&
         std::all_of(code.begin(), code.end(),
                     [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
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
// in milliseconds. Far beyond anything honest (the session clamps further), small enough that no
// arithmetic on them overflows.
constexpr int MAX_RESIMULATED = 1 << 20;
constexpr int MAX_ADVANTAGE = 1 << 20;
constexpr int MAX_RTT = 1 << 20;

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
  if (packet.checksum_frame)
  {
    d["x"] = picojson::value(static_cast<double>(*packet.checksum_frame));
    // A u64 does not survive a JSON number.
    u8 bytes[8];
    for (int i = 0; i < 8; ++i)
      bytes[i] = static_cast<u8>(packet.checksum >> (56 - 8 * i));
    d["h"] = picojson::value(ToHex(bytes, 8));
  }
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
  std::string code;
  int seat = -1;
  u32 seed = 0;
  int rtt = -1;
  // Counts pongs, so a caller can tell a new measurement from the same one read again.
  u32 rtt_sequence = 0;
  std::vector<Packet> inbox;
  std::optional<Packet> outgoing;

  // The I/O thread's own.
  Common::WebSocket ws;
  std::string me, peer, host, match_id;
  int revision = -1;
  int peer_seat = -1;
  bool peer_hello = false;
  std::optional<Clock::time_point> begun_at;
  int begin_tries = 0;
  Clock::time_point last_send{}, last_ping{}, last_hello{};

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

  Clock::time_point HoldUntil(const std::deque<Held>& queue)
  {
    // A deterministic 0..jitter extra per message; never earlier than the message before it.
    const u32 extra = net_jitter ? (++net_count * 2654435761u) % static_cast<u32>(net_jitter + 1) : 0;
    const auto due = Clock::now() + std::chrono::milliseconds(*net_delay + static_cast<int>(extra));
    return queue.empty() ? due : std::max(due, queue.back().due);
  }

  explicit Impl(RoomOptions room_options) : options(std::move(room_options))
  {
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

  void End(std::string why)
  {
    WARN_LOG_FMT(NETPLAY, "Orca room ended: {}", why);
    std::lock_guard lock(mutex);
    if (state == RoomState::Ended)
      return;
    state = RoomState::Ended;
    status = std::move(why);
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
        End(stop ? "You left the room" : "YouGame did not respond");
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
        o = o["result"].get<picojson::object>();
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
      if (room.empty())
      {
        // An invite the player opened in the app names the room.
        picojson::object hello;
        hello["type"] = picojson::value("hello");
        if (!post(bridge + "/sdk", hello, headers, &reply))
          return false;
        room = Str(reply, "room");
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
      request["players"] = picojson::value(2.0);
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
      std::string site = Env("ORCA_SITE");
      if (site.empty())
        site = "https://yougame.co";
      std::string player = Env("ORCA_TEST_PLAYER");
      if (player.empty())
        player = "orca-" + RandomString("abcdefghijklmnopqrstuvwxyz0123456789", 15);
      picojson::object request;
      request["dev"] = picojson::value(dev);
      request["player_id"] = picojson::value(player);
      request["name"] = picojson::value(options.player_name);
      request["players"] = picojson::value(2.0);
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
    std::lock_guard lock(mutex);
    code = room;
    return true;
  }

  void Run()
  {
    std::string url, ticket;
    if (!Ticket(&url, &ticket))
      return;
    if (stop)
    {
      End("You left the room");
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
      End(error.find("HTTP 4") != std::string::npos ?
              "The room refused to let you in (full, a different Orca build, or an old invite)" :
              "Could not reach YouGame's rooms");
      return;
    }
    SetStatus(fmt::format("Room {}: waiting for your friend", room));

    if (net_delay)
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: TEST network delay {} ms + 0..{} ms each way", *net_delay,
                   net_jitter);
    }
    while (!stop && State() != RoomState::Ended)
    {
      if (net_delay)
        FlushHeld();
      if (auto text = ws.ReceiveText(std::chrono::milliseconds(net_delay ? 1 : 2)))
      {
        if (net_delay)
          held_in.push_back({HoldUntil(held_in), std::move(*text)});
        else
          OnMessage(*text);
        continue;
      }
      if (!ws.IsOpen())
      {
        End(fmt::format("{} ({})",
                        State() == RoomState::Playing ? "Lost the connection to the room" :
                                                        "The room closed the connection",
                        ws.Error().empty() ? "no reason given" : ws.Error()));
        break;
      }
      Tick();
    }

    // Leaving mid-match reports it void, so neither player is left waiting for a result.
    if (!match_id.empty() && ws.IsOpen())
    {
      picojson::object finish;
      finish["t"] = picojson::value("finish");
      finish["matchId"] = picojson::value(match_id);
      finish["void"] = picojson::value(true);
      SendJson(finish, std::chrono::milliseconds(500), false);
    }
    if (ws.IsOpen())
    {
      picojson::object leave;
      leave["t"] = picojson::value("leave");
      SendJson(leave, std::chrono::milliseconds(500), false);
    }
    ws.Close();
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

    const RoomState current = State();
    if (current != RoomState::Playing)
    {
      // Say hello to the friend's Orca until it answers, so the host only starts the match once
      // both programs are up and agree on the build.
      if (!peer.empty() && now - last_hello >= HELLO_EVERY)
      {
        last_hello = now;
        picojson::object d;
        d["k"] = picojson::value("hello");
        d["c"] = picojson::value(options.compatibility);
        d["r"] = picojson::value(peer_hello);
        SendTo(peer, d);
      }
      if (begun_at && now - *begun_at >= BEGIN_RETRY)
        begun_at.reset();
      MaybeBegin();
      return;
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
    picojson::object d = PacketToJson(*packet);
    d["m"] = picojson::value(match_id);
    SendTo(peer, d);
  }

  void SendTo(const std::string& to, const picojson::object& d)
  {
    picojson::object message;
    message["t"] = picojson::value("msg");
    message["to"] = picojson::value(to);
    message["d"] = picojson::value(d);
    SendJson(message);
  }

  void MaybeBegin()
  {
    if (State() != RoomState::Waiting || host != me || peer.empty() || !peer_hello || begun_at)
      return;
    picojson::object begin;
    begin["t"] = picojson::value("begin");
    begin["revision"] = picojson::value(static_cast<double>(revision));
    // The room answers a begin it refuses with command-error and one it ignores with nothing: either
    // way, the next try waits BEGIN_RETRY, and after a minute of tries the room is not usable.
    if (++begin_tries > 20)
    {
      End("The room never started the match");
      return;
    }
    begun_at = Clock::now();
    SendJson(begin);
  }

  // The lobby roster rides on welcome, join, leave, back, away and lobby messages.
  void Roster(const picojson::object& m)
  {
    if (m.count("host"))
      host = Str(m, "host");
    const auto it = m.find("participants");
    if (it == m.end() || !it->second.is<picojson::array>())
      return;
    const auto& participants = it->second.get<picojson::array>();
    if (participants.size() > 2)
    {
      End("Orca rooms are for two players");
      return;
    }
    if (const auto rev = Int(m, "revision", 0, MAX_FRAME))
      revision = *rev;
    std::string other;
    for (const auto& p : participants)
    {
      if (!p.is<picojson::object>())
        continue;
      const std::string id = Str(p.get<picojson::object>(), "connectionId");
      if (id != me)
        other = id;
    }
    if (other != peer)
    {
      peer = other;
      peer_hello = false;
      begun_at.reset();
      begin_tries = 0;
      std::string room;
      {
        std::lock_guard lock(mutex);
        room = code;
      }
      SetStatus(peer.empty() ? fmt::format("Room {}: waiting for your friend", room) :
                               fmt::format("Room {}: your friend is here, starting", room));
    }
  }

  // Mid-match, a player who leaves or drops is out of it (rooms/src/index.ts onGone).
  bool PeerGone(const picojson::object& m) const
  {
    const auto it = m.find("player");
    return it != m.end() && it->second.is<picojson::object>() &&
           Str(it->second.get<picojson::object>(), "id") == peer;
  }

  void Start(const picojson::object& m)
  {
    const auto it = m.find("matchParticipants");
    const auto seed_value = m.find("seed");
    if (it == m.end() || !it->second.is<picojson::array>() || seed_value == m.end() ||
        !seed_value->second.is<double>() || seed_value->second.get<double>() < 0 ||
        seed_value->second.get<double>() > 4294967295.0)
    {
      End("The room started a match it didn't describe");
      return;
    }
    int my_seat = -1;
    peer_seat = -1;
    for (const auto& p : it->second.get<picojson::array>())
    {
      if (!p.is<picojson::object>())
        continue;
      const auto& o = p.get<picojson::object>();
      const auto slot = Int(o, "slot", 0, 1);
      if (!slot)
        continue;
      const std::string id = Str(o, "connectionId");
      if (id == me)
        my_seat = *slot;
      else if (id == peer)
        peer_seat = *slot;
    }
    if (my_seat < 0 || peer_seat < 0 || my_seat == peer_seat)
    {
      End("The room started a match without both of you");
      return;
    }
    match_id = Str(m, "matchId");
    std::lock_guard lock(mutex);
    seat = my_seat;
    seed = static_cast<u32>(seed_value->second.get<double>());
    state = RoomState::Playing;
    status = fmt::format("Room {}: playing (port {})", code, my_seat + 1);
    INFO_LOG_FMT(NETPLAY, "Orca room: match {} started, seat {}, seed {:08x}", match_id, my_seat,
                 seed);
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
    const RoomState current = State();

    if (t == "welcome")
    {
      me = Str(m, "me");
      if (Str(m, "lifecycle") != "game")
      {
        End("This room is not an Orca lobby");
        return;
      }
      if (m.count("playing") && m.at("playing").evaluate_as_boolean())
      {
        End("This room already has a match running");
        return;
      }
      Roster(m);
      std::lock_guard lock(mutex);
      if (state == RoomState::Connecting)
        state = RoomState::Waiting;
    }
    else if (t == "join" || t == "back" || t == "leave" || t == "away" || t == "lobby")
    {
      if (current == RoomState::Playing)
      {
        if ((t == "leave" || t == "away") && PeerGone(m))
          End("Your friend left the match");
        return;
      }
      Roster(m);
    }
    else if (t == "start")
    {
      if (current == RoomState::Waiting)
        Start(m);
    }
    else if (t == "result")
    {
      if (current == RoomState::Playing)
      {
        match_id.clear();
        End(m.count("void") && m.at("void").evaluate_as_boolean() ? "The match was cancelled" :
                                                                    "The match is over");
      }
    }
    else if (t == "msg")
    {
      if (peer.empty() || Str(m, "from") != peer)
        return;
      const auto d = m.find("d");
      if (d == m.end() || !d->second.is<picojson::object>())
        return;
      OnPeerMessage(d->second.get<picojson::object>(), current);
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
        }
      }
    }
    else if (t == "command-error")
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: {} refused: {}", Str(m, "action"), Str(m, "error"));
      if (Str(m, "action") == "begin")
      {
        std::lock_guard lock(mutex);
        status = fmt::format("Room {}: {}", code, Str(m, "error"));
      }
    }
    else if (t == "error")
    {
      End(Str(m, "error").empty() ? "The room reported an error" : Str(m, "error"));
    }
    else if (t == "kicked")
    {
      End("You were removed from the room");
    }
  }

  void OnPeerMessage(const picojson::object& d, RoomState current)
  {
    const std::string kind = Str(d, "k");
    if (kind == "hello")
    {
      if (Str(d, "c") != options.compatibility)
      {
        End("Your friend is running a different Orca build or setup");
        return;
      }
      if (!peer_hello)
      {
        peer_hello = true;
        // Answer at once rather than on the next hello tick.
        last_hello = {};
      }
      return;
    }
    if (kind != "p" || current != RoomState::Playing || Str(d, "m") != match_id)
      return;
    Packet packet;
    if (!PacketFromJson(d, &packet) || packet.seat != peer_seat)
    {
      End("Your friend's Orca sent a malformed message");
      return;
    }
    std::lock_guard lock(mutex);
    // Nobody read the inbox for a while (a long stall on this side). Each packet repeats every
    // input the sender hasn't seen acknowledged, so the newer half says all the older half did.
    if (inbox.size() >= MAX_INBOX)
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: inbox full, dropping the {} oldest packets", MAX_INBOX / 2);
      inbox.erase(inbox.begin(), inbox.begin() + MAX_INBOX / 2);
    }
    inbox.push_back(std::move(packet));
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

u32 YouGameRoom::Seed() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->seed;
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

int YouGameRoom::RoundTripMs(u32* sequence) const
{
  std::lock_guard lock(m_impl->mutex);
  if (sequence)
    *sequence = m_impl->rtt_sequence;
  return m_impl->rtt;
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
  pending = std::move(merged);
}

void YouGameRoom::Send(const Packet& packet)
{
  std::lock_guard lock(m_impl->mutex);
  CoalescePacket(&m_impl->outgoing, packet);
}

std::vector<Packet> YouGameRoom::Receive()
{
  std::lock_guard lock(m_impl->mutex);
  return std::exchange(m_impl->inbox, {});
}

bool YouGameRoom::Connected() const
{
  return GetState() == RoomState::Playing;
}

void YouGameRoom::Leave()
{
  m_impl->stop = true;
  if (m_impl->thread.joinable())
    m_impl->thread.join();
  m_impl->End("You left the room");
}
}  // namespace Orca::Net
