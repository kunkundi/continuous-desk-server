#include "ice_server_config_issuer.h"

#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <regex>
#include <stdexcept>

IceServerConfigIssuer::IceServerConfigIssuer(const nlohmann::json& servers,
                                             const std::string& default_secret,
                                             uint32_t ttl_seconds)
    : ttl_seconds_(ttl_seconds) {
  using nlohmann::json;
  if (!servers.is_array() || servers.size() > 16 || ttl_seconds < 60 ||
      ttl_seconds > 86400)
    throw std::invalid_argument(
        "Invalid ICE server configuration or credential TTL");
  const std::regex uri(
      R"(^(stun|turn):(\[[0-9A-Za-z:.%_-]+\]|[A-Za-z0-9._-]+)(:([0-9]{1,5}))?(\?transport=(udp|tcp))?$)");
  std::map<std::string, std::string> seen;
  size_t stun_count = 0, turn_count = 0, url_count = 0;
  for (const auto& entry : servers) {
    if (!entry.is_object() || !entry.contains("urls"))
      throw std::invalid_argument("ICE server entry requires urls");
    auto urls = entry["urls"];
    if (urls.is_string()) urls = json::array({urls});
    if (!urls.is_array() || urls.empty())
      throw std::invalid_argument("Invalid ICE urls");
    const auto secret = entry.value("shared_secret", default_secret);
    Server server;
    server.urls = json::array();
    std::string credential_host;
    uint16_t credential_port = 0;
    for (const auto& value : urls) {
      if (++url_count > 32 || !value.is_string())
        throw std::invalid_argument("Too many or invalid ICE urls");
      const auto url = value.get<std::string>();
      if (url.rfind("turns:", 0) == 0)
        throw std::invalid_argument(
            "TURN/TLS is not supported by the current libnice backend");
      std::smatch match;
      if (url.size() > 512 || !std::regex_match(url, match, uri))
        throw std::invalid_argument("Invalid ICE URL; use stun: or turn:");
      const bool turn = match[1] != "stun";
      if (!turn && match[5].matched)
        throw std::invalid_argument("Unsupported ICE URL transport");
      const int port = match[4].matched ? std::stoi(match[4]) : 3478;
      if (port == 0 || port > 65535)
        throw std::invalid_argument("Invalid ICE server port");
      std::string host = match[2];
      if ((host.front() == '[' && host.find(':') == std::string::npos) ||
          host.size() + 1 + std::to_string(port).size() > 255)
        throw std::invalid_argument("Invalid or oversized ICE server address");
      // Normalize default ports/transports and DNS case before deduplication.
      const auto zone = host.find('%');
      std::transform(
          host.begin(),
          zone == std::string::npos ? host.end() : host.begin() + zone,
          host.begin(), [](unsigned char c) { return std::tolower(c); });
      const std::string canonical =
          match[1].str() + ":" + host + ":" + std::to_string(port) +
          (turn ? (match[6] == "tcp" ? "?transport=tcp" : "?transport=udp")
                : "");
      if (turn && secret.empty())
        throw std::invalid_argument("TURN endpoint requires a shared secret");
      auto [existing, inserted] = seen.emplace(canonical, turn ? secret : "");
      if (!inserted) {
        if (turn && existing->second != secret)
          throw std::invalid_argument(
              "Duplicate TURN endpoint has conflicting shared secrets");
        continue;
      }
      if (turn ? ++turn_count > 8 : ++stun_count > 8)
        throw std::invalid_argument(
            "ICE list exceeds 8 STUN or 8 TURN endpoints");
      if (turn && credential_host.empty()) {
        credential_host =
            host.front() == '[' ? host.substr(1, host.size() - 2) : host;
        credential_port = static_cast<uint16_t>(port);
      }
      server.urls.push_back(canonical);
    }
    if (!credential_host.empty())
      server.credentials.emplace(secret, credential_host, credential_port,
                                 ttl_seconds_);
    if (!server.urls.empty()) servers_.push_back(std::move(server));
  }
}

nlohmann::json IceServerConfigIssuer::Issue(
    const std::string& recipient) const {
  using nlohmann::json;
  unsigned char random[16];
  if (RAND_bytes(random, sizeof(random)) != 1)
    throw std::runtime_error("Cannot create ICE credential identifier");
  const char* hex = "0123456789abcdef";
  std::string id;
  for (auto byte : random) {
    id += hex[byte >> 4];
    id += hex[byte & 15];
  }
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  json message;
  json ice = {{"version", 1},
              {"id", id},
              {"expires_at", now + ttl_seconds_},
              {"servers", json::array()}};
  for (const auto& server : servers_) {
    json entry = {{"urls", server.urls}};
    if (server.credentials) {
      const auto credentials =
          server.credentials->IssueAt(recipient + ":" + id, now);
      entry["username"] = credentials.username;
      entry["credential"] = credentials.password;
      entry["expires_at"] = credentials.expires_at;
      if (!message.contains("turn")) {
        message["turn"] = {{"host", credentials.host},
                           {"port", credentials.port},
                           {"username", credentials.username},
                           {"password", credentials.password},
                           {"expires_at", credentials.expires_at}};
      }
    }
    ice["servers"].push_back(std::move(entry));
  }
  message["ice"] = std::move(ice);
  return message;
}
