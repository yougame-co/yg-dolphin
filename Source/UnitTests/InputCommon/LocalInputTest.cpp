// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/LocalInput.h"

namespace
{
class LocalInputEnv
{
public:
  explicit LocalInputEnv(const char* value)
  {
    if (const char* old = std::getenv("ORCA_NO_LOCAL_INPUT"))
      m_old = old;
    Set(value);
  }
  ~LocalInputEnv() { Set(m_old ? m_old->c_str() : nullptr); }

private:
  static void Set(const char* value)
  {
#ifdef _WIN32
    _putenv_s("ORCA_NO_LOCAL_INPUT", value ? value : "");
#else
    if (value)
      setenv("ORCA_NO_LOCAL_INPUT", value, 1);
    else
      unsetenv("ORCA_NO_LOCAL_INPUT");
#endif
  }

  std::optional<std::string> m_old;
};
}  // namespace

TEST(LocalInput, OnlyExactOneDisablesLocalInput)
{
  for (const char* value : {static_cast<const char*>(nullptr), "", "0", "true", "01", "1 "})
  {
    const LocalInputEnv env(value);
    EXPECT_FALSE(InputCommon::LocalInputDisabled());
  }
  const LocalInputEnv env("1");
  EXPECT_TRUE(InputCommon::LocalInputDisabled());
}

TEST(LocalInput, NoBackendsOrDevicesAfterInitializeAndRefresh)
{
  const LocalInputEnv env("1");
  ControllerInterface controllers;
  controllers.Initialize({});
  EXPECT_TRUE(controllers.IsInit());
  EXPECT_EQ(controllers.GetInputBackendCount(), 0u);
  EXPECT_TRUE(controllers.GetAllDevices().empty());
  controllers.Initialize({});
  controllers.RefreshDevices();
  controllers.ChangeWindow(nullptr);
  EXPECT_EQ(controllers.GetInputBackendCount(), 0u);
  EXPECT_TRUE(controllers.GetAllDevices().empty());
  controllers.Shutdown();
  controllers.Initialize({});
  EXPECT_EQ(controllers.GetInputBackendCount(), 0u);
  controllers.Shutdown();
}

TEST(LocalInput, AdapterInitSkipsNativeContextAndConfigCallbacks)
{
  const LocalInputEnv env("1");
  ASSERT_FALSE(GCAdapter::IsInitialized());
  // No Config/System initialization: disabled Init must return before either is consulted.
  GCAdapter::Init();
  GCAdapter::Init();
  EXPECT_FALSE(GCAdapter::IsInitialized());
  GCAdapter::Shutdown();
  EXPECT_FALSE(GCAdapter::IsInitialized());
}

TEST(LocalInput, WiimoteInitializationSkipsScanningAndWait)
{
  const LocalInputEnv env("1");
  // Even the state-load path must return without reading config or starting a scanner.
  WiimoteReal::Initialize(Wiimote::InitializeMode::DO_WAIT_FOR_WIIMOTES);
  WiimoteReal::Initialize(Wiimote::InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES);
}
