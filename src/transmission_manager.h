/*
 * @Author: DI JUNKUN
 * @Date: 2025-06-26
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _TRANSMISSION_MANAGER_H_
#define _TRANSMISSION_MANAGER_H_

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <websocketpp/server.hpp>

struct TransmissionSnapshot {
  std::string transmission_id;
  std::string host_id;
  std::vector<std::string> guest_ids;
  size_t participant_count = 0;
  bool active = false;
};

class TransmissionManager {
 public:
  TransmissionManager();
  ~TransmissionManager();

  bool IsTransmissionExist(const std::string& transmission_id);
  bool ReleaseTransmission(const std::string& transmission_id);

  std::string IsHost(const std::string& user_id);
  std::string IsGuest(const std::string& user_id);
  bool IsHostOfTransmission(const std::string& user_id,
                            const std::string& transmission_id);

  std::vector<std::string> GetAllUserIdOfTransmission(
      const std::string& transmission_id);

  std::vector<TransmissionSnapshot> GetTransmissionSnapshots();
  std::vector<TransmissionSnapshot> GetTransmissionSnapshots(
      size_t limit, size_t offset, const std::string& search,
      size_t* filtered_count);

  std::string GetHostIdOfTransmission(const std::string& transmission_id);

  bool BindHostToTransmission(const std::string& host_id,
                              const std::string& transmission_id);
  bool BindGuestToTransmission(const std::string& guest_id,
                               const std::string& transmission_id);
  bool BindUserToWsHandle(const std::string& user_id,
                          websocketpp::connection_hdl hdl);
  void SetRemoteControlSessionCallback(
      std::function<void(const std::string&, const std::string&,
                         const std::string&, bool)>
          callback);
  void SetSessionTimeoutCallback(
      std::function<void(websocketpp::connection_hdl, const std::string&)> callback);

  bool ReleaseGuestFromTransmission(const std::string& guest_id);
  bool DisconnectTransmission(const std::string& transmission_id);
  size_t PruneDisconnectedTransmissions();
  std::string ReleaseUserSession(websocketpp::connection_hdl hdl);
  std::string ReleaseUserFromWsHandle(websocketpp::connection_hdl hdl);
  void RemoveWsHandleLastActiveTime(websocketpp::connection_hdl hdl);

  websocketpp::connection_hdl GetWsHandle(const std::string& user_id);
  std::string GetUserId(websocketpp::connection_hdl hdl);

  int UpdateWsHandleLastActiveTime(websocketpp::connection_hdl hdl);
  void ExpireInactiveSessions(std::chrono::steady_clock::time_point now =
                                 std::chrono::steady_clock::now());
  size_t GetActiveConnectionCount();

 private:
  void AliveChecker();

 private:
  std::map<std::string, std::string> transmission_host_id_list_;
  std::map<std::string, std::vector<std::string>> transmission_guest_id_list_;
  std::map<std::string, websocketpp::connection_hdl> user_id_ws_hdl_list_;
  std::map<websocketpp::connection_hdl, std::string,
           std::owner_less<websocketpp::connection_hdl>>
      ws_hdl_user_id_list_;
  std::map<websocketpp::connection_hdl, std::chrono::steady_clock::time_point,
           std::owner_less<websocketpp::connection_hdl>>
      ws_hdl_last_active_time_map_;
  std::function<void(const std::string&, const std::string&,
                     const std::string&, bool)>
      remote_control_session_callback_;
  std::function<void(websocketpp::connection_hdl, const std::string&)>
      session_timeout_callback_;

  std::thread ws_hdl_alive_checker_;
  std::recursive_mutex ws_hdl_alive_checker_mutex_;
  std::condition_variable_any ws_hdl_alive_checker_cv_;
  std::atomic<bool> exit_alive_checker_{false};
};

#endif
