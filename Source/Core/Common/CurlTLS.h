// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string_view>

#include <curl/curl.h>

#include "Common/FileUtil.h"

namespace Common
{
// Bundled libcurl uses mbedTLS, which has no notion of the system certificate store: point it at the
// system CA bundle. (macOS's system libcurl lacks WebSocket support, so Orca uses the bundled one;
// Windows builds of curl use the system store through Schannel, and other TLS backends find theirs.)
inline void ConfigureCurlTLS(CURL* curl)
{
  const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
  if (!info || !info->ssl_version)
    return;
  const std::string_view backend(info->ssl_version);
  // Schannel checks revocation and by default fails closed when the CRL or OCSP server can't be
  // reached (CRYPT_E_NO_REVOCATION_CHECK, CRYPT_E_REVOCATION_OFFLINE), which captive portals,
  // filtering proxies and some corporate networks cause. Revoked certificates are still refused.
  if (backend.find("Schannel") != std::string_view::npos)
  {
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
    return;
  }
  if (backend.find("mbedTLS") == std::string_view::npos)
    return;
  static constexpr std::array<const char*, 4> bundles{
      "/etc/ssl/cert.pem",                   // macOS, some BSDs
      "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu, Arch
      "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora, RHEL
      "/etc/ssl/ca-bundle.pem",              // openSUSE
  };
  for (const char* bundle : bundles)
  {
    if (File::Exists(bundle))
    {
      curl_easy_setopt(curl, CURLOPT_CAINFO, bundle);
      return;
    }
  }
}
}  // namespace Common
