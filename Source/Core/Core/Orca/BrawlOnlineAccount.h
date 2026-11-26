// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// A Brawl Online account for this player: sign in or make one, wait for the email to be verified,
// take a connect code and fetch the user.json Brawl Online's matchmaker reads. Brawl Online's
// accounts service (server/crates/accounts in its repository) answers errors as
// {"error": {"code", "message"}}, with a Retry-After header on 429.
//
// The password goes into one request body and is wiped with it; it is never kept. The session
// token and the play key are never logged: every log line goes through Redact.
//
// The screen is SignInFlow below: the three states and what each button does. The overlay draws
// it (UX/BrawlOnlineSignIn.h). OpenSignIn makes one, and none once this PC holds the player's
// user.json, so a player signs in once per PC.
namespace Orca::BrawlOnline
{
// The accounts API's base URL: defaultServiceUrls.accountsApi in Brawl Online's launcher
// (common/product.ts), the same host its launcher talks to by default. Paths start with /v1.
inline constexpr std::string_view ACCOUNTS_BASE_URL = "https://brawlonline.net";
// Brawl Online's website login page (website/login in its repository). Its "Forgot your
// password?" button asks the accounts service (POST /v1/auth/password-reset/request) to email a
// reset link to /reset-password. Opened in the system browser, never fetched here.
inline constexpr std::string_view FORGOT_PASSWORD_URL = "https://brawlonline.net/login/";
// Whether `url` is the one page this build may open in the system browser: FORGOT_PASSWORD_URL
// exactly, over https. The opener refuses anything else.
constexpr bool IsResetPageUrl(std::string_view url)
{
  return url == FORGOT_PASSWORD_URL && url.substr(0, 8) == "https://";
}
static_assert(IsResetPageUrl(FORGOT_PASSWORD_URL));

// Brawl Online's own rules (common/src/codes.rs, accounts password.rs).
inline constexpr std::size_t DISPLAY_NAME_MAX = 15;  // code points
inline constexpr std::size_t PASSWORD_MIN = 8;       // code points
inline constexpr std::size_t PASSWORD_MAX = 256;
inline constexpr std::size_t CODE_START_MIN = 2;  // ASCII letters
inline constexpr std::size_t CODE_START_MAX = 4;

// ---- Transport ----

// One HTTP answer. status 0: no answer at all (offline, DNS, TLS, timeout).
struct HttpResult
{
  int status = 0;
  std::string body;
  int retry_after_s = 0;  // the Retry-After header, 0 when absent
};

// What the client sends through. `path` starts with "/v1/"; `token`, when not empty, goes in an
// "Authorization: Bearer" header. Implementations never log the body or the token.
class Transport
{
public:
  virtual ~Transport() = default;
  virtual HttpResult Post(const std::string& path, const std::string& json_body,
                          const std::string& token) = 0;
  virtual HttpResult Get(const std::string& path, const std::string& token) = 0;
};

// The production transport: Common::HttpRequest against ACCOUNTS_BASE_URL, TLS verified.
std::unique_ptr<Transport> MakeHttpTransport();

// ---- Results ----

enum class Error
{
  None,
  Network,             // no answer
  BadCredentials,      // 401 bad_credentials: wrong email or password
  SignedOut,           // 401 unauthorized: the session ended (90 days unused, or signed out)
  Banned,              // 403 banned
  NeedsVerification,   // 403 email_not_verified: the email is not verified yet
  NotActivated,        // 409 not_activated: user.json needs a verified email and a code
  EmailTaken,          // 409 email_taken
  InvalidEmail,        // 400 invalid_email
  InvalidPassword,     // 400 invalid_password, or refused before sending
  InvalidDisplayName,  // 400 invalid_display_name, or refused before sending
  InvalidCode,         // 400 invalid_code, or refused before sending
  CodeTaken,           // 409 prefix_exhausted: every code with these letters is taken
  CodeAlreadySet,      // 409 code_already_set (EnsureConnectCode treats it as done)
  RateLimited,         // 429
  Server,              // 5xx, or an error this client doesn't know
  BadReply,            // an answer that isn't what the API sends
  NoSession,           // a call that needs a session made without one
  SaveFailed,          // user.json or the session could not be written
};

std::string_view ErrorName(Error error);
// What the screen says for `error`.
std::string UserMessage(Error error, int retry_after_s = 0);

struct Outcome
{
  Error error = Error::None;
  int status = 0;
  std::string code;     // the API's error code ("bad_credentials"), empty when none
  std::string message;  // the API's message, redacted
  int retry_after_s = 0;
  bool Ok() const { return error == Error::None; }
};

// GET /v1/me, the fields used here.
struct Me
{
  std::string uid;
  std::string display_name;
  std::string connect_code;  // empty until one is assigned
  bool email_verified = false;
  bool email_verification_required = true;
  // Whether the server lets this account play online once it has a code.
  bool Verified() const { return email_verified || !email_verification_required; }
};

// Exactly what Brawl Online's launcher writes to Dolphin's Online/user.json.
struct UserJson
{
  std::string uid;
  std::string play_key;
  std::string connect_code;
  std::string display_name;
  std::string latest_version;
  // The file's text: fields in the launcher's order, indented two spaces as
  // JSON.stringify(x, null, 2) does.
  std::string Serialize() const;
};

// The log line for a refused call: what was asked, the HTTP status and the API's error code, never
// its message (which can quote the email address). A code that isn't [a-z0-9_]{1,40} logs as "?".
std::string FailureLogLine(std::string_view what, const Outcome& outcome);

struct SessionOutcome : Outcome
{
  Me user;
};
struct MeOutcome : Outcome
{
  Me user;
};
struct UserJsonOutcome : Outcome
{
  UserJson user_json;
};

// ---- The client ----

class Client
{
public:
  explicit Client(std::unique_ptr<Transport> transport);

  // POST /v1/auth/login. On success the client holds the new session.
  SessionOutcome SignIn(std::string_view email, std::string_view password);
  // POST /v1/auth/signup (201). On success the client holds the new session.
  SessionOutcome SignUp(std::string_view email, std::string_view password,
                        std::string_view display_name);
  // GET /v1/me: verified yet? A 401 drops the held session.
  MeOutcome RefreshMe();
  // POST /v1/me/netplay {codeStart}. An account that already has a code counts as done.
  Outcome EnsureConnectCode(std::string_view code_start);
  // GET /v1/me/user-json (409 until verified and given a code).
  UserJsonOutcome FetchUserJson();

  bool HasSession() const { return !m_token.empty(); }
  // The held session token, for the store. Never log it.
  const std::string& SessionToken() const { return m_token; }
  void SetSession(std::string token) { m_token = std::move(token); }
  void ForgetSession();

private:
  std::unique_ptr<Transport> m_transport;
  std::string m_token;
};

// ---- Redaction ----

// `text` with every secret blanked: the values of "sessionToken", "playKey" and "password" JSON
// fields, "Bearer <token>", and each non-empty string in `secrets` wherever it appears.
std::string Redact(std::string_view text, std::initializer_list<std::string_view> secrets = {});

// Overwrites a string's bytes before clearing it, so a password doesn't linger in freed memory.
void Wipe(std::string& text);

// ---- Defaults from the YouGame name ----

// The first 2-4 ASCII letters of `yougame_name`, upper-cased; "YG" when it has fewer than 2.
std::string DefaultCodeStart(std::string_view yougame_name);
// `yougame_name` cut to Brawl Online's display-name rule: characters its fonts draw (printable
// ASCII without \ and `, Hiragana, Katakana), spaces trimmed, at most 15; "Player" when nothing
// is left.
std::string DefaultDisplayName(std::string_view yougame_name);

bool ValidCodeStart(std::string_view code_start);
bool ValidDisplayName(std::string_view name);
bool ValidPassword(std::string_view password);

// ---- The store: <User>/Online/brawlonline/<player>/{user.json,session.json} ----

// `player_id` as one path segment: [A-Za-z0-9_-] kept, anything else '_', at most 64 characters,
// with a hash of the original appended whenever a character was replaced or cut, or the result is a
// name Windows keeps for a device (CON, NUL, COM1...), so two ids never share a folder. Empty for an
// empty id or a dev id ("dev:...").
std::string SanitizePlayerId(std::string_view player_id);

class Store
{
public:
  // The store for the YouGame player `player_id`, under `user_dir` (Dolphin's User folder, with a
  // trailing separator; File::GetUserPath(D_USER_IDX) when empty). Nothing for an empty or dev id.
  static std::optional<Store> For(std::string_view player_id, std::string user_dir = {});

  const std::string& Folder() const { return m_folder; }
  std::string UserJsonPath() const { return m_folder + "user.json"; }
  std::string SessionPath() const { return m_folder + "session.json"; }

  // Written to a temporary file then renamed over the old one.
  bool SaveUserJson(const UserJson& user_json) const;
  bool SaveSession(std::string_view uid, std::string_view token) const;
  // The kept session token, if there is one.
  std::optional<std::string> LoadSession() const;
  bool HasUserJson() const;
  // Deletes session.json (the session ended); user.json stays.
  bool DropSession() const;
  // Deletes both files (signing out of Brawl Online on this player).
  bool Forget() const;

private:
  explicit Store(std::string folder) : m_folder(std::move(folder)) {}
  std::string m_folder;
};

// ---- The screen's state machine ----

// Three states: Sign in (email, password); Make an account (email, password, display name
// prefilled from the YouGame name); "Check your email, then press Continue". Continue checks the
// email (RefreshMe), takes a connect code (EnsureConnectCode), fetches user.json and saves it.
// Every call blocks on the network: drive it from a worker thread, never the CPU or GPU thread.
class SignInFlow
{
public:
  enum class Screen
  {
    SignIn,
    MakeAccount,
    CheckEmail,
    Done,
  };

  SignInFlow(std::unique_ptr<Transport> transport, Store store, std::string_view yougame_name);

  Screen CurrentScreen() const { return m_screen; }
  // The line under the form: empty, or what went wrong in words for the player.
  const std::string& Message() const { return m_message; }
  Error LastError() const { return m_error; }
  // The API's own message for the last refusal (redacted), empty when none. For the screen only.
  const std::string& ServerMessage() const { return m_server_message; }
  // The form's prefilled fields.
  const std::string& Email() const { return m_email; }
  const std::string& DisplayName() const { return m_display_name; }
  const std::string& CodeStart() const { return m_code_start; }

  // A kept session goes straight to Check your email and tries Continue once; otherwise Sign in.
  void Start();
  void ShowSignIn();
  void ShowMakeAccount();
  // Each wipes `password` before it returns, whatever happens.
  void SubmitSignIn(std::string_view email, std::string& password);
  void SubmitMakeAccount(std::string_view email, std::string& password,
                         std::string_view display_name);
  void Continue();
  // The screen was closed. From then on the flow saves nothing: a call in flight drops the session
  // and the keys it holds instead of writing them. Any thread, also while another runs a call.
  void Cancel() { m_cancelled.store(true, std::memory_order_release); }
  bool Cancelled() const { return m_cancelled.load(std::memory_order_acquire); }

  const Client& AccountClient() const { return m_client; }
  // Where the flow saved user.json once it is Done; empty before.
  std::string UserJsonPath() const;

private:
  void AfterSession(const SessionOutcome& outcome);
  void Fail(const Outcome& outcome);
  // Right before a save: when cancelled, wipes the held session and returns true.
  bool StopIfCancelled();

  Client m_client;
  Store m_store;
  Screen m_screen = Screen::SignIn;
  std::string m_email;
  std::string m_display_name;
  std::string m_code_start;
  std::string m_uid;
  std::string m_message;
  std::string m_server_message;
  Error m_error = Error::None;
  std::atomic<bool> m_cancelled = false;
};

// The user.json this PC keeps for the YouGame player `player_id`, or empty when none is stored or
// the id is empty or a dev id. `user_dir` as for Store::For.
std::string UserJsonPath(std::string_view player_id, std::string user_dir = {});

// The screen for the YouGame player `player_id` named `yougame_name`, on the production transport.
// Nothing for an empty or dev id, and nothing once this PC holds the player's user.json
// (UserJsonPath is then not empty). Call Start on a worker thread to show its first state.
std::unique_ptr<SignInFlow> OpenSignIn(std::string_view player_id, std::string_view yougame_name);
// The same on `transport`, under `user_dir` (tests).
std::unique_ptr<SignInFlow> OpenSignIn(std::string_view player_id, std::string_view yougame_name,
                                       std::unique_ptr<Transport> transport,
                                       std::string user_dir);
}  // namespace Orca::BrawlOnline
