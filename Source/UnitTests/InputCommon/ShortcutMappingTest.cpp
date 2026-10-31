// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "InputCommon/ControlReference/ExpressionParser.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/Shortcuts.h"

namespace
{
class HeldKey final : public ciface::Core::Device::Input
{
public:
  explicit HeldKey(std::string name) : m_name(std::move(name)) {}
  std::string GetName() const override { return m_name; }
  ControlState GetState() const override { return 1.0; }

private:
  std::string m_name;
};
class Keyboard final : public ciface::Core::Device
{
public:
  explicit Keyboard(std::string source) : m_source(std::move(source))
  {
    for (const auto* name : {"Return",   "RETURN",    "Tab",       "TAB",        "Shift",
                             "Ctrl",     "Alt",       "X",         "Left Shift", "Right Control",
                             "LCONTROL", "Shift_L",   "Control_R", "Up Arrow",   "Button S",
                             "ENTER",    "LEFTSHIFT", "LEFTCTRL",  "RIGHTSHIFT", "RIGHTCTRL"})
      AddInput(new HeldKey(name));
  }
  std::string GetSource() const override { return m_source; }
  std::string GetName() const override { return "Keyboard Mouse"; }

private:
  std::string m_source;
};
class Devices final : public ciface::Core::DeviceContainer
{
public:
  explicit Devices(std::string source) { m_devices.push_back(std::make_shared<Keyboard>(source)); }
  const auto& KeyboardDevice() const { return m_devices.front(); }
};
class ShortcutMappingTest : public testing::TestWithParam<const char*>
{
protected:
  void SetUp() override
  {
    auto& shortcuts = InputCommon::Shortcuts::Get();
    shortcuts.SetOutput({});
    shortcuts.FocusLost();
    shortcuts.SetFocus(true);
    ASSERT_TRUE(shortcuts.Configure("embed shortcuts menu=Shift+Tab chat=Ctrl+Enter"));
  }
  void TearDown() override
  {
    InputCommon::Shortcuts::Get().Configure("embed shortcuts");
    InputCommon::Shortcuts::Get().FocusLost();
  }
};
TEST_P(ShortcutMappingTest, ControllerMappingsUseClassifiedWindowEvents)
{
  using namespace InputCommon::Shortcuts;
  Devices devices(GetParam());
  ciface::Core::DeviceQualifier qualifier;
  qualifier.FromDevice(devices.KeyboardDevice().get());
  ciface::ExpressionParser::ControlEnvironment::VariableContainer variables;
  ciface::ExpressionParser::ControlEnvironment environment(devices, qualifier, variables);
  const auto state = [&](const std::string& name) {
    auto result = ciface::ExpressionParser::ParseExpression("`" + name + "`");
    EXPECT_EQ(result.status, ciface::ExpressionParser::ParseStatus::Successful);
    result.expr->UpdateReferences(environment);
    return result.expr->GetValue();
  };
  // Raw hardware already reports every key held. The window must classify Enter before Start
  // can see it, so a CPU poll ahead of the event cannot leak the shortcut's base key.
  EXPECT_EQ(state("Return"), 0.0);
  EXPECT_EQ(state("RETURN"), 0.0);
  EXPECT_EQ(state("Up Arrow"), 1.0);
  Get().KeyEvent("X", 0, true);
  EXPECT_EQ(state("X"), 1.0);
  Get().KeyEvent("Ctrl", Ctrl, true);
  EXPECT_EQ(state("Ctrl"), 1.0);
  Get().KeyEvent("Enter", Ctrl, true);
  for (const auto* name : {"Return", "RETURN", "Ctrl", "Right Control", "LCONTROL", "Control_R"})
    EXPECT_EQ(state(name), 0.0) << name;
  EXPECT_EQ(state("X"), 1.0);
  EXPECT_EQ(state("Up Arrow"), 1.0);
  Get().KeyEvent("Enter", Ctrl, false);
  EXPECT_EQ(state("Ctrl"), 0.0);  // remains swallowed until released
  Get().KeyEvent("Ctrl", 0, false);
  Get().KeyEvent("Shift", Shift, true);
  Get().KeyEvent("Tab", Shift, true);
  for (const auto* name : {"Tab", "TAB", "Shift", "Left Shift", "Shift_L"})
    EXPECT_EQ(state(name), 0.0) << name;
  Get().FocusLost();
  EXPECT_EQ(state("X"), 0.0);
  EXPECT_EQ(state("Return"), 0.0);
}
INSTANTIATE_TEST_SUITE_P(KeyboardBackends, ShortcutMappingTest,
                         testing::Values("Quartz", "DInput", "XInput2", "evdev"));

TEST(ShortcutMapping, GamepadsKeepTheirInputsDuringKeyboardShortcut)
{
  using namespace InputCommon::Shortcuts;
  Get().FocusLost();
  Get().SetFocus(true);
  ASSERT_TRUE(Get().Configure("embed shortcuts chat=Ctrl+Enter"));
  Get().KeyEvent("Enter", Ctrl, true);
  Devices devices("SDL");
  ciface::Core::DeviceQualifier qualifier;
  qualifier.FromDevice(devices.KeyboardDevice().get());
  ciface::ExpressionParser::ControlEnvironment::VariableContainer variables;
  ciface::ExpressionParser::ControlEnvironment environment(devices, qualifier, variables);
  auto result = ciface::ExpressionParser::ParseExpression("`RETURN` | `Button S`");
  result.expr->UpdateReferences(environment);
  EXPECT_EQ(result.expr->GetValue(), 1.0);
  Get().Configure("embed shortcuts");
  Get().FocusLost();
}
}  // namespace
