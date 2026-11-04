#include "nvr/api/rest_server.h"
#include "nvr/api/fmp4_muxer.h"
#include "nvr/media/stream_broker.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/live/live_controller.h"
#include "nvr/dispatcher/command_dispatcher.h"
#include "nvr/media/image_utils.h"
#include "nvr/common/logger.h"
#include "nvr/common/thread_pool.h"
#include "nvr/common/time_utils.h"
#include "nvr/storage/retention_manager.h"
#include "nvr/playback/playback_engine.h"
#include "3rdparty/nlohmann/json.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <filesystem>

#include <sstream>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <algorithm>
#include <vector>
#include <cstring>
#include <deque>
#include <mutex>
#include <condition_variable>

namespace nvr {

namespace {

struct StreamContext {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<MediaPacketPtr> q;
    std::atomic<bool> closed{false};
    static constexpr size_t kMaxDepth = 10; // freshness-first drop-stale bound (~300ms @ 30fps)
};

struct WsStreamContext {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<MediaPacketPtr> queue;
    std::atomic<bool> closed{false};
    bool awaiting_keyframe{true};
    static constexpr size_t kMaxQueueDepth = 30; // Full 1-second GOP capacity
};

std::string HttpStatusMessage(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "Response";
    }
}

std::string GetMimeType(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "application/javascript; charset=utf-8";
    if (ext == ".wasm") return "application/wasm";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".m3u8") return "application/vnd.apple.mpegurl; charset=utf-8";
    if (ext == ".mp4") return "video/mp4";
    return "application/octet-stream";
}

std::string ToLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
    return str;
}

std::string UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int val = 0;
            std::istringstream is(in.substr(i + 1, 2));
            if (is >> std::hex >> val) {
                out.push_back(static_cast<char>(val));
                i += 2;
            } else {
                out.push_back(in[i]);
            }
        } else if (in[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

nlohmann::json ParseQueryParams(const std::string& query) {
    nlohmann::json j = nlohmann::json::object();
    std::istringstream ss(query);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        if (pair.empty()) continue;
        auto eq = pair.find('=');
        if (eq == std::string::npos) {
            j[UrlDecode(pair)] = true;
        } else {
            std::string key = UrlDecode(pair.substr(0, eq));
            std::string val = UrlDecode(pair.substr(eq + 1));
            // Check if integer
            char* end = nullptr;
            long long int_val = std::strtoll(val.c_str(), &end, 10);
            if (end != val.c_str() && *end == '\0') {
                j[key] = int_val;
            } else if (val == "true") {
                j[key] = true;
            } else if (val == "false") {
                j[key] = false;
            } else {
                j[key] = val;
            }
        }
    }
    return j;
}

void SendHttpResponse(int client_fd, int status_code, const std::string& content_type, 
                      const std::string& body, const std::string& extra_headers = "",
                      const std::string& origin = "", bool is_head = false) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << " " << HttpStatusMessage(status_code) << "\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << RestServer::Instance().BuildCorsHeaders(origin, false);
    oss << "X-Content-Type-Options: nosniff\r\n";
    oss << "X-Frame-Options: SAMEORIGIN\r\n";
    oss << "Referrer-Policy: strict-origin-when-cross-origin\r\n";
    oss << "Cross-Origin-Opener-Policy: same-origin\r\n";
    oss << "Cross-Origin-Embedder-Policy: require-corp\r\n";
    oss << "Cross-Origin-Resource-Policy: cross-origin\r\n";
    if (RestServer::Instance().IsHstsEnabled()) {
        oss << "Strict-Transport-Security: max-age=31536000; includeSubDomains\r\n";
    }
    oss << "Connection: close\r\n";
    if (!extra_headers.empty()) {
        oss << extra_headers;
    }
    oss << "\r\n";
    if (!is_head) {
        oss << body;
    }

    std::string resp = oss.str();
    ssize_t sent = 0;
    size_t total = resp.size();
    while (sent < static_cast<ssize_t>(total)) {
        ssize_t n = send(client_fd, resp.data() + sent, total - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sent += n;
    }
}

// ============================================================================
// RFC 6455 WebSocket Cryptographic & Framing Helpers (Zero External Deps)
// ============================================================================
inline uint32_t Rol32(uint32_t value, unsigned int bits) {
    return (value << bits) | (value >> (32 - bits));
}

void Sha1(const uint8_t* data, size_t len, uint8_t out_hash[20]) {
    uint32_t h0 = 0x67452301U;
    uint32_t h1 = 0xEFCDAB89U;
    uint32_t h2 = 0x98BADCFEU;
    uint32_t h3 = 0x10325476U;
    uint32_t h4 = 0xC3D2E1F0U;

    uint64_t bit_len = static_cast<uint64_t>(len) * 8ULL;
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0x00);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFU));
    }

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[chunk + i * 4 + 0]) << 24) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 2]) << 8)  |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = Rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            uint32_t temp = Rol32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rol32(b, 30);
            b = a;
            a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out_hash[i * 4 + 0] = static_cast<uint8_t>((hs[i] >> 24) & 0xFFU);
        out_hash[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xFFU);
        out_hash[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xFFU);
        out_hash[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xFFU);
    }
}

std::string Base64Encode(const uint8_t* data, size_t len) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);

        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kTable[(n >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kTable[n & 0x3F] : '=');
    }
    return out;
}

bool SendAllBytes(int fd, const uint8_t* buf, size_t len) {
    size_t sent_total = 0;
    while (sent_total < len) {
        ssize_t n = send(fd, buf + sent_total, len - sent_total, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent_total += static_cast<size_t>(n);
    }
    return true;
}

// Non-blocking check for client disconnect, Close (0x08), or Ping (0x09) frames
bool DrainWebSocketClientFrames(int client_fd, std::vector<uint8_t>& pong_payload) {
    pong_payload.clear();
    uint8_t hdr[2];
    ssize_t n = recv(client_fd, hdr, 2, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) {
        return false; // Peer closed socket cleanly
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return true; // No incoming client control frame waiting
        }
        return false;
    }
    if (n < 2) return true;

    if (recv(client_fd, hdr, 2, 0) != 2) return false;

    uint8_t opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t payload_len = hdr[1] & 0x7F;
    if (payload_len == 126) {
        uint8_t ext[2];
        if (recv(client_fd, ext, 2, 0) != 2) return false;
        payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (recv(client_fd, ext, 8, 0) != 8) return false;
        payload_len = 0;
        for (int i = 0; i < 8; ++i) payload_len = (payload_len << 8) | ext[i];
    }

    uint8_t mask_key[4]{0, 0, 0, 0};
    if (masked) {
        if (recv(client_fd, mask_key, 4, 0) != 4) return false;
    }

    std::vector<uint8_t> body;
    if (payload_len > 0 && payload_len <= 4096) {
        body.resize(static_cast<size_t>(payload_len));
        size_t got = 0;
        while (got < body.size()) {
            ssize_t r = recv(client_fd, body.data() + got, body.size() - got, 0);
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        if (masked) {
            for (size_t i = 0; i < body.size(); ++i) body[i] ^= mask_key[i % 4];
        }
    }

    if (opcode == 0x08) {
        return false; // Client sent RFC 6455 Close frame
    }
    if (opcode == 0x09) {
        pong_payload = std::move(body);
    }
    return true;
}

// Ensures Annex-B access unit on a keyframe starts with VPS/SPS/PPS without duplicating existing in-band parameter sets
std::vector<uint8_t> DeduplicateAndPrependParamSets(
    const std::vector<uint8_t>& pkt_data,
    CodecType codec,
    bool is_keyframe,
    const std::vector<uint8_t>& vps,
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps) {
    if (!is_keyframe || sps.empty() || pps.empty()) {
        return pkt_data;
    }

    auto nals = Fmp4Muxer::ParseAnnexBNals(pkt_data.data(), pkt_data.size(), codec);
    bool has_vps = false, has_sps = false, has_pps = false;
    for (const auto& nal : nals) {
        if (codec == CodecType::H265) {
            if (nal.nal_type == 32) has_vps = true;
            else if (nal.nal_type == 33) has_sps = true;
            else if (nal.nal_type == 34) has_pps = true;
        } else {
            if (nal.nal_type == 7) has_sps = true;
            else if (nal.nal_type == 8) has_pps = true;
        }
    }

    static const uint8_t kStartCode[] = {0x00, 0x00, 0x00, 0x01};
    std::vector<uint8_t> out;
    out.reserve(vps.size() + sps.size() + pps.size() + pkt_data.size() + 16);

    if (codec == CodecType::H265 && !has_vps && !vps.empty()) {
        out.insert(out.end(), kStartCode, kStartCode + 4);
        out.insert(out.end(), vps.begin(), vps.end());
    }
    if (!has_sps && !sps.empty()) {
        out.insert(out.end(), kStartCode, kStartCode + 4);
        out.insert(out.end(), sps.begin(), sps.end());
    }
    if (!has_pps && !pps.empty()) {
        out.insert(out.end(), kStartCode, kStartCode + 4);
        out.insert(out.end(), pps.begin(), pps.end());
    }
    out.insert(out.end(), pkt_data.begin(), pkt_data.end());
    return out;
}

} // namespace

RestServer& RestServer::Instance() {
    static RestServer instance;
    return instance;
}

RestServer::RestServer() {
    config_.port = 8080;
    config_.web_root = "./web";
    config_.enabled = true;
}

RestServer::~RestServer() {
    Stop();
}

void RestServer::Configure(const ApiConfig& config) {
    config_ = config;
}

bool RestServer::IsOriginAllowed(const std::string& origin) const {
    if (origin.empty()) {
        return true;
    }
    if (config_.cors_allowed_origins.empty()) {
        return false;
    }
    for (const auto& allowed : config_.cors_allowed_origins) {
        if (allowed == "*" || ToLower(allowed) == ToLower(origin)) {
            return true;
        }
    }
    return false;
}

std::string RestServer::BuildCorsHeaders(const std::string& request_origin, bool is_preflight) const {
    if (config_.cors_allowed_origins.empty()) {
        return "";
    }
    bool is_wildcard = false;
    for (const auto& allowed : config_.cors_allowed_origins) {
        if (allowed == "*") {
            is_wildcard = true;
            break;
        }
    }

    if (request_origin.empty()) {
        if (is_wildcard) {
            std::string h = "Access-Control-Allow-Origin: *\r\n";
            if (is_preflight) {
                h += "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
                     "Access-Control-Allow-Headers: Content-Type, Authorization, X-Requested-With, Range\r\n"
                     "Access-Control-Max-Age: 86400\r\n";
            }
            return h;
        }
        return "";
    }

    if (!IsOriginAllowed(request_origin)) {
        return "";
    }

    if (is_wildcard) {
        std::string h = "Access-Control-Allow-Origin: *\r\n";
        if (is_preflight) {
            h += "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
                 "Access-Control-Allow-Headers: Content-Type, Authorization, X-Requested-With, Range\r\n"
                 "Access-Control-Max-Age: 86400\r\n";
        }
        return h;
    }

    std::string h = "Access-Control-Allow-Origin: " + request_origin + "\r\n"
                    "Vary: Origin\r\n"
                    "Access-Control-Allow-Credentials: true\r\n";
    if (is_preflight) {
        h += "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
             "Access-Control-Allow-Headers: Content-Type, Authorization, X-Requested-With, Range\r\n"
             "Access-Control-Max-Age: 86400\r\n";
    }
    return h;
}

bool RestServer::Start() {
    if (is_running_.load()) {
        return true;
    }

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        LOG_ERROR << "[RestServer] Failed to create socket: " << strerror(errno);
        return false;
    }

    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_WARN << "[RestServer] Failed setsockopt SO_REUSEADDR: " << strerror(errno);
    }
#ifdef SO_REUSEPORT
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        LOG_WARN << "[RestServer] Failed setsockopt SO_REUSEPORT: " << strerror(errno);
    }
#endif

    // Set non-blocking on server socket for poll loop
    int flags = fcntl(server_fd_, F_GETFL, 0);
    fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (config_.bind_address.empty() || config_.bind_address == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, config_.bind_address.c_str(), &addr.sin_addr) <= 0) {
            LOG_ERROR << "[RestServer] Invalid bind_address '" << config_.bind_address 
                      << "'; falling back to 0.0.0.0";
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        }
    }
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));

    if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR << "[RestServer] Failed to bind " 
                  << (config_.bind_address.empty() ? "0.0.0.0" : config_.bind_address)
                  << ":" << config_.port << ": " << strerror(errno);
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 256) < 0) {
        LOG_ERROR << "[RestServer] Failed to listen on socket: " << strerror(errno);
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    is_running_.store(true);
    size_t pool_size = std::clamp<size_t>(std::thread::hardware_concurrency(), 2, 4);
    worker_pool_ = std::make_unique<ThreadPool>(pool_size);
    listener_thread_ = std::thread(&RestServer::ListenerLoop, this);

    LOG_INFO << "[RestServer] HTTP REST Control Plane listening on " 
             << (config_.bind_address.empty() ? "0.0.0.0" : config_.bind_address)
             << ":" << config_.port
             << " (HTTPS Enforced: " << (config_.enforce_https ? "YES" : "NO")
             << ", HSTS: " << (IsHstsEnabled() ? "ENABLED" : "DISABLED")
             << ", WebRoot: " << config_.web_root << ")";
    return true;
}

void RestServer::Stop() {
    if (!is_running_.exchange(false)) {
        return;
    }

    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }

    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }

    if (worker_pool_) {
        worker_pool_->Shutdown();
        worker_pool_.reset();
    }

    {
        std::lock_guard<std::mutex> lock(snapshot_cache_mutex_);
        snapshot_cache_.clear();
    }

    LOG_INFO << "[RestServer] HTTP REST Control Plane stopped cleanly.";
}

void RestServer::ListenerLoop() {
    struct pollfd pfd;
    pfd.fd = server_fd_;
    pfd.events = POLLIN;

    while (is_running_.load()) {
        int ret = poll(&pfd, 1, 200); // 200 ms timeout to check is_running_
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue;

        if (pfd.revents & POLLIN) {
            while (is_running_.load()) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
                if (active_connections_.load() >= kMaxControlConnections) {
                    SendHttpResponse(client_fd, 503, "application/json", 
                                     R"({"success":false,"status_code":503,"message":"Server Busy: Connection limit reached"})");
                    close(client_fd);
                    continue;
                }
                if (worker_pool_ && is_running_.load()) {
                    worker_pool_->Enqueue([this, client_fd]() {
                        HandleClient(client_fd);
                    });
                } else {
                    close(client_fd);
                }
            }
        }
    }
}

void RestServer::HandleClient(int client_fd) {
    active_connections_.fetch_add(1, std::memory_order_relaxed);
    struct ConnGuard {
        std::atomic<int>& c;
        bool released{false};
        ~ConnGuard() { if (!released) c.fetch_sub(1, std::memory_order_relaxed); }
    } guard{active_connections_};

    // Set 5-second timeout on client socket to prevent slowloris / connection freeze
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Low-latency TCP socket tuning (disable Nagle delay on control plane responses)
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    std::string req_str;
    req_str.reserve(8192);
    std::vector<char> buffer(4096);
    size_t header_end = std::string::npos;
    while (req_str.size() < 8192) {
        ssize_t n = recv(client_fd, buffer.data(), buffer.size(), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        req_str.append(buffer.data(), static_cast<size_t>(n));
        header_end = req_str.find("\r\n\r\n");
        if (header_end != std::string::npos) break;
    }
    if (req_str.empty()) {
        close(client_fd);
        return;
    }
    if (header_end == std::string::npos) {
        SendHttpResponse(client_fd, 400, "text/plain", "Bad Request: Malformed HTTP Headers");
        close(client_fd);
        return;
    }

    std::string header_part = req_str.substr(0, header_end);
    std::string body = req_str.substr(header_end + 4);

    std::istringstream req_stream(header_part);
    std::string request_line;
    if (!std::getline(req_stream, request_line)) {
        close(client_fd);
        return;
    }
    if (!request_line.empty() && request_line.back() == '\r') {
        request_line.pop_back();
    }

    std::istringstream line_stream(request_line);
    std::string method, full_path, http_version;
    line_stream >> method >> full_path >> http_version;
    bool is_head = (method == "HEAD");

    // Parse Headers
    std::unordered_map<std::string, std::string> headers;
    std::string h_line;
    while (std::getline(req_stream, h_line)) {
        if (!h_line.empty() && h_line.back() == '\r') h_line.pop_back();
        if (h_line.empty()) continue;
        auto colon = h_line.find(':');
        if (colon != std::string::npos) {
            std::string key = ToLower(h_line.substr(0, colon));
            std::string val = h_line.substr(colon + 1);
            // Trim leading space
            size_t start = val.find_first_not_of(" \t");
            if (start != std::string::npos) val = val.substr(start);
            headers[key] = val;
        }
    }

    // Read remaining body if Content-Length specified
    if (headers.find("content-length") != headers.end()) {
        size_t content_length = 0;
        try {
            content_length = std::stoul(headers["content-length"]);
        } catch (...) {
            content_length = 0;
        }

        if (content_length > 65536) { // 64 KB limit
            SendHttpResponse(client_fd, 413, "application/json", 
                             R"({"success":false,"status_code":413,"message":"Payload Too Large: Limit is 64 KB"})");
            close(client_fd);
            return;
        }

        while (body.size() < content_length) {
            std::vector<char> extra(4096);
            ssize_t r = recv(client_fd, extra.data(), extra.size(), 0);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            body.append(extra.data(), r);
        }
    }

    std::string request_origin;
    auto it_origin = headers.find("origin");
    if (it_origin != headers.end()) {
        request_origin = it_origin->second;
    }

    // Enforce HTTPS / TLS transport if configured (e.g. edge reverse-proxy)
    if (config_.enforce_https && method != "OPTIONS") {
        bool is_tls = false;
        auto it_proto = headers.find("x-forwarded-proto");
        if (it_proto != headers.end() && (ToLower(it_proto->second) == "https" || ToLower(it_proto->second) == "wss")) {
            is_tls = true;
        }
        auto it_ssl = headers.find("x-forwarded-ssl");
        if (it_ssl != headers.end() && ToLower(it_ssl->second) == "on") {
            is_tls = true;
        }

        if (!is_tls) {
            if (method == "GET" || method == "HEAD") {
                std::string host = "localhost";
                auto it_host = headers.find("host");
                if (it_host != headers.end() && !it_host->second.empty()) {
                    host = it_host->second;
                }
                SendHttpResponse(client_fd, 301, "text/plain", "Redirecting to HTTPS...", 
                                 "Location: https://" + host + full_path + "\r\n", request_origin);
            } else {
                SendHttpResponse(client_fd, 426, "application/json", 
                                 R"({"success":false,"status_code":426,"message":"Upgrade Required: HTTPS / TLS transport encryption is enforced"})", "", request_origin);
            }
            close(client_fd);
            return;
        }
    }

    // CORS preflight handling
    if (method == "OPTIONS") {
        if (!IsOriginAllowed(request_origin)) {
            SendHttpResponse(client_fd, 403, "application/json", 
                             R"({"success":false,"status_code":403,"message":"CORS Origin Not Allowed"})", "", request_origin);
            close(client_fd);
            return;
        }
        std::string cors_preflight = BuildCorsHeaders(request_origin, true);
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << cors_preflight
            << "Content-Length: 0\r\n"
            << "Connection: close\r\n\r\n";
        std::string resp = oss.str();
        send(client_fd, resp.c_str(), resp.size(), MSG_NOSIGNAL);
        close(client_fd);
        return;
    }

    // Extract Path & Query
    std::string path = full_path;
    std::string query_str;
    auto q_pos = full_path.find('?');
    if (q_pos != std::string::npos) {
        path = full_path.substr(0, q_pos);
        query_str = full_path.substr(q_pos + 1);
    }
    nlohmann::json query_params = ParseQueryParams(query_str);

    // Extract Auth Token (Strict: Authorization header is required for REST control plane)
    std::string auth_token;
    if (headers.find("authorization") != headers.end()) {
        std::string auth = headers["authorization"];
        if (auth.rfind("Bearer ", 0) == 0 || auth.rfind("bearer ", 0) == 0) {
            auth_token = auth.substr(7);
        } else {
            auth_token = auth;
        }
    }

    // Support RFC 6455 Sec-WebSocket-Protocol: bearer, <token> for browser WebSocket streaming
    std::string ws_subprotocol;
    if (auth_token.empty() && headers.find("sec-websocket-protocol") != headers.end()) {
        std::string sec_proto = headers["sec-websocket-protocol"];
        auto comma_pos = sec_proto.find(',');
        if (comma_pos != std::string::npos) {
            std::string p1 = sec_proto.substr(0, comma_pos);
            std::string p2 = sec_proto.substr(comma_pos + 1);
            auto trim = [](std::string s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
                return s;
            };
            p1 = trim(p1);
            p2 = trim(p2);
            if (ToLower(p1) == "bearer") {
                auth_token = p2;
                ws_subprotocol = "bearer";
            } else {
                auth_token = p1;
            }
        } else if (sec_proto.rfind("bearer.", 0) == 0) {
            auth_token = sec_proto.substr(7);
            ws_subprotocol = "bearer";
        } else if (sec_proto.size() == 64) {
            auth_token = sec_proto;
            ws_subprotocol = sec_proto;
        }
    }

    // Restrict ?token= query parameter strictly and exclusively to media payload endpoints
    // (where native HTML <video>/<img> tags cannot set headers).
    // Query tokens are strictly FORBIDDEN on all control plane REST endpoints to prevent log leaks.
    bool is_media_endpoint = (path == "/api/v1/ws/stream" || 
                              path == "/api/v1/stream" ||
                              path.rfind("/snapshot") != std::string::npos ||
                              path.rfind("/recordings/playlist.m3u8") != std::string::npos ||
                              path.rfind("/recordings/video") != std::string::npos ||
                              path.rfind("/recordings/stream") != std::string::npos ||
                              path.rfind("/recordings/file") != std::string::npos);

    if (auth_token.empty() && is_media_endpoint && query_params.contains("token")) {
        if (query_params["token"].is_string()) {
            auth_token = query_params["token"].get<std::string>();
        } else if (query_params["token"].is_number()) {
            auth_token = std::to_string(query_params["token"].get<int64_t>());
        }
    }

    // 1. Static Web Asset Serving (Files under web_root)
    if (path == "/" || path == "/index.html" || path.rfind("/api/", 0) != 0) {
        // Fast-path decode and rejection of null bytes & traversal sequences
        std::string decoded_path = UrlDecode(path);
        if (decoded_path.find('\0') != std::string::npos ||
            decoded_path.find("..") != std::string::npos ||
            decoded_path.find('\\') != std::string::npos) {
            SendHttpResponse(client_fd, 403, "text/plain", "Forbidden", "", request_origin);
            close(client_fd);
            return;
        }

        // Catch nested/double-encoding bypasses (e.g. %252e%252e)
        if (decoded_path.find('%') != std::string::npos) {
            std::string d2 = UrlDecode(decoded_path);
            if (d2.find('\0') != std::string::npos || d2.find("..") != std::string::npos || d2.find('\\') != std::string::npos) {
                SendHttpResponse(client_fd, 403, "text/plain", "Forbidden", "", request_origin);
                close(client_fd);
                return;
            }
        }

        // Fast in-memory lexical path normalization (zero redundant disk syscalls)
        std::error_code ec;
        std::filesystem::path root_path = std::filesystem::weakly_canonical(config_.web_root, ec);
        if (std::filesystem::is_directory(root_path / "dist", ec)) {
            root_path = root_path / "dist";
        }
        std::string file_subpath = (decoded_path == "/" ? "/index.html" : decoded_path);
        std::filesystem::path target_path = (root_path / std::filesystem::path(file_subpath).relative_path()).lexically_normal();

        std::string root_str = root_path.lexically_normal().string();
        if (!root_str.empty() && root_str.back() != '/') {
            root_str += '/';
        }
        std::string target_str = target_path.string();

        // Enforce strict containment within web_root jail
        if (target_str.rfind(root_str, 0) != 0 && target_str != root_path.lexically_normal().string()) {
            SendHttpResponse(client_fd, 403, "text/plain", "Forbidden", "", request_origin);
            close(client_fd);
            return;
        }

        // Verify symlinks do not escape the jail root
        if (std::filesystem::is_symlink(target_path, ec)) {
            auto symlink_target = std::filesystem::canonical(target_path, ec);
            if (ec || symlink_target.string().rfind(root_str, 0) != 0) {
                SendHttpResponse(client_fd, 403, "text/plain", "Forbidden", "", request_origin);
                close(client_fd);
                return;
            }
            target_path = symlink_target;
        }

        if (std::filesystem::is_directory(target_path, ec)) {
            auto index_cand = target_path / "index.html";
            if (std::filesystem::is_regular_file(index_cand, ec)) {
                target_path = index_cand;
            }
        } else if (!std::filesystem::exists(target_path, ec)) {
            // SPA fallback: serve index.html for client-side router endpoints
            if (target_path.extension().empty()) {
                auto index_cand = root_path / "index.html";
                if (std::filesystem::is_regular_file(index_cand, ec)) {
                    target_path = index_cand;
                }
            }
        }

        std::string file_path = target_path.string();

        struct stat st;
        if (stat(file_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            int file_fd = open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
            if (file_fd >= 0) {
                size_t file_size = static_cast<size_t>(st.st_size);
                std::string mime = GetMimeType(file_path);
                std::ostringstream oss;
                oss << "HTTP/1.1 200 OK\r\n";
                oss << "Content-Type: " << mime << "\r\n";
                oss << "Content-Length: " << file_size << "\r\n";
                oss << BuildCorsHeaders(request_origin, false);
                oss << "Cache-Control: no-cache, no-store, must-revalidate\r\n";
                oss << "Pragma: no-cache\r\n";
                oss << "Expires: 0\r\n";
                oss << "X-Content-Type-Options: nosniff\r\n";
                oss << "X-Frame-Options: SAMEORIGIN\r\n";
                oss << "Referrer-Policy: strict-origin-when-cross-origin\r\n";
                oss << "Cross-Origin-Opener-Policy: same-origin\r\n";
                oss << "Cross-Origin-Embedder-Policy: require-corp\r\n";
                oss << "Cross-Origin-Resource-Policy: cross-origin\r\n";
                if (IsHstsEnabled()) {
                    oss << "Strict-Transport-Security: max-age=31536000; includeSubDomains\r\n";
                }
                oss << "Connection: close\r\n\r\n";

                std::string headers_str = oss.str();
                SendAllBytes(client_fd, reinterpret_cast<const uint8_t*>(headers_str.data()), headers_str.size());

                if (method == "GET" && file_size > 0) {
                    off_t offset = 0;
                    size_t remaining = file_size;
                    while (remaining > 0) {
                        size_t to_send = std::min(remaining, static_cast<size_t>(1048576));
                        ssize_t sent = sendfile(client_fd, file_fd, &offset, to_send);
                        if (sent <= 0) {
                            if (sent < 0 && errno == EINTR) continue;
                            break;
                        }
                        remaining -= static_cast<size_t>(sent);
                    }
                }
                close(file_fd);
                close(client_fd);
                return;
            }
        }

        // If file not found in web root, return 404
        SendHttpResponse(client_fd, 404, "text/plain", "Not Found");
        close(client_fd);
        return;
    }

    // 2. REST API Endpoints -> Forward directly to CommandDispatcher
    nlohmann::json body_json = nlohmann::json::object();
    if (!body.empty()) {
        try {
            body_json = nlohmann::json::parse(body);
        } catch (const nlohmann::json::parse_error& e) {
            SendHttpResponse(client_fd, 400, "application/json", 
                             nlohmann::json{
                                 {"success", false},
                                 {"status_code", 400},
                                 {"message", std::string("Malformed JSON body: ") + e.what()}
                             }.dump());
            close(client_fd);
            return;
        }
    }

    std::string target_action;
    nlohmann::json payload = body_json;

    // 3. Media Plane: WebSocket Zero-Transcode Live Video Streaming (RFC 6455)
    if (method == "GET" && path == "/api/v1/ws/stream") {

        int channel_id = query_params.value("channel", 1);
        if (channel_id <= 0 || channel_id > 64) {
            SendHttpResponse(client_fd, 400, "application/json", 
                             R"({"success":false,"status_code":400,"message":"Invalid channel parameter"})");
            close(client_fd);
            return;
        }

        std::string stream_str = query_params.value("stream", "sub");
        StreamType stream_type = (stream_str == "main") ? StreamType::MAIN : StreamType::SUB;

        if (HandleWebSocketHandshake(client_fd, header_part, ws_subprotocol)) {
            if (active_media_streams_.load() >= kMaxMediaStreams) {
                close(client_fd);
                return;
            }
            try {
                active_media_streams_.fetch_add(1);
                guard.released = true;
                active_connections_.fetch_sub(1, std::memory_order_relaxed);
                std::thread([this, client_fd, channel_id, stream_type]() {
                    HandleWebSocketStream(client_fd, channel_id, stream_type);
                    active_media_streams_.fetch_sub(1);
                }).detach();
                return;
            } catch (const std::exception& e) {
                active_media_streams_.fetch_sub(1);
                LOG_WARN << "Failed to spawn WebSocket stream thread: " << e.what();
                close(client_fd);
                return;
            }
        } else {
            close(client_fd);
            return;
        }
    }

    // 4. Media Plane: Zero-Transcoding Live Video Streaming (fMP4 / MSE)
    if (method == "GET" && path == "/api/v1/stream") {

        int channel_id = query_params.value("channel", 1);
        if (channel_id <= 0 || channel_id > 64) {
            SendHttpResponse(client_fd, 404, "application/json", 
                             R"({"success":false,"status_code":404,"message":"Camera channel not found"})");
            close(client_fd);
            return;
        }

        std::string stream_str = query_params.value("stream", "sub");
        StreamType stream_type = (stream_str == "main") ? StreamType::MAIN : StreamType::SUB;
        if (active_media_streams_.load() >= kMaxMediaStreams) {
            SendHttpResponse(client_fd, 503, "application/json", 
                             R"({"success":false,"status_code":503,"message":"Maximum media streams reached"})");
            close(client_fd);
            return;
        }
        try {
            active_media_streams_.fetch_add(1);
            guard.released = true;
            active_connections_.fetch_sub(1, std::memory_order_relaxed);
            std::thread([this, client_fd, channel_id, stream_type, request_origin]() {
                HandleStreamClient(client_fd, channel_id, stream_type, request_origin);
                active_media_streams_.fetch_sub(1);
            }).detach();
            return;
        } catch (const std::exception& e) {
            active_media_streams_.fetch_sub(1);
            LOG_WARN << "Failed to spawn fMP4 stream thread: " << e.what();
            close(client_fd);
            return;
        }
    }

    // 4. Media Plane: On-Demand Static Snapshot
    if (method == "GET" && path.rfind("/api/v1/cameras/", 0) == 0 && path.rfind("/snapshot") == path.size() - 9) {

        std::string cam_id_str = path.substr(16, path.size() - 16 - 9);
        int channel_id = std::atoi(cam_id_str.c_str());
        if (channel_id <= 0 || channel_id > 64) {
            SendHttpResponse(client_fd, 404, "application/json", 
                             R"({"success":false,"status_code":404,"message":"Camera channel not found"})");
            close(client_fd);
            return;
        }

        HandleSnapshotClient(client_fd, channel_id);
        return;
    }

    // 5. Media Plane: Recordings Query, Video Streaming & Clip Export
    if (method == "GET" && path == "/api/v1/recordings/dates") {
        HandleRecordingsDates(client_fd, request_origin);
        return;
    }
    if (method == "GET" && path == "/api/v1/recordings/playlist.m3u8") {
        HandleRecordingsPlaylist(client_fd, query_params, request_origin);
        return;
    }
    if (method == "GET" && path == "/api/v1/recordings") {
        HandleRecordingsList(client_fd, query_params, request_origin);
        return;
    }
    if (method == "GET" && path == "/api/v1/recordings/export/status") {
        HandleRecordingExportStatus(client_fd, query_params, request_origin);
        return;
    }
    if (method == "GET" && path == "/api/v1/recordings/export/download") {
        HandleRecordingExportDownload(client_fd, query_params, headers, request_origin);
        return;
    }
    if (method == "POST" && path == "/api/v1/recordings/export") {
        HandleRecordingExport(client_fd, method, path, body, request_origin);
        return;
    }
    if (method == "GET" && (path == "/api/v1/recordings/video" || path == "/api/v1/recordings/stream" || path == "/api/v1/recordings/file" || path == "/api/v1/recordings/thumbnail")) {
        HandleRecordingVideo(client_fd, query_params, headers, request_origin);
        return;
    }
    if (method == "POST" && (path == "/api/v1/recordings/lock" || (path.rfind("/api/v1/recordings/", 0) == 0 && path.rfind("/lock") == path.size() - 5))) {
        HandleRecordingLock(client_fd, method, path, body, request_origin);
        return;
    }

    // Route Mapping
    if ((method == "GET" || is_head) && path == "/api/v1/system/status") {
        target_action = "system.status";
        payload = nlohmann::json::object();
    } else if ((method == "GET" || is_head) && path == "/api/v1/diagnostics/trace") {
        target_action = "diagnostics.trace";
        payload = query_params;
    } else if ((method == "GET" || is_head) && path == "/api/v1/cameras") {
        target_action = "camera.list";
        payload = nlohmann::json::object();
    } else if (method == "POST" && path == "/api/v1/cameras") {
        target_action = "camera.add";
        payload = body_json;
    } else if ((method == "POST" || method == "PUT") && path.rfind("/api/v1/cameras/", 0) == 0 && path.find('/', 16) == std::string::npos) {
        // e.g. POST /api/v1/cameras/2 or PUT /api/v1/cameras/2
        std::string cam_id_str = path.substr(16);
        target_action = "camera.update";
        payload = body_json;
        payload["id"] = std::atoi(cam_id_str.c_str());
    } else if (method == "DELETE" && path.rfind("/api/v1/cameras/", 0) == 0 && path.find('/', 16) == std::string::npos) {
        // e.g. DELETE /api/v1/cameras/2
        std::string cam_id_str = path.substr(16);
        target_action = "camera.remove";
        payload = nlohmann::json::object();
        payload["channel_id"] = std::atoi(cam_id_str.c_str());
        payload["id"] = std::atoi(cam_id_str.c_str());
    } else if (method == "POST" && path.rfind("/api/v1/cameras/", 0) == 0 && path.rfind("/restart") == path.size() - 8) {
        // e.g. /api/v1/cameras/2/restart
        std::string cam_id_str = path.substr(16, path.size() - 16 - 8);
        target_action = "camera.restart";
        payload["channel_id"] = std::atoi(cam_id_str.c_str());
    } else if (method == "POST" && path.rfind("/api/v1/cameras/", 0) == 0 && path.rfind("/record/start") == path.size() - 13) {
        // e.g. /api/v1/cameras/2/record/start
        std::string cam_id_str = path.substr(16, path.size() - 16 - 13);
        target_action = "recording.start";
        payload = body_json;
        payload["channel_id"] = std::atoi(cam_id_str.c_str());
    } else if (method == "POST" && path.rfind("/api/v1/cameras/", 0) == 0 && path.rfind("/record/stop") == path.size() - 12) {
        // e.g. /api/v1/cameras/2/record/stop
        std::string cam_id_str = path.substr(16, path.size() - 16 - 12);
        target_action = "recording.stop";
        payload = body_json;
        payload["channel_id"] = std::atoi(cam_id_str.c_str());
    } else if (method == "GET" && path.rfind("/api/v1/cameras/", 0) == 0 && path.rfind("/record/status") == path.size() - 14) {
        // e.g. /api/v1/cameras/2/record/status
        std::string cam_id_str = path.substr(16, path.size() - 16 - 14);
        target_action = "recording.status";
        payload = query_params;
        payload["channel_id"] = std::atoi(cam_id_str.c_str());
    } else if (method == "GET" && path == "/api/v1/onvif/discover") {
        target_action = "onvif.discover";
        payload = query_params;
    } else if (method == "POST" && path == "/api/v1/onvif/probe") {
        target_action = "onvif.probe";
        payload = body_json;

    } else if (method == "GET" && path == "/api/v1/onvif/snapshot_uri") {
        target_action = "onvif.snapshot_uri";
        payload = query_params;
    } else if (method == "GET" && path == "/api/v1/onvif/encoder") {
        target_action = "onvif.encoder.get";
        payload = query_params;
    } else if (method == "POST" && path == "/api/v1/onvif/encoder") {
        target_action = "onvif.encoder.set";
        payload = body_json;

    } else if (method == "POST" && path == "/api/v1/onvif/reboot") {
        target_action = "onvif.system.reboot";
        payload = body_json;
    } else if (method == "POST" && path == "/api/v1/onvif/datetime") {
        target_action = "onvif.datetime.set";
        payload = body_json;
    } else if (method == "GET" && path == "/api/v1/onvif/network") {
        target_action = "onvif.network.interfaces";
        payload = query_params;

    } else if (method == "GET" && path == "/api/v1/layout") {
        target_action = "layout.get";
        payload = nlohmann::json::object();
    } else if (method == "POST" && path == "/api/v1/layout") {
        target_action = "layout.set";
    } else if (method == "POST" && (path == "/api/v1/layout/fullscreen" || path == "/api/v1/layout/exit-fullscreen")) {
        target_action = "layout.fullscreen";
        if (path == "/api/v1/layout/exit-fullscreen") {
            payload["exit"] = true;
        }

    // Storage Management Endpoints
    } else if (method == "GET" && (path == "/api/v1/storage" || path == "/api/v1/storage/status")) {
        target_action = "storage.get";
        payload = nlohmann::json::object();
    } else if (method == "POST" && (path == "/api/v1/storage/path" || path == "/api/v1/storage")) {
        target_action = "storage.set_path";
        payload = body_json;

    // Authentication & User Management Endpoints
    } else if (method == "POST" && path == "/api/v1/auth/login") {
        target_action = "auth.login";
        payload = body_json;
    } else if (method == "POST" && path == "/api/v1/auth/logout") {
        target_action = "auth.logout";
        payload = body_json;
    } else if (method == "GET" && path == "/api/v1/auth/me") {
        target_action = "auth.me";
        payload = nlohmann::json::object();
    } else if (method == "GET" && path == "/api/v1/users") {
        target_action = "user.list";
        payload = nlohmann::json::object();
    } else if (method == "POST" && path == "/api/v1/users") {
        target_action = "user.add";
        payload = body_json;
    } else if (method == "DELETE" && path.rfind("/api/v1/users/", 0) == 0 && path.find('/', 14) == std::string::npos) {
        std::string target_user = path.substr(14);
        target_action = "user.delete";
        payload = nlohmann::json::object();
        payload["username"] = target_user;
    } else if (method == "POST" && (path == "/api/v1/users/change_password" || path == "/api/v1/users/password")) {
        target_action = "user.change_password";
        payload = body_json;
    } else if (method == "POST" && path == "/api/v1/users/reset_password") {
        target_action = "user.reset_password";
        payload = body_json;

    } else if (method == "GET" && path == "/api/v1/commands") {
        // Discovery endpoint: list all registered command schemas
        auto cmds = CommandDispatcher::Instance().GetRegisteredCommands();
        nlohmann::json resp;
        resp["success"] = true;
        resp["status_code"] = 200;
        resp["data"] = cmds;
        SendHttpResponse(client_fd, 200, "application/json", resp.dump(2));
        close(client_fd);
        return;
    } else if (method == "POST" && path == "/api/v1/dispatch") {
        // Universal command dispatch endpoint
        if (body_json.contains("action") && body_json["action"].is_string()) {
            target_action = body_json["action"].get<std::string>();
            if (body_json.contains("payload")) {
                payload = body_json["payload"];
            }
        } else {
            SendHttpResponse(client_fd, 400, "application/json", 
                             R"({"success":false,"status_code":400,"message":"Missing 'action' in JSON envelope"})");
            close(client_fd);
            return;
        }
    } else {
        SendHttpResponse(client_fd, 404, "application/json", 
                         nlohmann::json{
                             {"success", false},
                             {"status_code", 404},
                             {"message", "Endpoint not found: " + method + " " + path}
                         }.dump());
        close(client_fd);
        return;
    }

    // Construct and dispatch CommandEnvelope
    CommandEnvelope env;
    env.action = target_action;
    env.auth_token = auth_token;
    env.payload = payload;
    env.timestamp_ms = time_utils::WallTimeMs();

    CommandResult result = CommandDispatcher::Instance().Dispatch(env);

    SendHttpResponse(client_fd, result.status_code, "application/json", result.ToJson().dump(2), "", request_origin, is_head);
    close(client_fd);
}

void RestServer::HandleStreamClient(int client_fd, int channel_id, StreamType stream_type, const std::string& request_origin) {
    if (stream_type == StreamType::MAIN) {
        CameraManager::Instance().EnsureMainStream(channel_id);
    } else {
        CameraManager::Instance().StartSubStream(channel_id);
    }
    CodecType cached_codec = StreamBroker::Instance().GetCachedCodec(channel_id, stream_type);
    std::string codec_hdr = "AUTO";
    if (cached_codec == CodecType::H265) {
        codec_hdr = "H265";
    } else if (cached_codec == CodecType::H264) {
        codec_hdr = "H264";
    }

    std::string cors_hdr = BuildCorsHeaders(request_origin, false);
    std::string http_header = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: video/mp4\r\n"
        "X-Stream-Codec: " + codec_hdr + "\r\n"
        "Access-Control-Expose-Headers: X-Stream-Codec\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Expires: 0\r\n"
        "Cross-Origin-Resource-Policy: cross-origin\r\n" +
        cors_hdr +
        "Connection: close\r\n\r\n";

    ssize_t sent = send(client_fd, http_header.c_str(), http_header.size(), MSG_NOSIGNAL);
    if (sent <= 0) {
        close(client_fd);
        return;
    }

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    auto ctx = std::make_shared<StreamContext>();

    auto sub_id = StreamBroker::Instance().Subscribe(
        channel_id, stream_type,
        [ctx](const MediaPacketPtr& pkt) {
            if (!pkt || pkt->IsAudio() || ctx->closed.load()) return;
            std::unique_lock<std::mutex> lock(ctx->mtx);
            if (ctx->q.size() >= StreamContext::kMaxDepth) {
                ctx->q.pop_front(); // head-drop stale frame
            }
            ctx->q.push_back(pkt);
            ctx->cv.notify_one();
        }
    );

    bool init_sent = false;
    std::vector<uint8_t> vps, sps, pps;
    bool is_h265 = (cached_codec == CodecType::H265);
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t seq = 1;
    uint64_t last_pts = 0;
    uint64_t base_pts = 0;
    bool has_base_pts = false;
    int idle_cycles = 0;

    LOG_INFO << "[RestServer] Client connected to live fMP4 stream (Cam: " 
             << channel_id << ", " << (stream_type == StreamType::MAIN ? "MAIN" : "SUB") << ")";

    while (is_running_.load() && !ctx->closed.load()) {
        MediaPacketPtr pkt;
        {
            std::unique_lock<std::mutex> lock(ctx->mtx);
            bool ok = ctx->cv.wait_for(lock, std::chrono::milliseconds(100), [&]() {
                return !ctx->q.empty() || !is_running_.load() || ctx->closed.load();
            });
            if (!is_running_.load() || ctx->closed.load()) break;
            if (!ok || ctx->q.empty()) {
                char probe = 0;
                ssize_t peek_ret = recv(client_fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
                if (peek_ret == 0 || (peek_ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    break;
                }
                idle_cycles++;
                if (idle_cycles >= 100) {
                    break;
                }
                continue;
            }
            pkt = ctx->q.front();
            ctx->q.pop_front();
            idle_cycles = 0;
        }

        CodecType pkt_codec = pkt->codec;
        if (pkt_codec == CodecType::UNKNOWN && !pkt->data.empty()) {
            pkt_codec = DetectNalCodecFromParamSets(pkt->data.data(), pkt->data.size());
        }

        if (pkt_codec == CodecType::H265) {
            if (!is_h265 && init_sent) {
                init_sent = false;
                has_base_pts = false;
                vps.clear();
                sps.clear();
                pps.clear();
            }
            is_h265 = true;
        } else if (pkt_codec == CodecType::H264) {
            if (is_h265 && init_sent) {
                init_sent = false;
                has_base_pts = false;
                vps.clear();
                sps.clear();
                pps.clear();
            }
            is_h265 = false;
        }

        // If packet contains new parameter sets (SPS/PPS/VPS), check for dynamic resolution change
        if (init_sent && pkt->HasParameterSets()) {
            if (is_h265) {
                std::vector<uint8_t> new_vps, new_sps, new_pps;
                Fmp4Muxer::ExtractParameterSetsH265(pkt->data.data(), pkt->data.size(), new_vps, new_sps, new_pps);
                if (!new_vps.empty() && new_vps != vps) vps = std::move(new_vps);
                if (!new_pps.empty() && new_pps != pps) pps = std::move(new_pps);
                if (!new_sps.empty()) {
                    uint32_t nw = 0, nh = 0;
                    if (Fmp4Muxer::ParseH265SpsDimensions(new_sps.data(), new_sps.size(), nw, nh) && nw > 0 && nh > 0) {
                        if (nw != width || nh != height || new_sps != sps) {
                            LOG_INFO << "[RestServer] Dynamic H.265 resolution change detected ("
                                     << width << "x" << height << " -> " << nw << "x" << nh << "); re-emitting init segment";
                            sps = std::move(new_sps);
                            width = nw;
                            height = nh;
                            init_sent = false;
                        }
                    }
                }
            } else {
                std::vector<uint8_t> new_sps, new_pps;
                Fmp4Muxer::ExtractParameterSets(pkt->data.data(), pkt->data.size(), new_sps, new_pps);
                if (!new_pps.empty() && new_pps != pps) pps = std::move(new_pps);
                if (!new_sps.empty()) {
                    uint32_t nw = 0, nh = 0;
                    if (Fmp4Muxer::ParseSpsDimensions(new_sps.data(), new_sps.size(), nw, nh) && nw > 0 && nh > 0) {
                        if (nw != width || nh != height || new_sps != sps) {
                            LOG_INFO << "[RestServer] Dynamic H.264 resolution change detected ("
                                     << width << "x" << height << " -> " << nw << "x" << nh << "); re-emitting init segment";
                            sps = std::move(new_sps);
                            width = nw;
                            height = nh;
                            init_sent = false;
                        }
                    }
                }
            }
        }

        if (!init_sent) {
            if (is_h265) {
                auto nals = Fmp4Muxer::ParseAnnexBNals(pkt->data.data(), pkt->data.size(), true);
                for (const auto& nal : nals) {
                    if (nal.type == 32 && vps.empty()) {
                        vps.assign(nal.data, nal.data + nal.size);
                    } else if (nal.type == 33 && sps.empty()) {
                        sps.assign(nal.data, nal.data + nal.size);
                    } else if (nal.type == 34 && pps.empty()) {
                        pps.assign(nal.data, nal.data + nal.size);
                    }
                }
                if (vps.empty() || sps.empty() || pps.empty()) {
                    Fmp4Muxer::ExtractParameterSetsH265(pkt->data.data(), pkt->data.size(), vps, sps, pps);
                }

                if (!vps.empty() && !sps.empty() && !pps.empty()) {
                    uint32_t pw = 0, ph = 0;
                    if (Fmp4Muxer::ParseH265SpsDimensions(sps.data(), sps.size(), pw, ph) && pw > 0 && ph > 0) {
                        width = pw;
                        height = ph;
                    }
                    if (width == 0 || height == 0) {
                        continue; // Wait until valid dimensions are extracted
                    }
                    auto init_seg = Fmp4Muxer::BuildInitSegmentH265(vps, sps, pps, width, height, 90000);
                    ssize_t n = send(client_fd, init_seg.data(), init_seg.size(), MSG_NOSIGNAL);
                    if (n <= 0) break;
                    init_sent = true;
                } else {
                    continue; // Wait until VPS, SPS, and PPS are captured
                }
            } else {
                auto nals = Fmp4Muxer::ParseAnnexBNals(pkt->data.data(), pkt->data.size(), false);
                for (const auto& nal : nals) {
                    if (nal.type == 7 && sps.empty()) {
                        sps.assign(nal.data, nal.data + nal.size);
                    } else if (nal.type == 8 && pps.empty()) {
                        pps.assign(nal.data, nal.data + nal.size);
                    }
                }
                if (sps.empty() || pps.empty()) {
                    Fmp4Muxer::ExtractParameterSets(pkt->data.data(), pkt->data.size(), sps, pps);
                }

                if (!sps.empty() && !pps.empty()) {
                    uint32_t pw = 0, ph = 0;
                    if (Fmp4Muxer::ParseSpsDimensions(sps.data(), sps.size(), pw, ph) && pw > 0 && ph > 0) {
                        width = pw;
                        height = ph;
                    }
                    if (width == 0 || height == 0) {
                        continue; // Wait until valid dimensions are extracted
                    }
                    auto init_seg = Fmp4Muxer::BuildInitSegment(sps, pps, width, height, 90000);
                    ssize_t n = send(client_fd, init_seg.data(), init_seg.size(), MSG_NOSIGNAL);
                    if (n <= 0) break;
                    init_sent = true;
                } else {
                    continue; // Wait until SPS and PPS are captured
                }
            }
        }

        // Ensure playback starts on an IDR keyframe
        if (!has_base_pts && !pkt->is_keyframe) {
            continue;
        }

        auto avcc = Fmp4Muxer::AnnexBToAvcc(pkt->data.data(), pkt->data.size(), is_h265);
        if (avcc.empty()) continue;

        if (!has_base_pts) {
            base_pts = (pkt->pts_us > 0) ? ((pkt->pts_us * 90) / 1000) : 0;
            has_base_pts = true;
            last_pts = 0;
        }

        uint64_t raw_pts = (pkt->pts_us > 0) ? ((pkt->pts_us * 90) / 1000) : (base_pts + last_pts + 3600);
        uint64_t norm_pts = (raw_pts >= base_pts) ? (raw_pts - base_pts) : 0;
        uint32_t dur = static_cast<uint32_t>((norm_pts > last_pts) ? (norm_pts - last_pts) : 3600);
        if (dur > 90000 || dur == 0) dur = 3600; // Clamp jump anomalies
        last_pts = norm_pts;

        auto seg = Fmp4Muxer::BuildMediaSegment(seq++, norm_pts, dur, pkt->is_keyframe, avcc.data(), avcc.size());
        ssize_t n = send(client_fd, seg.data(), seg.size(), MSG_NOSIGNAL);
        if (n <= 0) {
            // Client closed stream connection
            break;
        }
    }

    ctx->closed.store(true);
    StreamBroker::Instance().Unsubscribe(sub_id);
    close(client_fd);
    LOG_INFO << "[RestServer] Client disconnected from live fMP4 stream (Cam: " << channel_id << ")";
}

void RestServer::HandleSnapshotClient(int client_fd, int channel_id) {
    DecodedFramePtr frame = LiveController::Instance().GetLatestFrame(channel_id);
    if (frame && !frame->data.empty() && frame->width > 0 && frame->height > 0) {
        {
            std::lock_guard<std::mutex> lock(snapshot_cache_mutex_);
            auto it = snapshot_cache_.find(channel_id);
            if (it != snapshot_cache_.end() &&
                it->second.pts_us == frame->pts_us &&
                it->second.width == frame->width &&
                it->second.height == frame->height &&
                !it->second.bmp_bytes.empty()) {
                SendHttpResponse(client_fd, 200, "image/bmp", it->second.bmp_bytes, "Cache-Control: no-cache\r\n");
                close(client_fd);
                return;
            }
        }

        std::string bmp;
        if (ConvertFrameToBmp(frame, bmp, 320)) {
            {
                std::lock_guard<std::mutex> lock(snapshot_cache_mutex_);
                auto& entry = snapshot_cache_[channel_id];
                entry.pts_us = frame->pts_us;
                entry.width = frame->width;
                entry.height = frame->height;
                entry.bmp_bytes = bmp;
            }

            SendHttpResponse(client_fd, 200, "image/bmp", bmp, "Cache-Control: no-cache\r\n");
            close(client_fd);
            return;
        }
    }

    // Fallback: Camera offline or no frame rendered yet -> return SVG status
    std::string timestamp_str = time_utils::FormatIso8601(time_utils::WallTimeMs());
    bool online = CameraManager::Instance().IsCameraOnline(channel_id);
    std::string status_color = online ? "#2ed573" : "#ff4757";
    std::string status_text = online ? "ONLINE - READY" : "OFFLINE";

    LiveTileMetrics metrics = LiveController::Instance().GetChannelMetrics(channel_id);
    uint32_t stream_w = metrics.width;
    uint32_t stream_h = metrics.height;
    uint32_t canvas_w = (stream_w > 0) ? stream_w : 640;
    uint32_t canvas_h = (stream_h > 0) ? stream_h : 360;
    std::string res_label = (stream_w > 0 && stream_h > 0)
        ? (" (" + std::to_string(stream_w) + "x" + std::to_string(stream_h) + ")")
        : "";

    std::ostringstream svg;
    svg << "<svg xmlns='http://www.w3.org/2000/svg' width='" << canvas_w << "' height='" << canvas_h
        << "' viewBox='0 0 " << canvas_w << " " << canvas_h << "'>\n"
        << "<defs>\n"
        << "  <linearGradient id='bg' x1='0%' y1='0%' x2='100%' y2='100%'>\n"
        << "    <stop offset='0%' stop-color='#1e272e'/>\n"
        << "    <stop offset='100%' stop-color='#0f1418'/>\n"
        << "  </linearGradient>\n"
        << "</defs>\n"
        << "<rect width='" << canvas_w << "' height='" << canvas_h << "' fill='url(#bg)' rx='8'/>\n"
        << "<circle cx='32' cy='32' r='8' fill='" << status_color << "'/>\n"
        << "<text x='48' y='37' fill='#ecf0f1' font-family='sans-serif' font-size='16' font-weight='bold'>Camera " << channel_id << " Snapshot" << res_label << "</text>\n"
        << "<text x='32' y='180' fill='#a4b0be' font-family='sans-serif' font-size='20' font-weight='600'>" << status_text << "</text>\n"
        << "<text x='32' y='210' fill='#747d8c' font-family='sans-serif' font-size='14'>Timestamp: " << timestamp_str << "</text>\n"
        << "<text x='32' y='" << (canvas_h > 40 ? canvas_h - 30 : canvas_h) << "' fill='#57606f' font-family='sans-serif' font-size='12'>NVR v2.0 Platform (NXP i.MX 95) • Zero-Transcode Engine</text>\n"
        << "</svg>";

    std::string body = svg.str();
    SendHttpResponse(client_fd, 200, "image/svg+xml", body, "Cache-Control: no-cache\r\n");
    close(client_fd);
}

void RestServer::HandleRecordingsList(int client_fd, const nlohmann::json& query_params, const std::string& request_origin) {
    auto res = PlaybackEngine::Instance().QueryRecordings(query_params);
    nlohmann::json resp;
    resp["success"] = true;
    resp["status_code"] = 200;
    resp["data"] = res.value("segments", nlohmann::json::array());
    resp["segments"] = res.value("segments", nlohmann::json::array());
    resp["review_items"] = res.value("review_items", nlohmann::json::array());

    SendHttpResponse(client_fd, 200, "application/json", resp.dump(), "", request_origin);
    close(client_fd);
}

void RestServer::HandleRecordingsDates(int client_fd, const std::string& request_origin) {
    auto dates = PlaybackEngine::Instance().GetRecordingDates();
    nlohmann::json resp;
    resp["success"] = true;
    resp["status_code"] = 200;
    resp["data"] = dates;
    resp["dates"] = dates;

    SendHttpResponse(client_fd, 200, "application/json", resp.dump(), "", request_origin);
    close(client_fd);
}

void RestServer::HandleRecordingsPlaylist(int client_fd, const nlohmann::json& query_params, const std::string& request_origin) {
    auto res = PlaybackEngine::Instance().QueryRecordings(query_params);
    nlohmann::json segments = res.value("segments", nlohmann::json::array());

    std::ostringstream oss;
    oss << "#EXTM3U\n";
    oss << "#EXT-X-VERSION:7\n";

    double max_dur = 10.0;
    for (const auto& seg : segments) {
        double d = seg.value("duration_seconds", 60.0);
        if (d > max_dur) max_dur = d;
    }
    int target_duration = static_cast<int>(std::ceil(max_dur));
    if (target_duration < 1) target_duration = 10;

    oss << "#EXT-X-TARGETDURATION:" << target_duration << "\n";
    oss << "#EXT-X-MEDIA-SEQUENCE:0\n";
    oss << "#EXT-X-PLAYLIST-TYPE:VOD\n\n";

    std::string token_val = "";
    if (query_params.contains("token") && query_params["token"].is_string()) {
        token_val = query_params["token"].get<std::string>();
    }

    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& seg = segments[i];
        double dur = seg.value("duration_seconds", 60.0);
        std::string video_url = seg.value("video_url", "");
        if (video_url.empty()) {
            video_url = "/api/v1/recordings/video?path=" + seg.value("file_path", "");
        }
        if (!token_val.empty() && video_url.find("token=") == std::string::npos) {
            video_url += (video_url.find('?') == std::string::npos ? "?token=" : "&token=") + token_val;
        }

        if (i > 0) {
            oss << "#EXT-X-DISCONTINUITY\n";
        }
        char dur_buf[32]{0};
        snprintf(dur_buf, sizeof(dur_buf), "%.3f", dur);
        oss << "#EXTINF:" << dur_buf << ",\n";
        oss << video_url << "\n";
    }

    oss << "#EXT-X-ENDLIST\n";

    SendHttpResponse(client_fd, 200, "application/vnd.apple.mpegurl; charset=utf-8", oss.str(), "Cache-Control: no-cache\r\n", request_origin);
    close(client_fd);
}

void RestServer::HandleRecordingVideo(int client_fd, const nlohmann::json& query_params, const std::unordered_map<std::string, std::string>& headers, const std::string& request_origin) {
    std::string req_path = "";
    if (query_params.contains("path") && query_params["path"].is_string()) {
        req_path = query_params["path"].get<std::string>();
    } else if (query_params.contains("id")) {
        if (query_params["id"].is_string()) req_path = query_params["id"].get<std::string>();
        else if (query_params["id"].is_number()) req_path = std::to_string(query_params["id"].get<int64_t>());
    }

    if (req_path.empty() || req_path.find('\0') != std::string::npos || req_path.find("..") != std::string::npos) {
        SendHttpResponse(client_fd, 400, "text/plain", "Bad Request: Invalid video path", "", request_origin);
        close(client_fd);
        return;
    }

    // Canonical path resolution via PlaybackEngine
    std::string target_str = PlaybackEngine::Instance().GetSegmentLocalPath(req_path);
    std::error_code ec;
    std::filesystem::path target_path(target_str);

    if (target_str.empty() || !std::filesystem::is_regular_file(target_path, ec)) {
        SendHttpResponse(client_fd, 404, "text/plain", "Recording Not Found", "", request_origin);
        close(client_fd);
        return;
    }

    int file_fd = open(target_str.c_str(), O_RDONLY | O_CLOEXEC);
    if (file_fd < 0) {
        SendHttpResponse(client_fd, 500, "text/plain", "Failed to open video file", "", request_origin);
        close(client_fd);
        return;
    }

    struct stat st;
    if (fstat(file_fd, &st) != 0 || st.st_size <= 0) {
        close(file_fd);
        SendHttpResponse(client_fd, 500, "text/plain", "Empty video file", "", request_origin);
        close(client_fd);
        return;
    }

    size_t file_size = static_cast<size_t>(st.st_size);
    bool is_download = (query_params.contains("download") && (query_params["download"] == "true" || query_params["download"] == 1));

    std::string content_type = "video/mp4";
    std::string ext = target_path.extension().string();
    if (ext == ".bmp") {
        content_type = "image/bmp";
    } else if (ext == ".jpg" || ext == ".jpeg") {
        content_type = "image/jpeg";
    }

    std::string extra_headers = "";
    if (is_download) {
        extra_headers += "Content-Disposition: attachment; filename=\"" + target_path.filename().string() + "\"\r\n";
    }

    auto it_range = headers.find("range");
    if (it_range != headers.end() && it_range->second.rfind("bytes=", 0) == 0 && !is_download) {
        std::string range_val = it_range->second.substr(6);
        size_t dash_pos = range_val.find('-');
        if (dash_pos != std::string::npos) {
            size_t start = 0;
            size_t end = file_size - 1;
            try {
                std::string s_str = range_val.substr(0, dash_pos);
                if (!s_str.empty()) start = std::stoull(s_str);
                std::string e_str = range_val.substr(dash_pos + 1);
                if (!e_str.empty()) end = std::stoull(e_str);
            } catch (...) {}

            if (start < file_size) {
                if (end >= file_size) end = file_size - 1;
                size_t chunk = end - start + 1;

                std::ostringstream oss;
                oss << "HTTP/1.1 206 Partial Content\r\n";
                oss << "Content-Type: " << content_type << "\r\n";
                oss << "Content-Range: bytes " << start << "-" << end << "/" << file_size << "\r\n";
                oss << "Content-Length: " << chunk << "\r\n";
                oss << "Accept-Ranges: bytes\r\n";
                oss << BuildCorsHeaders(request_origin, false);
                oss << "X-Content-Type-Options: nosniff\r\n";
                oss << "Access-Control-Expose-Headers: Content-Range, Accept-Ranges, Content-Length\r\n";
                oss << "Cross-Origin-Opener-Policy: same-origin\r\n";
                oss << "Cross-Origin-Embedder-Policy: require-corp\r\n";
                oss << "Cross-Origin-Resource-Policy: cross-origin\r\n";
                oss << "Connection: close\r\n\r\n";

                std::string hstr = oss.str();
                SendAllBytes(client_fd, reinterpret_cast<const uint8_t*>(hstr.data()), hstr.size());

                off_t offset = static_cast<off_t>(start);
                size_t rem = chunk;
                while (rem > 0) {
                    size_t to_send = std::min(rem, static_cast<size_t>(1048576));
                    ssize_t sent = sendfile(client_fd, file_fd, &offset, to_send);
                    if (sent <= 0) {
                        if (sent < 0 && errno == EINTR) continue;
                        break;
                    }
                    rem -= static_cast<size_t>(sent);
                }
                close(file_fd);
                close(client_fd);
                return;
            }
        }
    }

    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << file_size << "\r\n";
    oss << "Accept-Ranges: bytes\r\n";
    oss << BuildCorsHeaders(request_origin, false);
    oss << "X-Content-Type-Options: nosniff\r\n";
    oss << "Access-Control-Expose-Headers: Content-Range, Accept-Ranges, Content-Length\r\n";
    oss << "Cross-Origin-Opener-Policy: same-origin\r\n";
    oss << "Cross-Origin-Embedder-Policy: require-corp\r\n";
    oss << "Cross-Origin-Resource-Policy: cross-origin\r\n";
    if (!extra_headers.empty()) oss << extra_headers;
    oss << "Connection: close\r\n\r\n";

    std::string hstr = oss.str();
    SendAllBytes(client_fd, reinterpret_cast<const uint8_t*>(hstr.data()), hstr.size());

    off_t offset = 0;
    size_t rem = file_size;
    while (rem > 0) {
        size_t to_send = std::min(rem, static_cast<size_t>(1048576));
        ssize_t sent = sendfile(client_fd, file_fd, &offset, to_send);
        if (sent <= 0) {
            if (sent < 0 && errno == EINTR) continue;
            break;
        }
        rem -= static_cast<size_t>(sent);
    }
    close(file_fd);
    close(client_fd);
}

void RestServer::HandleRecordingLock(int client_fd, const std::string& method, const std::string& path, const std::string& body, const std::string& request_origin) {
    (void)method;
    nlohmann::json bj = nlohmann::json::object();
    try {
        if (!body.empty()) bj = nlohmann::json::parse(body);
    } catch (...) {}

    bool lock_state = bj.value("locked", true);
    std::string req_path = bj.value("path", "");
    std::error_code ec;
    std::filesystem::path root_path = std::filesystem::weakly_canonical(RetentionManager::Instance().GetRecordingPath(), ec);

    std::filesystem::path found_file;
    if (!req_path.empty()) {
        std::filesystem::path cand = (root_path / std::filesystem::path(req_path).relative_path()).lexically_normal();
        if (std::filesystem::is_regular_file(cand, ec)) {
            found_file = cand;
        }
    } else {
        std::string id_str = "";
        if (path.rfind("/api/v1/recordings/", 0) == 0 && path.rfind("/lock") == path.size() - 5) {
            id_str = path.substr(19, path.size() - 19 - 5);
        } else if (bj.contains("id")) {
            if (bj["id"].is_number()) id_str = std::to_string(bj["id"].get<int64_t>());
            else if (bj["id"].is_string()) id_str = bj["id"].get<std::string>();
        }

        if (!id_str.empty()) {
            for (auto it = std::filesystem::recursive_directory_iterator(root_path, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::recursive_directory_iterator(); ++it) {
                if (it->is_regular_file(ec) && it->path().extension() == ".mp4") {
                    std::string fn = it->path().filename().string();
                    if (fn.find(id_str) != std::string::npos) {
                        found_file = it->path();
                        break;
                    }
                }
            }
        }
    }

    if (found_file.empty()) {
        SendHttpResponse(client_fd, 404, "application/json", R"({"success":false,"status_code":404,"message":"Recording not found"})", "", request_origin);
        close(client_fd);
        return;
    }

    auto lock_file = found_file.parent_path() / (found_file.stem().string() + ".locked");
    if (lock_state) {
        std::ofstream lf(lock_file.string());
        lf << "LOCKED" << std::endl;
        lf.close();
        LOG_INFO << "Locked segment: " << found_file.string();
    } else {
        std::filesystem::remove(lock_file, ec);
        LOG_INFO << "Unlocked segment: " << found_file.string();
    }

    nlohmann::json resp;
    resp["success"] = true;
    resp["status_code"] = 200;
    resp["is_locked"] = lock_state;
    SendHttpResponse(client_fd, 200, "application/json", resp.dump(), "", request_origin);
    close(client_fd);
}

void RestServer::HandleRecordingExport(int client_fd, const std::string& method, const std::string& path, const std::string& body, const std::string& request_origin) {
    (void)method;
    (void)path;
    nlohmann::json bj = nlohmann::json::object();
    try {
        if (!body.empty()) bj = nlohmann::json::parse(body);
    } catch (...) {}

    int ch = bj.value("channel_id", 1);
    int64_t start_ms = bj.value("start_time_ms", 0LL);
    int64_t end_ms = bj.value("end_time_ms", 0LL);
    std::string fname = bj.value("filename", "");

    if (start_ms <= 0) {
        std::string start_iso = bj.value("start_time", "");
        if (!start_iso.empty()) {
            struct tm tm{};
            if (strptime(start_iso.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm)) {
                start_ms = static_cast<int64_t>(timegm(&tm)) * 1000LL;
            }
        }
    }
    if (end_ms <= 0) {
        std::string end_iso = bj.value("end_time", "");
        if (!end_iso.empty()) {
            struct tm tm{};
            if (strptime(end_iso.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm)) {
                end_ms = static_cast<int64_t>(timegm(&tm)) * 1000LL;
            }
        }
    }

    std::string job_id = PlaybackEngine::Instance().StartExport(ch, start_ms, end_ms, fname);
    nlohmann::json resp;
    resp["success"] = true;
    resp["status_code"] = 200;
    resp["data"] = {{"job_id", job_id}, {"channel_id", ch}, {"state", "RUNNING"}};

    SendHttpResponse(client_fd, 200, "application/json", resp.dump(), "", request_origin);
    close(client_fd);
}

void RestServer::HandleRecordingExportStatus(int client_fd, const nlohmann::json& query_params, const std::string& request_origin) {
    std::string job_id = "";
    if (query_params.contains("job_id") && query_params["job_id"].is_string()) {
        job_id = query_params["job_id"].get<std::string>();
    }

    nlohmann::json res;
    if (job_id.empty()) {
        res = PlaybackEngine::Instance().ListExportJobs();
    } else {
        res = PlaybackEngine::Instance().GetExportStatus(job_id);
    }

    int code = (res.contains("error")) ? 404 : 200;
    nlohmann::json resp;
    resp["success"] = (code == 200);
    resp["status_code"] = code;
    resp["data"] = res;

    SendHttpResponse(client_fd, code, "application/json", resp.dump(), "", request_origin);
    close(client_fd);
}

void RestServer::HandleRecordingExportDownload(int client_fd, const nlohmann::json& query_params, const std::unordered_map<std::string, std::string>& headers, const std::string& request_origin) {
    (void)headers;
    std::string job_id = query_params.value("job_id", "");
    std::string file_path = "";

    if (!job_id.empty()) {
        auto status = PlaybackEngine::Instance().GetExportStatus(job_id);
        if (status.contains("output_path")) {
            file_path = status["output_path"].get<std::string>();
        }
    } else if (query_params.contains("path") && query_params["path"].is_string()) {
        file_path = PlaybackEngine::Instance().GetSegmentLocalPath(query_params["path"].get<std::string>());
    }

    std::error_code ec;
    if (file_path.empty() || !std::filesystem::exists(file_path, ec)) {
        SendHttpResponse(client_fd, 404, "text/plain", "Export File Not Found", "", request_origin);
        close(client_fd);
        return;
    }

    int file_fd = open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file_fd < 0) {
        SendHttpResponse(client_fd, 500, "text/plain", "Failed to open export file", "", request_origin);
        close(client_fd);
        return;
    }

    struct stat st;
    if (fstat(file_fd, &st) != 0 || st.st_size <= 0) {
        close(file_fd);
        SendHttpResponse(client_fd, 500, "text/plain", "Empty export file", "", request_origin);
        close(client_fd);
        return;
    }

    size_t file_size = static_cast<size_t>(st.st_size);
    std::string fname = std::filesystem::path(file_path).filename().string();
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n";
    oss << "Content-Type: video/mp4\r\n";
    oss << "Content-Length: " << file_size << "\r\n";
    oss << "Content-Disposition: attachment; filename=\"" << fname << "\"\r\n";
    oss << "X-Content-Type-Options: nosniff\r\n";
    oss << "Cross-Origin-Opener-Policy: same-origin\r\n";
    oss << "Cross-Origin-Embedder-Policy: require-corp\r\n";
    oss << "Cross-Origin-Resource-Policy: cross-origin\r\n";
    oss << BuildCorsHeaders(request_origin, false);
    oss << "Connection: close\r\n\r\n";

    std::string hstr = oss.str();
    SendAllBytes(client_fd, reinterpret_cast<const uint8_t*>(hstr.data()), hstr.size());

    off_t offset = 0;
    size_t rem = file_size;
    while (rem > 0) {
        size_t to_send = std::min(rem, static_cast<size_t>(1048576));
        ssize_t sent = sendfile(client_fd, file_fd, &offset, to_send);
        if (sent <= 0) {
            if (sent < 0 && errno == EINTR) continue;
            break;
        }
        rem -= static_cast<size_t>(sent);
    }
    close(file_fd);
    close(client_fd);
}

bool RestServer::HandleWebSocketHandshake(int client_fd, const std::string& request_headers, const std::string& subprotocol) {
    std::string sec_key;
    std::istringstream iss(request_headers);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string h_name = line.substr(0, colon);
        for (auto& c : h_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (h_name == "sec-websocket-key") {
            sec_key = line.substr(colon + 1);
            while (!sec_key.empty() && (sec_key.front() == ' ' || sec_key.front() == '\t')) {
                sec_key.erase(sec_key.begin());
            }
            while (!sec_key.empty() && (sec_key.back() == ' ' || sec_key.back() == '\t')) {
                sec_key.pop_back();
            }
            break;
        }
    }

    if (sec_key.empty()) {
        std::string resp = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                           R"({"status":"error","message":"Missing Sec-WebSocket-Key header"})";
        SendAllBytes(client_fd, reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
        return false;
    }

    static constexpr const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = sec_key + kWsGuid;
    uint8_t sha1_digest[20];
    Sha1(reinterpret_cast<const uint8_t*>(combined.data()), combined.size(), sha1_digest);
    std::string accept_key = Base64Encode(sha1_digest, 20);

    std::ostringstream oss;
    oss << "HTTP/1.1 101 Switching Protocols\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Accept: " << accept_key << "\r\n";
    if (!subprotocol.empty()) {
        oss << "Sec-WebSocket-Protocol: " << subprotocol << "\r\n";
    }
    oss << "Cache-Control: no-cache, no-store\r\n\r\n";
    std::string resp = oss.str();
    return SendAllBytes(client_fd, reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
}

std::vector<uint8_t> RestServer::BuildWebSocketFrame(const uint8_t* payload, size_t len, uint8_t opcode) {
    std::vector<uint8_t> frame;
    frame.reserve(10 + len);
    frame.push_back(static_cast<uint8_t>(0x80U | (opcode & 0x0FU))); // FIN=1

    uint64_t len64 = static_cast<uint64_t>(len);
    if (len64 <= 125ULL) {
        frame.push_back(static_cast<uint8_t>(len64));
    } else if (len64 <= 65535ULL) {
        frame.push_back(126U);
        frame.push_back(static_cast<uint8_t>((len64 >> 8) & 0xFFU));
        frame.push_back(static_cast<uint8_t>(len64 & 0xFFU));
    } else {
        frame.push_back(127U);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((len64 >> (i * 8)) & 0xFFU));
        }
    }
    if (payload && len > 0) {
        frame.insert(frame.end(), payload, payload + len);
    }
    return frame;
}

void RestServer::HandleWebSocketStream(int client_fd, int channel_id, StreamType stream_type) {
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int sndbuf = 512 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    struct timeval snd_tv{1, 0};
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));
    struct timeval rcv_tv{0, 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));

    if (stream_type == StreamType::MAIN) {
        CameraManager::Instance().EnsureMainStream(channel_id);
    } else {
        CameraManager::Instance().StartSubStream(channel_id);
    }

    auto ctx = std::make_shared<WsStreamContext>();

    StreamBroker::SubscriptionId sub_id = StreamBroker::Instance().Subscribe(
        channel_id, stream_type,
        [ctx](const MediaPacketPtr& pkt) {
            if (!pkt || pkt->IsAudio() || ctx->closed.load(std::memory_order_relaxed)) {
                return;
            }
            std::lock_guard<std::mutex> lock(ctx->mtx);
            if (ctx->queue.size() >= WsStreamContext::kMaxQueueDepth) {
                if (pkt->is_keyframe) {
                    ctx->queue.clear();
                    ctx->awaiting_keyframe = false;
                } else {
                    while (!ctx->queue.empty() && !ctx->queue.front()->is_keyframe) {
                        ctx->queue.pop_front();
                    }
                    if (!ctx->queue.empty()) {
                        ctx->queue.pop_front();
                        while (!ctx->queue.empty() && !ctx->queue.front()->is_keyframe) {
                            ctx->queue.pop_front();
                        }
                    }
                    if (ctx->queue.empty()) {
                        ctx->awaiting_keyframe = true;
                        return;
                    }
                }
            }
            if (ctx->awaiting_keyframe) {
                if (!pkt->is_keyframe) {
                    return;
                }
                ctx->awaiting_keyframe = false;
            }
            ctx->queue.push_back(pkt);
            ctx->cv.notify_one();
        });

    bool config_sent = false;
    CodecType last_codec = CodecType::UNKNOWN;
    std::vector<uint8_t> last_vps, last_sps, last_pps;

    auto send_config_if_ready = [&](CodecType current_codec) -> bool {
        if (current_codec == CodecType::UNKNOWN) return true;
        std::vector<uint8_t> vps, sps, pps;
        StreamBroker::Instance().GetCachedParameterSets(channel_id, stream_type, vps, sps, pps);
        if (config_sent && current_codec == last_codec &&
            vps == last_vps && sps == last_sps && pps == last_pps) {
            return true;
        }
        if (sps.empty() || pps.empty()) {
            return true; // Wait until SPS/PPS are extracted before emitting config
        }

        uint32_t width = 0, height = 0;
        std::string codec_name = (current_codec == CodecType::H265) ? "H265" : "H264";
        std::string codec_string;
        std::string hvc1_codec_string;
        std::vector<uint8_t> desc_record;

        if (current_codec == CodecType::H265) {
            auto meta = Fmp4Muxer::ParseHevcSpsMeta(sps.data(), sps.size());
            width  = meta.width;
            height = meta.height;
            codec_string      = Fmp4Muxer::BuildHevcCodecString(sps.data(), sps.size(), false);
            hvc1_codec_string = Fmp4Muxer::BuildHevcCodecString(sps.data(), sps.size(), true);
            desc_record       = Fmp4Muxer::BuildHvcCRecord(vps, sps, pps);
        } else {
            codec_string      = Fmp4Muxer::BuildAvcCodecString(sps.data(), sps.size());
            hvc1_codec_string = codec_string;
            desc_record       = Fmp4Muxer::BuildAvcCRecord(sps, pps);
            if (Fmp4Muxer::ParseSpsDimensions(sps.data(), sps.size(), width, height)) {
                // Dimensions parsed
            }
        }

        std::ostringstream js;
        js << R"({"type":"config","codec":")" << codec_name
           << R"(","codec_string":")" << codec_string
           << R"(","hvc1_codec_string":")" << hvc1_codec_string
           << R"(","width":)" << width
           << R"(,"height":)" << height
           << R"(,"timebase":1000000)"
           << R"(,"description_b64":")" << Base64Encode(desc_record.data(), desc_record.size()) << R"(")"
           << R"(,"vps_b64":")" << Base64Encode(vps.data(), vps.size()) << R"(")"
           << R"(,"sps_b64":")" << Base64Encode(sps.data(), sps.size()) << R"(")"
           << R"(,"pps_b64":")" << Base64Encode(pps.data(), pps.size()) << R"("})";
        std::string cfg_str = js.str();
        auto ws_cfg = BuildWebSocketFrame(
            reinterpret_cast<const uint8_t*>(cfg_str.data()), cfg_str.size(), 0x01);
        if (!SendAllBytes(client_fd, ws_cfg.data(), ws_cfg.size())) {
            return false;
        }

        config_sent = true;
        last_codec  = current_codec;
        last_vps    = std::move(vps);
        last_sps    = std::move(sps);
        last_pps    = std::move(pps);
        return true;
    };

    CodecType initial_codec = StreamBroker::Instance().GetCachedCodec(channel_id, stream_type);
    if (!send_config_if_ready(initial_codec)) {
        ctx->closed.store(true);
    }

    // Only dispatch cached IDR if it was captured within the last 45ms (so no intermediate P-frames were missed)
    auto initial_kf = StreamBroker::Instance().GetLatestKeyframe(channel_id, stream_type);
    if (initial_kf && initial_kf->wall_time_ms > 0 &&
        (time_utils::WallTimeMs() - initial_kf->wall_time_ms) <= 45) {
        std::lock_guard<std::mutex> lock(ctx->mtx);
        if (ctx->queue.empty()) {
            ctx->queue.push_back(initial_kf);
            ctx->awaiting_keyframe = false;
        }
    }

    while (is_running_.load() && !ctx->closed.load()) {
        std::vector<uint8_t> pong_payload;
        if (!DrainWebSocketClientFrames(client_fd, pong_payload)) {
            break;
        }
        if (!pong_payload.empty()) {
            auto pong_frame = BuildWebSocketFrame(pong_payload.data(), pong_payload.size(), 0x0A);
            if (!SendAllBytes(client_fd, pong_frame.data(), pong_frame.size())) break;
        }

        MediaPacketPtr pkt;
        {
            std::unique_lock<std::mutex> lock(ctx->mtx);
            ctx->cv.wait_for(lock, std::chrono::milliseconds(250), [&ctx, this]() {
                return !ctx->queue.empty() || !is_running_.load() || ctx->closed.load();
            });
            if (!is_running_.load() || ctx->closed.load()) break;
            if (ctx->queue.empty()) continue;
            pkt = std::move(ctx->queue.front());
            ctx->queue.pop_front();
        }

        if (!pkt || pkt->IsAudio() || pkt->data.empty()) continue;

        CodecType pkt_codec = (pkt->codec != CodecType::UNKNOWN)
            ? pkt->codec
            : StreamBroker::Instance().GetCachedCodec(channel_id, stream_type);
        if (pkt_codec == CodecType::UNKNOWN) {
            pkt_codec = DetectNalCodecFromParamSets(pkt->data.data(), pkt->data.size());
        }
        if (pkt_codec == CodecType::UNKNOWN) pkt_codec = CodecType::H264;

        if (!send_config_if_ready(pkt_codec)) {
            break;
        }

        std::vector<uint8_t> vps, sps, pps;
        StreamBroker::Instance().GetCachedParameterSets(channel_id, stream_type, vps, sps, pps);
        std::vector<uint8_t> annexb_payload = DeduplicateAndPrependParamSets(
            pkt->data, pkt_codec, pkt->is_keyframe, vps, sps, pps);

        // 12-byte little-endian binary header: [uint64_t pts_us][uint32_t flags]
        std::vector<uint8_t> binary_payload;
        binary_payload.reserve(12 + annexb_payload.size());
        uint64_t pts_us = static_cast<uint64_t>(pkt->pts_us >= 0 ? pkt->pts_us : 0);
        for (int i = 0; i < 8; ++i) {
            binary_payload.push_back(static_cast<uint8_t>((pts_us >> (i * 8)) & 0xFFU));
        }
        uint32_t flags = (pkt->is_keyframe ? 0x01U : 0x00U) |
                         ((pkt_codec == CodecType::H265) ? 0x02U : 0x00U);
        for (int i = 0; i < 4; ++i) {
            binary_payload.push_back(static_cast<uint8_t>((flags >> (i * 8)) & 0xFFU));
        }
        binary_payload.insert(binary_payload.end(), annexb_payload.begin(), annexb_payload.end());

        auto ws_frame = BuildWebSocketFrame(binary_payload.data(), binary_payload.size(), 0x02);
        if (!SendAllBytes(client_fd, ws_frame.data(), ws_frame.size())) {
            break;
        }
    }

    ctx->closed.store(true);
    StreamBroker::Instance().Unsubscribe(sub_id);
    close(client_fd);
}

} // namespace nvr
