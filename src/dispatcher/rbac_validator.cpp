#include "nvr/dispatcher/rbac_validator.h"
#include "nvr/common/logger.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace nvr {

RbacValidator& RbacValidator::Instance() {
    static RbacValidator instance;
    return instance;
}

RbacValidator::RbacValidator() {
    // Default initial provisioned accounts
    accounts_["admin"] = UserAccount{"admin", "Admin@123", UserRole::ADMIN};
    accounts_["operator"] = UserAccount{"operator", "Operator@123", UserRole::OPERATOR};
    accounts_["viewer"] = UserAccount{"viewer", "Viewer@123", UserRole::VIEWER};
}

std::string RbacValidator::GenerateSecureToken() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) {
        oss << std::setw(16) << dis(gen);
    }
    return oss.str(); // 64 hex characters (256-bit entropy)
}

bool RbacValidator::Authenticate(const std::string& username, const std::string& password, 
                                 std::string& out_token, UserRole& out_role) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = accounts_.find(username);
    if (it == accounts_.end() || it->second.password != password) {
        LOG_WARN << "RbacValidator: Authentication failed for user '" << username << "'";
        return false;
    }

    out_role = it->second.role;

    // Bounded session capacity check
    if (sessions_.size() >= kMaxActiveSessions) {
        // Prune expired
        auto now = std::chrono::steady_clock::now();
        for (auto s_it = sessions_.begin(); s_it != sessions_.end();) {
            if (s_it->second.expiry <= now) {
                s_it = sessions_.erase(s_it);
            } else {
                ++s_it;
            }
        }
        // If still at capacity, evict an arbitrary session
        if (sessions_.size() >= kMaxActiveSessions && !sessions_.empty()) {
            sessions_.erase(sessions_.begin());
        }
    }

    std::string token = GenerateSecureToken();
    SessionInfo session;
    session.token = token;
    session.username = username;
    session.role = out_role;
    session.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(86400); // 24 hours

    sessions_[token] = session;
    out_token = token;

    LOG_INFO << "RbacValidator: Session created for '" << username << "' [" 
             << UserRoleToString(out_role) << "] (active sessions: " << sessions_.size() << ")";
    return true;
}

bool RbacValidator::ValidateToken(const std::string& token, UserRole required_role, UserRole* out_role) {
    if (token.empty()) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) {
        return false;
    }

    // Monotonic expiration check
    if (it->second.expiry <= now) {
        return false;
    }

    if (out_role) {
        *out_role = it->second.role;
    }

    // Role hierarchy check: ADMIN (3) >= OPERATOR (2) >= VIEWER (1)
    return static_cast<uint8_t>(it->second.role) >= static_cast<uint8_t>(required_role);
}

std::string RbacValidator::CreateSession(const std::string& username, UserRole role, uint32_t ttl_seconds) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    if (sessions_.size() >= kMaxActiveSessions) {
        auto now = std::chrono::steady_clock::now();
        for (auto s_it = sessions_.begin(); s_it != sessions_.end();) {
            if (s_it->second.expiry <= now) {
                s_it = sessions_.erase(s_it);
            } else {
                ++s_it;
            }
        }
        if (sessions_.size() >= kMaxActiveSessions && !sessions_.empty()) {
            sessions_.erase(sessions_.begin());
        }
    }

    std::string token = GenerateSecureToken();
    SessionInfo session;
    session.token = token;
    session.username = username;
    session.role = role;
    session.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds);

    sessions_[token] = session;
    return token;
}

void RbacValidator::RevokeSession(const std::string& token) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    sessions_.erase(token);
}

void RbacValidator::SetUserAccount(const std::string& username, const std::string& password, UserRole role) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    accounts_[username] = UserAccount{username, password, role};
}

bool RbacValidator::RemoveUserAccount(const std::string& username) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return accounts_.erase(username) > 0;
}

void RbacValidator::PruneExpired() {
    auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second.expiry <= now) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void RbacValidator::ClearAllSessions() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    sessions_.clear();
}

} // namespace nvr
