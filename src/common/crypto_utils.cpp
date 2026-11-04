#include "nvr/common/crypto_utils.h"
#include "nvr/common/logger.h"

#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/err.h>

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>

namespace nvr {
namespace crypto {

namespace {

std::string BytesToHex(const uint8_t* data, size_t length) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < length; ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

bool HexToBytes(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.length() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        char* end = nullptr;
        long val = std::strtol(byte_str.c_str(), &end, 16);
        if (end != byte_str.c_str() + 2 || val < 0 || val > 255) {
            return false;
        }
        out.push_back(static_cast<uint8_t>(val));
    }
    return true;
}

} // namespace

std::string GenerateRandomHex(size_t byte_count) {
    if (byte_count == 0) return "";
    std::vector<uint8_t> buf(byte_count);
    if (RAND_bytes(buf.data(), static_cast<int>(byte_count)) != 1) {
        LOG_ERROR << "[Crypto] RAND_bytes failed";
        return "";
    }
    return BytesToHex(buf.data(), byte_count);
}

std::string HashPasswordPBKDF2(const std::string& password, const std::string& salt_hex, int iterations) {
    std::vector<uint8_t> salt_bytes;
    if (!HexToBytes(salt_hex, salt_bytes) || salt_bytes.empty()) {
        salt_bytes.assign(salt_hex.begin(), salt_hex.end());
    }

    constexpr size_t kHashBytes = 32; // SHA-256 output length
    uint8_t out_hash[kHashBytes];

    int rc = PKCS5_PBKDF2_HMAC(
        password.c_str(),
        static_cast<int>(password.length()),
        salt_bytes.data(),
        static_cast<int>(salt_bytes.size()),
        iterations > 0 ? iterations : 100000,
        EVP_sha256(),
        static_cast<int>(kHashBytes),
        out_hash
    );

    if (rc != 1) {
        LOG_ERROR << "[Crypto] PKCS5_PBKDF2_HMAC failed";
        return "";
    }

    return BytesToHex(out_hash, kHashBytes);
}

bool VerifyPassword(const std::string& password, const std::string& salt_hex, const std::string& stored_hash_hex, int iterations) {
    if (stored_hash_hex.empty()) return false;
    std::string computed = HashPasswordPBKDF2(password, salt_hex, iterations);
    if (computed.empty() || computed.length() != stored_hash_hex.length()) {
        return false;
    }
    // Constant time comparison
    return CRYPTO_memcmp(computed.data(), stored_hash_hex.data(), computed.length()) == 0;
}

std::string GenerateSessionToken() {
    return GenerateRandomHex(32); // 256 bits = 64 hex characters
}

std::string EncryptAesGcm(const std::string& plaintext, const std::string& key_hex) {
    std::vector<uint8_t> key_bytes;
    if (!HexToBytes(key_hex, key_bytes) || key_bytes.size() != 32) {
        // Derive key from string if not exactly 32-byte hex
        key_bytes.resize(32, 0);
        for (size_t i = 0; i < key_hex.size() && i < 32; ++i) {
            key_bytes[i] = static_cast<uint8_t>(key_hex[i]);
        }
    }

    constexpr size_t kIvLen = 12; // 96-bit IV recommended for GCM
    constexpr size_t kTagLen = 16; // 128-bit authentication tag

    uint8_t iv[kIvLen];
    if (RAND_bytes(iv, kIvLen) != 1) return "";

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";

    std::vector<uint8_t> ciphertext(plaintext.size() + 16);
    int outlen = 0;
    int total_len = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvLen, nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_bytes.data(), iv) != 1 ||
        EVP_EncryptUpdate(ctx, ciphertext.data(), &outlen, reinterpret_cast<const uint8_t*>(plaintext.data()), static_cast<int>(plaintext.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return "";
    }
    total_len = outlen;

    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + total_len, &outlen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return "";
    }
    total_len += outlen;

    uint8_t tag[kTagLen];
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen, tag) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return "";
    }
    EVP_CIPHER_CTX_free(ctx);

    // Format: iv_hex + ":" + tag_hex + ":" + ciphertext_hex
    return BytesToHex(iv, kIvLen) + ":" + BytesToHex(tag, kTagLen) + ":" + BytesToHex(ciphertext.data(), total_len);
}

std::string DecryptAesGcm(const std::string& ciphertext_payload, const std::string& key_hex) {
    if (ciphertext_payload.empty()) return "";

    auto p1 = ciphertext_payload.find(':');
    if (p1 == std::string::npos) return ciphertext_payload; // Legacy plaintext fallback
    auto p2 = ciphertext_payload.find(':', p1 + 1);
    if (p2 == std::string::npos) return ciphertext_payload;

    std::string iv_hex = ciphertext_payload.substr(0, p1);
    std::string tag_hex = ciphertext_payload.substr(p1 + 1, p2 - (p1 + 1));
    std::string ct_hex = ciphertext_payload.substr(p2 + 1);

    std::vector<uint8_t> iv, tag, ct, key_bytes;
    if (!HexToBytes(iv_hex, iv) || !HexToBytes(tag_hex, tag) || !HexToBytes(ct_hex, ct) ||
        !HexToBytes(key_hex, key_bytes) || key_bytes.size() != 32) {
        key_bytes.resize(32, 0);
        for (size_t i = 0; i < key_hex.size() && i < 32; ++i) {
            key_bytes[i] = static_cast<uint8_t>(key_hex[i]);
        }
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";

    std::vector<uint8_t> plaintext(ct.size() + 16);
    int outlen = 0;
    int total_len = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_bytes.data(), iv.data()) != 1 ||
        EVP_DecryptUpdate(ctx, plaintext.data(), &outlen, ct.data(), static_cast<int>(ct.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return "";
    }
    total_len = outlen;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()), const_cast<uint8_t*>(tag.data())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return "";
    }

    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + total_len, &outlen) <= 0) {
        // Tag verification failed or corrupt ciphertext
        EVP_CIPHER_CTX_free(ctx);
        return "";
    }
    total_len += outlen;
    EVP_CIPHER_CTX_free(ctx);

    return std::string(reinterpret_cast<const char*>(plaintext.data()), total_len);
}

std::string MaskRtspUrl(const std::string& url) {
    auto scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) return url;
    auto at_pos = url.find('@', scheme_pos + 3);
    if (at_pos == std::string::npos) return url;
    auto colon_pos = url.find(':', scheme_pos + 3);
    if (colon_pos == std::string::npos || colon_pos > at_pos) return url;

    return url.substr(0, colon_pos + 1) + "*****" + url.substr(at_pos);
}

std::string GetOrCreateVaultKey(const std::string& vault_key_path) {
    std::error_code ec;
    std::filesystem::path p(vault_key_path);
    if (std::filesystem::exists(p, ec)) {
        std::ifstream ifs(p);
        std::string line;
        if (std::getline(ifs, line)) {
            // Trim whitespace
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
                line.pop_back();
            }
            if (line.size() == 64) return line;
        }
    }

    // Generate fresh 256-bit key
    std::string new_key = GenerateRandomHex(32);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::ofstream ofs(p);
    ofs << new_key << "\n";
    ofs.close();

    // Restrict permissions to read/write by owner only (chmod 0600)
    chmod(p.string().c_str(), S_IRUSR | S_IWUSR);
    LOG_INFO << "[Crypto] Generated and persisted fresh hardware vault key to " << vault_key_path;
    return new_key;
}

} // namespace crypto
} // namespace nvr
