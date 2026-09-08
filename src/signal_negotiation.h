/*
 * @Author: DI JUNKUN
 * @Date: 2025-06-26
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SIGNAL_NEGOTIATION_H_
#define _SIGNAL_NEGOTIATION_H_

#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

#include "device_db_manager.h"
#include "ice_server_config_issuer.h"
#include "transmission_manager.h"
#include "turn_credentials.h"

using nlohmann::json;

class SignalNegotiation {
 public:
  SignalNegotiation(
      std::shared_ptr<TransmissionManager> transmission_manager,
      DeviceDBManager* device_db,
      std::shared_ptr<TurnCredentialIssuer> turn_credential_issuer = nullptr,
      std::shared_ptr<IceServerConfigIssuer> ice_config_issuer = nullptr);
  ~SignalNegotiation();

  void SetSendMsgCallback(
      std::function<void(websocketpp::connection_hdl, json)> send_msg) {
    send_msg_ = send_msg;
  }

  bool login_user(websocketpp::connection_hdl hdl, const json& j);
  bool leave_transmission(websocketpp::connection_hdl hdl, const json& j);
  bool query_user_id_list(websocketpp::connection_hdl hdl, const json& j);
  bool join_transmission(websocketpp::connection_hdl hdl, const json& j);
  bool offer(websocketpp::connection_hdl hdl, const json& j);
  bool answer(websocketpp::connection_hdl hdl, const json& j);
  bool new_candidate(websocketpp::connection_hdl hdl, const json& j);
  bool new_candidate_mid(websocketpp::connection_hdl hdl, const json& j);
  bool change_password(websocketpp::connection_hdl hdl, const json& j);
  bool turn_credentials(websocketpp::connection_hdl hdl, const json& j);
  bool client_info(websocketpp::connection_hdl hdl, const json& j);
  void OnWebClientDisconnect(const std::string& user_id);

 private:
  struct PasswordChangeResult {
    std::string password_fingerprint;
    json response;
  };

  bool AddTurnCredentials(json& message, const std::string& user_id) const;
  void AddLoginIceConfig(json& message, const json& request,
                         const std::string& user_id) const;
  void AddConnectionIceConfig(json& message, const std::string& user_id) const;

  std::shared_ptr<TransmissionManager> transmission_manager_;
  DeviceDBManager* device_db_manager_;
  std::shared_ptr<TurnCredentialIssuer> turn_credential_issuer_;
  std::shared_ptr<IceServerConfigIssuer> ice_config_issuer_;
  std::function<void(websocketpp::connection_hdl, json)> send_msg_;
  std::mutex password_change_mutex_;
  std::unordered_map<std::string, PasswordChangeResult>
      password_change_results_;
  std::deque<std::string> password_change_result_order_;
};

#endif
