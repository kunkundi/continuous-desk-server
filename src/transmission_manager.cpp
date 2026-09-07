#include "transmission_manager.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <utility>

#include "log.h"

namespace {

bool HasConnectionOwner(websocketpp::connection_hdl hdl) {
  static const websocketpp::connection_hdl empty_hdl;
  std::owner_less<websocketpp::connection_hdl> less;
  return less(hdl, empty_hdl) || less(empty_hdl, hdl);
}

bool SameConnection(websocketpp::connection_hdl lhs,
                    websocketpp::connection_hdl rhs) {
  if (!HasConnectionOwner(lhs) || !HasConnectionOwner(rhs)) {
    return false;
  }

  std::owner_less<websocketpp::connection_hdl> less;
  return !less(lhs, rhs) && !less(rhs, lhs);
}

bool ContainsText(const std::string& value, const std::string& search) {
  return value.find(search) != std::string::npos;
}

bool HasUserConnection(
    const std::map<websocketpp::connection_hdl, std::string,
                   std::owner_less<websocketpp::connection_hdl>>&
        ws_hdl_user_id_list,
    const std::string& user_id) {
  return std::any_of(ws_hdl_user_id_list.begin(), ws_hdl_user_id_list.end(),
                     [&user_id](const auto& pair) {
                       return pair.second == user_id;
                     });
}

bool TransmissionMatchesSearch(const std::string& transmission_id,
                               const std::string& host_id,
                               const std::vector<std::string>* guest_ids,
                               const std::string& search) {
  if (search.empty()) {
    return true;
  }
  if (ContainsText(transmission_id, search) || ContainsText(host_id, search)) {
    return true;
  }
  if (!guest_ids) {
    return false;
  }
  return std::any_of(guest_ids->begin(), guest_ids->end(),
                     [&search](const std::string& guest_id) {
                       return ContainsText(guest_id, search);
                     });
}

}  // namespace

TransmissionManager::TransmissionManager() {
  ws_hdl_alive_checker_ = std::thread(&TransmissionManager::AliveChecker, this);
}

TransmissionManager::~TransmissionManager() {
  exit_alive_checker_ = true;
  ws_hdl_alive_checker_cv_.notify_all();
  if (ws_hdl_alive_checker_.joinable()) {
    ws_hdl_alive_checker_.join();
  }
}

bool TransmissionManager::IsTransmissionExist(
    const std::string& transmission_id) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  return transmission_host_id_list_.count(transmission_id);
}

bool TransmissionManager::ReleaseTransmission(
    const std::string& transmission_id) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  auto host_it = transmission_host_id_list_.find(transmission_id);
  std::string host_id =
      host_it != transmission_host_id_list_.end() ? host_it->second : "";
  auto guest_it = transmission_guest_id_list_.find(transmission_id);
  if (guest_it != transmission_guest_id_list_.end()) {
    if (remote_control_session_callback_) {
      for (const auto& guest_id : guest_it->second) {
        remote_control_session_callback_(transmission_id, host_id, guest_id,
                                         false);
      }
    }
    transmission_guest_id_list_.erase(guest_it);
  }
  transmission_host_id_list_.erase(transmission_id);
  return true;
}

std::string TransmissionManager::IsHost(const std::string& user_id) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  for (const auto& pair : transmission_host_id_list_) {
    if (pair.second == user_id) return pair.first;
  }
  return "";
}

std::string TransmissionManager::IsGuest(const std::string& user_id) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  for (const auto& pair : transmission_guest_id_list_) {
    const auto& list = pair.second;
    if (std::find(list.begin(), list.end(), user_id) != list.end())
      return pair.first;
  }
  return "";
}

bool TransmissionManager::IsHostOfTransmission(
    const std::string& user_id, const std::string& transmission_id) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  auto host_it = transmission_host_id_list_.find(transmission_id);
  if (host_it == transmission_host_id_list_.end()) {
    LOG_WARN("Transmission [{}] does not exist", transmission_id);
    return false;
  }
  return host_it->second == user_id;
}

std::vector<std::string> TransmissionManager::GetAllUserIdOfTransmission(
    const std::string& transmission_id) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  std::vector<std::string> result;
  auto host_it = transmission_host_id_list_.find(transmission_id);
  if (host_it != transmission_host_id_list_.end()) {
    result.push_back(host_it->second);
  }
  auto guest_it = transmission_guest_id_list_.find(transmission_id);
  if (guest_it != transmission_guest_id_list_.end()) {
    result.insert(result.end(), guest_it->second.begin(),
                  guest_it->second.end());
  }
  return result;
}

std::vector<TransmissionSnapshot> TransmissionManager::GetTransmissionSnapshots() {
  size_t ignored_count = 0;
  return GetTransmissionSnapshots((std::numeric_limits<size_t>::max)(), 0, "",
                                  &ignored_count);
}

std::vector<TransmissionSnapshot> TransmissionManager::GetTransmissionSnapshots(
    size_t limit, size_t offset, const std::string& search,
    size_t* filtered_count) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  std::vector<TransmissionSnapshot> result;
  if (limit > 0) {
    result.reserve((std::min)(limit, transmission_host_id_list_.size()));
  }

  size_t matched_count = 0;
  for (const auto& host_pair : transmission_host_id_list_) {
    auto guest_it = transmission_guest_id_list_.find(host_pair.first);
    const std::vector<std::string>* guest_ids =
        guest_it != transmission_guest_id_list_.end() ? &guest_it->second
                                                      : nullptr;
    if (!TransmissionMatchesSearch(host_pair.first, host_pair.second,
                                   guest_ids, search)) {
      continue;
    }
    if (matched_count++ < offset) {
      continue;
    }
    if (result.size() >= limit) {
      continue;
    }

    TransmissionSnapshot snapshot;
    snapshot.transmission_id = host_pair.first;
    snapshot.host_id = host_pair.second;
    if (guest_ids) {
      snapshot.guest_ids = guest_it->second;
    }
    snapshot.participant_count = 1 + snapshot.guest_ids.size();
    snapshot.active = true;
    result.push_back(snapshot);
  }

  if (filtered_count) {
    *filtered_count = matched_count;
  }
  return result;
}

std::string TransmissionManager::GetHostIdOfTransmission(
    const std::string& transmission_id) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  auto host_it = transmission_host_id_list_.find(transmission_id);
  if (host_it != transmission_host_id_list_.end()) {
    return host_it->second;
  }

  return "";
}

bool TransmissionManager::BindHostToTransmission(
    const std::string& host_id, const std::string& transmission_id) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  if (!transmission_host_id_list_.count(transmission_id)) {
    transmission_host_id_list_[transmission_id] = host_id;
    LOG_INFO("Bind host [{}] to transmission [{}]", host_id, transmission_id);
    return true;
  }
  LOG_WARN("Transmission [{}] already has host", transmission_id);
  return false;
}

bool TransmissionManager::BindGuestToTransmission(
    const std::string& guest_id, const std::string& transmission_id) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  auto host_it = transmission_host_id_list_.find(transmission_id);
  if (host_it == transmission_host_id_list_.end()) {
    LOG_WARN("Transmission [{}] does not exist", transmission_id);
    return false;
  }
  if (host_it->second == guest_id) {
    return false;
  }

  auto& guests = transmission_guest_id_list_[transmission_id];
  if (std::find(guests.begin(), guests.end(), guest_id) != guests.end()) {
    return false;
  }
  guests.push_back(guest_id);
  if (remote_control_session_callback_) {
    remote_control_session_callback_(transmission_id, host_it->second, guest_id,
                                     true);
  }
  LOG_INFO("Bind guest [{}] to transmission [{}]", guest_id, transmission_id);
  return true;
}

bool TransmissionManager::BindUserToWsHandle(const std::string& user_id,
                                             websocketpp::connection_hdl hdl) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  user_id_ws_hdl_list_[user_id] = hdl;
  ws_hdl_user_id_list_[hdl] = user_id;
  UpdateWsHandleLastActiveTime(hdl);
  return true;
}

void TransmissionManager::SetRemoteControlSessionCallback(
    std::function<void(const std::string&, const std::string&,
                       const std::string&, bool)>
        callback) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  remote_control_session_callback_ = std::move(callback);
}

void TransmissionManager::SetSessionTimeoutCallback(
    std::function<void(websocketpp::connection_hdl, const std::string&)> callback) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  session_timeout_callback_ = std::move(callback);
}

bool TransmissionManager::ReleaseGuestFromTransmission(
    const std::string& guest_id) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  bool released = false;
  for (auto map_it = transmission_guest_id_list_.begin();
       map_it != transmission_guest_id_list_.end();) {
    auto& list = map_it->second;
    auto remove_begin = std::remove(list.begin(), list.end(), guest_id);
    if (remove_begin == list.end()) {
      ++map_it;
      continue;
    }

    auto host_it = transmission_host_id_list_.find(map_it->first);
    std::string host_id =
        host_it != transmission_host_id_list_.end() ? host_it->second : "";
    if (remote_control_session_callback_) {
      remote_control_session_callback_(map_it->first, host_id, guest_id,
                                       false);
    }
    list.erase(remove_begin, list.end());
    released = true;
    if (list.empty()) {
      map_it = transmission_guest_id_list_.erase(map_it);
    } else {
      ++map_it;
    }
  }
  return released;
}

bool TransmissionManager::DisconnectTransmission(
    const std::string& transmission_id) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  if (!IsTransmissionExist(transmission_id)) {
    return true;
  }
  return ReleaseTransmission(transmission_id);
}

size_t TransmissionManager::PruneDisconnectedTransmissions() {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);

  std::vector<std::string> disconnected_transmissions;
  for (const auto& host_pair : transmission_host_id_list_) {
    if (!HasUserConnection(ws_hdl_user_id_list_, host_pair.second)) {
      disconnected_transmissions.push_back(host_pair.first);
    }
  }

  size_t pruned_connections = 0;
  for (const auto& transmission_id : disconnected_transmissions) {
    auto guest_it = transmission_guest_id_list_.find(transmission_id);
    if (guest_it != transmission_guest_id_list_.end()) {
      pruned_connections += guest_it->second.size();
    }
    ReleaseTransmission(transmission_id);
  }

  for (auto map_it = transmission_guest_id_list_.begin();
       map_it != transmission_guest_id_list_.end();) {
    auto host_it = transmission_host_id_list_.find(map_it->first);
    std::string host_id =
        host_it != transmission_host_id_list_.end() ? host_it->second : "";
    auto& guests = map_it->second;
    for (auto guest_it = guests.begin(); guest_it != guests.end();) {
      if (HasUserConnection(ws_hdl_user_id_list_, *guest_it)) {
        ++guest_it;
        continue;
      }
      if (remote_control_session_callback_) {
        remote_control_session_callback_(map_it->first, host_id, *guest_it,
                                         false);
      }
      guest_it = guests.erase(guest_it);
      ++pruned_connections;
    }
    if (guests.empty()) {
      map_it = transmission_guest_id_list_.erase(map_it);
    } else {
      ++map_it;
    }
  }

  return pruned_connections;
}

std::string TransmissionManager::ReleaseUserSession(
    websocketpp::connection_hdl hdl) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  std::string user_id = ReleaseUserFromWsHandle(hdl);
  if (user_id.empty()) {
    return "";
  }

  if (HasUserConnection(ws_hdl_user_id_list_, user_id)) {
    return "";
  }

  if (ReleaseGuestFromTransmission(user_id)) {
    LOG_INFO("Guest [{}] disconnected, releasing it from transmission", user_id);
  }

  std::string transmission_id = IsHost(user_id);
  if (!transmission_id.empty()) {
    LOG_INFO("Host [{}] disconnected, releasing transmission [{}]", user_id,
             transmission_id);
    ReleaseTransmission(transmission_id);
    return user_id;
  }

  return user_id;
}

std::string TransmissionManager::ReleaseUserFromWsHandle(
    websocketpp::connection_hdl hdl) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  ws_hdl_last_active_time_map_.erase(hdl);
  auto hdl_it = ws_hdl_user_id_list_.find(hdl);
  if (hdl_it == ws_hdl_user_id_list_.end()) {
    return "";
  }

  std::string user_id = hdl_it->second;
  ws_hdl_user_id_list_.erase(hdl_it);

  auto user_it = user_id_ws_hdl_list_.find(user_id);
  if (user_it != user_id_ws_hdl_list_.end() &&
      SameConnection(user_it->second, hdl)) {
    bool reassigned = false;
    for (const auto& pair : ws_hdl_user_id_list_) {
      if (pair.second == user_id) {
        user_it->second = pair.first;
        reassigned = true;
        break;
      }
    }
    if (!reassigned) {
      user_id_ws_hdl_list_.erase(user_it);
    }
  }
  return user_id;
}

void TransmissionManager::RemoveWsHandleLastActiveTime(
    websocketpp::connection_hdl hdl) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  ws_hdl_last_active_time_map_.erase(hdl);
}

websocketpp::connection_hdl TransmissionManager::GetWsHandle(
    const std::string& user_id) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  auto it = user_id_ws_hdl_list_.find(user_id);
  if (it != user_id_ws_hdl_list_.end()) {
    return it->second;
  }

  return websocketpp::connection_hdl();
}

std::string TransmissionManager::GetUserId(websocketpp::connection_hdl hdl) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  auto it = ws_hdl_user_id_list_.find(hdl);
  if (it != ws_hdl_user_id_list_.end()) {
    return it->second;
  }
  return "";
}

int TransmissionManager::UpdateWsHandleLastActiveTime(
    websocketpp::connection_hdl hdl) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  // Only authenticated sessions belong in the presence watchdog. A late
  // heartbeat must not revive a session that has already been released.
  if (ws_hdl_user_id_list_.find(hdl) == ws_hdl_user_id_list_.end()) {
    return -1;
  }
  ws_hdl_last_active_time_map_[hdl] = std::chrono::steady_clock::now();
  return 0;
}

size_t TransmissionManager::GetActiveConnectionCount() {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  size_t count = 0;
  for (const auto& host_pair : transmission_host_id_list_) {
    auto guest_it = transmission_guest_id_list_.find(host_pair.first);
    if (guest_it != transmission_guest_id_list_.end()) {
      count += guest_it->second.size();
    }
  }
  return count;
}

void TransmissionManager::AliveChecker() {
  while (!exit_alive_checker_) {
    std::unique_lock<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
    if (ws_hdl_alive_checker_cv_.wait_for(
            lock, std::chrono::seconds(10),
            [this]() { return exit_alive_checker_.load(); })) {
      break;
    }

    ExpireInactiveSessions();
  }
}

void TransmissionManager::ExpireInactiveSessions(
    std::chrono::steady_clock::time_point now) {
  std::lock_guard<std::recursive_mutex> lock(ws_hdl_alive_checker_mutex_);
  for (auto it = ws_hdl_last_active_time_map_.begin();
       it != ws_hdl_last_active_time_map_.end();) {
    auto hdl = it->first;
    if (hdl.expired() || now - it->second > std::chrono::seconds(10)) {
      it = ws_hdl_last_active_time_map_.erase(it);
      std::string user_id = ReleaseUserSession(hdl);
      if (session_timeout_callback_) {
        // Close every expired connection, including an old connection whose
        // device has another live session. Only the final session logs out.
        session_timeout_callback_(hdl, user_id);
      }
    } else {
      ++it;
    }
  }
}
