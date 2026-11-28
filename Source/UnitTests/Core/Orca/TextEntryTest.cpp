// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
namespace ImStb
{
#include <imstb_textedit.h>
}

#include "Core/Orca/UX/TextEntry.h"

namespace TE = Orca::UX::TextEntry;

namespace
{
std::u32string Typed(const TE::Drained& events)
{
  std::u32string text;
  for (const TE::Event& event : events)
  {
    if (event.key == TE::Key::NoKey)
      text += event.point;
  }
  return text;
}

struct Capture
{
  Capture() { TE::SetCapturing(true); }
  ~Capture() { TE::SetCapturing(false); }
};
}  // namespace

TEST(OrcaTextEntry, NothingQueuesWhileNotCapturing)
{
  TE::SetCapturing(false);
  TE::Press(TE::Key::Enter, false);
  TE::Type("abc");
  TE::TypeUtf16('x');
  EXPECT_FALSE(TE::Capturing());
  TE::SetCapturing(true);
  EXPECT_TRUE(TE::Drain().empty());
  TE::SetCapturing(false);
}

TEST(OrcaTextEntry, ClosingDropsWhatWasNotDrained)
{
  {
    Capture capture;
    TE::Type("secret");
  }
  Capture capture;
  EXPECT_TRUE(TE::Drain().empty());
}

TEST(OrcaTextEntry, KeysAndTextInOrder)
{
  Capture capture;
  TE::Type("a");
  TE::Press(TE::Key::Tab, true);
  TE::Type("b");
  const TE::Drained events = TE::Drain();
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[0].point, U'a');
  EXPECT_EQ(events[1].key, TE::Key::Tab);
  EXPECT_TRUE(events[1].shift);
  EXPECT_EQ(events[2].point, U'b');
  EXPECT_TRUE(TE::Drain().empty());
}

TEST(OrcaTextEntry, Utf8DecodesAndDropsWhatCantBeTyped)
{
  Capture capture;
  TE::Type("h\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8E\xAE");  // h é € 🎮
  EXPECT_EQ(Typed(TE::Drain()), U"hé€\U0001F3AE");
  TE::Type("\x01\t\r\x7F");                // control characters
  TE::Type("\xC0\xAF");                    // overlong '/'
  TE::Type("\xED\xA0\x80");                // a surrogate
  TE::Type("\xEF\x9C\x80");                // U+F700, a macOS function key
  TE::Type("\xC2\x85");                    // C1 control
  TE::Type("\xE2\x82");                    // cut short
  EXPECT_EQ(Typed(TE::Drain()), U"");
  TE::Type("\xFFok");
  EXPECT_EQ(Typed(TE::Drain()), U"ok");
}

TEST(OrcaTextEntry, Utf16JoinsSurrogatePairs)
{
  Capture capture;
  TE::TypeUtf16(0xD83C);
  TE::TypeUtf16(0xDFAE);
  TE::TypeUtf16(0xDFAE);  // a low half alone
  TE::TypeUtf16(0x08);    // backspace's character
  TE::TypeUtf16(U'z');
  EXPECT_EQ(Typed(TE::Drain()), U"\U0001F3AEz");
}

TEST(OrcaTextEntry, PlatformKeys)
{
  EXPECT_EQ(TE::KeyFromWin32(0x09), TE::Key::Tab);
  EXPECT_EQ(TE::KeyFromWin32(0x0D), TE::Key::Enter);
  EXPECT_EQ(TE::KeyFromWin32(0x1B), TE::Key::Escape);
  EXPECT_EQ(TE::KeyFromWin32(0x08), TE::Key::Backspace);
  EXPECT_EQ(TE::KeyFromWin32('A'), TE::Key::NoKey);
  EXPECT_EQ(TE::KeyFromMac(0x30), TE::Key::Tab);
  EXPECT_EQ(TE::KeyFromMac(0x4C), TE::Key::Enter);
  EXPECT_EQ(TE::KeyFromMac(0x33), TE::Key::Backspace);
  EXPECT_EQ(TE::KeyFromMac(0x00), TE::Key::NoKey);  // A
  EXPECT_EQ(TE::KeyFromX11(0xFE20), TE::Key::Tab);
  EXPECT_EQ(TE::KeyFromX11(0xFF0D), TE::Key::Enter);
  EXPECT_EQ(TE::KeyFromX11(0xFF1B), TE::Key::Escape);
  EXPECT_EQ(TE::KeyFromX11('a'), TE::Key::NoKey);
}

TEST(OrcaTextEntry, FieldHoldsOneLineAndWipes)
{
  TE::Field<4> field;
  EXPECT_EQ(field.View(), "");
  field.Set("abcdef");
  EXPECT_EQ(field.View(), "abc");
  field.Set("x");
  EXPECT_EQ(field.View(), "x");
  EXPECT_EQ(field.text[1], '\0');
  field.Wipe();
  for (const char c : field.text)
    EXPECT_EQ(c, '\0');
}

// The platform loops' one gate: nothing is kept from the game unless a screen captures, and a
// release always reaches it.
TEST(OrcaTextEntry, SwallowsOnlyPressesWhileCapturing)
{
  static_assert(!TE::Swallows(false, false));
  EXPECT_FALSE(TE::Swallows(false, false));
  EXPECT_FALSE(TE::Swallows(false, true));
  EXPECT_FALSE(TE::Swallows(true, true));
  EXPECT_TRUE(TE::Swallows(true, false));
}

TEST(OrcaTextEntry, WipeBytesZeroesEveryByte)
{
  char bytes[9] = "password";
  TE::WipeBytes(bytes, sizeof(bytes));
  for (const char c : bytes)
    EXPECT_EQ(c, '\0');
  TE::WipeBytes(nullptr, 0);
}

namespace
{
bool Holds(const char* data, std::size_t size, std::string_view text)
{
  if (!data || size < text.size())
    return false;
  for (std::size_t i = 0; i + text.size() <= size; ++i)
  {
    if (std::memcmp(data + i, text.data(), text.size()) == 0)
      return true;
  }
  return false;
}

template <typename T>
bool Holds(const ImVector<T>& vector, std::string_view text)
{
  return Holds(reinterpret_cast<const char*>(vector.Data),
               static_cast<std::size_t>(vector.Capacity) * sizeof(T), text);
}

// One frame with the password field drawn through TextEntry::Input.
void PasswordFrame(TE::Field<32>& field, bool focus)
{
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280, 720);
  io.DeltaTime = 1.0f / 60;
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("##orca-text-entry-test", nullptr, ImGuiWindowFlags_NoDecoration);
  TE::Input("##pw", field.text, sizeof(field.text), 10, 10, 400, 40, true, focus);
  ImGui::End();
  ImGui::Render();
}
}  // namespace

// What ImGui keeps of a typed password (the edit buffer, its revert copy, the undo record of
// deleted text, the last field's text) is zeroed, and the edit ends.
TEST(OrcaTextEntry, ForgetImGuiTextZeroesImGuisCopies)
{
  ImGuiContext* context = ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
  io.Fonts->AddFontDefault();
  const std::size_t failures = ImGuiAssertFailures();
  TE::Field<32> field;
  PasswordFrame(field, true);
  PasswordFrame(field, false);
  for (const char c : std::string_view("hunter22xy"))
    io.AddInputCharacter(static_cast<unsigned>(c));
  PasswordFrame(field, false);
  // Two backspaces: "xy" goes into the undo record.
  for (int i = 0; i < 2; ++i)
  {
    io.AddKeyEvent(ImGuiKey_Backspace, true);
    PasswordFrame(field, false);
    io.AddKeyEvent(ImGuiKey_Backspace, false);
    PasswordFrame(field, false);
  }
  ImGuiContext& g = *context;
  ASSERT_EQ(field.View(), "hunter22");
  ASSERT_NE(g.ActiveId, 0u);
  ASSERT_EQ(g.InputTextState.ID, g.ActiveId);
  ASSERT_TRUE(Holds(g.InputTextState.TextA, "hunter22"));
  ASSERT_TRUE(g.InputTextState.Stb);
  const ImStb::StbUndoState& undo = g.InputTextState.Stb->undostate;
  ASSERT_GT(undo.undo_point, 0);

  TE::ForgetImGuiText();
  EXPECT_EQ(g.ActiveId, 0u);
  EXPECT_EQ(g.InputTextState.ID, 0u);
  EXPECT_FALSE(Holds(g.InputTextState.TextA, "hunter"));
  EXPECT_FALSE(Holds(g.InputTextState.TextToRevertTo, "hunter"));
  EXPECT_FALSE(Holds(g.InputTextDeactivatedState.TextA, "hunter"));
  for (const char c : undo.undo_char)
    EXPECT_EQ(c, '\0');
  EXPECT_EQ(undo.undo_point, 0);
  EXPECT_EQ(undo.redo_point, IMSTB_TEXTEDIT_UNDOSTATECOUNT);

  // The next frames read the field's own buffer again: wiped, the box is empty.
  field.Wipe();
  PasswordFrame(field, true);
  PasswordFrame(field, false);
  EXPECT_EQ(field.View(), "");
  EXPECT_FALSE(Holds(g.InputTextState.TextA, "hunter"));
  EXPECT_EQ(ImGuiAssertFailures(), failures);
  ImGui::DestroyContext(context);
}

namespace
{
std::string Hex(std::string_view bytes)
{
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string hex;
  for (const char c : bytes)
  {
    const auto b = static_cast<unsigned char>(c);
    hex += DIGITS[b >> 4];
    hex += DIGITS[b & 15];
  }
  return hex;
}

std::vector<TE::Key> KeysOf(const TE::Drained& events)
{
  std::vector<TE::Key> keys;
  for (const TE::Event& event : events)
  {
    if (event.key != TE::Key::NoKey)
      keys.push_back(event.key);
  }
  return keys;
}
}  // namespace

TEST(OrcaTextEntryApp, TextLinesTypeTheirUtf8)
{
  Capture capture;
  EXPECT_TRUE(TE::AppLine("text " + Hex("hunter2")));
  EXPECT_TRUE(TE::AppLine("text " + Hex("\xc3\xa9 \xf0\x9f\x98\x80")));  // a paste: spaces and any character
  EXPECT_EQ(Typed(TE::Drain()), U"hunter2\u00e9 \U0001F600");
}

TEST(OrcaTextEntryApp, KeyLinesPressTheirKey)
{
  Capture capture;
  for (const char* name : {"tab", "enter", "escape", "backspace", "delete", "left", "right", "up",
                           "down", "home", "end"})
    EXPECT_TRUE(TE::AppLine(std::string("key ") + name));
  EXPECT_TRUE(TE::AppLine("key tab shift"));
  const TE::Drained events = TE::Drain();
  EXPECT_EQ(KeysOf(events),
            (std::vector<TE::Key>{TE::Key::Tab, TE::Key::Enter, TE::Key::Escape,
                                  TE::Key::Backspace, TE::Key::Delete, TE::Key::Left,
                                  TE::Key::Right, TE::Key::Up, TE::Key::Down, TE::Key::Home,
                                  TE::Key::End, TE::Key::Tab}));
  ASSERT_EQ(events.size(), 12u);
  EXPECT_FALSE(events[0].shift);
  EXPECT_TRUE(events[11].shift);
}

TEST(OrcaTextEntryApp, MalformedLinesAreTakenAndDropped)
{
  Capture capture;
  // Each is the app's (true: nobody else gets it), and none types or presses anything.
  for (const std::string line :
       {std::string("text "), std::string("text 6"), std::string("text 6G"),
        std::string("text 4A"), std::string("text 61 62"),
        "text " + std::string((TE::MAX_APP_TEXT + 1) * 2, 'a'), std::string("key "),
        std::string("key f1"), std::string("key TAB"), std::string("key tab ctrl"),
        std::string("key tab shift ")})
    EXPECT_TRUE(TE::AppLine(line)) << line;
  EXPECT_TRUE(TE::Drain().empty());
  // The longest one taken still types.
  EXPECT_TRUE(TE::AppLine("text " + std::string(TE::MAX_APP_TEXT * 2, '6')));
  EXPECT_EQ(TE::Drain().size(), 256u);  // the queue's own limit
}

TEST(OrcaTextEntryApp, NothingIsTypedWithNoScreenOpen)
{
  TE::SetCapturing(false);
  EXPECT_TRUE(TE::AppLine("text " + Hex("secret")));
  EXPECT_TRUE(TE::AppLine("key enter"));
  Capture capture;
  EXPECT_TRUE(TE::Drain().empty());
}

TEST(OrcaTextEntryApp, OtherLinesAreNotTheirs)
{
  for (const char* line : {"show", "quit", "rect 1 2 3 4", "texts", "keys tab", "text", "key",
                           "yg {\"push\":{}}", " text 61"})
    EXPECT_FALSE(TE::AppLine(line)) << line;
}

namespace
{
struct Fed
{
  float x, y;
  unsigned buttons;
};
std::vector<Fed> s_fed;
void Sink(float x, float y, unsigned buttons)
{
  s_fed.push_back({x, y, buttons});
}
struct MouseSink
{
  MouseSink()
  {
    s_fed.clear();
    TE::SetMouseSinkForTest(Sink);
  }
  ~MouseSink() { TE::SetMouseSinkForTest(nullptr); }
};
}  // namespace

// The window's mouse reaches the overlay only while a screen captures, and is let go once after.
TEST(OrcaTextEntry, TheMouseReachesTheOverlayOnlyWhileCapturing)
{
  MouseSink sink;
  TE::Mouse(10, 20, 1);
  EXPECT_TRUE(s_fed.empty());
  {
    Capture capture;
    TE::Mouse(10, 20, 1);
    TE::Mouse(11, 21, 0);
  }
  TE::Mouse(12, 22, 1);
  TE::Mouse(13, 23, 1);
  ASSERT_EQ(s_fed.size(), 3u);
  EXPECT_EQ(s_fed[0].x, 10);
  EXPECT_EQ(s_fed[0].y, 20);
  EXPECT_EQ(s_fed[0].buttons, 1u);
  EXPECT_EQ(s_fed[1].buttons, 0u);
  // The release after capturing ended: off the window, nothing held.
  EXPECT_EQ(s_fed[2].x, -1);
  EXPECT_EQ(s_fed[2].y, -1);
  EXPECT_EQ(s_fed[2].buttons, 0u);
}

// The panel counts as a hit only while a screen captures, and is forgotten when it stops.
TEST(OrcaTextEntry, TheFormIsAHitOnlyWhileCapturing)
{
  {
    Capture capture;
    EXPECT_FALSE(TE::OverForm(50, 50));
    TE::SetFormRect(40, 30, 160, 130);
    EXPECT_TRUE(TE::OverForm(50, 50));
    EXPECT_TRUE(TE::OverForm(40, 30));
    EXPECT_FALSE(TE::OverForm(160, 50));
    EXPECT_FALSE(TE::OverForm(50, 130));
    EXPECT_FALSE(TE::OverForm(10, 50));
  }
  EXPECT_FALSE(TE::OverForm(50, 50));
  {
    Capture capture;
    EXPECT_FALSE(TE::OverForm(50, 50));
  }
}
