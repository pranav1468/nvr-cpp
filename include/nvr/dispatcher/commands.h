#pragma once

#include <string>
#include <cstdint>
#include "3rdparty/nlohmann/json.hpp"

namespace nvr {

enum class UserRole : uint8_t {
    NONE = 0,
    VIEWER = 1,
    OPERATOR = 2,
    ADMIN = 3
};

inline std::string UserRoleToString(UserRole role) {
    switch (role) {
        case UserRole::ADMIN:    return "ADMIN";
        case UserRole::OPERATOR: return "OPERATOR";
        case UserRole::VIEWER:   return "VIEWER";
        default:                 return "NONE";
    }
}

inline UserRole StringToUserRole(const std::string& str) {
    if (str == "ADMIN" || str == "admin") return UserRole::ADMIN;
    if (str == "OPERATOR" || str == "operator") return UserRole::OPERATOR;
    if (str == "VIEWER" || str == "viewer") return UserRole::VIEWER;
    return UserRole::NONE;
}

struct CommandEnvelope {
    std::string command_id;
    std::string action;
    std::string auth_token;
    std::string client_id;
    nlohmann::json payload;
    int64_t timestamp_ms{0};
};

struct CommandResult {
    bool success{false};
    int status_code{200};
    std::string message;
    nlohmann::json data;

    static CommandResult Success(const nlohmann::json& d = nlohmann::json::object(), const std::string& msg = "OK") {
        CommandResult r;
        r.success = true;
        r.status_code = 200;
        r.message = msg;
        r.data = d;
        return r;
    }

    static CommandResult Error(int code, const std::string& msg, const nlohmann::json& d = nlohmann::json::object()) {
        CommandResult r;
        r.success = false;
        r.status_code = code;
        r.message = msg;
        r.data = d;
        return r;
    }

    nlohmann::json ToJson() const {
        nlohmann::json j;
        j["success"] = success;
        j["status_code"] = status_code;
        j["message"] = message;
        j["data"] = data;
        return j;
    }
};

} // namespace nvr
