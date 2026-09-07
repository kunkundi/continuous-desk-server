/*
 * @Author: DI JUNKUN
 * @Date: 2025-06-26
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SIGNAL_SERVER_H_
#define _SIGNAL_SERVER_H_

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <websocketpp/config/asio.hpp>
#include <websocketpp/http/constants.hpp>
#include <websocketpp/server.hpp>

#include "admin_auth.h"
#include "admin_controller.h"
#include "device_db_manager.h"
#include "geo_location_resolver.h"
#include "presence_manager.h"
#include "signal_negotiation.h"

using nlohmann::json;

typedef websocketpp::server<websocketpp::config::asio_tls> server;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context>
    context_ptr;
typedef unsigned int connection_id;

class SignalServer {
 public:
  SignalServer();
  SignalServer(uint16_t port, std::string certs_dir, std::string db_path);
  ~SignalServer();

  bool OnOpen(websocketpp::connection_hdl hdl);
  bool OnClose(websocketpp::connection_hdl hdl);
  bool OnFail(websocketpp::connection_hdl hdl);
  void OnHttp(websocketpp::connection_hdl hdl);
  context_ptr OnTlsInit(websocketpp::connection_hdl hdl);
  bool OnPing(websocketpp::connection_hdl hdl, std::string s);
  bool OnPong(websocketpp::connection_hdl hdl, std::string s);
  void OnSessionTimeout(websocketpp::connection_hdl hdl,
                        const std::string& device_id);

  void Run();
  void SendMsg(websocketpp::connection_hdl hdl, json message);
  void OnMessage(websocketpp::connection_hdl hdl, server::message_ptr msg);

 private:
  struct GeoIpLookupJob {
    std::string client_ip;
    std::chrono::steady_clock::time_point run_at =
        std::chrono::steady_clock::now();
  };

  struct GeoIpLookupJobLater {
    bool operator()(const GeoIpLookupJob& lhs,
                    const GeoIpLookupJob& rhs) const {
      return lhs.run_at > rhs.run_at;
    }
  };

  void ScheduleRuntimeHeartbeat();
  void ScheduleRecoveredSessionCleanup();
  std::string GetClientIp(websocketpp::connection_hdl hdl);
  void EnqueueClientNetworkInfo(websocketpp::connection_hdl hdl,
                               const std::string& device_id);
  void EnqueueGeoIpLookup(const std::string& client_ip,
                          std::chrono::milliseconds delay);
  void ProcessGeoIpLookup(const GeoIpLookupJob& job);
  void StartClientNetworkInfoWorker();
  void StopClientNetworkInfoWorker();
  void ProcessClientNetworkInfoJobs();

  server server_;
  uint16_t port_ = 9090;
  std::string certs_dir_ = "/var/lib/crossdesk/certs";
  std::string db_path_ = "/var/lib/crossdesk/db/crossdesk-server.db";
  std::map<websocketpp::connection_hdl, connection_id,
           std::owner_less<websocketpp::connection_hdl>>
      ws_connections_;
  std::map<websocketpp::connection_hdl, std::string,
           std::owner_less<websocketpp::connection_hdl>>
      ws_connection_ips_;
  unsigned int ws_connection_id_ = 0;

  std::shared_ptr<TransmissionManager> transmission_manager_;
  std::unique_ptr<DeviceDBManager> device_db_manager_;
  std::unique_ptr<SignalNegotiation> signal_negotiation_;
  std::unique_ptr<GeoLocationResolver> geo_location_resolver_;
  std::unique_ptr<PresenceManager> presence_manager_;
  std::unique_ptr<AdminAuth> admin_auth_;
  std::unique_ptr<AdminController> admin_controller_;

  std::mutex network_info_mutex_;
  std::condition_variable network_info_cv_;
  std::priority_queue<GeoIpLookupJob, std::vector<GeoIpLookupJob>,
                      GeoIpLookupJobLater>
      network_info_jobs_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      pending_ip_lookup_at_;
  std::unordered_map<std::string, int> geo_ip_failure_counts_;
  std::thread network_info_worker_;
  bool network_info_stop_ = false;
};

#endif
