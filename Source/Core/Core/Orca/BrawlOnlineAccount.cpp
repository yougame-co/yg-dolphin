// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/BrawlOnlineAccount.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include <fmt/format.h>
#include <picojson.h>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#ifdef ORCA_BO_LIVE
#include "Common/HttpRequest.h"
#endif
#include "Common/Logging/Log.h"
#include "Core/Orca/SafeJson.h"

namespace Orca::BrawlOnline
{
namespace
{
constexpr std::string_view REDACTED = "[redacted]";
#ifdef ORCA_BO_LIVE
constexpr auto HTTP_TIMEOUT = std::chrono::seconds(15);
#endif

// ---- UTF-8 ----

// The next code point of `text` at `*pos`, advancing it; nothing for a malformed sequence (then
// `*pos` moves past one byte).
std::optional<char32_t> NextCodePoint(std::string_view text, std::size_t* pos)
{
  const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
  const unsigned char lead = byte(*pos);
  std::size_t length;
  char32_t point;
  if (lead < 0x80)
  {
    ++*pos;
    return lead;
  }
  if ((lead & 0xE0) == 0xC0)
  {
    length = 2;
    point = lead & 0x1F;
  }
  else if ((lead & 0xF0) == 0xE0)
  {
    length = 3;
    point = lead & 0x0F;
  }
  else if ((lead & 0xF8) == 0xF0)
  {
    length = 4;
    point = lead & 0x07;
  }
  else
  {
    ++*pos;
    return std::nullopt;
  }
  if (*pos + length > text.size())
  {
    ++*pos;
    return std::nullopt;
  }
  for (std::size_t i = 1; i < length; ++i)
  {
    if ((byte(*pos + i) & 0xC0) != 0x80)
    {
      ++*pos;
      return std::nullopt;
    }
    point = (point << 6) | (byte(*pos + i) & 0x3F);
  }
  static constexpr std::array<char32_t, 5> MIN{0, 0, 0x80, 0x800, 0x10000};
  if (point < MIN[length] || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF))
  {
    ++*pos;
    return std::nullopt;
  }
  *pos += length;
  return point;
}

// The code points of `text`; nothing if it isn't valid UTF-8.
std::optional<std::u32string> Decode(std::string_view text)
{
  std::u32string points;
  for (std::size_t pos = 0; pos < text.size();)
  {
    const auto point = NextCodePoint(text, &pos);
    if (!point)
      return std::nullopt;
    points.push_back(*point);
  }
  return points;
}

void AppendUtf8(std::string& out, char32_t point)
{
  if (point < 0x80)
  {
    out.push_back(static_cast<char>(point));
  }
  else if (point < 0x800)
  {
    out.push_back(static_cast<char>(0xC0 | (point >> 6)));
    out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
  }
  else if (point < 0x10000)
  {
    out.push_back(static_cast<char>(0xE0 | (point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
  }
  else
  {
    out.push_back(static_cast<char>(0xF0 | (point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
  }
}

// Brawl Online's is_name_char: printable ASCII without \ and `, Hiragana, Katakana, ・ and ー.
bool IsNameChar(char32_t c)
{
  const bool allowed = (c >= U' ' && c <= U'~') || (c >= 0x3041 && c <= 0x3093) ||
                       (c >= 0x30A1 && c <= 0x30F4) || c == 0x30FB || c == 0x30FC;
  return allowed && c != U'\\' && c != U'`';
}

bool IsAsciiLetter(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

char Upper(char c)
{
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// ---- JSON ----

// Appends `text` as a JSON string. `out` must already have room (see BodyCapacity), so the
// password is never copied into a second buffer by a reallocation.
void AppendJsonString(std::string& out, std::string_view text)
{
  static constexpr char HEX[] = "0123456789abcdef";
  out.push_back('"');
  for (const char ch : text)
  {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\')
    {
      out.push_back('\\');
      out.push_back(ch);
    }
    else if (c < 0x20)
    {
      out.append("\\u00");
      out.push_back(HEX[c >> 4]);
      out.push_back(HEX[c & 0xF]);
    }
    else
    {
      out.push_back(ch);
    }
  }
  out.push_back('"');
}

// Room for a body of fixed text plus these fields, each escaped at worst 6 bytes per byte.
std::size_t BodyCapacity(std::initializer_list<std::string_view> fields)
{
  std::size_t capacity = 128;
  for (const auto field : fields)
    capacity += 6 * field.size() + 32;
  return capacity;
}

std::string StringField(const picojson::object& o, const char* key)
{
  const auto it = o.find(key);
  return it != o.end() && it->second.is<std::string>() ? it->second.get<std::string>() : "";
}

std::optional<bool> BoolField(const picojson::object& o, const char* key)
{
  const auto it = o.find(key);
  if (it == o.end() || !it->second.is<bool>())
    return std::nullopt;
  return it->second.get<bool>();
}

std::optional<Me> ParseMe(const picojson::value& value)
{
  if (!value.is<picojson::object>())
    return std::nullopt;
  const auto& o = value.get<picojson::object>();
  Me me;
  me.uid = StringField(o, "uid");
  me.display_name = StringField(o, "displayName");
  me.connect_code = StringField(o, "connectCode");  // null until assigned
  me.email_verified = BoolField(o, "emailVerified").value_or(false);
  me.email_verification_required = BoolField(o, "emailVerificationRequired").value_or(true);
  if (me.uid.empty())
    return std::nullopt;
  return me;
}

std::optional<UserJson> ParseUserJson(const picojson::value& value)
{
  if (!value.is<picojson::object>())
    return std::nullopt;
  const auto& o = value.get<picojson::object>();
  UserJson u;
  u.uid = StringField(o, "uid");
  u.play_key = StringField(o, "playKey");
  u.connect_code = StringField(o, "connectCode");
  u.display_name = StringField(o, "displayName");
  u.latest_version = StringField(o, "latestVersion");
  if (u.uid.empty() || u.play_key.empty() || u.connect_code.empty())
    return std::nullopt;
  return u;
}

Error ErrorForCode(std::string_view code, int status)
{
  static constexpr std::array<std::pair<std::string_view, Error>, 13> CODES{{
      {"bad_credentials", Error::BadCredentials},
      {"unauthorized", Error::SignedOut},
      {"banned", Error::Banned},
      {"email_not_verified", Error::NeedsVerification},
      {"not_activated", Error::NotActivated},
      {"email_taken", Error::EmailTaken},
      {"invalid_email", Error::InvalidEmail},
      {"invalid_password", Error::InvalidPassword},
      {"invalid_display_name", Error::InvalidDisplayName},
      {"invalid_code", Error::InvalidCode},
      {"prefix_exhausted", Error::CodeTaken},
      {"code_already_set", Error::CodeAlreadySet},
      {"rate_limited", Error::RateLimited},
  }};
  if (status == 429)
    return Error::RateLimited;
  for (const auto& [name, error] : CODES)
  {
    if (code == name)
      return error;
  }
  if (status == 401)
    return Error::SignedOut;
  return Error::Server;
}

// "Too many attempts. Try again in 42 s." -> 42, for an answer that lost its Retry-After header.
int SecondsInMessage(std::string_view message)
{
  const std::size_t at = message.find(" in ");
  if (at == std::string_view::npos)
    return 0;
  int seconds = 0;
  const char* begin = message.data() + at + 4;
  const auto [end, ec] = std::from_chars(begin, message.data() + message.size(), seconds);
  return ec == std::errc() && end != begin && seconds > 0 ? seconds : 0;
}

// Reads one answer: `*body` gets the JSON object of a 2xx answer listed in `ok`.
Outcome Read(std::string_view what, const HttpResult& reply, std::initializer_list<int> ok,
             picojson::value* body)
{
  Outcome outcome;
  outcome.status = reply.status;
  if (reply.status == 0)
  {
    outcome.error = Error::Network;
    WARN_LOG_FMT(NETPLAY, "Brawl Online: {}: no answer", what);
    return outcome;
  }
  picojson::value json;
  const bool parsed =
      Json::ParseWholeJson(reply.body, &json).empty() && json.is<picojson::object>();
  if (std::find(ok.begin(), ok.end(), reply.status) != ok.end())
  {
    if (!parsed)
    {
      outcome.error = Error::BadReply;
      WARN_LOG_FMT(NETPLAY, "Brawl Online: {}: HTTP {} with an unreadable body", what,
                   reply.status);
      return outcome;
    }
    *body = std::move(json);
    return outcome;
  }
  if (parsed)
  {
    const auto& o = json.get<picojson::object>();
    if (const auto e = o.find("error"); e != o.end() && e->second.is<picojson::object>())
    {
      outcome.code = StringField(e->second.get<picojson::object>(), "code");
      outcome.message = Redact(StringField(e->second.get<picojson::object>(), "message"));
    }
  }
  outcome.error = ErrorForCode(outcome.code, reply.status);
  if (outcome.error == Error::RateLimited)
  {
    outcome.retry_after_s =
        reply.retry_after_s > 0 ? reply.retry_after_s : SecondsInMessage(outcome.message);
  }
  // Status and code only: never a body, which can carry a token, nor the message, which can quote
  // the email address. The message goes to the screen alone.
  WARN_LOG_FMT(NETPLAY, "{}", FailureLogLine(what, outcome));
  return outcome;
}

SessionOutcome ReadSession(std::string_view what, const HttpResult& reply, int ok_status,
                           std::string* token)
{
  SessionOutcome result;
  picojson::value body;
  static_cast<Outcome&>(result) = Read(what, reply, {ok_status}, &body);
  if (!result.Ok())
    return result;
  const auto& o = body.get<picojson::object>();
  const auto user = o.find("user");
  std::optional<Me> me = user != o.end() ? ParseMe(user->second) : std::nullopt;
  std::string session = StringField(o, "sessionToken");
  if (session.empty() || !me)
  {
    result.error = Error::BadReply;
    WARN_LOG_FMT(NETPLAY, "Brawl Online: {}: no session in the answer", what);
    return result;
  }
  result.user = std::move(*me);
  *token = std::move(session);
  return result;
}

Outcome Refused(Error error)
{
  Outcome outcome;
  outcome.error = error;
  return outcome;
}

#ifndef _WIN32
// Writes `text` to a new file at `path` that is owner read/write only from the moment it exists.
// O_EXCL also refuses a link planted at `path`; a stale file left by a crash is removed first.
// False when any step fails, the permission step included.
bool WritePrivateFile(const std::string& path, std::string_view text)
{
  constexpr int FLAGS = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
  constexpr mode_t MODE = S_IRUSR | S_IWUSR;
  int fd = open(path.c_str(), FLAGS, MODE);
  if (fd < 0 && errno == EEXIST)
  {
    if (unlink(path.c_str()) != 0)
      return false;
    fd = open(path.c_str(), FLAGS, MODE);
  }
  if (fd < 0)
    return false;
  // The umask can only take bits away from MODE; this states it exactly.
  bool ok = fchmod(fd, MODE) == 0;
  for (std::size_t done = 0; ok && done < text.size();)
  {
    const ssize_t n = write(fd, text.data() + done, text.size() - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      ok = false;
    else
      done += static_cast<std::size_t>(n);
  }
  if (close(fd) != 0)
    ok = false;
  return ok;
}
#endif

// Writes `text` to `path` through a temporary file beside it, so a crash leaves the old file or
// the new one, never half of one. Both files hold keys for the player's account. On POSIX the
// temporary file is created owner read/write only (WritePrivateFile) and the rename keeps that. On
// Windows the file inherits the ACL of the folder it is in, which is in the player's own profile
// (the app runs Orca with a user folder under its per-user data folder).
bool WriteAtomically(const std::string& path, std::string_view text)
{
  if (!File::CreateFullPath(path))
    return false;
  const std::string temp = path + ".tmp";
#ifdef _WIN32
  const bool written = File::WriteStringToFile(temp, text);
#else
  const bool written = WritePrivateFile(temp, text);
#endif
  if (written && File::Rename(temp, path))
    return true;
  File::Delete(temp, File::IfAbsentBehavior::NoConsoleWarning);
  return false;
}

std::uint64_t Fnv1a(std::string_view text)
{
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const char c : text)
  {
    hash ^= static_cast<unsigned char>(c);
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// The segment characters SanitizePlayerId keeps.
bool IsSegmentChar(char c)
{
  return IsAsciiLetter(c) || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// A name Windows reserves for a device in every folder (CON, PRN, AUX, NUL, COM0-9, LPT0-9), in any
// case: a folder by that name can't be made or opened as one.
bool IsWindowsDeviceName(std::string_view name)
{
  std::string upper;
  for (const char c : name)
    upper.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
  if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL")
    return true;
  return upper.size() == 4 && (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0) &&
         upper[3] >= '0' && upper[3] <= '9';
}

bool StartsWithDev(std::string_view id)
{
  if (id.size() < 4)
    return false;
  for (std::size_t i = 0; i < 3; ++i)
  {
    if (Upper(id[i]) != "DEV"[i])
      return false;
  }
  return id[3] == ':';
}

// ---- The production transport ----

#ifdef ORCA_BO_LIVE
class HttpTransport final : public Transport
{
public:
  HttpResult Post(const std::string& path, const std::string& json_body,
                  const std::string& token) override
  {
    return Fetch(true, path, json_body, token);
  }
  HttpResult Get(const std::string& path, const std::string& token) override
  {
    return Fetch(false, path, {}, token);
  }

private:
  static HttpResult Fetch(bool post, const std::string& path, const std::string& body,
                          const std::string& token)
  {
    // Peer verification stays on: HttpRequest sets up curl's TLS (Common/CurlTLS.h) and never
    // turns CURLOPT_SSL_VERIFYPEER or VERIFYHOST off.
    Common::HttpRequest http(HTTP_TIMEOUT);
    Common::HttpRequest::Headers headers{{"Content-Type", "application/json"},
                                         {"Accept", "application/json"},
                                         {"User-Agent", "Orca"}};
    if (!token.empty())
      headers["Authorization"] = "Bearer " + token;
    const std::string url = std::string(ACCOUNTS_BASE_URL) + path;
    // All return codes: the error bodies are read here, and HttpRequest's Ok_Only would log them.
    const Common::HttpRequest::Response response =
        post ? http.Post(url, std::string_view(body), headers,
                         Common::HttpRequest::AllowedReturnCodes::All) :
               http.Get(url, headers, Common::HttpRequest::AllowedReturnCodes::All);
    if (auto it = headers.find("Authorization"); it != headers.end() && it->second)
      Wipe(*it->second);
    HttpResult result;
    if (!response)
      return result;
    result.status = http.GetLastResponseCode();
    result.body.assign(response->begin(), response->end());
    std::string retry = http.GetHeaderValue("retry-after");
    if (retry.empty())
      retry = http.GetHeaderValue("Retry-After");
    int seconds = 0;
    if (const auto [end, ec] = std::from_chars(retry.data(), retry.data() + retry.size(), seconds);
        ec == std::errc() && seconds > 0)
    {
      result.retry_after_s = seconds;
    }
    return result;
  }
};
}  // namespace

std::unique_ptr<Transport> MakeHttpTransport()
{
  INFO_LOG_FMT(NETPLAY, "Brawl Online: {}", LIVE_BUILD_MARKER);
  return std::make_unique<HttpTransport>();
}

std::unique_ptr<Transport> MakeTransport()
{
  return MakeHttpTransport();
}
#else
// ---- The stand-in (builds without ORCA_BO_LIVE) ----

// Answers the five calls the client makes the way the accounts API does, from memory. Nothing it
// is given leaves the process, and it never reads the email or the password.
class TestTransport final : public Transport
{
public:
  HttpResult Post(const std::string& path, const std::string& json_body,
                  const std::string& token) override
  {
    if (path == "/v1/auth/login")
      return {200, SessionAnswer(true)};
    if (path == "/v1/auth/signup")
    {
      if (std::string name = Field(json_body, "displayName"); !name.empty())
        m_display_name = std::move(name);
      // A new account asks for its email once; Continue then finds it verified.
      return {201, SessionAnswer(false)};
    }
    if (path == "/v1/me/netplay" && token == TOKEN)
    {
      if (!m_connect_code.empty())
        return {409, R"({"error":{"code":"code_already_set","message":"Already set."}})"};
      m_connect_code = Field(json_body, "codeStart") + "#000";
      return {200, "{}"};
    }
    return Unknown(token);
  }

  HttpResult Get(const std::string& path, const std::string& token) override
  {
    if (token != TOKEN)
      return Unknown(token);
    if (path == "/v1/me")
      return {200, MeAnswer(true)};
    if (path == "/v1/me/user-json")
    {
      if (m_connect_code.empty())
        return {409, R"({"error":{"code":"not_activated","message":"Not activated."}})"};
      std::string body = R"({"uid":")" + std::string(UID) + R"(","playKey":")" +
                         std::string(PLAY_KEY) + R"(","connectCode":)";
      body.reserve(body.size() + 6 * (m_connect_code.size() + m_display_name.size()) + 64);
      AppendJsonString(body, m_connect_code);
      body.append(",\"displayName\":");
      AppendJsonString(body, m_display_name);
      body.append(R"(,"latestVersion":"test-build"})");
      return {200, std::move(body)};
    }
    return {404, R"({"error":{"code":"not_found","message":"Not found."}})"};
  }

private:
  static constexpr std::string_view TOKEN = "test-build-session";
  static constexpr std::string_view UID = "00000000-0000-4000-8000-000000000000";
  static constexpr std::string_view PLAY_KEY = "test-build-not-a-play-key";

  static HttpResult Unknown(const std::string& token)
  {
    if (token.empty() || token != TOKEN)
      return {401, R"({"error":{"code":"unauthorized","message":"Signed out."}})"};
    return {404, R"({"error":{"code":"not_found","message":"Not found."}})"};
  }

  // A string field of a request body the client wrote; empty when absent.
  static std::string Field(const std::string& json_body, const char* key)
  {
    picojson::value json;
    if (!Json::ParseWholeJson(json_body, &json).empty() || !json.is<picojson::object>())
      return {};
    return StringField(json.get<picojson::object>(), key);
  }

  std::string MeAnswer(bool verified) const
  {
    std::string body = R"({"uid":")" + std::string(UID) + R"(","emailVerified":)" +
                       (verified ? "true" : "false") +
                       R"(,"emailVerificationRequired":true,"displayName":)";
    body.reserve(body.size() + 6 * (m_display_name.size() + m_connect_code.size()) + 64);
    AppendJsonString(body, m_display_name);
    body.append(",\"connectCode\":");
    if (m_connect_code.empty())
      body.append("null");
    else
      AppendJsonString(body, m_connect_code);
    body.append("}");
    return body;
  }

  std::string SessionAnswer(bool verified) const
  {
    return R"({"sessionToken":")" + std::string(TOKEN) + R"(","user":)" + MeAnswer(verified) + "}";
  }

  std::string m_display_name = "Player";
  std::string m_connect_code;
};
}  // namespace

std::unique_ptr<Transport> MakeTestTransport()
{
  return std::make_unique<TestTransport>();
}

std::unique_ptr<Transport> MakeTransport()
{
  return MakeTestTransport();
}
#endif

// ---- Errors ----

std::string_view ErrorName(Error error)
{
  switch (error)
  {
  case Error::None:
    return "none";
  case Error::Network:
    return "network";
  case Error::BadCredentials:
    return "bad_credentials";
  case Error::SignedOut:
    return "signed_out";
  case Error::Banned:
    return "banned";
  case Error::NeedsVerification:
    return "needs_verification";
  case Error::NotActivated:
    return "not_activated";
  case Error::EmailTaken:
    return "email_taken";
  case Error::InvalidEmail:
    return "invalid_email";
  case Error::InvalidPassword:
    return "invalid_password";
  case Error::InvalidDisplayName:
    return "invalid_display_name";
  case Error::InvalidCode:
    return "invalid_code";
  case Error::CodeTaken:
    return "code_taken";
  case Error::CodeAlreadySet:
    return "code_already_set";
  case Error::RateLimited:
    return "rate_limited";
  case Error::Server:
    return "server";
  case Error::BadReply:
    return "bad_reply";
  case Error::NoSession:
    return "no_session";
  case Error::SaveFailed:
    return "save_failed";
  }
  return "unknown";
}

std::string UserMessage(Error error, int retry_after_s)
{
  switch (error)
  {
  case Error::None:
  case Error::CodeAlreadySet:
    return "";
  case Error::Network:
    return "Couldn't reach Brawl Online. Check your connection and try again.";
  case Error::BadCredentials:
    return "Wrong email or password.";
  case Error::SignedOut:
    return "You were signed out of Brawl Online. Sign in again.";
  case Error::Banned:
    return "This Brawl Online account is banned.";
  case Error::NeedsVerification:
  case Error::NotActivated:
    return "Check your email for the link from Brawl Online, then press Continue.";
  case Error::EmailTaken:
    return "That email already has a Brawl Online account. Sign in instead.";
  case Error::InvalidEmail:
    return "That email address doesn't look right.";
  case Error::InvalidPassword:
    return fmt::format("Passwords are {} to {} characters.", PASSWORD_MIN, PASSWORD_MAX);
  case Error::InvalidDisplayName:
    return fmt::format("Names are 1 to {} letters, numbers or symbols.", DISPLAY_NAME_MAX);
  case Error::InvalidCode:
    return "Connect codes start with 2 to 4 letters (A to Z).";
  case Error::CodeTaken:
    return "Every connect code with those letters is taken. Try other letters.";
  case Error::RateLimited:
    if (retry_after_s > 0 && retry_after_s <= 90)
      return fmt::format("Too many tries. Wait {} seconds and try again.", retry_after_s);
    return "Too many tries. Wait a minute and try again.";
  case Error::Server:
    return "Brawl Online had a problem. Try again in a moment.";
  case Error::BadReply:
    return "Brawl Online sent an answer Orca couldn't read. Try again later.";
  case Error::NoSession:
    return "Sign in to Brawl Online first.";
  case Error::SaveFailed:
    return "Couldn't save your Brawl Online account on this computer.";
  }
  return "Something went wrong. Try again.";
}

std::string FailureLogLine(std::string_view what, const Outcome& outcome)
{
  const std::string_view code = outcome.code;
  const bool plain =
      !code.empty() && code.size() <= 40 && std::all_of(code.begin(), code.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
      });
  return fmt::format("Brawl Online: {}: HTTP {} {}", what, outcome.status,
                     code.empty() ? "-" : plain ? code : "?");
}

// ---- UserJson ----

std::string UserJson::Serialize() const
{
  std::string out;
  out.reserve(BodyCapacity({uid, play_key, connect_code, display_name, latest_version}));
  const auto field = [&out](std::string_view key, std::string_view value, bool last) {
    out.append("  ");
    AppendJsonString(out, key);
    out.append(": ");
    AppendJsonString(out, value);
    out.append(last ? "\n" : ",\n");
  };
  out.append("{\n");
  field("uid", uid, false);
  field("playKey", play_key, false);
  field("connectCode", connect_code, false);
  field("displayName", display_name, false);
  field("latestVersion", latest_version, true);
  out.append("}");
  return out;
}

// ---- Client ----

Client::Client(std::unique_ptr<Transport> transport) : m_transport(std::move(transport))
{
}

void Client::ForgetSession()
{
  Wipe(m_token);
}

SessionOutcome Client::SignIn(std::string_view email, std::string_view password)
{
  SessionOutcome result;
  if (email.empty())
  {
    static_cast<Outcome&>(result) = Refused(Error::InvalidEmail);
    return result;
  }
  if (password.empty())
  {
    static_cast<Outcome&>(result) = Refused(Error::BadCredentials);
    return result;
  }
  std::string body;
  body.reserve(BodyCapacity({email, password}));
  body.append("{\"email\":");
  AppendJsonString(body, email);
  body.append(",\"password\":");
  AppendJsonString(body, password);
  body.append("}");
  const HttpResult reply = m_transport->Post("/v1/auth/login", body, {});
  Wipe(body);
  std::string token;
  result = ReadSession("sign in", reply, 200, &token);
  if (result.Ok())
    m_token = std::move(token);
  return result;
}

SessionOutcome Client::SignUp(std::string_view email, std::string_view password,
                              std::string_view display_name)
{
  SessionOutcome result;
  Error refused = Error::None;
  if (email.empty() || email.find('@') == std::string_view::npos)
    refused = Error::InvalidEmail;
  else if (!ValidPassword(password))
    refused = Error::InvalidPassword;
  else if (!ValidDisplayName(display_name))
    refused = Error::InvalidDisplayName;
  if (refused != Error::None)
  {
    static_cast<Outcome&>(result) = Refused(refused);
    return result;
  }
  std::string body;
  body.reserve(BodyCapacity({email, password, display_name}));
  body.append("{\"email\":");
  AppendJsonString(body, email);
  body.append(",\"password\":");
  AppendJsonString(body, password);
  body.append(",\"displayName\":");
  AppendJsonString(body, display_name);
  body.append("}");
  const HttpResult reply = m_transport->Post("/v1/auth/signup", body, {});
  Wipe(body);
  std::string token;
  result = ReadSession("make an account", reply, 201, &token);
  if (result.Ok())
    m_token = std::move(token);
  return result;
}

MeOutcome Client::RefreshMe()
{
  MeOutcome result;
  if (!HasSession())
  {
    static_cast<Outcome&>(result) = Refused(Error::NoSession);
    return result;
  }
  picojson::value body;
  static_cast<Outcome&>(result) =
      Read("account", m_transport->Get("/v1/me", m_token), {200}, &body);
  if (result.error == Error::SignedOut)
    ForgetSession();
  if (!result.Ok())
    return result;
  if (auto me = ParseMe(body))
    result.user = std::move(*me);
  else
    result.error = Error::BadReply;
  return result;
}

Outcome Client::EnsureConnectCode(std::string_view code_start)
{
  if (!HasSession())
    return Refused(Error::NoSession);
  if (!ValidCodeStart(code_start))
    return Refused(Error::InvalidCode);
  std::string upper(code_start);
  std::transform(upper.begin(), upper.end(), upper.begin(), Upper);
  std::string body = "{\"codeStart\":";
  AppendJsonString(body, upper);
  body.append("}");
  picojson::value answer;
  Outcome outcome = Read("connect code", m_transport->Post("/v1/me/netplay", body, m_token),
                         {200}, &answer);
  if (outcome.error == Error::SignedOut)
    ForgetSession();
  if (outcome.error == Error::CodeAlreadySet)
    outcome = Outcome{};
  return outcome;
}

UserJsonOutcome Client::FetchUserJson()
{
  UserJsonOutcome result;
  if (!HasSession())
  {
    static_cast<Outcome&>(result) = Refused(Error::NoSession);
    return result;
  }
  picojson::value body;
  static_cast<Outcome&>(result) =
      Read("user.json", m_transport->Get("/v1/me/user-json", m_token), {200}, &body);
  if (result.error == Error::SignedOut)
    ForgetSession();
  if (!result.Ok())
    return result;
  if (auto user_json = ParseUserJson(body))
    result.user_json = std::move(*user_json);
  else
    result.error = Error::BadReply;
  return result;
}

// ---- Redaction ----

std::string Redact(std::string_view text, std::initializer_list<std::string_view> secrets)
{
  std::string out(text);
  for (const auto secret : secrets)
  {
    if (secret.empty())
      continue;
    for (std::size_t at = out.find(secret); at != std::string::npos;
         at = out.find(secret, at + REDACTED.size()))
    {
      out.replace(at, secret.size(), REDACTED);
    }
  }
  // "key": "value" for the secret fields.
  for (const std::string_view key : {"\"sessionToken\"", "\"playKey\"", "\"password\""})
  {
    for (std::size_t at = out.find(key); at != std::string::npos; at = out.find(key, at + 1))
    {
      std::size_t pos = at + key.size();
      while (pos < out.size() && (out[pos] == ' ' || out[pos] == '\t' || out[pos] == '\n'))
        ++pos;
      if (pos >= out.size() || out[pos] != ':')
        continue;
      ++pos;
      while (pos < out.size() && (out[pos] == ' ' || out[pos] == '\t' || out[pos] == '\n'))
        ++pos;
      if (pos >= out.size() || out[pos] != '"')
        continue;
      const std::size_t start = pos + 1;
      std::size_t end = start;
      while (end < out.size() && out[end] != '"')
        end += out[end] == '\\' ? 2 : 1;
      end = std::min(end, out.size());
      if (out.compare(start, end - start, REDACTED) != 0)
        out.replace(start, end - start, REDACTED);
    }
  }
  // "Bearer <token>", any case.
  static constexpr std::string_view BEARER = "bearer ";
  for (std::size_t at = 0; at + BEARER.size() <= out.size(); ++at)
  {
    bool match = true;
    for (std::size_t i = 0; i < BEARER.size() && match; ++i)
      match = (out[at + i] | 0x20) == BEARER[i];
    if (!match)
      continue;
    const std::size_t start = at + BEARER.size();
    std::size_t end = start;
    while (end < out.size() && out[end] != ' ' && out[end] != '"' && out[end] != ',' &&
           out[end] != '\n' && out[end] != '\r' && out[end] != '}')
    {
      ++end;
    }
    if (end > start && out.compare(start, end - start, REDACTED) != 0)
      out.replace(start, end - start, REDACTED);
    at = start;
  }
  return out;
}

void Wipe(std::string& text)
{
  volatile char* bytes = text.data();
  for (std::size_t i = 0; i < text.size(); ++i)
    bytes[i] = 0;
  text.clear();
}

// ---- Defaults and rules ----

std::string DefaultCodeStart(std::string_view yougame_name)
{
  std::string code;
  for (const char c : yougame_name)
  {
    if (code.size() == CODE_START_MAX)
      break;
    if (IsAsciiLetter(c))
      code.push_back(Upper(c));
  }
  return code.size() >= CODE_START_MIN ? code : "YG";
}

std::string DefaultDisplayName(std::string_view yougame_name)
{
  std::u32string kept;
  for (std::size_t pos = 0; pos < yougame_name.size();)
  {
    const auto point = NextCodePoint(yougame_name, &pos);
    if (point && IsNameChar(*point) && !(kept.empty() && *point == U' '))
      kept.push_back(*point);
  }
  if (kept.size() > DISPLAY_NAME_MAX)
    kept.resize(DISPLAY_NAME_MAX);
  while (!kept.empty() && kept.back() == U' ')
    kept.pop_back();
  if (kept.empty())
    return "Player";
  std::string out;
  for (const char32_t point : kept)
    AppendUtf8(out, point);
  return out;
}

bool ValidCodeStart(std::string_view code_start)
{
  return code_start.size() >= CODE_START_MIN && code_start.size() <= CODE_START_MAX &&
         std::all_of(code_start.begin(), code_start.end(), IsAsciiLetter);
}

bool ValidDisplayName(std::string_view name)
{
  const auto points = Decode(name);
  if (!points || points->empty() || points->size() > DISPLAY_NAME_MAX)
    return false;
  if (std::all_of(points->begin(), points->end(), [](char32_t c) { return c == U' '; }))
    return false;
  return std::all_of(points->begin(), points->end(), IsNameChar);
}

bool ValidPassword(std::string_view password)
{
  auto points = Decode(password);
  const bool ok = points && points->size() >= PASSWORD_MIN && points->size() <= PASSWORD_MAX;
  if (points)
    std::fill(points->begin(), points->end(), U'\0');
  return ok;
}

// ---- Store ----

std::string SanitizePlayerId(std::string_view player_id)
{
  static constexpr std::size_t MAX_KEPT = 64;
  if (player_id.empty() || StartsWithDev(player_id))
    return "";
  std::string out;
  bool changed = player_id.size() > MAX_KEPT;
  for (const char c : player_id.substr(0, MAX_KEPT))
  {
    const bool safe = IsSegmentChar(c);
    out.push_back(safe ? c : '_');
    changed |= !safe;
  }
  changed |= IsWindowsDeviceName(out);
  if (changed)
    out += fmt::format("-{:016x}", Fnv1a(player_id));
  return out;
}

std::optional<Store> Store::For(std::string_view player_id, std::string user_dir)
{
  const std::string segment = SanitizePlayerId(player_id);
  // The folder's one segment from the player: [A-Za-z0-9_-] only, the kept 64 plus "-" and 16 hex
  // digits at most. Checked again here so no other path into the store can widen it.
  if (segment.empty() || segment.size() > 64 + 17 ||
      !std::all_of(segment.begin(), segment.end(), IsSegmentChar) || IsWindowsDeviceName(segment))
  {
    return std::nullopt;
  }
  if (user_dir.empty())
    user_dir = File::GetUserPath(D_USER_IDX);
  if (user_dir.empty())
    return std::nullopt;
  if (user_dir.back() != '/' && user_dir.back() != '\\')
    user_dir.push_back('/');
  Store store(user_dir + std::string(STORE_FOLDER) + segment + "/");
  // The game's own Online/user.json belongs to whoever runs Dolphin outside YouGame.
  if (store.UserJsonPath() == user_dir + "Online/user.json")
    return std::nullopt;
  return store;
}

bool Store::SaveUserJson(const UserJson& user_json) const
{
  std::string text = user_json.Serialize();
  const bool ok = WriteAtomically(UserJsonPath(), text);
  Wipe(text);
  if (!ok)
    WARN_LOG_FMT(NETPLAY, "Brawl Online: could not write user.json");
  return ok;
}

bool Store::SaveSession(std::string_view uid, std::string_view token) const
{
  std::string text;
  text.reserve(BodyCapacity({uid, token}));
  text.append("{\n  \"uid\": ");
  AppendJsonString(text, uid);
  text.append(",\n  \"sessionToken\": ");
  AppendJsonString(text, token);
  text.append("\n}");
  const bool ok = WriteAtomically(SessionPath(), text);
  Wipe(text);
  if (!ok)
    WARN_LOG_FMT(NETPLAY, "Brawl Online: could not write session.json");
  return ok;
}

std::optional<std::string> Store::LoadSession() const
{
  const std::string path = SessionPath();
  std::string text;
  if (!File::Exists(path) || !File::ReadFileToString(path, text))
    return std::nullopt;
  picojson::value json;
  const bool parsed = Json::ParseWholeJson(text, &json).empty() && json.is<picojson::object>();
  Wipe(text);
  if (!parsed)
    return std::nullopt;
  std::string token = StringField(json.get<picojson::object>(), "sessionToken");
  if (token.empty())
    return std::nullopt;
  return token;
}

bool Store::HasUserJson() const
{
  return File::Exists(UserJsonPath());
}

bool Store::DropSession() const
{
  return !File::Exists(SessionPath()) ||
         File::Delete(SessionPath(), File::IfAbsentBehavior::NoConsoleWarning);
}

bool Store::Forget() const
{
  const bool session = DropSession();
  const bool user = !File::Exists(UserJsonPath()) ||
                    File::Delete(UserJsonPath(), File::IfAbsentBehavior::NoConsoleWarning);
  return session && user;
}

// ---- SignInFlow ----

SignInFlow::SignInFlow(std::unique_ptr<Transport> transport, Store store,
                       std::string_view yougame_name)
    : m_client(std::move(transport)), m_store(std::move(store)),
      m_display_name(DefaultDisplayName(yougame_name)),
      m_code_start(DefaultCodeStart(yougame_name))
{
}

void SignInFlow::Start()
{
  m_message.clear();
  m_server_message.clear();
  m_error = Error::None;
  if (auto token = m_store.LoadSession())
  {
    m_client.SetSession(std::move(*token));
    m_screen = Screen::CheckEmail;
    Continue();
    return;
  }
  m_screen = Screen::SignIn;
}

void SignInFlow::ShowSignIn()
{
  m_screen = Screen::SignIn;
  m_message.clear();
  m_server_message.clear();
  m_error = Error::None;
}

void SignInFlow::ShowMakeAccount()
{
  m_screen = Screen::MakeAccount;
  m_message.clear();
  m_server_message.clear();
  m_error = Error::None;
}

bool SignInFlow::StopIfCancelled()
{
  if (!Cancelled())
    return false;
  m_client.ForgetSession();
  return true;
}

void SignInFlow::SubmitSignIn(std::string_view email, std::string& password)
{
  if (Cancelled())
  {
    Wipe(password);
    return;
  }
  m_email = std::string(email);
  const SessionOutcome outcome = m_client.SignIn(email, password);
  Wipe(password);
  if (!outcome.Ok())
  {
    Fail(outcome);
    return;
  }
  AfterSession(outcome);
}

void SignInFlow::SubmitMakeAccount(std::string_view email, std::string& password,
                                   std::string_view display_name)
{
  if (Cancelled())
  {
    Wipe(password);
    return;
  }
  m_email = std::string(email);
  m_display_name = std::string(display_name);
  const SessionOutcome outcome = m_client.SignUp(email, password, display_name);
  Wipe(password);
  if (!outcome.Ok())
  {
    Fail(outcome);
    return;
  }
  AfterSession(outcome);
}

void SignInFlow::AfterSession(const SessionOutcome& outcome)
{
  m_uid = outcome.user.uid;
  if (StopIfCancelled())
    return;
  // Kept even if it can't be written: this run still holds it, and Continue works.
  m_store.SaveSession(m_uid, m_client.SessionToken());
  m_screen = Screen::CheckEmail;
  m_message.clear();
  m_server_message.clear();
  m_error = Error::None;
  if (outcome.user.Verified())
    Continue();
  else
    m_message = UserMessage(Error::NeedsVerification);
}

void SignInFlow::Continue()
{
  if (StopIfCancelled())
    return;
  if (!m_client.HasSession())
  {
    Fail(Refused(Error::NoSession));
    m_screen = Screen::SignIn;
    return;
  }
  const MeOutcome me = m_client.RefreshMe();
  if (!me.Ok())
  {
    Fail(me);
    return;
  }
  m_uid = me.user.uid;
  if (!me.user.Verified())
  {
    m_screen = Screen::CheckEmail;
    m_error = Error::NeedsVerification;
    m_message = UserMessage(Error::NeedsVerification);
    m_server_message.clear();
    return;
  }
  if (me.user.connect_code.empty())
  {
    Outcome code = m_client.EnsureConnectCode(m_code_start);
    // Every code with the player's letters taken: the YouGame letters instead.
    if (code.error == Error::CodeTaken && m_code_start != "YG")
    {
      m_code_start = "YG";
      code = m_client.EnsureConnectCode(m_code_start);
    }
    if (!code.Ok())
    {
      Fail(code);
      return;
    }
  }
  UserJsonOutcome user_json = m_client.FetchUserJson();
  if (!user_json.Ok())
  {
    Fail(user_json);
    return;
  }
  const bool cancelled = StopIfCancelled();
  const bool saved = !cancelled && m_store.SaveUserJson(user_json.user_json);
  Wipe(user_json.user_json.play_key);
  if (cancelled)
    return;
  if (!saved)
  {
    Fail(Refused(Error::SaveFailed));
    return;
  }
  m_screen = Screen::Done;
  m_message.clear();
  m_server_message.clear();
  m_error = Error::None;
}

std::string SignInFlow::UserJsonPath() const
{
  return m_screen == Screen::Done ? m_store.UserJsonPath() : std::string();
}

void SignInFlow::Fail(const Outcome& outcome)
{
  m_error = outcome.error;
  m_message = UserMessage(outcome.error, outcome.retry_after_s);
  m_server_message = outcome.message;
  if (outcome.error == Error::SignedOut || outcome.error == Error::NoSession)
  {
    m_store.DropSession();
    m_screen = Screen::SignIn;
  }
  else if (outcome.error == Error::NeedsVerification || outcome.error == Error::NotActivated)
  {
    m_screen = Screen::CheckEmail;
  }
}

std::string UserJsonPath(std::string_view player_id, std::string user_dir)
{
  const auto store = Store::For(player_id, std::move(user_dir));
  return store && store->HasUserJson() ? store->UserJsonPath() : std::string();
}

std::unique_ptr<SignInFlow> OpenSignIn(std::string_view player_id, std::string_view yougame_name)
{
  return OpenSignIn(player_id, yougame_name, MakeTransport(), {});
}

std::unique_ptr<SignInFlow> OpenSignIn(std::string_view player_id, std::string_view yougame_name,
                                       std::unique_ptr<Transport> transport, std::string user_dir)
{
  auto store = Store::For(player_id, std::move(user_dir));
  // Signed in on this PC already: the screen never shows again for this player.
  if (!store || store->HasUserJson())
    return nullptr;
  return std::make_unique<SignInFlow>(std::move(transport), std::move(*store), yougame_name);
}
}  // namespace Orca::BrawlOnline
