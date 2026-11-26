// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/BrawlOnlineSignIn.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <imgui.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
extern char** environ;
#endif

#include "Core/NetPlayProto.h"
#include "Core/Orca/BrawlOnlineAccount.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/TextEntry.h"

#ifdef ORCA_SIGNIN_PREVIEW
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/Probe.h"
#endif

namespace Orca::UX::BrawlOnlineSignIn
{
namespace
{
namespace BO = Orca::BrawlOnline;
using Screen = BO::SignInFlow::Screen;
using TextEntry::Key;

constexpr std::string_view TITLE = "Sign in to Brawl Online";
constexpr std::string_view NOTE =
    "Your password goes only to Brawl Online's servers. YouGame remembers the link on this PC.";
constexpr std::string_view WORKING = "Talking to Brawl Online...";
constexpr std::string_view BROWSER =
    "Brawl Online's page is opening in your browser: press \"Forgot your password?\" there.";

// Sizes, in overlay units.
constexpr float PANEL_MAX_WIDTH = 76;
constexpr float PAD = 3;
constexpr float TITLE_SIZE = 5;
constexpr float TEXT_SIZE = 2.6f;
constexpr float FIELD_HEIGHT = 5.4f;
constexpr float BUTTON_SIZE = 3.0f;
constexpr float GAP = 1.4f;

enum class Item
{
  Email,
  Password,
  Name,
  Primary,   // Sign in, or Create account
  Switch,    // to the other form, or back to Sign in
  Forgot,
  Continue,  // after the email link was clicked
  Cancel,
};

bool IsField(Item item)
{
  return item == Item::Email || item == Item::Password || item == Item::Name;
}

std::vector<Item> ItemsOf(Screen screen)
{
  switch (screen)
  {
  case Screen::MakeAccount:
    return {Item::Email, Item::Password, Item::Name, Item::Primary, Item::Switch, Item::Cancel};
  case Screen::CheckEmail:
    return {Item::Continue, Item::Switch, Item::Cancel};
  default:
    return {Item::Email,  Item::Password, Item::Primary,
            Item::Switch, Item::Forgot,   Item::Cancel};
  }
}

std::string_view LabelOf(Item item, Screen screen)
{
  switch (item)
  {
  case Item::Email:
    return "Email";
  case Item::Password:
    return "Password";
  case Item::Name:
    return "Display name";
  case Item::Primary:
    return screen == Screen::MakeAccount ? "Create account" : "Sign in";
  case Item::Switch:
    return screen == Screen::SignIn      ? "Create account" :
           screen == Screen::MakeAccount ? "I have an account" :
                                           "Use another account";
  case Item::Forgot:
    return "Forgot password?";
  case Item::Continue:
    return "Continue";
  case Item::Cancel:
    return "Cancel";
  }
  return "";
}

// What the screen shows, copied from the flow after each call.
struct View
{
  Screen screen = Screen::SignIn;
  BO::Error error = BO::Error::None;
  std::string message;
  std::string server_message;
};

struct Session
{
  std::unique_ptr<BO::SignInFlow> flow;
  Done done;
  // Set while a worker runs the flow. The video thread touches the flow only while it is clear.
  std::atomic<bool> busy = false;

  // The video thread's own, except that Close wipes the three fields from any thread. typed_lock
  // guards them: Draw holds it while it reads or edits them, and Close takes it to wipe them.
  std::mutex typed_lock;
  TextEntry::Field<256> email;
  TextEntry::Field<BO::PASSWORD_MAX * 4 + 1> password;
  TextEntry::Field<BO::DISPLAY_NAME_MAX * 4 + 1> name;
  Screen shown = Screen::SignIn;  // the screen `focus` indexes
  int focus = 0;
  bool focus_changed = true;
  std::string note;  // a line of the screen's own, after Forgot password?

  ~Session() { WipeTyped(); }

  // Holding typed_lock, or once no other thread can reach the session.
  void WipeTyped()
  {
    email.Wipe();
    password.Wipe();
    name.Wipe();
  }

  void Publish()
  {
    std::lock_guard lock(m_view_lock);
    m_view.screen = flow->CurrentScreen();
    m_view.error = flow->LastError();
    m_view.message = flow->Message();
    m_view.server_message = flow->ServerMessage();
  }
  View Snapshot()
  {
    std::lock_guard lock(m_view_lock);
    return m_view;
  }

private:
  std::mutex m_view_lock;
  View m_view;
};

std::mutex s_lock;
std::shared_ptr<Session> s_session;  // guarded by s_lock
// Screens closed off the video thread: its next Draw wipes what was typed into them and ImGui's
// copies (the video thread owns both). Guarded by s_lock.
std::vector<std::shared_ptr<Session>> s_closed;
bool (*s_online_check_for_test)() = nullptr;  // guarded by s_lock

std::shared_ptr<Session> Current()
{
  std::lock_guard lock(s_lock);
  return s_session;
}

// Video thread, holding session.typed_lock: wipes `session`'s fields and ImGui's copies of them.
void Forget(Session& session)
{
  session.WipeTyped();
  TextEntry::ForgetImGuiText();
}

// Closes `session` if it is still the open one. Video thread, holding session->typed_lock.
void CloseIf(const std::shared_ptr<Session>& session)
{
  {
    std::lock_guard lock(s_lock);
    if (s_session != session)
      return;
    s_session.reset();
  }
  // A call still in flight saves nothing now.
  session->flow->Cancel();
  TextEntry::SetCapturing(false);
  Forget(*session);
}

// Runs `job` on the flow on a thread of its own. The thread holds the session, so a screen closed
// meanwhile is freed when the call returns. Every call ends within the transport's timeout.
template <typename Job>
void Run(const std::shared_ptr<Session>& session, Job job)
{
  session->busy.store(true, std::memory_order_release);
  std::thread([session, job = std::move(job)]() mutable {
    job(*session->flow);
    session->Publish();
    session->busy.store(false, std::memory_order_release);
  }).detach();
}

// Opens Brawl Online's password page in the system browser. Only BrawlOnline::FORGOT_PASSWORD_URL
// (IsResetPageUrl): anything else is refused. The constant goes to the opener as one argument with
// no shell between.
void OpenInBrowser(std::string_view url)
{
  if (!BO::IsResetPageUrl(url))
    return;
  std::thread([target = std::string(BO::FORGOT_PASSWORD_URL)] {
#ifdef _WIN32
    const std::wstring wide(target.begin(), target.end());  // ASCII
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
    const char* program = "/usr/bin/open";
#else
    const char* program = "xdg-open";
#endif
    char* argv[] = {const_cast<char*>(program), const_cast<char*>(target.c_str()), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, program, nullptr, nullptr, argv, environ) == 0)
      waitpid(pid, nullptr, 0);
#endif
  }).detach();
}

// The heap copy a worker wipes, so moving it between threads leaves no copy behind.
std::shared_ptr<std::string> Secret(std::string_view text)
{
  return std::make_shared<std::string>(text);
}

// Presses `item` on `screen`. Video thread, with no call in flight.
void Activate(const std::shared_ptr<Session>& s, Item item, Screen screen)
{
  switch (item)
  {
  case Item::Primary:
  {
    s->note.clear();
    auto email = Secret(s->email.View());
    auto password = Secret(s->password.View());
    s->password.Wipe();
    // Ends ImGui's edit and zeroes its copies of the text (the edit buffer, undo, the last field's
    // text), so nothing of the password stays in ImGui or is written back into the field.
    TextEntry::ForgetImGuiText();
    ImGui::SetWindowFocus(nullptr);
    s->focus_changed = true;
    if (screen == Screen::MakeAccount)
    {
      auto name = std::make_shared<std::string>(s->name.View());
      Run(s, [email, password, name](BO::SignInFlow& flow) {
        flow.SubmitMakeAccount(*email, *password, *name);
        BO::Wipe(*email);
      });
    }
    else
    {
      Run(s, [email, password](BO::SignInFlow& flow) {
        flow.SubmitSignIn(*email, *password);
        BO::Wipe(*email);
      });
    }
    return;
  }
  case Item::Switch:
    s->note.clear();
    if (screen == Screen::SignIn)
      s->flow->ShowMakeAccount();
    else
      s->flow->ShowSignIn();
    s->Publish();
    return;
  case Item::Forgot:
    OpenInBrowser(BO::FORGOT_PASSWORD_URL);
    s->note = std::string(BROWSER);
    return;
  case Item::Continue:
    s->note.clear();
    Run(s, [](BO::SignInFlow& flow) { flow.Continue(); });
    return;
  case Item::Cancel:
    CloseIf(s);
    return;
  default:
    return;
  }
}

// The line under the form, and whether it reports a problem.
std::string StatusOf(const Session& s, const View& view, bool busy, bool* problem)
{
  *problem = false;
  if (busy)
    return std::string(WORKING);
  if (!s.note.empty())
    return s.note;
  const bool waiting = view.error == BO::Error::None || view.error == BO::Error::NeedsVerification ||
                       view.error == BO::Error::NotActivated;
  if (view.screen == Screen::CheckEmail && waiting)
    return BO::UserMessage(BO::Error::NeedsVerification);
  if (view.error == BO::Error::None)
    return {};
  *problem = true;
  return view.server_message.empty() ? view.message : view.server_message;
}

ImVec2 ButtonSize(float unit, std::string_view label)
{
  const ImVec2 text = Kit::Measure(Kit::LabelLook(unit, BUTTON_SIZE), label);
  return ImVec2(text.x + 2.8f * unit, text.y + 1.1f * unit);
}

// Lays the panel out from `top`, drawing it when `dl` is set. Returns its height.
float Layout(ImDrawList* dl, const std::shared_ptr<Session>& s, const View& view, bool busy,
             const std::vector<Item>& items, float unit, float centre_x, float top, float width)
{
  const float pad = PAD * unit;
  const float left = centre_x - width / 2 + pad;
  const float inner = width - 2 * pad;
  const ImU32 accent = Kit::Accent();
  const float alpha = busy ? 0.5f : 1.0f;
  float y = top + pad;

  // The title shrinks to the panel's inner width on a small display, never past its edges.
  float title_size = TITLE_SIZE;
  if (const float wide = Kit::Measure(Kit::TitleLook(unit, TITLE_SIZE), TITLE).x; wide > inner)
    title_size *= inner / wide;
  const Kit::TextLook title = Kit::TitleLook(unit, title_size);
  if (dl)
    Kit::Title(dl, ImVec2(centre_x, y), unit, TITLE, title_size, Kit::Align::Centre);
  y += Kit::Measure(title, TITLE).y + GAP * unit;

  const Kit::TextLook text = Kit::LabelLook(unit, TEXT_SIZE);
  const float line = Kit::Measure(text, "Ag").y;
  for (const std::string& part : Kit::Wrap(text, NOTE, inner))
  {
    if (dl)
      Kit::Label(dl, ImVec2(centre_x, y), unit, part, TEXT_SIZE, Kit::Align::Centre,
                 IM_COL32(235, 235, 240, 255));
    y += line;
  }
  y += GAP * unit;

  // The fields: a label, then the box ImGui's InputText edits.
  const float box_h = FIELD_HEIGHT * unit;
  const ImGuiStyle& style = ImGui::GetStyle();
  const float scale = std::max(0.01f, style.FontScaleMain * style.FontScaleDpi);
  for (std::size_t i = 0; i < items.size(); ++i)
  {
    const Item item = items[i];
    if (!IsField(item))
      continue;
    const bool focused = static_cast<int>(i) == s->focus;
    if (dl)
      Kit::Label(dl, ImVec2(left, y), unit, LabelOf(item, view.screen), TEXT_SIZE);
    y += line + 0.4f * unit;
    if (dl)
    {
      const ImVec2 min(left, y), max(left + inner, y + box_h);
      dl->AddRectFilled(min, max, IM_COL32(8, 10, 18, 235), 0.8f * unit);
      dl->AddRect(min, max, focused ? accent : IM_COL32(255, 255, 255, 90), 0.8f * unit, 0,
                  focused ? 0.35f * unit : 0.18f * unit);
      ImGui::PushFont(nullptr, box_h * 0.5f / scale);
      char* buffer = item == Item::Email    ? s->email.text :
                     item == Item::Password ? s->password.text :
                                              s->name.text;
      const std::size_t size = item == Item::Email    ? sizeof(s->email.text) :
                               item == Item::Password ? sizeof(s->password.text) :
                                                        sizeof(s->name.text);
      const char* id = item == Item::Email    ? "##bo-email" :
                       item == Item::Password ? "##bo-password" :
                                                "##bo-name";
      const bool active = TextEntry::Input(id, buffer, size, left, y, inner, box_h,
                                           item == Item::Password,
                                           focused && s->focus_changed && !busy);
      ImGui::PopFont();
      // The field lost the keyboard (a click elsewhere): ask again next frame.
      if (focused && !active && !busy)
        s->focus_changed = true;
      else if (focused)
        s->focus_changed = false;
    }
    y += box_h + GAP * unit;
  }

  // The status line: what's happening, or what went wrong in the server's words.
  bool problem = false;
  const std::string status = StatusOf(*s, view, busy, &problem);
  if (!status.empty())
  {
    for (const std::string& part : Kit::Wrap(text, status, inner))
    {
      if (dl)
        Kit::Label(dl, ImVec2(centre_x, y), unit, part, TEXT_SIZE, Kit::Align::Centre,
                   problem ? IM_COL32(255, 128, 112, 255) : IM_COL32_WHITE);
      y += line;
    }
    y += GAP * unit;
  }

  // The main button on a row of its own, then the rest, side by side when they fit.
  const auto button = [&](std::size_t i, ImVec2 pos) {
    const Item item = items[i];
    const bool focused = static_cast<int>(i) == s->focus;
    const bool main = item == Item::Primary || item == Item::Continue;
    const Kit::Tone tone = focused ? Kit::Tone::Gold : main ? Kit::Tone::Blue : Kit::Tone::Grey;
    const Kit::Box box = Kit::LabelPlate(dl, pos, unit, LabelOf(item, view.screen), BUTTON_SIZE,
                                         Kit::Align::Left, tone, alpha);
    if (focused)
      dl->AddRect(ImVec2(box.min.x - 0.4f * unit, box.min.y - 0.4f * unit),
                  ImVec2(box.max.x + 0.4f * unit, box.max.y + 0.4f * unit), accent, unit, 0,
                  0.3f * unit);
  };
  std::vector<std::size_t> rest;
  for (std::size_t i = 0; i < items.size(); ++i)
  {
    const Item item = items[i];
    if (IsField(item))
      continue;
    if (item == Item::Primary || item == Item::Continue)
    {
      const ImVec2 size = ButtonSize(unit, LabelOf(item, view.screen));
      if (dl)
        button(i, ImVec2(centre_x - size.x / 2, y));
      y += size.y + GAP * unit;
    }
    else
    {
      rest.push_back(i);
    }
  }
  float row_w = 0, row_h = 0;
  for (const std::size_t i : rest)
  {
    const ImVec2 size = ButtonSize(unit, LabelOf(items[i], view.screen));
    row_w += size.x + (row_w > 0 ? GAP * unit : 0);
    row_h = std::max(row_h, size.y);
  }
  if (row_w <= inner)
  {
    float x = centre_x - row_w / 2;
    for (const std::size_t i : rest)
    {
      if (dl)
        button(i, ImVec2(x, y));
      x += ButtonSize(unit, LabelOf(items[i], view.screen)).x + GAP * unit;
    }
    y += row_h;
  }
  else
  {
    for (const std::size_t i : rest)
    {
      const ImVec2 size = ButtonSize(unit, LabelOf(items[i], view.screen));
      if (dl)
        button(i, ImVec2(centre_x - size.x / 2, y));
      y += size.y + 0.6f * unit;
    }
  }
  return y + pad - top;
}
// Makes `flow` the open screen, replacing one already open. Nothing while online play runs.
std::shared_ptr<Session> Show(std::unique_ptr<BO::SignInFlow> flow, Done done)
{
  if (!flow || OnlineActive())
    return nullptr;
  auto session = std::make_shared<Session>();
  session->email.Set(flow->Email());
  session->name.Set(flow->DisplayName());
  session->flow = std::move(flow);
  session->done = std::move(done);
  session->Publish();
  {
    std::lock_guard lock(s_lock);
    if (s_session)
    {
      s_session->flow->Cancel();
      s_closed.push_back(std::move(s_session));
    }
    s_session = session;
  }
  TextEntry::SetCapturing(true);
  return session;
}

#ifdef ORCA_SIGNIN_PREVIEW
// Screenshots only, and only in a build configured with ORCA_SIGNIN_PREVIEW=ON (never a release:
// the package scripts and Tools/orca/bundle-key-guard.py refuse it). ORCA_BO_SIGNIN_PREVIEW=form,
// error or verify opens the screen at the first frame in that state, over canned answers made here
// in-process: nothing reaches the network, and no real account is involved.
constexpr const char* PREVIEW_ENV = "ORCA_BO_SIGNIN_PREVIEW";

class PreviewTransport final : public BO::Transport
{
public:
  explicit PreviewTransport(bool verify) : m_verify(verify) {}
  BO::HttpResult Post(const std::string& path, const std::string&, const std::string&) override
  {
    if (path != "/v1/auth/login")
      return {};
    if (!m_verify)
    {
      return {401, R"({"error":{"code":"bad_credentials","message":"Wrong email or password."}})"};
    }
    return {200, R"({"sessionToken":"preview-session","user":)" + Me() + "}"};
  }
  BO::HttpResult Get(const std::string& path, const std::string&) override
  {
    return path == "/v1/me" ? BO::HttpResult{200, Me()} : BO::HttpResult{};
  }

private:
  static std::string Me()
  {
    return R"({"uid":"00000000-0000-4000-8000-000000000000","email":"player@example.com",)"
           R"("emailVerified":false,"emailVerificationRequired":true,"displayName":"Player",)"
           R"("connectCode":null,"playKey":null,"latestVersion":"1.0","userJson":null})";
  }
  bool m_verify;
};

void OpenPreview()
{
  const std::string mode = Orca::GetEnv(PREVIEW_ENV);
  if (mode.empty() || !TestKnobsAllowed())
    return;
  if (mode != "form" && mode != "error" && mode != "verify")
  {
    WARN_LOG_FMT(CORE, "{}: '{}' is not form, error or verify", PREVIEW_ENV, mode);
    return;
  }
  // A folder of the preview's own per state, so a kept preview session never shows in another.
  const std::string dir = File::GetUserPath(D_CACHE_IDX) + "BrawlOnlinePreview/" + mode + "/";
  auto flow = BO::OpenSignIn("preview", "Player", std::make_unique<PreviewTransport>(mode == "verify"),
                             dir);
  const std::shared_ptr<Session> session = Show(std::move(flow), {});
  if (!session)
    return;
  if (mode == "form")
  {
    Run(session, [](BO::SignInFlow& f) { f.Start(); });
    return;
  }
  session->email.Set("player@example.com");
  Run(session, [](BO::SignInFlow& f) {
    f.Start();
    std::string password = "preview-password";
    f.SubmitSignIn("player@example.com", password);
  });
}
#endif
}  // namespace

bool OnlineActive()
{
  bool (*check)() = nullptr;
  {
    std::lock_guard lock(s_lock);
    check = s_online_check_for_test;
  }
  if (check)
    return check();
  // Any room counts, one still connecting included: neutral pads would reach it once it joins.
  return Orca::Online::HasRoom() || NetPlay::IsNetPlayRunning();
}

void SetOnlineCheckForTest(bool (*check)())
{
  std::lock_guard lock(s_lock);
  s_online_check_for_test = check;
}

std::string Open(std::string_view player_id, std::string_view yougame_name, Done done)
{
  // Signed in on this PC already: no screen, the kept file.
  if (std::string path = BO::UserJsonPath(player_id); !path.empty())
    return path;
  if (OnlineActive())
    return {};
  OpenFlow(BO::OpenSignIn(player_id, yougame_name), std::move(done));
  return {};
}

bool OpenFlow(std::unique_ptr<BrawlOnline::SignInFlow> flow, Done done)
{
  const std::shared_ptr<Session> session = Show(std::move(flow), std::move(done));
  if (!session)
    return false;
  // A kept session goes straight to checking the email (SignInFlow::Start).
  Run(session, [](BO::SignInFlow& f) { f.Start(); });
  return true;
}

bool IsOpen()
{
  std::lock_guard lock(s_lock);
  return s_session != nullptr;
}

void Close()
{
  std::shared_ptr<Session> closed;
  {
    std::lock_guard lock(s_lock);
    if (s_session)
    {
      closed = s_session;
      closed->flow->Cancel();
      s_closed.push_back(std::move(s_session));
    }
  }
  TextEntry::SetCapturing(false);
  // Taken after s_lock is let go: Draw takes typed_lock first, then s_lock.
  if (closed)
  {
    std::lock_guard typed(closed->typed_lock);
    closed->WipeTyped();
  }
}

void Draw()
{
#ifdef ORCA_SIGNIN_PREVIEW
  static bool s_preview_checked = false;
  if (!s_preview_checked)
  {
    s_preview_checked = true;
    OpenPreview();
  }
#endif
  std::vector<std::shared_ptr<Session>> closed;
  {
    std::lock_guard lock(s_lock);
    closed.swap(s_closed);
  }
  for (const std::shared_ptr<Session>& old : closed)
  {
    std::lock_guard typed(old->typed_lock);
    Forget(*old);
  }
  const std::shared_ptr<Session> s = Current();
  if (!s)
    return;
  std::unique_lock typed(s->typed_lock);
  // Online play started under the screen: close it at once. While it captures, the input gate is
  // closed and every pad reads neutral, which a match would send as this player's real input.
  if (OnlineActive())
  {
    CloseIf(s);
    return;
  }
  bool busy = s->busy.load(std::memory_order_acquire);
  View view = s->Snapshot();
  if (!busy && view.screen == Screen::Done)
  {
    const std::string path = s->flow->UserJsonPath();
    Done done = std::move(s->done);
    CloseIf(s);
    typed.unlock();
    if (done)
      done(path);
    return;
  }
  std::vector<Item> items = ItemsOf(view.screen);
  if (view.screen != s->shown)
  {
    s->shown = view.screen;
    s->focus = 0;
    s->focus_changed = true;
  }
  s->focus = std::clamp(s->focus, 0, static_cast<int>(items.size()) - 1);

  for (const TextEntry::Event& event : TextEntry::Drain())
  {
    if (event.key == Key::Escape)
    {
      CloseIf(s);
      return;
    }
    if (busy)
      continue;
    const int n = static_cast<int>(items.size());
    const auto move = [&](int by) {
      s->focus = (s->focus + by + n) % n;
      s->focus_changed = true;
    };
    if (event.key == Key::Tab)
    {
      move(event.shift ? -1 : 1);
    }
    else if (event.key == Key::Down)
    {
      move(1);
    }
    else if (event.key == Key::Up)
    {
      move(-1);
    }
    else if (event.key == Key::Enter)
    {
      const Item item = items[s->focus];
      Activate(s, IsField(item) ? Item::Primary : item, view.screen);
      // The rest of this frame's keys belonged to the screen that was just left.
      break;
    }
    else if (IsField(items[s->focus]))
    {
      TextEntry::FeedImGui(event);
    }
  }
  if (Current() != s)
    return;
  busy = s->busy.load(std::memory_order_acquire);
  view = s->Snapshot();
  items = ItemsOf(view.screen);
  if (view.screen != s->shown)
  {
    s->shown = view.screen;
    s->focus = 0;
    s->focus_changed = true;
  }

  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float unit = OverlayUnit(display.y);
  if (unit <= 0 || !(display.x >= 1))
    return;
  const float width = std::min(display.x - 4 * unit, PANEL_MAX_WIDTH * unit);
  const float centre_x = display.x / 2;
  const float height = Layout(nullptr, s, view, busy, items, unit, centre_x, 0, width);
  const float top = std::max(0.0f, (display.y - height) / 2);

  // A button has the focus: no field keeps ImGui's keyboard.
  if (s->focus_changed && !IsField(items[s->focus]))
  {
    ImGui::SetWindowFocus(nullptr);
    s->focus_changed = false;
  }
  else if (s->focus_changed)
  {
    ImGui::SetNextWindowFocus();
  }
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(display);
  constexpr ImGuiWindowFlags FLAGS = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoScrollWithMouse;
  if (ImGui::Begin("##orca-brawl-online-sign-in", nullptr, FLAGS))
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // The game dims behind the panel.
    dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 150));
    Kit::Plate(dl, ImVec2(centre_x - width / 2, top), ImVec2(centre_x + width / 2, top + height),
               unit, Kit::Tone::Panel);
    Layout(dl, s, view, busy, items, unit, centre_x, top, width);
  }
  ImGui::End();
}
}  // namespace Orca::UX::BrawlOnlineSignIn
