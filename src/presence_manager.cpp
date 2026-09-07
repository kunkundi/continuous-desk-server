#include "presence_manager.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <unordered_map>

#include "log.h"

namespace {

bool IsWebClient(const std::string& device_id) {
  return device_id.rfind("web-", 0) == 0;
}

bool IsCloneClient(const std::string& device_id) {
  return device_id.rfind("C-", 0) == 0;
}

bool ShouldTrackOnlineDevice(const std::string& device_id) {
  return !IsWebClient(device_id) && !IsCloneClient(device_id);
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::string Trim(std::string value) {
  auto is_space = [](unsigned char ch) { return std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [&](unsigned char ch) { return !is_space(ch); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [&](unsigned char ch) { return !is_space(ch); })
                  .base(),
              value.end());
  return value;
}

bool ContainsText(const std::string& value, const std::string& pattern) {
  return value.find(pattern) != std::string::npos;
}

bool IsChinaCountry(const std::string& country) {
  std::string normalized = ToLower(Trim(country));
  return normalized == "china" || normalized == "cn" ||
         ContainsText(country, "中国");
}

std::string NormalizeChinaProvince(const std::string& region,
                                   const std::string& location) {
  std::string value = ToLower(region + " " + location);
  const std::vector<std::pair<std::string, std::vector<std::string>>> matchers =
      {
          {"anhui", {"anhui", "安徽"}},
          {"beijing", {"beijing", "北京"}},
          {"chongqing", {"chongqing", "重庆"}},
          {"fujian", {"fujian", "福建"}},
          {"gansu", {"gansu", "甘肃"}},
          {"guangdong", {"guangdong", "广东"}},
          {"guangxi", {"guangxi", "广西"}},
          {"guizhou", {"guizhou", "贵州"}},
          {"hainan", {"hainan", "海南"}},
          {"hebei", {"hebei", "河北"}},
          {"heilongjiang", {"heilongjiang", "黑龙江"}},
          {"henan", {"henan", "河南"}},
          {"hongkong", {"hong kong", "hongkong", "香港"}},
          {"hubei", {"hubei", "湖北"}},
          {"hunan", {"hunan", "湖南"}},
          {"inner_mongolia",
           {"inner mongolia", "neimenggu", "内蒙古"}},
          {"jiangsu", {"jiangsu", "江苏"}},
          {"jiangxi", {"jiangxi", "江西"}},
          {"jilin", {"jilin", "吉林"}},
          {"liaoning", {"liaoning", "辽宁"}},
          {"macau", {"macau", "macao", "澳门"}},
          {"ningxia", {"ningxia", "宁夏"}},
          {"qinghai", {"qinghai", "青海"}},
          {"shaanxi", {"shaanxi", "shanxi sheng", "陕西"}},
          {"shandong", {"shandong", "山东"}},
          {"shanghai", {"shanghai", "上海"}},
          {"shanxi", {"shanxi", "山西"}},
          {"sichuan", {"sichuan", "四川"}},
          {"taiwan", {"taiwan", "台湾"}},
          {"tianjin", {"tianjin", "天津"}},
          {"tibet", {"tibet", "xizang", "西藏"}},
          {"xinjiang", {"xinjiang", "新疆"}},
          {"yunnan", {"yunnan", "云南"}},
          {"zhejiang", {"zhejiang", "浙江"}},
      };

  for (const auto& matcher : matchers) {
    for (const auto& pattern : matcher.second) {
      if (ContainsText(value, pattern) ||
          ContainsText(region + " " + location, pattern)) {
        return matcher.first;
      }
    }
  }
  return "";
}

}  // namespace

void PresenceManager::OnLogin(const std::string& user_id,
                              const std::string& device_id,
                              websocketpp::connection_hdl hdl) {
  {
    std::lock_guard<std::mutex> lock(online_devices_mutex_);
    if (ShouldTrackOnlineDevice(device_id)) {
      online_devices_.insert(device_id);
    } else if (IsWebClient(device_id)) {
      online_web_clients_.insert(device_id);
    }
  }
  if (db_) {
    db_->SetDeviceOnline(device_id, true);
  }
  NotifyUserDevices(user_id, device_id, true);
}

void PresenceManager::OnLogout(const std::string& device_id) {
  std::string user_id = device_id;
  {
    std::lock_guard<std::mutex> lock(associations_mutex_);
    associations_.erase(device_id);
  }
  {
    std::lock_guard<std::mutex> lock(online_devices_mutex_);
    if (ShouldTrackOnlineDevice(device_id)) {
      online_devices_.erase(device_id);
    } else if (IsWebClient(device_id)) {
      online_web_clients_.erase(device_id);
    }
  }
  if (db_) {
    db_->SetDeviceOnline(device_id, false);
  }
  {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    device_network_info_.erase(device_id);
  }
  if (!user_id.empty()) {
    NotifyUserDevices(user_id, device_id, false);
  }
}

bool PresenceManager::IsOnline(const std::string& device_id) const {
  if (!db_) return false;
  auto res = db_->BatchQueryOnline({device_id});
  return !res.empty() && res[0].second;
}

size_t PresenceManager::GetOnlineDeviceCount() const {
  std::lock_guard<std::mutex> lock(online_devices_mutex_);
  return online_devices_.size();
}

size_t PresenceManager::GetOnlineWebClientCount() const {
  std::lock_guard<std::mutex> lock(online_devices_mutex_);
  return online_web_clients_.size();
}

void PresenceManager::SetDeviceNetworkInfo(
    const std::string& device_id, const ClientNetworkInfo& network_info) {
  if (device_id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(network_info_mutex_);
  device_network_info_[device_id] = network_info;
}

bool PresenceManager::GetDeviceNetworkInfo(
    const std::string& device_id, ClientNetworkInfo* network_info) const {
  if (!network_info) {
    return false;
  }
  std::lock_guard<std::mutex> lock(network_info_mutex_);
  auto it = device_network_info_.find(device_id);
  if (it == device_network_info_.end()) {
    return false;
  }
  *network_info = it->second;
  return true;
}

bool PresenceManager::HasDeviceWithClientIp(
    const std::string& client_ip) const {
  if (client_ip.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(network_info_mutex_);
  for (const auto& pair : device_network_info_) {
    if (pair.second.client_ip == client_ip) {
      return true;
    }
  }
  return false;
}

size_t PresenceManager::UpdateDevicesWithClientIp(
    const std::string& client_ip, const ClientNetworkInfo& network_info) {
  if (client_ip.empty()) {
    return 0;
  }
  size_t updated = 0;
  std::lock_guard<std::mutex> lock(network_info_mutex_);
  for (auto& pair : device_network_info_) {
    if (pair.second.client_ip == client_ip) {
      pair.second = network_info;
      ++updated;
    }
  }
  return updated;
}

ClientGeoDistribution PresenceManager::GetClientGeoDistribution() const {
  ClientGeoDistribution distribution;
  std::vector<std::string> online_devices;
  {
    std::lock_guard<std::mutex> lock(online_devices_mutex_);
    online_devices.assign(online_devices_.begin(), online_devices_.end());
    online_devices.insert(online_devices.end(), online_web_clients_.begin(),
                          online_web_clients_.end());
  }

  std::unordered_map<std::string, ClientNetworkInfo> network_info;
  {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    for (const auto& device_id : online_devices) {
      auto it = device_network_info_.find(device_id);
      if (it != device_network_info_.end()) {
        network_info.emplace(device_id, it->second);
      }
    }
  }

  std::unordered_map<std::string, int64_t> province_counts;
  std::unordered_map<std::string, int64_t> country_counts;
  for (const auto& device_id : online_devices) {
    ++distribution.total_count;
    auto it = network_info.find(device_id);
    if (it == network_info.end()) {
      ++distribution.unknown_count;
      continue;
    }

    const auto& info = it->second;
    std::string province = NormalizeChinaProvince(info.region, info.location);
    std::string country = Trim(info.country);
    if (!province.empty()) {
      ++distribution.domestic_count;
      ++province_counts[province];
    } else if (IsChinaCountry(country)) {
      ++distribution.domestic_count;
    } else if (!country.empty()) {
      ++distribution.foreign_count;
      ++country_counts[country];
    } else {
      ++distribution.unknown_count;
    }
  }

  for (const auto& pair : province_counts) {
    distribution.provinces.push_back({pair.first, pair.second});
  }
  for (const auto& pair : country_counts) {
    distribution.countries.push_back({pair.first, pair.second});
  }
  std::sort(distribution.provinces.begin(), distribution.provinces.end(),
            [](const ProvinceUserCount& lhs, const ProvinceUserCount& rhs) {
              if (lhs.count != rhs.count) {
                return lhs.count > rhs.count;
              }
              return lhs.province < rhs.province;
            });
  std::sort(distribution.countries.begin(), distribution.countries.end(),
            [](const CountryUserCount& lhs, const CountryUserCount& rhs) {
              if (lhs.count != rhs.count) {
                return lhs.count > rhs.count;
              }
              return lhs.country < rhs.country;
            });
  return distribution;
}

std::vector<std::pair<std::string, bool>> PresenceManager::BatchQuery(
    const std::vector<std::string>& device_ids) const {
  std::vector<std::pair<std::string, bool>> result;
  if (db_) {
    return db_->BatchQueryOnline(device_ids);
  }
  return result;
}

void PresenceManager::NotifyUserDevices(const std::string& user_id,
                                        const std::string& changed_device_id,
                                        bool online) {
  (void)user_id;

  if (!send_to_device_) {
    return;
  }

  std::vector<std::string> watchers;
  {
    std::lock_guard<std::mutex> lock(associations_mutex_);
    for (const auto& kv : associations_) {
      const auto& watcher = kv.first;
      const auto& watched_set = kv.second;
      if (watched_set.find(changed_device_id) != watched_set.end()) {
        watchers.push_back(watcher);
      }
    }
  }

  if (watchers.empty()) {
    return;
  }
  std::vector<std::string> targets = watchers;
  if (db_) {
    auto statuses = db_->BatchQueryOnline(watchers);
    targets.clear();
    for (const auto& p : statuses) {
      if (p.first == changed_device_id) {
        continue;
      }
      if (p.second) targets.push_back(p.first);
    }
  }
  nlohmann::json j = {
      {"type", "presence_update"},
      {"id", changed_device_id},
      {"online", online},
  };
  for (const auto& id : targets) {
    send_to_device_(id, j);
  }
}

void PresenceManager::UpdateUserDevices(
    const std::string& user_id, const std::vector<std::string>& device_ids,
    bool replace) {
  std::lock_guard<std::mutex> lock(associations_mutex_);
  auto& setref = associations_[user_id];
  if (replace) {
    setref.clear();
  }
  for (const auto& id : device_ids) {
    setref.insert(id);
  }
}
