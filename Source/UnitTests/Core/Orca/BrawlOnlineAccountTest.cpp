// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Brawl Online accounts (Core/Orca/BrawlOnlineAccount.h) against a fake transport: answers shaped
// like Brawl Online's accounts API (server/crates/accounts api.rs and error.rs), no network.

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/ScopeGuard.h"
#include "Core/Orca/BrawlOnlineAccount.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/UX/BrawlOnlineSignIn.h"
#include "Core/Orca/UX/TextEntry.h"

namespace BO = Orca::BrawlOnline;

namespace
{
constexpr const char* TOKEN = "tok_0123456789abcdef";
constexpr const char* PLAY_KEY = "pk_fedcba9876543210";

struct Sent
{
  std::string method;
  std::string path;
  std::string body;
  std::string token;
};

// Answers in order; records every request. Shared with the test so it can look after the client
// took ownership.
struct FakeLog
{
  std::deque<BO::HttpResult> answers;
  std::vector<Sent> sent;
  // Runs as each request is sent, before it is answered, with its path.
  std::function<void(const std::string& path)> during;
};

class FakeTransport final : public BO::Transport
{
public:
  explicit FakeTransport(std::shared_ptr<FakeLog> log) : m_log(std::move(log)) {}
  BO::HttpResult Post(const std::string& path, const std::string& json_body,
                      const std::string& token) override
  {
    return Answer("POST", path, json_body, token);
  }
  BO::HttpResult Get(const std::string& path, const std::string& token) override
  {
    return Answer("GET", path, {}, token);
  }

private:
  BO::HttpResult Answer(std::string method, const std::string& path, const std::string& body,
                        const std::string& token)
  {
    m_log->sent.push_back({std::move(method), path, body, token});
    if (m_log->during)
      m_log->during(path);
    if (m_log->answers.empty())
      return {};
    BO::HttpResult answer = std::move(m_log->answers.front());
    m_log->answers.pop_front();
    return answer;
  }
  std::shared_ptr<FakeLog> m_log;
};

std::string MeJson(bool verified, const std::string& code = "")
{
  const std::string connect = code.empty() ? "null" : "\"" + code + "\"";
  return std::string(R"({"uid":"6f1c1f6e-0000-4000-8000-000000000001","email":"a@b.co",)") +
         R"("emailVerified":)" + (verified ? "true" : "false") +
         R"(,"emailVerificationRequired":true,"displayName":"luis","connectCode":)" + connect +
         R"(,"playKey":null,"rulesVersion":0,"currentRulesVersion":1,"latestVersion":"1.0",)" +
         R"("role":"user","userJson":null})";
}

BO::HttpResult Session(int status, bool verified)
{
  return {status, std::string(R"({"sessionToken":")") + TOKEN + R"(","user":)" + MeJson(verified) +
                      "}"};
}

BO::HttpResult ApiError(int status, const std::string& code, const std::string& message,
                        int retry_after = 0)
{
  return {status, R"({"error":{"code":")" + code + R"(","message":")" + message + R"("}})",
          retry_after};
}

BO::HttpResult UserJsonAnswer()
{
  return {200, std::string(R"({"uid":"6f1c1f6e-0000-4000-8000-000000000001","playKey":")") +
                   PLAY_KEY +
                   R"(","connectCode":"LUIS#123","displayName":"luis","latestVersion":"1.0"})"};
}

struct Fixture
{
  std::shared_ptr<FakeLog> log = std::make_shared<FakeLog>();
  BO::Client client{std::make_unique<FakeTransport>(log)};
};

// A user folder of the test's own, deleted on the way out.
class UserFolder
{
public:
  UserFolder() : m_dir(File::CreateTempDir() + "/") {}
  ~UserFolder() { File::DeleteDirRecursively(m_dir); }
  const std::string& Path() const { return m_dir; }

private:
  std::string m_dir;
};
}  // namespace

TEST(BrawlOnlineAccount, SignInOk)
{
  Fixture f;
  f.log->answers.push_back(Session(200, true));
  const auto result = f.client.SignIn("a@b.co", "hunter2hunter2");
  ASSERT_TRUE(result.Ok());
  EXPECT_TRUE(result.user.email_verified);
  EXPECT_EQ(result.user.display_name, "luis");
  EXPECT_EQ(f.client.SessionToken(), TOKEN);
  ASSERT_EQ(f.log->sent.size(), 1u);
  EXPECT_EQ(f.log->sent[0].method, "POST");
  EXPECT_EQ(f.log->sent[0].path, "/v1/auth/login");
  EXPECT_EQ(f.log->sent[0].body, R"({"email":"a@b.co","password":"hunter2hunter2"})");
  EXPECT_TRUE(f.log->sent[0].token.empty());
}

TEST(BrawlOnlineAccount, SignInWrongPassword)
{
  Fixture f;
  f.log->answers.push_back(ApiError(401, "bad_credentials", "Wrong email or password"));
  const auto result = f.client.SignIn("a@b.co", "nope-nope");
  EXPECT_EQ(result.error, BO::Error::BadCredentials);
  EXPECT_EQ(result.status, 401);
  EXPECT_FALSE(f.client.HasSession());
  EXPECT_EQ(BO::UserMessage(result.error), "Wrong email or password.");
}

TEST(BrawlOnlineAccount, SignInNoAnswerIsNetwork)
{
  Fixture f;
  EXPECT_EQ(f.client.SignIn("a@b.co", "whatever1").error, BO::Error::Network);
}

TEST(BrawlOnlineAccount, SignInEscapesTheBody)
{
  Fixture f;
  f.log->answers.push_back(Session(200, true));
  f.client.SignIn("a@b.co", "pa\"ss\\wo\nrd");
  ASSERT_EQ(f.log->sent.size(), 1u);
  EXPECT_EQ(f.log->sent[0].body, R"({"email":"a@b.co","password":"pa\"ss\\wo\u000ard"})");
}

TEST(BrawlOnlineAccount, SignUpOk)
{
  Fixture f;
  f.log->answers.push_back(Session(201, false));
  const auto result = f.client.SignUp("a@b.co", "longenough", "luis");
  ASSERT_TRUE(result.Ok());
  EXPECT_FALSE(result.user.Verified());
  EXPECT_TRUE(f.client.HasSession());
  ASSERT_EQ(f.log->sent.size(), 1u);
  EXPECT_EQ(f.log->sent[0].path, "/v1/auth/signup");
  EXPECT_EQ(f.log->sent[0].body,
            R"({"email":"a@b.co","password":"longenough","displayName":"luis"})");
}

TEST(BrawlOnlineAccount, SignUpRefusesBadFieldsWithoutSending)
{
  Fixture f;
  EXPECT_EQ(f.client.SignUp("a@b.co", "short", "luis").error, BO::Error::InvalidPassword);
  EXPECT_EQ(f.client.SignUp("not-an-email", "longenough", "luis").error,
            BO::Error::InvalidEmail);
  EXPECT_EQ(f.client.SignUp("a@b.co", "longenough", "sixteen chars!!!").error,
            BO::Error::InvalidDisplayName);
  EXPECT_TRUE(f.log->sent.empty());
}

TEST(BrawlOnlineAccount, SignUpEmailTaken)
{
  Fixture f;
  f.log->answers.push_back(ApiError(409, "email_taken", "An account with this email exists"));
  EXPECT_EQ(f.client.SignUp("a@b.co", "longenough", "luis").error, BO::Error::EmailTaken);
}

TEST(BrawlOnlineAccount, UnverifiedNetplayIsNeedsVerification)
{
  Fixture f;
  f.client.SetSession(TOKEN);
  f.log->answers.push_back(
      ApiError(403, "email_not_verified", "Verify your email before activating online play"));
  const auto result = f.client.EnsureConnectCode("luis");
  EXPECT_EQ(result.error, BO::Error::NeedsVerification);
  ASSERT_EQ(f.log->sent.size(), 1u);
  EXPECT_EQ(f.log->sent[0].path, "/v1/me/netplay");
  EXPECT_EQ(f.log->sent[0].body, R"({"codeStart":"LUIS"})");
  EXPECT_EQ(f.log->sent[0].token, TOKEN);
  EXPECT_TRUE(f.client.HasSession());
}

TEST(BrawlOnlineAccount, ConnectCodeAlreadySetIsDone)
{
  Fixture f;
  f.client.SetSession(TOKEN);
  f.log->answers.push_back(
      ApiError(409, "code_already_set", "This account already has a connect code"));
  EXPECT_TRUE(f.client.EnsureConnectCode("LU").Ok());
}

TEST(BrawlOnlineAccount, ConnectCodeRefusesBadLettersWithoutSending)
{
  Fixture f;
  f.client.SetSession(TOKEN);
  EXPECT_EQ(f.client.EnsureConnectCode("L").error, BO::Error::InvalidCode);
  EXPECT_EQ(f.client.EnsureConnectCode("LUISA").error, BO::Error::InvalidCode);
  EXPECT_EQ(f.client.EnsureConnectCode("L1").error, BO::Error::InvalidCode);
  EXPECT_TRUE(f.log->sent.empty());
}

TEST(BrawlOnlineAccount, UserJson409IsNotActivated)
{
  Fixture f;
  f.client.SetSession(TOKEN);
  f.log->answers.push_back(
      ApiError(409, "not_activated", "Verify your email and pick a connect code first"));
  const auto result = f.client.FetchUserJson();
  EXPECT_EQ(result.error, BO::Error::NotActivated);
  EXPECT_EQ(BO::UserMessage(result.error),
            "Check your email for the link from Brawl Online, then press Continue.");
}

TEST(BrawlOnlineAccount, UserJsonOk)
{
  Fixture f;
  f.client.SetSession(TOKEN);
  f.log->answers.push_back(UserJsonAnswer());
  const auto result = f.client.FetchUserJson();
  ASSERT_TRUE(result.Ok());
  EXPECT_EQ(result.user_json.play_key, PLAY_KEY);
  EXPECT_EQ(result.user_json.connect_code, "LUIS#123");
  EXPECT_EQ(f.log->sent[0].method, "GET");
  EXPECT_EQ(f.log->sent[0].path, "/v1/me/user-json");
}

TEST(BrawlOnlineAccount, RateLimited)
{
  Fixture f;
  f.log->answers.push_back(
      ApiError(429, "rate_limited", "Too many attempts. Try again in 42 s.", 42));
  const auto result = f.client.SignIn("a@b.co", "whatever1");
  EXPECT_EQ(result.error, BO::Error::RateLimited);
  EXPECT_EQ(result.retry_after_s, 42);
  EXPECT_EQ(BO::UserMessage(result.error, result.retry_after_s),
            "Too many tries. Wait 42 seconds and try again.");
  // Without the header, the seconds in the message; a 429 with any code is rate limited.
  f.log->answers.push_back(ApiError(429, "other", "Too many attempts. Try again in 7 s."));
  const auto again = f.client.SignIn("a@b.co", "whatever1");
  EXPECT_EQ(again.error, BO::Error::RateLimited);
  EXPECT_EQ(again.retry_after_s, 7);
  EXPECT_EQ(BO::UserMessage(BO::Error::RateLimited),
            "Too many tries. Wait a minute and try again.");
}

TEST(BrawlOnlineAccount, RefreshMeSignedOutDropsTheSession)
{
  Fixture f;
  f.client.SetSession(TOKEN);
  f.log->answers.push_back(ApiError(401, "unauthorized", "Not logged in"));
  EXPECT_EQ(f.client.RefreshMe().error, BO::Error::SignedOut);
  EXPECT_FALSE(f.client.HasSession());
  EXPECT_EQ(f.client.RefreshMe().error, BO::Error::NoSession);
}

TEST(BrawlOnlineAccount, ServerErrorAndUnreadableBody)
{
  Fixture f;
  f.log->answers.push_back({500, "<html>oops</html>"});
  EXPECT_EQ(f.client.SignIn("a@b.co", "whatever1").error, BO::Error::Server);
  f.log->answers.push_back({200, "not json"});
  EXPECT_EQ(f.client.SignIn("a@b.co", "whatever1").error, BO::Error::BadReply);
  f.log->answers.push_back({200, R"({"user":{}})"});
  EXPECT_EQ(f.client.SignIn("a@b.co", "whatever1").error, BO::Error::BadReply);
}

TEST(BrawlOnlineAccount, Redact)
{
  EXPECT_EQ(BO::Redact(R"({"sessionToken":"abc","user":{"playKey" : "xyz"}})"),
            R"({"sessionToken":"[redacted]","user":{"playKey" : "[redacted]"}})");
  EXPECT_EQ(BO::Redact(R"({"email":"a@b.co","password":"p\"w"})"),
            R"({"email":"a@b.co","password":"[redacted]"})");
  EXPECT_EQ(BO::Redact("Authorization: Bearer abc.def-123 sent"),
            "Authorization: Bearer [redacted] sent");
  EXPECT_EQ(BO::Redact("bearer xyz"), "bearer [redacted]");
  EXPECT_EQ(BO::Redact("key tok_1 and again tok_1.", {"tok_1", ""}),
            "key [redacted] and again [redacted].");
  EXPECT_EQ(BO::Redact("nothing secret"), "nothing secret");
  // A null play key stays null.
  EXPECT_EQ(BO::Redact(R"({"playKey":null})"), R"({"playKey":null})");
}

TEST(BrawlOnlineAccount, Wipe)
{
  std::string password = "hunter2hunter2";
  BO::Wipe(password);
  EXPECT_TRUE(password.empty());
}

TEST(BrawlOnlineAccount, DefaultCodeStart)
{
  EXPECT_EQ(BO::DefaultCodeStart("luis"), "LUIS");
  EXPECT_EQ(BO::DefaultCodeStart("luis_rosias"), "LUIS");
  EXPECT_EQ(BO::DefaultCodeStart("x9z"), "XZ");
  EXPECT_EQ(BO::DefaultCodeStart("a1b2c3d4e5"), "ABCD");
  EXPECT_EQ(BO::DefaultCodeStart("Bo"), "BO");
  EXPECT_EQ(BO::DefaultCodeStart("q"), "YG");
  EXPECT_EQ(BO::DefaultCodeStart("123"), "YG");
  EXPECT_EQ(BO::DefaultCodeStart(""), "YG");
  EXPECT_EQ(BO::DefaultCodeStart("\xC3\xA9t\xC3\xA9"), "YG");  // "été": one ASCII letter
  EXPECT_TRUE(BO::ValidCodeStart(BO::DefaultCodeStart("anything")));
}

TEST(BrawlOnlineAccount, DefaultDisplayName)
{
  EXPECT_EQ(BO::DefaultDisplayName("luis"), "luis");
  EXPECT_EQ(BO::DefaultDisplayName("  spaced out  "), "spaced out");
  EXPECT_EQ(BO::DefaultDisplayName("a_very_long_name_indeed"), "a_very_long_nam");
  EXPECT_EQ(BO::DefaultDisplayName("back\\tick`s"), "backticks");
  const std::string katakana = "\xE3\x83\xAB\xE3\x82\xA4\xE3\x82\xB9";  // three Katakana letters
  EXPECT_EQ(BO::DefaultDisplayName(katakana), katakana);
  EXPECT_EQ(BO::DefaultDisplayName("\xC3\xA9\xC3\xA9"), "Player");  // no drawable characters
  EXPECT_EQ(BO::DefaultDisplayName(""), "Player");
  EXPECT_EQ(BO::DefaultDisplayName("               x"), "x");
  EXPECT_TRUE(BO::ValidDisplayName(BO::DefaultDisplayName("a_very_long_name_indeed")));
  EXPECT_FALSE(BO::ValidDisplayName("   "));
  EXPECT_FALSE(BO::ValidDisplayName(""));
}

TEST(BrawlOnlineAccount, PasswordRule)
{
  EXPECT_FALSE(BO::ValidPassword("1234567"));
  EXPECT_TRUE(BO::ValidPassword("12345678"));
  EXPECT_TRUE(BO::ValidPassword(std::string(256, 'a')));
  EXPECT_FALSE(BO::ValidPassword(std::string(257, 'a')));
  // Code points, not bytes: four two-byte characters are too short.
  EXPECT_FALSE(BO::ValidPassword("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"));
}

TEST(BrawlOnlineAccount, StorePaths)
{
  const std::string user = "/u/User/";
  EXPECT_FALSE(BO::Store::For("", user));
  EXPECT_FALSE(BO::Store::For("dev:orca-abc", user));
  EXPECT_FALSE(BO::Store::For("DEV:orca-abc", user));

  const auto plain = BO::Store::For("b3a1c2d4-uuid_ok", user);
  ASSERT_TRUE(plain);
  const std::string folder = user + std::string(BO::STORE_FOLDER);
  EXPECT_EQ(plain->Folder(), folder + "b3a1c2d4-uuid_ok/");
  EXPECT_EQ(plain->UserJsonPath(), folder + "b3a1c2d4-uuid_ok/user.json");
  EXPECT_EQ(plain->SessionPath(), folder + "b3a1c2d4-uuid_ok/session.json");

  // Separators, dots and the like never leave the player's folder, and two ids that sanitize
  // alike still get different folders.
  const std::string escaped = BO::SanitizePlayerId("../../Online");
  EXPECT_EQ(escaped.find('/'), std::string::npos);
  EXPECT_EQ(escaped.find('.'), std::string::npos);
  EXPECT_NE(BO::SanitizePlayerId("a/b"), BO::SanitizePlayerId("a_b"));
  EXPECT_NE(BO::SanitizePlayerId("a/b"), BO::SanitizePlayerId("a\\b"));
  EXPECT_LE(BO::SanitizePlayerId(std::string(500, 'x')).size(), 64u + 17u);

  for (const char* id : {"..", ".", "/", "Online", "../user.json", "a:b", "x/../.."})
  {
    const auto store = BO::Store::For(id, user);
    ASSERT_TRUE(store) << id;
    EXPECT_NE(store->UserJsonPath(), user + "Online/user.json") << id;
    EXPECT_EQ(store->Folder().rfind(folder, 0), 0u) << id;
  }
  // Every folder is the user folder, the fixed part and one [A-Za-z0-9_-] segment of at most 81.
  for (const char* id : {"..", "a b", "a\nb", "\xC3\xA9t\xC3\xA9", "C:", "x\\..\\y", "luis@yougame.co"})
  {
    const std::string segment = BO::SanitizePlayerId(id);
    ASSERT_FALSE(segment.empty()) << id;
    EXPECT_LE(segment.size(), 64u + 17u) << id;
    for (const char c : segment)
    {
      EXPECT_TRUE((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_')
          << id;
    }
    EXPECT_EQ(BO::Store::For(id, user)->Folder(), folder + segment + "/");
  }
  // Names Windows keeps for devices get the hash too, in any case.
  for (const char* id : {"CON", "con", "Nul", "PRN", "AUX", "COM1", "com9", "LPT0", "lpt3"})
  {
    const std::string segment = BO::SanitizePlayerId(id);
    EXPECT_NE(segment, id) << id;
    EXPECT_EQ(segment.rfind(std::string(id) + "-", 0), 0u) << id;
  }
  EXPECT_EQ(BO::SanitizePlayerId("CONSOLE"), "CONSOLE");
  EXPECT_EQ(BO::SanitizePlayerId("COM10"), "COM10");
  // The user folder's trailing separator is added when missing.
  EXPECT_EQ(BO::Store::For("p1", "/u/User")->Folder(), folder + "p1/");
}

TEST(BrawlOnlineAccount, StoreWritesAndReads)
{
  UserFolder dir;
  const auto store = BO::Store::For("player-1", dir.Path());
  ASSERT_TRUE(store);
  EXPECT_FALSE(store->LoadSession());
  ASSERT_TRUE(store->SaveSession("uid-1", TOKEN));
  EXPECT_EQ(store->LoadSession().value_or(""), TOKEN);
  EXPECT_FALSE(File::Exists(store->SessionPath() + ".tmp"));

  BO::UserJson u{"uid-1", PLAY_KEY, "LUIS#123", "luis", "1.0"};
  ASSERT_TRUE(store->SaveUserJson(u));
  std::string text;
  ASSERT_TRUE(File::ReadFileToString(store->UserJsonPath(), text));
  EXPECT_EQ(text, std::string("{\n  \"uid\": \"uid-1\",\n  \"playKey\": \"") + PLAY_KEY +
                      "\",\n  \"connectCode\": \"LUIS#123\",\n  \"displayName\": \"luis\",\n"
                      "  \"latestVersion\": \"1.0\"\n}");
  EXPECT_FALSE(File::Exists(dir.Path() + "Online/user.json"));

  EXPECT_TRUE(store->DropSession());
  EXPECT_FALSE(store->LoadSession());
  EXPECT_TRUE(store->HasUserJson());
  EXPECT_TRUE(store->Forget());
  EXPECT_FALSE(store->HasUserJson());
}

#ifndef _WIN32
TEST(BrawlOnlineAccount, StoreFilesAreOwnerOnlyFromTheStart)
{
  UserFolder dir;
  const auto store = BO::Store::For("player-1", dir.Path());
  ASSERT_TRUE(store);
  // With no umask at all, a default-mode file would be readable by everyone.
  const mode_t old_mask = umask(0);
  const Common::ScopeGuard restore([old_mask] { umask(old_mask); });
  // A readable temporary file left by a crash is replaced, never written into.
  const std::string stale = store->SessionPath() + ".tmp";
  ASSERT_TRUE(File::CreateFullPath(stale));
  ASSERT_TRUE(File::WriteStringToFile(stale, "stale"));
  ASSERT_EQ(chmod(stale.c_str(), 0644), 0);

  ASSERT_TRUE(store->SaveSession("uid-1", TOKEN));
  ASSERT_TRUE(store->SaveUserJson({"uid-1", PLAY_KEY, "LUIS#123", "luis", "1.0"}));
  for (const std::string& path : {store->SessionPath(), store->UserJsonPath()})
  {
    struct stat info = {};
    ASSERT_EQ(stat(path.c_str(), &info), 0) << path;
    EXPECT_EQ(info.st_mode & 0777, 0600u) << path;
  }
  EXPECT_FALSE(File::Exists(stale));
  EXPECT_EQ(store->LoadSession().value_or(""), TOKEN);
}
#endif

TEST(BrawlOnlineAccount, FlowMakeAccountThenContinue)
{
  UserFolder dir;
  auto log = std::make_shared<FakeLog>();
  auto store = BO::Store::For("player-1", dir.Path());
  ASSERT_TRUE(store);
  BO::SignInFlow flow(std::make_unique<FakeTransport>(log), *store, "luis");
  flow.Start();
  EXPECT_EQ(flow.CurrentScreen(), BO::SignInFlow::Screen::SignIn);
  flow.ShowMakeAccount();
  EXPECT_EQ(flow.DisplayName(), "luis");
  EXPECT_EQ(flow.CodeStart(), "LUIS");

  log->answers.push_back(Session(201, false));
  std::string password = "longenough";
  flow.SubmitMakeAccount("a@b.co", password, flow.DisplayName());
  EXPECT_TRUE(password.empty());
  EXPECT_EQ(flow.CurrentScreen(), BO::SignInFlow::Screen::CheckEmail);
  EXPECT_EQ(store->LoadSession().value_or(""), TOKEN);

  // Continue before the link was clicked: still waiting.
  log->answers.push_back({200, MeJson(false)});
  flow.Continue();
  EXPECT_EQ(flow.CurrentScreen(), BO::SignInFlow::Screen::CheckEmail);
  EXPECT_EQ(flow.LastError(), BO::Error::NeedsVerification);

  // Verified: code, user.json, saved.
  log->answers.push_back({200, MeJson(true)});
  log->answers.push_back({200, MeJson(true, "LUIS#123")});
  log->answers.push_back(UserJsonAnswer());
  flow.Continue();
  EXPECT_EQ(flow.CurrentScreen(), BO::SignInFlow::Screen::Done);
  EXPECT_TRUE(store->HasUserJson());
  ASSERT_GE(log->sent.size(), 3u);
  EXPECT_EQ(log->sent[log->sent.size() - 2].path, "/v1/me/netplay");
  EXPECT_EQ(log->sent.back().path, "/v1/me/user-json");
  // The flow ends at the saved file, which the module-level lookup finds too.
  EXPECT_EQ(flow.UserJsonPath(), store->UserJsonPath());
  EXPECT_EQ(BO::UserJsonPath("player-1", dir.Path()), store->UserJsonPath());
}

// The screen closed while signing in: the answer arrives, but nothing is saved and the session the
// client got is dropped, so nothing more is asked either.
TEST(BrawlOnlineAccount, FlowCancelledDuringSignInSavesNothing)
{
  UserFolder dir;
  auto log = std::make_shared<FakeLog>();
  auto store = BO::Store::For("player-1", dir.Path());
  ASSERT_TRUE(store);
  BO::SignInFlow flow(std::make_unique<FakeTransport>(log), *store, "luis");
  flow.Start();
  log->answers.push_back(Session(200, true));
  log->answers.push_back({200, MeJson(true, "LUIS#123")});
  log->answers.push_back(UserJsonAnswer());
  log->during = [&flow](const std::string& path) {
    if (path == "/v1/auth/login")
      flow.Cancel();
  };
  std::string password = "longenough";
  flow.SubmitSignIn("a@b.co", password);
  EXPECT_TRUE(password.empty());
  EXPECT_TRUE(flow.Cancelled());
  EXPECT_FALSE(File::Exists(store->SessionPath()));
  EXPECT_FALSE(store->HasUserJson());
  EXPECT_FALSE(flow.AccountClient().HasSession());
  EXPECT_TRUE(flow.UserJsonPath().empty());
  ASSERT_EQ(log->sent.size(), 1u);

  // Anything pressed after the close sends nothing at all.
  password = "longenough";
  flow.SubmitSignIn("a@b.co", password);
  flow.Continue();
  EXPECT_TRUE(password.empty());
  EXPECT_EQ(log->sent.size(), 1u);
}

// The screen closed while Continue fetched user.json: the keys are not written.
TEST(BrawlOnlineAccount, FlowCancelledDuringContinueSavesNoUserJson)
{
  UserFolder dir;
  auto store = BO::Store::For("player-1", dir.Path());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store->SaveSession("uid-1", TOKEN));
  auto log = std::make_shared<FakeLog>();
  BO::SignInFlow flow(std::make_unique<FakeTransport>(log), *store, "luis");
  log->answers.push_back({200, MeJson(true, "LUIS#123")});
  log->answers.push_back(UserJsonAnswer());
  log->during = [&flow](const std::string& path) {
    if (path == "/v1/me/user-json")
      flow.Cancel();
  };
  flow.Start();
  ASSERT_GE(log->sent.size(), 2u);
  EXPECT_EQ(log->sent.back().path, "/v1/me/user-json");
  EXPECT_FALSE(store->HasUserJson());
  EXPECT_FALSE(flow.AccountClient().HasSession());
  EXPECT_NE(flow.CurrentScreen(), BO::SignInFlow::Screen::Done);
  EXPECT_TRUE(flow.UserJsonPath().empty());
}

TEST(BrawlOnlineAccount, FlowWrongPasswordStaysOnSignIn)
{
  UserFolder dir;
  auto log = std::make_shared<FakeLog>();
  BO::SignInFlow flow(std::make_unique<FakeTransport>(log), *BO::Store::For("p", dir.Path()),
                      "luis");
  flow.Start();
  log->answers.push_back(ApiError(401, "bad_credentials", "Wrong email or password"));
  std::string password = "wrongwrong";
  flow.SubmitSignIn("a@b.co", password);
  EXPECT_TRUE(password.empty());
  EXPECT_EQ(flow.CurrentScreen(), BO::SignInFlow::Screen::SignIn);
  EXPECT_EQ(flow.Message(), "Wrong email or password.");
  // The server's own words stay with the screen.
  EXPECT_EQ(flow.ServerMessage(), "Wrong email or password");
  EXPECT_TRUE(flow.UserJsonPath().empty());
}

TEST(BrawlOnlineAccount, FlowKeptSessionExpiredGoesToSignIn)
{
  UserFolder dir;
  auto store = BO::Store::For("p", dir.Path());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store->SaveSession("uid-1", TOKEN));
  auto log = std::make_shared<FakeLog>();
  BO::SignInFlow flow(std::make_unique<FakeTransport>(log), *store, "luis");
  log->answers.push_back(ApiError(401, "unauthorized", "Not logged in"));
  flow.Start();
  EXPECT_EQ(flow.CurrentScreen(), BO::SignInFlow::Screen::SignIn);
  EXPECT_EQ(flow.LastError(), BO::Error::SignedOut);
  EXPECT_FALSE(store->LoadSession());
  ASSERT_EQ(log->sent.size(), 1u);
  EXPECT_EQ(log->sent[0].token, TOKEN);
}

#ifdef ORCA_BO_LIVE
TEST(BrawlOnlineAccount, BaseUrlIsBrawlOnlinesDefault)
{
  EXPECT_EQ(BO::ACCOUNTS_BASE_URL, "https://brawlonline.net");
}
#endif

TEST(BrawlOnlineAccount, FailureLogLineLeavesOutTheMessage)
{
  Fixture f;
  f.log->answers.push_back(
      ApiError(409, "email_taken", "An account with ada@example.com already exists"));
  const auto result = f.client.SignUp("ada@example.com", "longenough", "ada");
  EXPECT_EQ(result.error, BO::Error::EmailTaken);
  // The message stays in the result for the screen...
  EXPECT_EQ(result.message, "An account with ada@example.com already exists");
  // ...and never reaches the log line.
  const std::string line = BO::FailureLogLine("make an account", result);
  EXPECT_EQ(line, "Brawl Online: make an account: HTTP 409 email_taken");
  EXPECT_EQ(line.find("ada@"), std::string::npos);

  BO::Outcome odd;
  odd.status = 400;
  odd.code = "bad code: ada@example.com";
  EXPECT_EQ(BO::FailureLogLine("sign in", odd), "Brawl Online: sign in: HTTP 400 ?");
  odd.code.clear();
  EXPECT_EQ(BO::FailureLogLine("sign in", odd), "Brawl Online: sign in: HTTP 400 -");
}

TEST(BrawlOnlineAccount, UserJsonPathOnlyWhenStored)
{
  UserFolder dir;
  EXPECT_TRUE(BO::UserJsonPath("", dir.Path()).empty());
  EXPECT_TRUE(BO::UserJsonPath("dev:orca-abc", dir.Path()).empty());
  EXPECT_TRUE(BO::UserJsonPath("player-1", dir.Path()).empty());
  const auto store = BO::Store::For("player-1", dir.Path());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store->SaveUserJson({"uid-1", PLAY_KEY, "LUIS#123", "luis", "1.0"}));
  EXPECT_EQ(BO::UserJsonPath("player-1", dir.Path()), store->UserJsonPath());
  // Another player on the same PC still has none.
  EXPECT_TRUE(BO::UserJsonPath("player-2", dir.Path()).empty());
}

TEST(BrawlOnlineAccount, OpenSignInSkipsWhenStored)
{
  UserFolder dir;
  auto log = std::make_shared<FakeLog>();
  EXPECT_FALSE(BO::OpenSignIn("", "luis", std::make_unique<FakeTransport>(log), dir.Path()));
  EXPECT_FALSE(
      BO::OpenSignIn("dev:orca-abc", "luis", std::make_unique<FakeTransport>(log), dir.Path()));
  EXPECT_TRUE(BO::OpenSignIn("player-1", "luis", std::make_unique<FakeTransport>(log), dir.Path()));

  ASSERT_TRUE(BO::Store::For("player-1", dir.Path())
                  ->SaveUserJson({"uid-1", PLAY_KEY, "LUIS#123", "luis", "1.0"}));
  EXPECT_FALSE(BO::OpenSignIn("player-1", "luis", std::make_unique<FakeTransport>(log), dir.Path()));
  EXPECT_TRUE(BO::OpenSignIn("player-2", "luis", std::make_unique<FakeTransport>(log), dir.Path()));
  EXPECT_TRUE(log->sent.empty());
}

#ifdef ORCA_BO_LIVE
TEST(BrawlOnlineAccount, ForgotPasswordIsOnBrawlOnlinesHost)
{
  EXPECT_EQ(BO::FORGOT_PASSWORD_URL.rfind(BO::ACCOUNTS_BASE_URL, 0), 0u);
  EXPECT_EQ(BO::FORGOT_PASSWORD_URL, "https://brawlonline.net/login/");
}

TEST(BrawlOnlineAccount, OnlyTheResetPageOpensInABrowser)
{
  EXPECT_TRUE(BO::IsResetPageUrl(BO::FORGOT_PASSWORD_URL));
  EXPECT_FALSE(BO::IsResetPageUrl(""));
  EXPECT_FALSE(BO::IsResetPageUrl("http://brawlonline.net/login/"));
  EXPECT_FALSE(BO::IsResetPageUrl("https://brawlonline.net/login/x"));
  EXPECT_FALSE(BO::IsResetPageUrl("https://brawlonline.net/login"));
  EXPECT_FALSE(BO::IsResetPageUrl("https://example.com/"));
  EXPECT_FALSE(BO::IsResetPageUrl("file:///etc/passwd"));
}
#endif

// Links live builds and test builds make never share a folder, either way round.
TEST(BrawlOnlineAccount, TestLinksAreKeptApartFromLiveOnes)
{
  const std::string live(BO::LIVE_STORE_FOLDER);
  const std::string test(BO::TEST_STORE_FOLDER);
  EXPECT_NE(live, test);
  EXPECT_NE(test.rfind(live, 0), 0u);
  EXPECT_NE(live.rfind(test, 0), 0u);
  EXPECT_EQ(BO::STORE_FOLDER, BO::LIVE ? BO::LIVE_STORE_FOLDER : BO::TEST_STORE_FOLDER);
  const auto store = BO::Store::For("player-1", "/u/User/");
  ASSERT_TRUE(store);
  EXPECT_EQ(store->Folder(), "/u/User/" + std::string(BO::STORE_FOLDER) + "player-1/");
#ifndef ORCA_BO_LIVE
  EXPECT_FALSE(BO::LIVE);
  EXPECT_EQ(store->Folder().rfind("/u/User/" + live, 0), std::string::npos);
#endif
}

#ifndef ORCA_BO_LIVE
// Without ORCA_BO_LIVE the screen runs on the stand-in: a sign-in completes in-process, and the
// link it makes is saved in the test folder, never where a live build looks.
TEST(BrawlOnlineAccount, TestBuildSignInSavesInTheTestFolder)
{
  UserFolder dir;
  auto flow = BO::OpenSignIn("player-1", "luis", BO::MakeTransport(), dir.Path());
  ASSERT_TRUE(flow);
  flow->Start();
  std::string password = "anything-at-all";
  flow->SubmitSignIn("a@b.co", password);
  EXPECT_TRUE(password.empty());
  ASSERT_EQ(flow->CurrentScreen(), BO::SignInFlow::Screen::Done) << flow->Message();
  const std::string saved = flow->UserJsonPath();
  EXPECT_EQ(saved, dir.Path() + std::string(BO::TEST_STORE_FOLDER) + "player-1/user.json");
  EXPECT_TRUE(File::Exists(saved));
  EXPECT_FALSE(File::Exists(dir.Path() + std::string(BO::LIVE_STORE_FOLDER)));
  std::string text;
  ASSERT_TRUE(File::ReadFileToString(saved, text));
  EXPECT_NE(text.find("\"LUIS#000\""), std::string::npos) << text;
}

// A new account on the stand-in asks for the email once, then Continue finishes it.
TEST(BrawlOnlineAccount, TestBuildMakeAccountThenContinue)
{
  UserFolder dir;
  auto flow = BO::OpenSignIn("player-2", "luis", BO::MakeTestTransport(), dir.Path());
  ASSERT_TRUE(flow);
  flow->Start();
  std::string password = "longenough";
  flow->SubmitMakeAccount("a@b.co", password, "Luis");
  EXPECT_EQ(flow->CurrentScreen(), BO::SignInFlow::Screen::CheckEmail);
  flow->Continue();
  ASSERT_EQ(flow->CurrentScreen(), BO::SignInFlow::Screen::Done) << flow->Message();
  EXPECT_EQ(flow->UserJsonPath().rfind(dir.Path() + std::string(BO::TEST_STORE_FOLDER), 0), 0u);
}
#endif

namespace
{
namespace Screen = Orca::UX::BrawlOnlineSignIn;

// Online play "runs" for the test's lifetime; nothing is left open or capturing afterwards.
struct OnlineForTest
{
  explicit OnlineForTest(bool (*check)()) { Screen::SetOnlineCheckForTest(check); }
  ~OnlineForTest()
  {
    Screen::Close();
    Screen::SetOnlineCheckForTest(nullptr);
  }
};
bool Online()
{
  return true;
}
}  // namespace

// While online play runs the screen never opens, so the keyboard is never captured and the input
// gate stays open: neutral pads would reach the other players as real input.
TEST(BrawlOnlineSignInScreen, NeverOpensWhileOnline)
{
  OnlineForTest online(Online);
  UserFolder dir;
  auto log = std::make_shared<FakeLog>();
  EXPECT_TRUE(Screen::OnlineActive());
  EXPECT_FALSE(Screen::OpenFlow(
      BO::OpenSignIn("player-1", "luis", std::make_unique<FakeTransport>(log), dir.Path())));
  EXPECT_FALSE(Screen::IsOpen());
  EXPECT_FALSE(Orca::UX::TextEntry::Capturing());
  EXPECT_TRUE(log->sent.empty());
}

// Under the desktop app this game hosts a room of its own from boot. Alone in it, nobody else sees
// its pads, so the sign-in screen may open; any sign of another player keeps it shut.
TEST(BrawlOnlineSignInScreen, HostingAloneIsNotOnline)
{
  using Orca::Online::OthersInputs;
  using Orca::Online::OthersPlaying;
  OthersInputs alone;
  alone.has_room = true;
  alone.joining = false;
  EXPECT_FALSE(OthersPlaying(alone));
  EXPECT_FALSE(OthersPlaying(OthersInputs{}));

  OthersInputs in = alone;
  in.friends_in_game = true;
  EXPECT_TRUE(OthersPlaying(in));
  in = alone;
  in.drop_in_pending = true;
  EXPECT_TRUE(OthersPlaying(in));
  in = alone;
  in.match_live = true;
  EXPECT_TRUE(OthersPlaying(in));
  in = alone;
  in.joining = true;
  EXPECT_TRUE(OthersPlaying(in));
  // Still connecting: the room hasn't said whether this game hosts or joins a friend's.
  in = alone;
  in.joining = std::nullopt;
  EXPECT_TRUE(OthersPlaying(in));

  // An ended room keeps nothing shut by itself; friends still in the game do.
  in = alone;
  in.room_ended = true;
  in.joining = std::nullopt;
  in.drop_in_pending = true;
  EXPECT_FALSE(OthersPlaying(in));
  in.friends_in_game = true;
  EXPECT_TRUE(OthersPlaying(in));
  // Solo play after leaving the room: only friends in the game count.
  in = OthersInputs{};
  in.friends_in_game = true;
  EXPECT_TRUE(OthersPlaying(in));
}

// The real check: no room and nobody in the game is not online; friends in the game are.
TEST(BrawlOnlineSignInScreen, FriendsInGameIsOnline)
{
  Screen::SetOnlineCheckForTest(nullptr);
  Orca::Online::SetFriendsInGame(false);
  EXPECT_FALSE(Screen::OnlineActive());
  Orca::Online::SetFriendsInGame(true);
  EXPECT_TRUE(Screen::OnlineActive());
  Orca::Online::SetFriendsInGame(false);
  EXPECT_FALSE(Screen::OnlineActive());
}

// A build without ORCA_BO_LIVE says on the screen, in one plain line, that nothing leaves it.
TEST(BrawlOnlineSignInScreen, TestBuildLineShows)
{
#ifdef ORCA_BO_LIVE
  EXPECT_EQ(Screen::Note().find("Test build"), std::string_view::npos);
#else
  EXPECT_EQ(Screen::Note(), "Test build: nothing is sent to Brawl Online.");
#endif
}

// Online play that starts under an open screen closes it at the next frame, keyboard released.
TEST(BrawlOnlineSignInScreen, ClosesWhenOnlineStarts)
{
  static std::atomic<bool> s_online = false;
  OnlineForTest online([] { return s_online.load(); });
  UserFolder dir;
  auto log = std::make_shared<FakeLog>();
  ASSERT_TRUE(Screen::OpenFlow(
      BO::OpenSignIn("player-1", "luis", std::make_unique<FakeTransport>(log), dir.Path())));
  EXPECT_TRUE(Screen::IsOpen());
  EXPECT_TRUE(Orca::UX::TextEntry::Capturing());
  s_online = true;
  Screen::Draw();
  EXPECT_FALSE(Screen::IsOpen());
  EXPECT_FALSE(Orca::UX::TextEntry::Capturing());
  s_online = false;
}
