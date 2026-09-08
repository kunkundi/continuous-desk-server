#include "signal_negotiation.h"

#include <openssl/sha.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

#include "log.h"

namespace {

bool GetStringField(const json& j, const char* key, std::string& value) {
  if (!j.contains(key) || !j[key].is_string()) {
    return false;
  }
  value = j[key].get<std::string>();
  return true;
}

bool CopyOptionalIceUfrag(const json& source, json& destination) {
  const auto field = source.find("ufrag");
  if (field == source.end()) {
    return true;
  }
  if (!field->is_string()) {
    return false;
  }

  // Match the client's bounds without rewriting the ICE generation tag.
  // Missing/empty tags remain compatible with older clients; the receiver
  // checks a non-empty tag against its remote SDP.
  const auto& ufrag = field->get_ref<const std::string&>();
  if (ufrag.size() > 256 || ufrag.find('\0') != std::string::npos ||
      ufrag.find_first_of(" \t\r\n") != std::string::npos) {
    return false;
  }
  destination["ufrag"] = ufrag;
  return true;
}

bool ShouldTrackClientInfo(const std::string& user_id) {
  return !user_id.empty() && user_id.rfind("web-", 0) != 0 &&
         user_id.rfind("C-", 0) != 0;
}

std::string PasswordFingerprint(const std::string& password) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(password.data()),
         password.size(), digest);
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (unsigned char byte : digest) {
    stream << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return stream.str();
}

}  // namespace

SignalNegotiation::SignalNegotiation(
    std::shared_ptr<TransmissionManager> transmission_manager,
    DeviceDBManager* device_db,
    std::shared_ptr<TurnCredentialIssuer> turn_credential_issuer,
    std::shared_ptr<IceServerConfigIssuer> ice_config_issuer)
    : transmission_manager_(transmission_manager),
      device_db_manager_(device_db),
      turn_credential_issuer_(std::move(turn_credential_issuer)),
      ice_config_issuer_(std::move(ice_config_issuer)) {}

SignalNegotiation::~SignalNegotiation() {}

bool SignalNegotiation::AddTurnCredentials(json& message,
                                           const std::string& user_id) const {
  if (user_id.empty()) return false;
  if (ice_config_issuer_) {
    auto config = ice_config_issuer_->Issue(user_id);
    if (!config.contains("turn")) return false;
    message["turn"] = std::move(config["turn"]);
    return true;
  }
  if (!turn_credential_issuer_) return false;

  const TurnCredentials credentials = turn_credential_issuer_->Issue(user_id);
  message["turn"] = {{"host", credentials.host},
                     {"port", credentials.port},
                     {"username", credentials.username},
                     {"password", credentials.password},
                     {"expires_at", credentials.expires_at}};
  return true;
}

void SignalNegotiation::AddLoginIceConfig(json& message, const json& request,
                                          const std::string& user_id) const {
  if (ice_config_issuer_) message["ice_config_version"] = 1;
  const auto version = request.find("ice_config_version");
  if (!ice_config_issuer_ || version == request.end() ||
      !version->is_number_integer() || *version != 1)
    AddTurnCredentials(message, user_id);
}

void SignalNegotiation::AddConnectionIceConfig(
    json& message, const std::string& user_id) const {
  if (ice_config_issuer_) {
    message.update(ice_config_issuer_->Issue(user_id));
  } else {
    AddTurnCredentials(message, user_id);
  }
}

bool SignalNegotiation::login_user(websocketpp::connection_hdl hdl,
                                   const json& j) {
  std::string host_id_with_pwd;
  if (!GetStringField(j, "user_id", host_id_with_pwd)) {
    LOG_ERROR("login_user missing or invalid field: user_id");
    return false;
  }

  std::string host_id;
  std::string password;
  std::string return_host_id;

  if (host_id_with_pwd.find("@") != std::string::npos) {
    host_id = host_id_with_pwd.substr(0, host_id_with_pwd.find("@"));
    password = host_id_with_pwd.substr(host_id_with_pwd.find("@") + 1);
  } else {
    host_id = host_id_with_pwd;
    password = "";
  }

  if (host_id.find("C-") == std::string::npos) {
    DeviceCredential dev_cred =
        device_db_manager_->AddDevice(host_id, password);

    std::string ret_host_id = dev_cred.device_id;
    std::string ret_password = dev_cred.password;
    bool update_password = dev_cred.update;

    // Check if AddDevice failed
    if (ret_host_id.empty()) {
      std::string reason = "Failed to register device";
      if (!host_id.empty() &&
          device_db_manager_->VerifyDevice(host_id, password) == -1) {
        reason = "Incorrect password";
      }
      LOG_ERROR("Failed to add device for host_id [{}]", host_id);
      json message = {{"type", "login"},
                      {"user_id", ""},
                      {"status", "fail"},
                      {"reason", reason}};
      send_msg_(hdl, message);
      return true;
    }

    bool update_success = ret_host_id != "" && update_password;
    bool login_success =
        (ret_host_id != "" && ret_password == "") && !update_password;
    bool register_success =
        (ret_host_id != "" && ret_password != "") && (ret_host_id != host_id);

    if (register_success) {
      LOG_INFO("New client, assign id [{}] to it", ret_host_id);
      return_host_id = ret_host_id + "@" + ret_password;
    } else if (login_success) {
      LOG_INFO("Receive login request with id [{}]", ret_host_id);
      return_host_id = ret_host_id;
    } else if (update_success) {
      LOG_INFO("Client [{}] update password", ret_host_id);
      return_host_id = ret_host_id;
    }

    bool success = transmission_manager_->BindUserToWsHandle(ret_host_id, hdl);
    transmission_manager_->BindHostToTransmission(ret_host_id, ret_host_id);

    if (success) {
      if (ShouldTrackClientInfo(ret_host_id) &&
          !device_db_manager_->UpdateDeviceClientInfo(ret_host_id, "", "")) {
        LOG_WARN("Failed to clear client information for [{}]", ret_host_id);
      }
      json message = {{"type", "login"},
                      {"user_id", return_host_id},
                      {"status", "success"}};
      AddLoginIceConfig(message, j, ret_host_id);
      send_msg_(hdl, message);
    } else {
      json message = {
          {"type", "login"}, {"user_id", return_host_id}, {"status", "fail"}};
      send_msg_(hdl, message);
    }
  } else {
    bool success = transmission_manager_->BindUserToWsHandle(host_id, hdl);
    transmission_manager_->BindHostToTransmission(host_id, host_id);
    LOG_INFO("Receive login request with id [{}]", host_id);

    if (success) {
      json message = {
          {"type", "login"}, {"user_id", host_id}, {"status", "success"}};
      AddLoginIceConfig(message, j, host_id);
      send_msg_(hdl, message);
    } else {
      json message = {
          {"type", "login"}, {"user_id", host_id}, {"status", "fail"}};
      send_msg_(hdl, message);
    }
  }

  return true;
}

bool SignalNegotiation::client_info(websocketpp::connection_hdl hdl,
                                    const json& j) {
  std::string version;
  std::string platform;
  if (!GetStringField(j, "version", version) || version.empty() ||
      version.size() > 64) {
    LOG_ERROR("client_info missing or invalid field: version");
    return false;
  }
  if (!GetStringField(j, "platform", platform) || platform.empty() ||
      platform.size() > 32) {
    LOG_ERROR("client_info missing or invalid field: platform");
    return false;
  }

  const std::string user_id = transmission_manager_->GetUserId(hdl);
  if (user_id.empty()) {
    LOG_WARN("Ignore client_info from unauthenticated connection");
    return false;
  }

  if (!ShouldTrackClientInfo(user_id)) {
    return true;
  }

  if (!device_db_manager_->UpdateDeviceClientInfo(user_id, version,
                                                   platform)) {
    LOG_ERROR("Failed to store client information for [{}]", user_id);
    return false;
  }

  LOG_INFO("Client [{}] reports version [{}] on [{}]", user_id, version,
           platform);
  return true;
}

bool SignalNegotiation::leave_transmission(websocketpp::connection_hdl hdl,
                                           const json& j) {
  std::string transmission_id;
  std::string user_id;
  if (!GetStringField(j, "transmission_id", transmission_id) ||
      !GetStringField(j, "user_id", user_id)) {
    LOG_ERROR("leave_transmission missing required fields");
    return false;
  }

  LOG_INFO("[{}] leaves transmission [{}]", user_id.c_str(),
           transmission_id.c_str());

  json message = {{"type", "user_leave_transmission"},
                  {"transmission_id", transmission_id},
                  {"user_id", user_id}};

  std::vector<std::string> user_id_list =
      transmission_manager_->GetAllUserIdOfTransmission(transmission_id);

  for (const auto& id : user_id_list) {
    if (id != user_id) {
      send_msg_(transmission_manager_->GetWsHandle(id), message);
    }
  }

  // transmission_manager_->ReleaseUserFromWsHandle(hdl);

  bool is_host =
      transmission_manager_->IsHostOfTransmission(user_id, transmission_id);

  if (is_host) {
    transmission_manager_->ReleaseTransmission(transmission_id);
    LOG_INFO("Release transmission [{}] due to host leaves", transmission_id);
  } else {
    transmission_manager_->ReleaseGuestFromTransmission(user_id);
  }

  return true;
}

bool SignalNegotiation::query_user_id_list(websocketpp::connection_hdl hdl,
                                           const json& j) {
  std::string transmission_id_pwd;
  if (!GetStringField(j, "transmission_id", transmission_id_pwd)) {
    LOG_ERROR("query_user_id_list missing or invalid field: transmission_id");
    return false;
  }

  std::string transmission_id;
  std::string password;

  if (transmission_id_pwd.find("@") != std::string::npos) {
    transmission_id =
        transmission_id_pwd.substr(0, transmission_id_pwd.find("@"));
    password = transmission_id_pwd.substr(transmission_id_pwd.find("@") + 1);
  } else {
    transmission_id = transmission_id_pwd;
    password = "";
  }

  int ret = device_db_manager_->VerifyDevice(transmission_id, password);

  if (0 == ret) {
    std::vector<std::string> user_id_list =
        transmission_manager_->GetAllUserIdOfTransmission(transmission_id);

    json message = {{"type", "user_id_list"},
                    {"transmission_id", transmission_id},
                    {"user_id_list", user_id_list},
                    {"status", "success"}};

    send_msg_(hdl, message);
  } else if (-1 == ret) {
    std::vector<std::string> user_id_list;
    json message = {{"type", "user_id_list"},
                    {"transmission_id", transmission_id},
                    {"user_id_list", user_id_list},
                    {"status", "failed"},
                    {"reason", "Incorrect password"}};

    send_msg_(hdl, message);
  } else if (-2 == ret) {
    std::vector<std::string> user_id_list;
    json message = {{"type", "user_id_list"},
                    {"transmission_id", transmission_id},
                    {"user_id_list", user_id_list},
                    {"status", "failed"},
                    {"reason", "No such transmission id"}};

    send_msg_(hdl, message);
  }

  return true;
}

bool SignalNegotiation::join_transmission(websocketpp::connection_hdl hdl,
                                          const json& j) {
  std::string transmission_id_pwd;
  if (!GetStringField(j, "transmission_id", transmission_id_pwd)) {
    LOG_ERROR("join_transmission missing or invalid field: transmission_id");
    return false;
  }

  std::string transmission_id;
  std::string password;

  if (transmission_id_pwd.find("@") != std::string::npos) {
    transmission_id =
        transmission_id_pwd.substr(0, transmission_id_pwd.find("@"));
    password = transmission_id_pwd.substr(transmission_id_pwd.find("@") + 1);
  } else {
    transmission_id = transmission_id_pwd;
    password = "";
  }

  std::string user_id;
  if (!GetStringField(j, "user_id", user_id)) {
    LOG_ERROR("join_transmission missing or invalid field: user_id");
    return false;
  }

  LOG_INFO("[{}] joins transmission [{}]", user_id.c_str(),
           transmission_id.c_str());

  int ret = device_db_manager_->VerifyDevice(transmission_id, password);

  if (0 == ret) {
    std::string host_id =
        transmission_manager_->GetHostIdOfTransmission(transmission_id);
    websocketpp::connection_hdl host_hdl =
        transmission_manager_->GetWsHandle(host_id);

    if (host_id.empty() || host_hdl.expired()) {
      LOG_WARN("Remote [{}] is unavailable, cannot join transmission",
               transmission_id.c_str());
      json message = {{"type", "user_join_transmission"},
                      {"transmission_id", transmission_id},
                      {"status", "failed"},
                      {"reason", "Remote unavailable"}};
      send_msg_(hdl, message);
      return true;
    }

    if (transmission_manager_->GetUserId(hdl) != user_id) {
      LOG_WARN("Reject connection request with unauthenticated sender");
      return false;
    }
    transmission_manager_->BindGuestToTransmission(user_id, transmission_id);

    json message = {{"type", "user_join_transmission"},
                    {"transmission_id", transmission_id},
                    {"user_id", user_id},
                    {"status", "success"}};

    AddConnectionIceConfig(message, host_id);
    send_msg_(host_hdl, message);
  } else if (-1 == ret) {
    LOG_ERROR("Password incorrect for transmission id [{}]",
              transmission_id.c_str());
    json message = {{"type", "user_join_transmission"},
                    {"transmission_id", transmission_id},
                    {"status", "failed"},
                    {"reason", "Incorrect password"}};

    send_msg_(hdl, message);
  } else if (-2 == ret) {
    LOG_ERROR("No such transmission id [{}]", transmission_id.c_str());
    json message = {{"type", "user_join_transmission"},
                    {"transmission_id", transmission_id},
                    {"status", "failed"},
                    {"reason", "No such transmission id"}};

    send_msg_(hdl, message);
  }

  return true;
}

bool SignalNegotiation::offer(websocketpp::connection_hdl hdl, const json& j) {
  std::string transmission_id;
  std::string remote_user_id;
  std::string user_id;
  if (!GetStringField(j, "transmission_id", transmission_id) ||
      !GetStringField(j, "remote_user_id", remote_user_id) ||
      !GetStringField(j, "user_id", user_id)) {
    LOG_ERROR("offer missing required fields");
    return false;
  }

  // Credentials are issued only to authenticated participants of an authorized
  // join.
  const auto host_id =
      transmission_manager_->GetHostIdOfTransmission(transmission_id);
  const auto members =
      transmission_manager_->GetAllUserIdOfTransmission(transmission_id);
  if (user_id == remote_user_id ||
      (user_id != host_id && remote_user_id != host_id) ||
      transmission_manager_->GetUserId(hdl) != user_id ||
      std::find(members.begin(), members.end(), user_id) == members.end() ||
      std::find(members.begin(), members.end(), remote_user_id) ==
          members.end()) {
    LOG_WARN("Reject offer outside an authorized transmission");
    return false;
  }

  websocketpp::connection_hdl destination_hdl =
      transmission_manager_->GetWsHandle(remote_user_id);

  std::string sdp;
  if (GetStringField(j, "sdp", sdp)) {
    json message = {
        {"type", "offer"},
        {"transmission_id", transmission_id},
        {"remote_user_id", user_id},
        {"sdp", sdp},
    };
    AddConnectionIceConfig(message, remote_user_id);
    LOG_INFO("[{}] send offer to [{}]", user_id, remote_user_id);
    send_msg_(destination_hdl, message);

  } else {
    LOG_ERROR("Invalid offer msg");
  }

  return true;
}

bool SignalNegotiation::answer(websocketpp::connection_hdl hdl, const json& j) {
  std::string transmission_id;
  std::string remote_user_id;
  std::string user_id;
  if (!GetStringField(j, "transmission_id", transmission_id) ||
      !GetStringField(j, "remote_user_id", remote_user_id) ||
      !GetStringField(j, "user_id", user_id)) {
    LOG_ERROR("answer missing required fields");
    return false;
  }

  websocketpp::connection_hdl destination_hdl =
      transmission_manager_->GetWsHandle(remote_user_id);

  std::string sdp;
  if (GetStringField(j, "sdp", sdp)) {
    json message = {{"type", "answer"},
                    {"sdp", sdp},
                    {"remote_user_id", user_id},
                    {"transmission_id", transmission_id}};
    LOG_INFO("[{}] send answer to [{}]", user_id, remote_user_id);
    send_msg_(destination_hdl, message);
  } else {
    LOG_ERROR("Invalid answer msg");
  }

  return true;
}

bool SignalNegotiation::new_candidate(websocketpp::connection_hdl hdl,
                                      const json& j) {
  std::string transmission_id;
  std::string candidate;
  std::string user_id;
  std::string remote_user_id;
  if (!GetStringField(j, "transmission_id", transmission_id) ||
      !GetStringField(j, "sdp", candidate) ||
      !GetStringField(j, "user_id", user_id) ||
      !GetStringField(j, "remote_user_id", remote_user_id)) {
    LOG_ERROR("new_candidate missing required fields");
    return false;
  }

  websocketpp::connection_hdl destination_hdl =
      transmission_manager_->GetWsHandle(remote_user_id);

  // LOG_INFO("send candidate [{}]", candidate.c_str());
  json message = {{"type", "new_candidate"},
                  {"sdp", candidate},
                  {"remote_user_id", user_id},
                  {"transmission_id", transmission_id}};
  if (!CopyOptionalIceUfrag(j, message)) {
    LOG_WARN("new_candidate has an invalid ICE username fragment");
    return false;
  }
  send_msg_(destination_hdl, message);

  return true;
}

bool SignalNegotiation::new_candidate_mid(websocketpp::connection_hdl hdl,
                                          const json& j) {
  std::string transmission_id;
  std::string user_id;
  std::string remote_user_id;
  std::string candidate;
  std::string mid;
  if (!GetStringField(j, "transmission_id", transmission_id) ||
      !GetStringField(j, "user_id", user_id) ||
      !GetStringField(j, "remote_user_id", remote_user_id) ||
      !GetStringField(j, "candidate", candidate) ||
      !GetStringField(j, "mid", mid)) {
    LOG_ERROR("new_candidate_mid missing required fields");
    return false;
  }

  websocketpp::connection_hdl destination_hdl =
      transmission_manager_->GetWsHandle(remote_user_id);

  // LOG_INFO("send candidate [{}]", candidate.c_str());
  json message = {{"type", "new_candidate_mid"},
                  {"remote_user_id", user_id},
                  {"transmission_id", transmission_id},
                  {"candidate", candidate},
                  {"mid", mid}};
  if (!CopyOptionalIceUfrag(j, message)) {
    LOG_WARN("new_candidate_mid has an invalid ICE username fragment");
    return false;
  }
  send_msg_(destination_hdl, message);

  return true;
}

bool SignalNegotiation::change_password(websocketpp::connection_hdl hdl,
                                        const json& j) {
  constexpr size_t kMaxRememberedPasswordChanges = 4096;
  constexpr size_t kMaxPasswordChangeRequestIdLength = 128;
  json message = {{"type", "change_password"}};
  std::string request_id;
  if (!GetStringField(j, "request_id", request_id) || request_id.empty() ||
      request_id.size() > kMaxPasswordChangeRequestIdLength) {
    message["status"] = "fail";
    message["reason"] = "Missing or invalid request ID";
    send_msg_(hdl, message);
    return true;
  }
  message["request_id"] = request_id;

  const std::string user_id = transmission_manager_->GetUserId(hdl);
  message["user_id"] = user_id;

  if (user_id.empty()) {
    message["status"] = "fail";
    message["reason"] = "Not authenticated";
  } else if (user_id.rfind("C-", 0) == 0 ||
             user_id.rfind("web-", 0) == 0) {
    // Controller and browser identities are temporary and are not authenticated
    // against a persisted device password.
    message["status"] = "fail";
    message["reason"] = "Password changes are not allowed for this identity";
  } else {
    std::string new_password;
    if (!GetStringField(j, "new_password", new_password) ||
        new_password.size() != 6) {
      message["status"] = "fail";
      message["reason"] = "Password must contain exactly 6 characters";
    } else {
      const std::string cache_key = user_id + "\n" + request_id;
      const std::string password_fingerprint =
          PasswordFingerprint(new_password);
      std::lock_guard<std::mutex> lock(password_change_mutex_);
      const auto cached = password_change_results_.find(cache_key);
      if (cached != password_change_results_.end()) {
        if (cached->second.password_fingerprint != password_fingerprint) {
          message["status"] = "fail";
          message["reason"] = "Request ID was already used";
        } else {
          message = cached->second.response;
          LOG_INFO("Replay password change result for authenticated client "
                   "[{}] request [{}]",
                   user_id, request_id);
        }
      } else if (!device_db_manager_->UpdatePassword(user_id, new_password)) {
        message["status"] = "fail";
        message["reason"] = "Failed to update password";
      } else {
        message["status"] = "success";
        password_change_results_.emplace(
            cache_key, PasswordChangeResult{password_fingerprint, message});
        password_change_result_order_.push_back(cache_key);
        while (password_change_result_order_.size() >
               kMaxRememberedPasswordChanges) {
          password_change_results_.erase(
              password_change_result_order_.front());
          password_change_result_order_.pop_front();
        }
        LOG_INFO("Authenticated client [{}] changed its device password",
                 user_id);
      }
    }
  }

  send_msg_(hdl, message);
  return true;
}

bool SignalNegotiation::turn_credentials(websocketpp::connection_hdl hdl,
                                         const json& j) {
  (void)j;
  json message = {{"type", "turn_credentials"}};
  const std::string user_id = transmission_manager_->GetUserId(hdl);
  if (user_id.empty()) {
    message["status"] = "fail";
    message["reason"] = "Not authenticated";
  } else if (AddTurnCredentials(message, user_id)) {
    message["status"] = "success";
  } else {
    message["status"] = "fail";
    message["reason"] = "TURN credentials are not configured";
  }
  send_msg_(hdl, message);
  return true;
}

void SignalNegotiation::OnWebClientDisconnect(const std::string& user_id) {
  // Extract pure user_id (remove password part if exists)
  std::string pure_user_id = user_id;
  size_t at_pos = user_id.find("@");
  if (at_pos != std::string::npos) {
    pure_user_id = user_id.substr(0, at_pos);
  }

  // Check if this is a web client (starts with "web-")
  if (pure_user_id.find("web-") == 0) {
    if (!device_db_manager_->RemoveDevice(pure_user_id)) {
      LOG_WARN("Failed to remove web client device [{}] from database",
               pure_user_id);
    }
  }
}
