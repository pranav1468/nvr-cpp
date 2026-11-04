#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace nvr {
namespace crypto {

// Generate cryptographically secure random bytes formatted as a hex string
std::string GenerateRandomHex(size_t byte_count);

// Hash a password using PBKDF2-HMAC-SHA256 (100,000 iterations standard)
std::string HashPasswordPBKDF2(const std::string& password, const std::string& salt_hex, int iterations = 100000);

// Constant-time password verification to prevent timing side-channel attacks
bool VerifyPassword(const std::string& password, const std::string& salt_hex, const std::string& stored_hash_hex, int iterations = 100000);

// Generate 256-bit cryptographically secure session bearer token (64 hex characters)
std::string GenerateSessionToken();

// AES-256-GCM authenticated encryption for camera credentials at rest
std::string EncryptAesGcm(const std::string& plaintext, const std::string& key_hex);
std::string DecryptAesGcm(const std::string& ciphertext_payload, const std::string& key_hex);

// Mask passwords embedded inside RTSP URLs (e.g. rtsp://user:pass@host -> rtsp://user:*****@host)
std::string MaskRtspUrl(const std::string& url);

// Retrieve or initialize the local machine vault key (stored in config/vault.key)
std::string GetOrCreateVaultKey(const std::string& vault_key_path = "config/vault.key");

} // namespace crypto
} // namespace nvr
