// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <SFML/Network/IpAddress.hpp>
#include <SFML/Network/TcpListener.hpp>
#include <SFML/Network/TcpSocket.hpp>
#include <gtest/gtest.h>

#include "Common/Analytics.h"
#include "Common/HttpRequest.h"
#include "Common/Network.h"
#include "Common/TraversalClient.h"
#include "Common/WebSocket.h"
#include "Core/HW/EXI/BBA/TAPServerConnection.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayServer.h"
#include "Core/Orca/Session/DirectLink.h"
#include "Core/Orca/Session/Keyframe.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/YouGameRoom.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "Core/PowerPC/GDBStub.h"
#include "Core/Rollback/ReplayStream.h"
#include "UICommon/AutoUpdate.h"

using namespace std::chrono_literals;

namespace
{
class ReplaySource
{
public:
  explicit ReplaySource(const char* value)
  {
    if (const char* old = std::getenv("ORCA_REPLAY_SOURCE"))
      m_old = old;
    Set(value);
  }
  ~ReplaySource() { Set(m_old ? m_old->c_str() : nullptr); }

  static void Set(const char* value)
  {
#ifdef _WIN32
    _putenv_s("ORCA_REPLAY_SOURCE", value ? value : "");
#else
    if (value)
      setenv("ORCA_REPLAY_SOURCE", value, 1);
    else
      unsetenv("ORCA_REPLAY_SOURCE");
#endif
  }

private:
  std::optional<std::string> m_old;
};
}  // namespace

TEST(OrcaOfflineReplay, OnlyProductFileSourcesDisableNetworking)
{
  for (const char* source : {"match.ygr", "./saved match.ygr", "/tmp/match.ygr",
                             "C:\\replays\\match.ygr", "missing-file.ygr", "WS://file.ygr"})
  {
    const ReplaySource replay(source);
    EXPECT_TRUE(Rollback::ReplayStream::OfflineReplay()) << source;
    EXPECT_TRUE(Rollback::ReplayStream::ProductMode());
  }
  for (const char* source : {static_cast<const char*>(nullptr), "", "ws://127.0.0.1/watch/match",
                             "wss://rooms.invalid/watch/match"})
  {
    const ReplaySource replay(source);
    EXPECT_FALSE(Rollback::ReplayStream::OfflineReplay());
  }
}

TEST(OrcaOfflineReplay, HttpWebSocketAndTelemetryNeverConnectEvenToLoopback)
{
  // A listening endpoint distinguishes a refusal before connect from a failed request after it.
  sf::TcpListener listener;
  ASSERT_EQ(listener.listen(0, sf::IpAddress::LocalHost), sf::Socket::Status::Done);
  listener.setBlocking(false);
  const std::string origin = "127.0.0.1:" + std::to_string(listener.getLocalPort());
  Common::HttpRequest http(50ms);  // Also covers a client constructed before playback mode.
  const ReplaySource replay("saved.ygr");
  EXPECT_FALSE(http.Get("http://" + origin));
  EXPECT_FALSE(http.Post("http://" + origin, "report"));
  const std::vector<u8> bytes{1, 2};
  EXPECT_FALSE(http.Post("http://" + origin, bytes));
  std::vector<Common::HttpRequest::Multiform> form{{"report", "test"}};
  EXPECT_FALSE(http.PostMultiform("http://" + origin, form));
  EXPECT_EQ(http.GetLastResponseCode(), 0);

  Common::WebSocket socket;
  std::string error;
  EXPECT_FALSE(socket.Connect("ws://" + origin, 50ms, &error));
  EXPECT_EQ(error, "network off for file replay");
  EXPECT_FALSE(socket.IsOpen());
  Common::HttpAnalyticsBackend analytics("http://" + origin);
  analytics.Send("test report");

  Orca::UX::StartControllerStream("http://" + origin, "offline-fixture");
  double age = 0;
  EXPECT_EQ(Orca::UX::LatestSnapshot(&age), nullptr);
  Orca::UX::StopControllerStream();

  ExpansionInterface::TAPServerConnection tap(origin, [](std::string&&) {}, 1500);
  EXPECT_FALSE(tap.Activate());
  EXPECT_FALSE(tap.IsActivated());

  sf::TcpSocket accepted;
  EXPECT_EQ(listener.accept(accepted), sf::Socket::Status::NotReady);
}

TEST(OrcaOfflineReplay, DirectLinkRefusesBeforeStartingItsWorker)
{
  const ReplaySource replay("saved.ygr");
  EXPECT_FALSE(Orca::Net::DirectOptionsFromEnv());
  Orca::Net::DirectOptions options;
  options.me = "offline-player";
  options.room = "offline-room";
  options.ice.stun_host.clear();
  options.loopback = true;
  Orca::Net::DirectLink direct;
  EXPECT_FALSE(direct.Start(options));
  EXPECT_TRUE(direct.TakeSignals().empty());
  // Refusing the start must leave the object unstarted. A live watch can still start normally.
  ReplaySource::Set("wss://rooms.invalid/watch/match");
  EXPECT_TRUE(direct.Start(options));
  direct.Stop();
}

TEST(OrcaOfflineReplay, RoomsAndTicketRequestsRefuseSynchronously)
{
  const ReplaySource replay("saved.ygr");
  Orca::Net::YouGameRoom room({});
  EXPECT_EQ(room.GetState(), Orca::Net::RoomState::Ended);
  EXPECT_EQ(room.Status(), "network off for file replay");
  EXPECT_FALSE(room.Connected());
  std::string ticket, url, error;
  EXPECT_FALSE(room.FreshTicket(true, &ticket, &url, &error));
  EXPECT_EQ(error, "network off for file replay");
  Orca::Online::Start();
  EXPECT_EQ(Orca::Online::RoomGeneration(), 0u);
}

TEST(OrcaOfflineReplay, KeyframeUploadsDownloadsAndDeletesNeverAskForATicket)
{
  const ReplaySource replay("saved.ygr");
  int tickets = 0;
  auto store = Orca::Net::MakeHttpKeyframeStore(
      [&](bool, std::string*, std::string*, std::string*) {
        ++tickets;
        return false;
      });
  const std::vector<u8> bytes{1, 2, 3};
  const std::string hash = Orca::Net::KeyframeHash(bytes);
  Orca::Net::KeyframeInfo info{1, "replay-1-" + hash.substr(0, 8), bytes.size(), hash, ""};
  std::string error;
  EXPECT_FALSE(store->Put(info, bytes, &error));
  EXPECT_EQ(error, "network off for file replay");
  EXPECT_FALSE(store->Get(info, [](u64, u64) { return true; }, &error));
  EXPECT_EQ(error, "network off for file replay");
  store->Delete(info.id);
  store.reset();
  EXPECT_EQ(tickets, 0);
}

TEST(OrcaOfflineReplay, ClassicNetPlayAndTraversalRefuseBeforeSetup)
{
  const ReplaySource replay("saved.ygr");
  EXPECT_FALSE(Common::EnsureTraversalClient("127.0.0.1", 9, 9, 0));
  Common::ENet::WakeupThread(nullptr);
  EXPECT_FALSE(Common::ENet::SendPacket(nullptr, sf::Packet{}, 0));
  NetPlay::NetTraversalConfig traversal{};
  NetPlay::NetPlayClient client("127.0.0.1", 9, nullptr, "offline", traversal);
  EXPECT_FALSE(client.IsConnected());
  NetPlay::NetPlayServer server(0, false, nullptr, traversal);
  EXPECT_FALSE(server.is_connected);
}

TEST(OrcaOfflineReplay, UpdateCheckReturnsBeforeRequestOrUpdaterLaunch)
{
  class Checker final : public AutoUpdateChecker
  {
  public:
    bool notified = false;

  private:
    void OnUpdateAvailable(const NewVersionInformation&) override { notified = true; }
  } checker;
  const ReplaySource replay("saved.ygr");
  checker.CheckForUpdate("dev", "test-revision", AutoUpdateChecker::CheckType::Automatic);
  EXPECT_FALSE(checker.notified);
}

TEST(OrcaOfflineReplay, DebugListenerRefusesBeforeSocketSetup)
{
  const ReplaySource replay("saved.ygr");
  GDBStub::Init(0);
  EXPECT_FALSE(GDBStub::IsActive());
}
