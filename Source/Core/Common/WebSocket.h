// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Common
{
// A WebSocket client for text messages (ws:// and wss://), built on libcurl's WebSocket support.
// Not thread-safe: connect, send and receive from one thread (an I/O thread owns the socket).
class WebSocket final
{
public:
  WebSocket();
  ~WebSocket();
  WebSocket(const WebSocket&) = delete;
  WebSocket& operator=(const WebSocket&) = delete;

  // Opens the connection and completes the upgrade. False, with a reason in *error, on failure.
  bool Connect(const std::string& url, std::chrono::milliseconds timeout, std::string* error);

  // Sends one text message, waiting at most `timeout` for the socket to take it.
  bool SendText(std::string_view text, std::chrono::milliseconds timeout);

  // The next complete text message, waiting at most `timeout`; nullopt on timeout or when the
  // connection closed (check IsOpen).
  std::optional<std::string> ReceiveText(std::chrono::milliseconds timeout);

  bool IsOpen() const;
  // Why the connection closed or failed (empty while open).
  const std::string& Error() const;
  // The status code of the server's close frame (0: none came).
  u16 CloseCode() const;

  // Sends a close frame (best effort) and releases the connection.
  void Close(u16 code = 1000);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
}  // namespace Common
