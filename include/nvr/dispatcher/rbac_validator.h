#pragma once

#include "nvr/dispatcher/commands.h"
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <chrono>

namespace nvr {

struct SessionInfo {
    std::string token;
    std::string username;
    UserRole role{UserRole::VIEWER};
    std::chrono::steady_clock::time_point expiry;
};

struct UserAccount {
    std::string username;
    std::string password; // Plain or hashed credential
    UserRole role{UserRole::VIEWER};
};

class RbacValidator {
public:
    static RbacValidator& Instance();

    // Authenticate user against stored accounts and issue session token
    bool Authenticate(const std::string& username, const std::string& password, std::string& out_token, UserRole& out_role);

    // Validate token against required role level
    bool ValidateToken(const std::string& token, UserRole required_role, UserRole* out_role = nullptr);

    // Manual session creation (e.g. internal or pre-authenticated sessions)
    std::string CreateSession(const std::string& username, UserRole role, uint32_t ttl_seconds = 86400);

    // Revoke session token
    void RevokeSession(const std::string& token);

    // Manage user accounts
    void SetUserAccount(const std::string& username, const std::string& password, UserRole role);
    bool RemoveUserAccount(const std::string& username);

    // Periodic cleanup of expired tokens
    void PruneExpired();

    // Clear all sessions (e.g. for testing or shutdown)
    void ClearAllSessions();

private:
    RbacValidator();
    ~RbacValidator() = default;
    RbacValidator(const RbacValidator&) = delete;
    RbacValidator& operator=(const RbacValidator&) = delete;

    std::string GenerateSecureToken();

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, SessionInfo> sessions_; // token -> SessionInfo
    std::unordered_map<std::string, UserAccount> accounts_; // username -> UserAccount

    static constexpr size_t kMaxActiveSessions = 128;
};

} // namespace nvr
