/*
 * @Author: DI JUNKUN
 * @Date: 2026-02-28
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _PRESENCE_MANAGER_H_
#define _PRESENCE_MANAGER_H_

#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <websocketpp/server.hpp>

#include "device_db_manager.h"

using nlohmann::json;

struct OnlineSession {
  std::string user_id;
  std::string device_id;
  websocketpp::connection_hdl hdl;
};

class PresenceManager {
 public:
  void SetDeviceDB(DeviceDBManager* db) { db_ = db; }
  void SetSendMsgCallback(
      std::function<void(websocketpp::connection_hdl, json)> send_msg) {
    send_msg_ = send_msg;
  }
  void SetSendToDeviceCallback(
      std::function<void(const std::string&, json)> send_to_device) {
    send_to_device_ = send_to_device;
  }

  void OnLogin(const std::string& user_id, const std::string& device_id,
               websocketpp::connection_hdl hdl);
  void OnLogout(const std::string& device_id);
  bool IsOnline(const std::string& device_id) const;
  size_t GetOnlineDeviceCount() const;
  size_t GetOnlineWebClientCount() const;
  void SetDeviceNetworkInfo(const std::string& device_id,
                            const ClientNetworkInfo& network_info);
  bool GetDeviceNetworkInfo(const std::string& device_id,
                            ClientNetworkInfo* network_info) const;
  bool HasDeviceWithClientIp(const std::string& client_ip) const;
  size_t UpdateDevicesWithClientIp(const std::string& client_ip,
                                   const ClientNetworkInfo& network_info);
  ClientGeoDistribution GetClientGeoDistribution() const;
  std::vector<std::pair<std::string, bool>> BatchQuery(
      const std::vector<std::string>& device_ids) const;
  void NotifyUserDevices(const std::string& user_id,
                         const std::string& changed_device_id, bool online);
  void UpdateUserDevices(const std::string& user_id,
                         const std::vector<std::string>& device_ids,
                         bool replace = true);

 private:
  std::function<void(websocketpp::connection_hdl, json)> send_msg_;
  DeviceDBManager* db_ = nullptr;
  std::function<void(const std::string&, json)> send_to_device_;
  mutable std::mutex associations_mutex_;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      associations_;
  mutable std::mutex online_devices_mutex_;
  std::unordered_set<std::string> online_devices_;
  std::unordered_set<std::string> online_web_clients_;
  mutable std::mutex network_info_mutex_;
  std::unordered_map<std::string, ClientNetworkInfo> device_network_info_;
};

#endif
