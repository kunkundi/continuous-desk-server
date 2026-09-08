/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-08
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ICE_SERVER_CONFIG_ISSUER_H_
#define _ICE_SERVER_CONFIG_ISSUER_H_

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "turn_credentials.h"

class IceServerConfigIssuer {
 public:
  IceServerConfigIssuer(const nlohmann::json& servers,
                        const std::string& default_secret,
                        uint32_t ttl_seconds);
  nlohmann::json Issue(const std::string& recipient) const;

 private:
  struct Server {
    nlohmann::json urls;
    std::optional<TurnCredentialIssuer> credentials;
  };
  std::vector<Server> servers_;
  uint32_t ttl_seconds_;
};

#endif
