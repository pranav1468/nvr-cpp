#include "nvr/api/rest_server.h"
#include "nvr/api/fmp4_muxer.h"
#include "nvr/media/stream_broker.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/dispatcher/command_dispatcher.h"
#include "nvr/common/logger.h"
#include "nvr/common/thread_pool.h"
#include "nvr/common/time_utils.h"
#include "3rdparty/nlohmann/json.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/stat.h>

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
    static constexpr size_t kMaxDepth = 30; // freshness-first drop-stale bound
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
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
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
                      const std::string& body, const std::string& extra_headers = "") {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << " " << HttpStatusMessage(status_code) << "\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Access-Control-Allow-Origin: *\r\n";
    oss << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n";
    oss << "Access-Control-Allow-Headers: Content-Type, Authorization, X-Requested-With\r\n";
    oss << "Connection: close\r\n";
    if (!extra_headers.empty()) {
        oss << extra_headers;
    }
    oss << "\r\n";
    oss << body;

    std::string resp = oss.str();
    ssize_t sent = 0;
    size_t total = resp.size();
    while (sent < static_cast<ssize_t>(total)) {
        ssize_t n = send(client_fd, resp.data() + sent, total - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += n;
    }
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

    // Set non-blocking on server socket for poll loop
    int flags = fcntl(server_fd_, F_GETFL, 0);
    fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));

    if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR << "[RestServer] Failed to bind port " << config_.port << ": " << strerror(errno);
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 32) < 0) {
        LOG_ERROR << "[RestServer] Failed to listen on socket: " << strerror(errno);
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    is_running_.store(true);
    worker_pool_ = std::make_unique<ThreadPool>(8);
    media_pool_ = std::make_unique<ThreadPool>(16);
    listener_thread_ = std::thread(&RestServer::ListenerLoop, this);

    LOG_INFO << "[RestServer] HTTP REST Control Plane listening on port " << config_.port
             << " (WebRoot: " << config_.web_root << ")";
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

    if (media_pool_) {
        media_pool_->Shutdown();
        media_pool_.reset();
    }

    if (worker_pool_) {
        worker_pool_->Shutdown();
        worker_pool_.reset();
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
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
            if (client_fd >= 0) {
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
    // Set 5-second timeout on client socket to prevent slowloris / connection freeze
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::vector<char> buffer(8192);
    ssize_t n = recv(client_fd, buffer.data(), buffer.size() - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    buffer[n] = '\0';
    std::string req_str(buffer.data(), n);

    // Find end of headers
    size_t header_end = req_str.find("\r\n\r\n");
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

        if (content_length > 1048576) { // 1 MB limit
            SendHttpResponse(client_fd, 413, "application/json", 
                             R"({"success":false,"status_code":413,"message":"Payload Too Large"})");
            close(client_fd);
            return;
        }

        while (body.size() < content_length) {
            std::vector<char> extra(4096);
            ssize_t r = recv(client_fd, extra.data(), extra.size(), 0);
            if (r <= 0) break;
            body.append(extra.data(), r);
        }
    }

    // CORS preflight handling
    if (method == "OPTIONS") {
        SendHttpResponse(client_fd, 200, "text/plain", "");
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

    // Extract Auth Token (from Authorization header or query parameter)
    std::string auth_token;
    if (headers.find("authorization") != headers.end()) {
        std::string auth = headers["authorization"];
        if (auth.rfind("Bearer ", 0) == 0 || auth.rfind("bearer ", 0) == 0) {
            auth_token = auth.substr(7);
        } else {
            auth_token = auth;
        }
    }
    if (auth_token.empty() && query_params.contains("token")) {
        auth_token = query_params["token"].get<std::string>();
    }

    // 1. Static Web Asset Serving (Files under ./web)
    if (path == "/" || path == "/index.html" || path.rfind("/api/", 0) != 0) {
        // Path Traversal Security Guard
        if (path.find("..") != std::string::npos) {
            SendHttpResponse(client_fd, 403, "text/plain", "Forbidden");
            close(client_fd);
            return;
        }

        std::string file_subpath = (path == "/" ? "/index.html" : path);
        std::string file_path = config_.web_root + file_subpath;

        struct stat st;
        if (stat(file_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            std::ifstream file(file_path, std::ios::binary);
            if (file) {
                std::string content((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
                SendHttpResponse(client_fd, 200, GetMimeType(file_path), content);
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

    // 3. Media Plane: Zero-Transcoding Live Video Streaming (fMP4 / MSE)
    if (method == "GET" && path == "/api/v1/stream") {
        int channel_id = query_params.value("channel", 1);
        std::string stream_str = query_params.value("stream", "sub");
        StreamType stream_type = (stream_str == "main") ? StreamType::MAIN : StreamType::SUB;
        if (media_pool_) {
            media_pool_->Enqueue([this, client_fd, channel_id, stream_type]() {
                HandleStreamClient(client_fd, channel_id, stream_type);
            });
        } else {
            close(client_fd);
        }
        return;
    }

    // 4. Media Plane: On-Demand Static Snapshot
    if (method == "GET" && path.rfind("/api/v1/cameras/", 0) == 0 && path.rfind("/snapshot") == path.size() - 9) {
        std::string cam_id_str = path.substr(16, path.size() - 16 - 9);
        int channel_id = std::atoi(cam_id_str.c_str());
        HandleSnapshotClient(client_fd, channel_id);
        return;
    }

    // Route Mapping
    if (method == "POST" && path == "/api/v1/auth/login") {
        target_action = "auth.login";
    } else if (method == "POST" && path == "/api/v1/auth/logout") {
        target_action = "auth.logout";
    } else if (method == "GET" && path == "/api/v1/system/status") {
        target_action = "system.status";
        payload = nlohmann::json::object();
    } else if (method == "GET" && path == "/api/v1/cameras") {
        target_action = "camera.list";
        payload = nlohmann::json::object();
    } else if (method == "POST" && path.rfind("/api/v1/cameras/", 0) == 0 && path.rfind("/restart") == path.size() - 8) {
        // e.g. /api/v1/cameras/2/restart
        std::string cam_id_str = path.substr(16, path.size() - 16 - 8);
        target_action = "camera.restart";
        payload["channel_id"] = std::atoi(cam_id_str.c_str());
    } else if (method == "GET" && path == "/api/v1/layout") {
        target_action = "layout.get";
        payload = nlohmann::json::object();
    } else if (method == "POST" && path == "/api/v1/layout") {
        target_action = "layout.set";
    } else if (method == "POST" && path == "/api/v1/layout/fullscreen") {
        target_action = "layout.fullscreen";
    } else if (method == "GET" && path == "/api/v1/recordings") {
        target_action = "recordings.query";
        payload = query_params;
    } else if (method == "POST" && path.rfind("/api/v1/recordings/", 0) == 0 && path.rfind("/lock") == path.size() - 5) {
        // e.g. /api/v1/recordings/123/lock
        std::string seg_id_str = path.substr(19, path.size() - 19 - 5);
        target_action = "recordings.lock";
        payload["segment_id"] = std::strtoll(seg_id_str.c_str(), nullptr, 10);
        if (!payload.contains("locked")) {
            payload["locked"] = true;
        }
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

    SendHttpResponse(client_fd, result.status_code, "application/json", result.ToJson().dump(2));
    close(client_fd);
}

void RestServer::HandleStreamClient(int client_fd, int channel_id, StreamType stream_type) {
    const char* http_header = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: video/mp4\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Expires: 0\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Connection: close\r\n\r\n";

    ssize_t sent = send(client_fd, http_header, strlen(http_header), MSG_NOSIGNAL);
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
            if (!pkt || ctx->closed.load()) return;
            std::unique_lock<std::mutex> lock(ctx->mtx);
            if (ctx->q.size() >= StreamContext::kMaxDepth) {
                ctx->q.pop_front(); // head-drop stale frame
            }
            ctx->q.push_back(pkt);
            ctx->cv.notify_one();
        }
    );

    bool init_sent = false;
    std::vector<uint8_t> sps, pps;
    uint32_t width = (stream_type == StreamType::MAIN ? 1920 : 640);
    uint32_t height = (stream_type == StreamType::MAIN ? 1080 : 360);
    uint32_t seq = 1;
    uint64_t last_pts = 0;
    uint64_t base_pts = 0;
    bool has_base_pts = false;

    LOG_INFO << "[RestServer] Client connected to live fMP4 stream (Cam: " 
             << channel_id << ", " << (stream_type == StreamType::MAIN ? "MAIN" : "SUB") << ")";

    while (is_running_.load() && !ctx->closed.load()) {
        MediaPacketPtr pkt;
        {
            std::unique_lock<std::mutex> lock(ctx->mtx);
            bool ok = ctx->cv.wait_for(lock, std::chrono::milliseconds(400), [&]() {
                return !ctx->q.empty() || !is_running_.load() || ctx->closed.load();
            });
            if (!is_running_.load() || ctx->closed.load()) break;
            if (!ok || ctx->q.empty()) continue;
            pkt = ctx->q.front();
            ctx->q.pop_front();
        }

        if (!pkt || pkt->data.empty()) continue;

        // 1. Accumulate SPS (type 7) and PPS (type 8) from packet NAL units
        auto nals = Fmp4Muxer::ParseAnnexBNals(pkt->data.data(), pkt->data.size());
        for (const auto& nal : nals) {
            if (nal.type == 7 && sps.empty()) {
                sps.assign(nal.data, nal.data + nal.size);
            } else if (nal.type == 8 && pps.empty()) {
                pps.assign(nal.data, nal.data + nal.size);
            }
        }

        if (!init_sent) {
            // Check if we also have parameter sets via direct extractor
            if (sps.empty() || pps.empty()) {
                Fmp4Muxer::ExtractParameterSets(pkt->data.data(), pkt->data.size(), sps, pps);
            }

            if (!sps.empty() && !pps.empty()) {
                uint32_t pw = 0, ph = 0;
                if (Fmp4Muxer::ParseSpsDimensions(sps.data(), sps.size(), pw, ph)) {
                    width = pw;
                    height = ph;
                }
                auto init_seg = Fmp4Muxer::BuildInitSegment(sps, pps, width, height, 90000);
                ssize_t n = send(client_fd, init_seg.data(), init_seg.size(), MSG_NOSIGNAL);
                if (n <= 0) break;
                init_sent = true;
            } else {
                continue; // Wait until both SPS and PPS are captured
            }
        }

        // Ensure playback starts on an IDR keyframe
        if (!has_base_pts && !pkt->is_keyframe) {
            continue;
        }

        auto avcc = Fmp4Muxer::AnnexBToAvcc(pkt->data.data(), pkt->data.size());
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
    std::string timestamp_str = time_utils::FormatIso8601(time_utils::WallTimeMs());
    bool online = CameraManager::Instance().IsCameraOnline(channel_id);
    std::string status_color = online ? "#2ed573" : "#ff4757";
    std::string status_text = online ? "ONLINE - READY" : "OFFLINE";

    std::ostringstream svg;
    svg << "<svg xmlns='http://www.w3.org/2000/svg' width='640' height='360' viewBox='0 0 640 360'>\n"
        << "<defs>\n"
        << "  <linearGradient id='bg' x1='0%' y1='0%' x2='100%' y2='100%'>\n"
        << "    <stop offset='0%' stop-color='#1e272e'/>\n"
        << "    <stop offset='100%' stop-color='#0f1418'/>\n"
        << "  </linearGradient>\n"
        << "</defs>\n"
        << "<rect width='640' height='360' fill='url(#bg)' rx='8'/>\n"
        << "<circle cx='32' cy='32' r='8' fill='" << status_color << "'/>\n"
        << "<text x='48' y='37' fill='#ecf0f1' font-family='sans-serif' font-size='16' font-weight='bold'>Camera " << channel_id << " Snapshot</text>\n"
        << "<text x='32' y='180' fill='#a4b0be' font-family='sans-serif' font-size='20' font-weight='600'>" << status_text << "</text>\n"
        << "<text x='32' y='210' fill='#747d8c' font-family='sans-serif' font-size='14'>Timestamp: " << timestamp_str << "</text>\n"
        << "<text x='32' y='330' fill='#57606f' font-family='sans-serif' font-size='12'>NVR v2.0 Platform (NXP i.MX 95) • Zero-Transcode Engine</text>\n"
        << "</svg>";

    std::string body = svg.str();
    SendHttpResponse(client_fd, 200, "image/svg+xml", body, "Cache-Control: no-cache\r\n");
    close(client_fd);
}

} // namespace nvr
