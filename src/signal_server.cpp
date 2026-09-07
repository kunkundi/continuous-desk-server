#include "signal_server.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "common.h"
#include "log.h"
#include "signal_negotiation.h"

namespace {

constexpr long kRuntimeHeartbeatIntervalMs = 5000;
constexpr long kRecoveredSessionCleanupDelayMs = 120000;
constexpr size_t kMaxClientNetworkInfoJobs = 1024;
constexpr int kDefaultGeoIpFailureTtlMs = 60000;
constexpr int kDefaultGeoIpFailureMaxTtlMs = 1800000;
constexpr int kDefaultTurnCredentialTtlSeconds = 3600;

int EnvMillis(const char* name, int fallback, int min_value, int max_value) {
  const char* raw = std::getenv(name);
  if (!raw) {
    return fallback;
  }
  char* end = nullptr;
  long value = std::strtol(raw, &end, 10);
  if (end == raw || value < min_value || value > max_value) {
    return fallback;
  }
  return static_cast<int>(value);
}

std::string EnvString(const char* name) {
  const char* raw = std::getenv(name);
  return raw ? raw : "";
}

std::shared_ptr<TurnCredentialIssuer> CreateTurnCredentialIssuer() {
  const std::string secret = EnvString("COTURN_AUTH_SECRET");
  std::string host = EnvString("COTURN_PUBLIC_HOST");
  if (host.empty()) {
    host = EnvString("EXTERNAL_IP");
  }
  if (secret.empty() || host.empty()) {
    LOG_WARN(
        "Dynamic TURN credentials disabled: COTURN_AUTH_SECRET or TURN public "
        "host is empty");
    return nullptr;
  }

  const int port = EnvMillis("COTURN_PORT", 3478, 1, 65535);
  const int ttl_seconds =
      EnvMillis("COTURN_CREDENTIAL_TTL_SECONDS",
                kDefaultTurnCredentialTtlSeconds, 60, 86400);
  LOG_INFO("Dynamic TURN credentials enabled for [{}:{}], TTL [{}] seconds",
           host, port, ttl_seconds);
  return std::make_shared<TurnCredentialIssuer>(
      secret, host, static_cast<uint16_t>(port),
      static_cast<uint32_t>(ttl_seconds));
}

std::chrono::milliseconds GeoIpFailureRetryDelay(int failure_count) {
  int64_t delay_ms =
      EnvMillis("CROSSDESK_GEOIP_FAILURE_TTL_MS",
                kDefaultGeoIpFailureTtlMs, 0, 3600000);
  int64_t max_delay_ms =
      EnvMillis("CROSSDESK_GEOIP_FAILURE_MAX_TTL_MS",
                kDefaultGeoIpFailureMaxTtlMs, 0, 86400000);
  max_delay_ms = std::max(delay_ms, max_delay_ms);
  if (delay_ms <= 0) {
    return std::chrono::milliseconds(0);
  }

  for (int i = 1; i < failure_count && delay_ms < max_delay_ms; ++i) {
    delay_ms = delay_ms > max_delay_ms / 2 ? max_delay_ms : delay_ms * 2;
  }
  return std::chrono::milliseconds(delay_ms);
}

void SetJsonResponse(server::connection_ptr con,
                     websocketpp::http::status_code::value status,
                     const json& body) {
  con->set_status(status);
  con->append_header("Content-Type", "application/json; charset=utf-8");
  con->append_header("Access-Control-Allow-Origin", "*");
  con->append_header("Access-Control-Allow-Methods", "GET, OPTIONS");
  con->append_header("Access-Control-Allow-Headers", "Content-Type");
  con->append_header("Cache-Control", "no-store");
  con->set_body(body.dump());
}

void SetAdminResponse(server::connection_ptr con,
                      const AdminHttpResponse& response) {
  con->set_status(
      static_cast<websocketpp::http::status_code::value>(response.status));
  if (!response.content_type.empty()) {
    con->append_header("Content-Type", response.content_type);
  }
  for (const auto& header : response.headers) {
    con->append_header(header.first, header.second);
  }
  con->set_body(response.body);
}

void RestorePersistedRemoteControlSessions(
    const std::shared_ptr<TransmissionManager>& transmission,
    DeviceDBManager* db) {
  if (!transmission || !db) {
    return;
  }

  size_t restored_connections = 0;
  for (const auto& session : db->ListRemoteControlSessions(
           static_cast<size_t>(std::numeric_limits<int>::max()), 0, "")) {
    transmission->BindHostToTransmission(session.host_id,
                                         session.transmission_id);
    for (const auto& guest_id : session.guest_ids) {
      if (transmission->BindGuestToTransmission(guest_id,
                                                session.transmission_id)) {
        ++restored_connections;
      }
    }
  }
  if (restored_connections > 0) {
    LOG_INFO("Restored {} persisted remote control connection(s)",
             restored_connections);
  }
}

}  // namespace

SignalServer::SignalServer() {
  server_.set_error_channels(websocketpp::log::elevel::none);
  server_.set_access_channels(websocketpp::log::alevel::none);
  server_.init_asio();

  server_.set_open_handler(
      std::bind(&SignalServer::OnOpen, this, std::placeholders::_1));
  server_.set_close_handler(
      std::bind(&SignalServer::OnClose, this, std::placeholders::_1));
  server_.set_fail_handler(
      std::bind(&SignalServer::OnFail, this, std::placeholders::_1));
  server_.set_message_handler(std::bind(&SignalServer::OnMessage, this,
                                        std::placeholders::_1,
                                        std::placeholders::_2));
  server_.set_http_handler(
      std::bind(&SignalServer::OnHttp, this, std::placeholders::_1));
  server_.set_tls_init_handler(
      std::bind(&SignalServer::OnTlsInit, this, std::placeholders::_1));
  server_.set_ping_handler(std::bind(&SignalServer::OnPing, this,
                                     std::placeholders::_1,
                                     std::placeholders::_2));
  server_.set_pong_handler(std::bind(&SignalServer::OnPong, this,
                                     std::placeholders::_1,
                                     std::placeholders::_2));

  transmission_manager_ = std::make_shared<TransmissionManager>();
  device_db_manager_ = std::make_unique<DeviceDBManager>(db_path_);
  transmission_manager_->SetRemoteControlSessionCallback(
      [this](const std::string& transmission_id, const std::string& host_id,
             const std::string& guest_id, bool started) {
        if (!device_db_manager_) {
          return;
        }
        if (started) {
          device_db_manager_->StartRemoteControlSession(transmission_id,
                                                        host_id, guest_id);
        } else {
          device_db_manager_->EndRemoteControlSession(transmission_id, host_id,
                                                      guest_id);
        }
      });
  RestorePersistedRemoteControlSessions(transmission_manager_,
                                        device_db_manager_.get());
  signal_negotiation_ = std::make_unique<SignalNegotiation>(
      transmission_manager_, device_db_manager_.get(),
      CreateTurnCredentialIssuer());
  signal_negotiation_->SetSendMsgCallback(std::bind(&SignalServer::SendMsg,
                                                    this, std::placeholders::_1,
                                                    std::placeholders::_2));
  if (GeoLocationResolver::IsEnabled()) {
    geo_location_resolver_ = std::make_unique<GeoLocationResolver>();
  }
  presence_manager_ = std::make_unique<PresenceManager>();
  presence_manager_->SetSendMsgCallback(std::bind(&SignalServer::SendMsg, this,
                                                  std::placeholders::_1,
                                                  std::placeholders::_2));
  presence_manager_->SetDeviceDB(device_db_manager_.get());
  presence_manager_->SetSendToDeviceCallback(
      [this](const std::string& id, json msg) {
        SendMsg(transmission_manager_->GetWsHandle(id), msg);
      });
  transmission_manager_->SetSessionTimeoutCallback(
      std::bind(&SignalServer::OnSessionTimeout, this, std::placeholders::_1,
                std::placeholders::_2));
  admin_auth_ = std::make_unique<AdminAuth>();
  admin_controller_ = std::make_unique<AdminController>(
      admin_auth_.get(), presence_manager_.get(), transmission_manager_,
      device_db_manager_.get(), [this](const std::string& id, json msg) {
        SendMsg(transmission_manager_->GetWsHandle(id), msg);
      });
  if (geo_location_resolver_) {
    StartClientNetworkInfoWorker();
  }
}

SignalServer::SignalServer(uint16_t port, std::string certs_dir,
                           std::string db_path)
    : port_(port), certs_dir_(certs_dir), db_path_(db_path) {
  LOG_INFO(
      "Starting CrossDesk Signaling Server on port {}, certs_dir: {}, "
      "db_path: {}",
      port_, certs_dir_, db_path_);

  server_.set_error_channels(websocketpp::log::elevel::none);
  server_.set_access_channels(websocketpp::log::alevel::none);
  server_.init_asio();

  server_.set_open_handler(
      std::bind(&SignalServer::OnOpen, this, std::placeholders::_1));
  server_.set_close_handler(
      std::bind(&SignalServer::OnClose, this, std::placeholders::_1));
  server_.set_fail_handler(
      std::bind(&SignalServer::OnFail, this, std::placeholders::_1));
  server_.set_message_handler(std::bind(&SignalServer::OnMessage, this,
                                        std::placeholders::_1,
                                        std::placeholders::_2));
  server_.set_http_handler(
      std::bind(&SignalServer::OnHttp, this, std::placeholders::_1));
  server_.set_tls_init_handler(
      std::bind(&SignalServer::OnTlsInit, this, std::placeholders::_1));
  server_.set_ping_handler(std::bind(&SignalServer::OnPing, this,
                                     std::placeholders::_1,
                                     std::placeholders::_2));
  server_.set_pong_handler(std::bind(&SignalServer::OnPong, this,
                                     std::placeholders::_1,
                                     std::placeholders::_2));

  transmission_manager_ = std::make_shared<TransmissionManager>();
  device_db_manager_ = std::make_unique<DeviceDBManager>(db_path_);
  transmission_manager_->SetRemoteControlSessionCallback(
      [this](const std::string& transmission_id, const std::string& host_id,
             const std::string& guest_id, bool started) {
        if (!device_db_manager_) {
          return;
        }
        if (started) {
          device_db_manager_->StartRemoteControlSession(transmission_id,
                                                        host_id, guest_id);
        } else {
          device_db_manager_->EndRemoteControlSession(transmission_id, host_id,
                                                      guest_id);
        }
      });
  RestorePersistedRemoteControlSessions(transmission_manager_,
                                        device_db_manager_.get());
  signal_negotiation_ = std::make_unique<SignalNegotiation>(
      transmission_manager_, device_db_manager_.get(),
      CreateTurnCredentialIssuer());
  signal_negotiation_->SetSendMsgCallback(std::bind(&SignalServer::SendMsg,
                                                    this, std::placeholders::_1,
                                                    std::placeholders::_2));
  if (GeoLocationResolver::IsEnabled()) {
    geo_location_resolver_ = std::make_unique<GeoLocationResolver>();
  }
  presence_manager_ = std::make_unique<PresenceManager>();
  presence_manager_->SetSendMsgCallback(std::bind(&SignalServer::SendMsg, this,
                                                  std::placeholders::_1,
                                                  std::placeholders::_2));
  presence_manager_->SetDeviceDB(device_db_manager_.get());
  presence_manager_->SetSendToDeviceCallback(
      [this](const std::string& id, json msg) {
        SendMsg(transmission_manager_->GetWsHandle(id), msg);
      });
  transmission_manager_->SetSessionTimeoutCallback(
      std::bind(&SignalServer::OnSessionTimeout, this, std::placeholders::_1,
                std::placeholders::_2));
  admin_auth_ = std::make_unique<AdminAuth>();
  admin_controller_ = std::make_unique<AdminController>(
      admin_auth_.get(), presence_manager_.get(), transmission_manager_,
      device_db_manager_.get(), [this](const std::string& id, json msg) {
        SendMsg(transmission_manager_->GetWsHandle(id), msg);
      });
  if (geo_location_resolver_) {
    StartClientNetworkInfoWorker();
  }
}

SignalServer::~SignalServer() {
  StopClientNetworkInfoWorker();
  if (transmission_manager_) {
    transmission_manager_->SetSessionTimeoutCallback({});
  }
}

std::string SignalServer::GetClientIp(websocketpp::connection_hdl hdl) {
  try {
    server::connection_ptr con = server_.get_con_from_hdl(hdl);
    websocketpp::lib::asio::error_code ec;
    auto endpoint = con->get_raw_socket().remote_endpoint(ec);
    if (ec) {
      LOG_WARN("Failed to get websocket peer endpoint: {}", ec.message());
      return "";
    }
    return endpoint.address().to_string();
  } catch (const std::exception& e) {
    LOG_WARN("Failed to get websocket peer endpoint: {}", e.what());
  }
  return "";
}

void SignalServer::EnqueueClientNetworkInfo(websocketpp::connection_hdl hdl,
                                            const std::string& device_id) {
  if (!presence_manager_ || device_id.empty()) {
    return;
  }

  std::string client_ip;
  auto ip_it = ws_connection_ips_.find(hdl);
  if (ip_it != ws_connection_ips_.end()) {
    client_ip = ip_it->second;
  }
  if (client_ip.empty()) {
    client_ip = GetClientIp(hdl);
  }

  ClientNetworkInfo network_info;
  network_info.client_ip = client_ip;
  presence_manager_->SetDeviceNetworkInfo(device_id, network_info);
  if (geo_location_resolver_) {
    EnqueueGeoIpLookup(client_ip, std::chrono::milliseconds(0));
  }
}

void SignalServer::EnqueueGeoIpLookup(
    const std::string& client_ip, std::chrono::milliseconds delay) {
  if (client_ip.empty()) {
    return;
  }

  const auto run_at = std::chrono::steady_clock::now() +
                      std::max(delay, std::chrono::milliseconds(0));
  {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    if (network_info_stop_) {
      return;
    }
    auto pending = pending_ip_lookup_at_.find(client_ip);
    if (pending != pending_ip_lookup_at_.end()) {
      return;
    }
    if (network_info_jobs_.size() >= kMaxClientNetworkInfoJobs) {
      if (GeoLocationResolver::IsEnabled()) {
        LOG_WARN("GeoIP lookup queue is full, dropping [{}]", client_ip);
      }
      return;
    }
    pending_ip_lookup_at_[client_ip] = run_at;
    network_info_jobs_.push({client_ip, run_at});
  }
  network_info_cv_.notify_one();
}

void SignalServer::ProcessGeoIpLookup(const GeoIpLookupJob& job) {
  {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    auto pending = pending_ip_lookup_at_.find(job.client_ip);
    if (pending == pending_ip_lookup_at_.end() ||
        pending->second != job.run_at) {
      return;
    }
  }

  if (!presence_manager_ ||
      !presence_manager_->HasDeviceWithClientIp(job.client_ip)) {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    pending_ip_lookup_at_.erase(job.client_ip);
    geo_ip_failure_counts_.erase(job.client_ip);
    return;
  }

  GeoLocationResolveResult resolve_result;
  resolve_result.info.client_ip = job.client_ip;
  if (geo_location_resolver_) {
    resolve_result = geo_location_resolver_->ResolveWithRetryInfo(
        job.client_ip);
  }

  if (!presence_manager_->HasDeviceWithClientIp(job.client_ip)) {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    pending_ip_lookup_at_.erase(job.client_ip);
    geo_ip_failure_counts_.erase(job.client_ip);
    return;
  }

  const ClientNetworkInfo& network_info = resolve_result.info;
  if (resolve_result.resolved) {
    {
      std::lock_guard<std::mutex> lock(network_info_mutex_);
      pending_ip_lookup_at_.erase(job.client_ip);
      geo_ip_failure_counts_.erase(job.client_ip);
    }
    size_t updated = presence_manager_->UpdateDevicesWithClientIp(
        job.client_ip, network_info);
    if (GeoLocationResolver::IsEnabled()) {
      LOG_INFO("GeoIP lookup for [{}] resolved [{}] and updated {} client(s)",
               job.client_ip, network_info.location, updated);
    }
    return;
  }

  if (GeoLocationResolver::IsEnabled()) {
    LOG_INFO("GeoIP lookup for [{}] returned Unknown", job.client_ip);
  }
  if (!resolve_result.retryable) {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    pending_ip_lookup_at_.erase(job.client_ip);
    geo_ip_failure_counts_.erase(job.client_ip);
    return;
  }

  int failure_count = 0;
  {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    failure_count = ++geo_ip_failure_counts_[job.client_ip];
  }
  std::chrono::milliseconds retry_delay =
      GeoIpFailureRetryDelay(failure_count);
  if (retry_delay.count() > 0 &&
      presence_manager_->HasDeviceWithClientIp(job.client_ip)) {
    const auto retry_at = std::chrono::steady_clock::now() + retry_delay;
    {
      std::lock_guard<std::mutex> lock(network_info_mutex_);
      pending_ip_lookup_at_.erase(job.client_ip);
      if (!network_info_stop_ &&
          network_info_jobs_.size() < kMaxClientNetworkInfoJobs) {
        pending_ip_lookup_at_[job.client_ip] = retry_at;
        network_info_jobs_.push({job.client_ip, retry_at});
      } else {
        if (GeoLocationResolver::IsEnabled()) {
          LOG_WARN("GeoIP lookup queue is full, dropping [{}]", job.client_ip);
        }
      }
    }
    network_info_cv_.notify_one();
  } else {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    pending_ip_lookup_at_.erase(job.client_ip);
    geo_ip_failure_counts_.erase(job.client_ip);
  }
}

void SignalServer::StartClientNetworkInfoWorker() {
  if (network_info_worker_.joinable()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    network_info_stop_ = false;
  }
  network_info_worker_ =
      std::thread(&SignalServer::ProcessClientNetworkInfoJobs, this);
}

void SignalServer::StopClientNetworkInfoWorker() {
  {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    network_info_stop_ = true;
    decltype(network_info_jobs_) empty_jobs;
    network_info_jobs_.swap(empty_jobs);
    pending_ip_lookup_at_.clear();
    geo_ip_failure_counts_.clear();
  }
  network_info_cv_.notify_all();
  if (network_info_worker_.joinable()) {
    network_info_worker_.join();
  }
}

void SignalServer::ProcessClientNetworkInfoJobs() {
  while (true) {
    GeoIpLookupJob job;
    {
      std::unique_lock<std::mutex> lock(network_info_mutex_);
      while (true) {
        network_info_cv_.wait(lock, [this] {
          return network_info_stop_ || !network_info_jobs_.empty();
        });
        if (network_info_stop_) {
          return;
        }

        const auto run_at = network_info_jobs_.top().run_at;
        const auto now = std::chrono::steady_clock::now();
        if (run_at <= now) {
          job = network_info_jobs_.top();
          network_info_jobs_.pop();
          break;
        }

        network_info_cv_.wait_until(lock, run_at, [this, run_at] {
          return network_info_stop_ ||
                 (!network_info_jobs_.empty() &&
                  network_info_jobs_.top().run_at < run_at);
        });
        if (network_info_stop_) {
          return;
        }
      }
    }

    ProcessGeoIpLookup(job);
  }
}

bool SignalServer::OnOpen(websocketpp::connection_hdl hdl) {
  connection_id conn_id = ws_connection_id_++;
  ws_connections_[hdl] = conn_id;
  std::string client_ip = GetClientIp(hdl);
  ws_connection_ips_[hdl] = client_ip;
  LOG_INFO("Websocket connection [{}] opened from [{}]", conn_id,
           client_ip.empty() ? "Unknown" : client_ip);
  return true;
}

bool SignalServer::OnClose(websocketpp::connection_hdl hdl) {
  std::string user_id = transmission_manager_->ReleaseUserSession(hdl);
  transmission_manager_->RemoveWsHandleLastActiveTime(hdl);
  auto conn_it = ws_connections_.find(hdl);
  connection_id conn_id =
      (conn_it != ws_connections_.end()) ? conn_it->second : 0;
  if (!user_id.empty()) {
    LOG_INFO("Websocket connection [{}|{}] closed", conn_id, user_id);
    // Remove web client from database on disconnect
    if (signal_negotiation_) {
      signal_negotiation_->OnWebClientDisconnect(user_id);
    }
  }
  if (presence_manager_ && !user_id.empty()) {
    presence_manager_->OnLogout(user_id);
  }
  ws_connections_.erase(hdl);
  ws_connection_ips_.erase(hdl);
  return true;
}

bool SignalServer::OnFail(websocketpp::connection_hdl hdl) {
  std::string user_id = transmission_manager_->ReleaseUserSession(hdl);
  transmission_manager_->RemoveWsHandleLastActiveTime(hdl);
  auto conn_it = ws_connections_.find(hdl);
  connection_id conn_id =
      (conn_it != ws_connections_.end()) ? conn_it->second : 0;
  if (!user_id.empty()) {
    LOG_INFO("Websocket connection [{}|{}] failed", conn_id, user_id);
    // Remove web client from database on disconnect
    if (signal_negotiation_) {
      signal_negotiation_->OnWebClientDisconnect(user_id);
    }
  }
  if (presence_manager_ && !user_id.empty()) {
    presence_manager_->OnLogout(user_id);
  }
  ws_connections_.erase(hdl);
  ws_connection_ips_.erase(hdl);
  return true;
}

void SignalServer::OnHttp(websocketpp::connection_hdl hdl) {
  server::connection_ptr con = server_.get_con_from_hdl(hdl);
  const std::string resource = con->get_resource();

  if (admin_controller_ && AdminController::IsAdminRoute(resource)) {
    AdminHttpRequest request;
    request.method = con->get_request().get_method();
    request.resource = resource;
    request.body = con->get_request_body();
    request.cookie = con->get_request_header("Cookie");
    SetAdminResponse(con, admin_controller_->Handle(request));
    return;
  }

  if (resource == "/stats" || resource == "/api/stats") {
    OnlineDurationStats duration_stats;
    if (device_db_manager_) {
      duration_stats = device_db_manager_->GetOnlineDurationStats();
    }
    size_t active_connection_count =
        transmission_manager_
            ? transmission_manager_->GetActiveConnectionCount()
            : 0;
    if (device_db_manager_) {
      active_connection_count = std::max(
          active_connection_count,
          static_cast<size_t>(
              device_db_manager_->CountActiveRemoteControlConnections()));
    }
    json body = {
        {"online_device_count",
         presence_manager_ ? presence_manager_->GetOnlineDeviceCount() : 0},
        {"online_web_client_count",
         presence_manager_ ? presence_manager_->GetOnlineWebClientCount() : 0},
        {"active_connection_count", active_connection_count},
        {"online_duration_seconds", duration_stats.current_online_seconds},
        {"total_online_seconds", duration_stats.total_online_seconds},
        {"total_control_seconds", duration_stats.total_control_seconds},
        {"total_controlled_seconds",
         duration_stats.total_controlled_seconds},
    };
    SetJsonResponse(con, websocketpp::http::status_code::ok, body);
    return;
  }

  SetJsonResponse(
      con, websocketpp::http::status_code::not_found,
      {{"error", "not_found"},
       {"message", "available endpoints: /stats, /api/stats"}});
}

context_ptr SignalServer::OnTlsInit(websocketpp::connection_hdl hdl) {
  namespace asio = websocketpp::lib::asio;
  context_ptr ctx = websocketpp::lib::make_shared<asio::ssl::context>(
      asio::ssl::context::sslv23);

  try {
    ctx->set_options(
        asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
        asio::ssl::context::no_sslv3 | asio::ssl::context::single_dh_use);

    std::string cert_file = certs_dir_ + "/api.crossdesk.cn_bundle.crt";
    std::string key_file = certs_dir_ + "/api.crossdesk.cn.key";

    // Check if certificate files exist
    if (!std::filesystem::exists(cert_file)) {
      LOG_ERROR("Certificate file not found: {}", cert_file);
      throw std::runtime_error("Certificate file not found: " + cert_file);
    }
    if (!std::filesystem::exists(key_file)) {
      LOG_ERROR("Private key file not found: {}", key_file);
      throw std::runtime_error("Private key file not found: " + key_file);
    }

    ctx->use_certificate_chain_file(cert_file);
    ctx->use_private_key_file(key_file, asio::ssl::context::pem);

    SSL_CTX_set_cipher_list(ctx->native_handle(),
                            "ECDHE-ECDSA-AES256-GCM-SHA384:"
                            "ECDHE-RSA-AES256-GCM-SHA384:"
                            "ECDHE-ECDSA-AES128-GCM-SHA256:"
                            "ECDHE-RSA-AES128-GCM-SHA256");
  } catch (std::exception& e) {
    LOG_ERROR("Failed to initialize TLS context: {}", e.what());
    throw;  // Re-throw to prevent invalid context from being used
  }
  return ctx;
}

bool SignalServer::OnPing(websocketpp::connection_hdl hdl, std::string s) {
  transmission_manager_->UpdateWsHandleLastActiveTime(hdl);
  return true;
}

bool SignalServer::OnPong(websocketpp::connection_hdl hdl, std::string s) {
  transmission_manager_->UpdateWsHandleLastActiveTime(hdl);
  return true;
}

void SignalServer::OnSessionTimeout(websocketpp::connection_hdl hdl,
                                     const std::string& device_id) {
  if (!device_id.empty()) {
    LOG_INFO("Device [{}] heartbeat timed out", device_id);
    presence_manager_->OnLogout(device_id);
    signal_negotiation_->OnWebClientDisconnect(device_id);
  }
  // A resumed peer must reconnect and log in, rather than keep receiving
  // pongs on a socket that no longer has an authenticated session.
  websocketpp::lib::error_code ec;
  server_.close(hdl, websocketpp::close::status::going_away,
                "Heartbeat timeout", ec);
}

void SignalServer::ScheduleRuntimeHeartbeat() {
  server_.set_timer(
      kRuntimeHeartbeatIntervalMs,
      [this](websocketpp::lib::error_code const& ec) {
        if (ec) {
          return;
        }
        if (device_db_manager_) {
          device_db_manager_->RecordRuntimeHeartbeat();
        }
        ScheduleRuntimeHeartbeat();
      });
}

void SignalServer::ScheduleRecoveredSessionCleanup() {
  server_.set_timer(
      kRecoveredSessionCleanupDelayMs,
      [this](websocketpp::lib::error_code const& ec) {
        if (ec) {
          return;
        }
        if (!transmission_manager_) {
          return;
        }
        size_t pruned =
            transmission_manager_->PruneDisconnectedTransmissions();
        if (pruned > 0) {
          LOG_INFO("Pruned {} disconnected restored remote control "
                   "connection(s)",
                   pruned);
        }
      });
}

void SignalServer::Run() {
  if (!std::filesystem::exists(certs_dir_)) {
    std::string message = "Certs dir [" + certs_dir_ + "] does not exist";
    LOG_ERROR("{}", message);
    throw std::runtime_error(message);
  }

  // Verify certificate files exist
  std::string cert_file = certs_dir_ + "/api.crossdesk.cn_bundle.crt";
  std::string key_file = certs_dir_ + "/api.crossdesk.cn.key";
  if (!std::filesystem::exists(cert_file)) {
    std::string message = "Certificate file not found: " + cert_file;
    LOG_ERROR("{}", message);
    throw std::runtime_error(message);
  }
  if (!std::filesystem::exists(key_file)) {
    std::string message = "Private key file not found: " + key_file;
    LOG_ERROR("{}", message);
    throw std::runtime_error(message);
  }

  server_.set_reuse_addr(true);
  LOG_INFO("Signal server starting on port [{}]", port_);
  LOG_INFO("Certificate directory: [{}]", certs_dir_);
  LOG_INFO("Database path: [{}]", db_path_);

  // Listen on all interfaces (0.0.0.0)
  namespace asio = websocketpp::lib::asio;
  asio::error_code ec;
  server_.listen(asio::ip::tcp::v4(), port_, ec);
  if (ec) {
    std::string message =
        "Failed to listen on port " + std::to_string(port_) + ": " +
        ec.message();
    LOG_ERROR("{}", message);
    throw std::runtime_error(message);
  }
  LOG_INFO("Successfully bound to port [{}]", port_);

  server_.start_accept(ec);
  if (ec) {
    std::string message = "Failed to start accepting connections: " +
                          ec.message();
    LOG_ERROR("{}", message);
    throw std::runtime_error(message);
  }
  LOG_INFO("Signal server listening on port [{}], waiting for connections...",
           port_);

  if (device_db_manager_) {
    device_db_manager_->RecordRuntimeHeartbeat();
  }
  ScheduleRuntimeHeartbeat();
  ScheduleRecoveredSessionCleanup();

  try {
    server_.run();
    LOG_INFO("Server run() returned");
  } catch (std::exception& e) {
    LOG_ERROR("Server error: {}, attempting to restart...", e.what());
    // Try to restart the server
    try {
      server_.stop();
      server_.listen(port_);
      server_.start_accept();
      server_.run();
    } catch (std::exception& e2) {
      LOG_ERROR("Failed to restart server: {}", e2.what());
      throw;
    }
  } catch (...) {
    LOG_ERROR("Unknown error occurred in server");
    throw;
  }
}

void SignalServer::SendMsg(websocketpp::connection_hdl hdl, json message) {
  if (hdl.expired()) {
    LOG_ERROR("Destination hdl invalid, msg: {}", message.dump());
    return;
  }

  try {
    server_.send(hdl, message.dump(), websocketpp::frame::opcode::text);
  } catch (const std::exception& e) {
    LOG_ERROR("Failed to send message: {}", e.what());
  } catch (...) {
    LOG_ERROR("Failed to send message: unknown error");
  }
}

void SignalServer::OnMessage(websocketpp::connection_hdl hdl,
                             server::message_ptr msg) {
  if (!signal_negotiation_) {
    return;
  }

  try {
    std::string payload = msg->get_payload();
    json j;
    try {
      j = json::parse(payload);
    } catch (json::parse_error& e) {
      LOG_ERROR("Failed to parse JSON message: {}", e.what());
      return;
    }

    if (!j.contains("type") || !j["type"].is_string()) {
      LOG_ERROR("Message missing 'type' field");
      return;
    }

    std::string type = j["type"].get<std::string>();

    switch (HASH_STRING_PIECE(type.c_str())) {
      case "ping"_H: {
        if (transmission_manager_) {
          transmission_manager_->UpdateWsHandleLastActiveTime(hdl);
          json message = {{"type", "pong"}};
          SendMsg(hdl, message);
        }
        break;
      }
      case "login"_H:
        signal_negotiation_->login_user(hdl, j);
        if (presence_manager_) {
          std::string id = transmission_manager_->GetUserId(hdl);
          if (!id.empty()) {
            presence_manager_->OnLogin(id, id, hdl);
            EnqueueClientNetworkInfo(hdl, id);
          }
        }
        break;
      case "user_leave_transmission"_H:
        signal_negotiation_->leave_transmission(hdl, j);
        break;
      case "query_user_id_list"_H:
        signal_negotiation_->query_user_id_list(hdl, j);
        break;
      case "join_transmission"_H:
        signal_negotiation_->join_transmission(hdl, j);
        break;
      case "offer"_H:
        signal_negotiation_->offer(hdl, j);
        break;
      case "answer"_H:
        signal_negotiation_->answer(hdl, j);
        break;
      case "new_candidate"_H:
        signal_negotiation_->new_candidate(hdl, j);
        break;
      case "new_candidate_mid"_H:
        signal_negotiation_->new_candidate_mid(hdl, j);
        break;
      case "change_password"_H:
        signal_negotiation_->change_password(hdl, j);
        break;
      case "turn_credentials"_H:
        signal_negotiation_->turn_credentials(hdl, j);
        break;
      case "client_info"_H:
        signal_negotiation_->client_info(hdl, j);
        break;
      case "recent_connections_presence"_H: {
        const std::string user_id = transmission_manager_->GetUserId(hdl);
        if (user_id.empty() ||
            (j.contains("user_id") && j["user_id"] != user_id)) {
          LOG_WARN("Ignore presence request for unauthenticated or mismatched user");
          break;
        }
        if (j.contains("subscribe") && !j["subscribe"].is_boolean()) {
          LOG_WARN("Ignore presence request with invalid subscribe field");
          break;
        }
        std::vector<std::string> device_ids;
        if (j.contains("devices") && j["devices"].is_array()) {
          for (auto& v : j["devices"]) {
            if (v.is_string()) device_ids.push_back(v.get<std::string>());
          }
        }
        if (presence_manager_) {
          // New clients explicitly replace subscriptions or only query.
          // Legacy queries may contain just one device, so merge them.
          if (!j.contains("subscribe") || j["subscribe"].get<bool>()) {
            presence_manager_->UpdateUserDevices(user_id, device_ids,
                                                  j.contains("subscribe"));
          }
          auto statuses = presence_manager_->BatchQuery(device_ids);
          json resp = {{"type", "presence"}, {"devices", json::array()}};
          for (const auto& p : statuses) {
            resp["devices"].push_back({{"id", p.first}, {"online", p.second}});
          }
          SendMsg(hdl, resp);
        }
        break;
      }
      default:
        LOG_WARN("Unknown message type: {}", type);
        break;
    }
  } catch (std::exception& e) {
    LOG_ERROR("Error processing message: {}", e.what());
  } catch (...) {
    LOG_ERROR("Unknown error processing message");
  }
}
