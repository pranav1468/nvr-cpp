#include "nvr/api/rest_server.h"
#include "nvr/common/logger.h"
#include "nvr/common/thread_pool.h"
#include "nvr/storage/segment_index.h"
#include "nvr/storage/retention_manager.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/recording/recording_scheduler.h"
#include "nvr/live/live_controller.h"
#include "nvr/live/qml_video_bridge.h"
#include "3rdparty/toojpeg.h"
#include "3rdparty/nlohmann/json.hpp"
#include <csignal>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <sys/sysinfo.h>
#include <cstdlib>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace nvr {

RestServer& RestServer::Instance() {
    static RestServer instance;
    return instance;
}

RestServer::RestServer() = default;

RestServer::~RestServer() {
    Stop();
}

bool RestServer::Start(int port, const std::string& web_root) {
    if (running_) {
        LOG_WARN << "RestServer is already running on port " << port_;
        return true;
    }

    port_ = port;
    web_root_ = web_root;

    // Create server socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        LOG_ERROR << "RestServer: Failed to create socket: " << strerror(errno);
        return false;
    }

    // Set SO_REUSEADDR
    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_WARN << "RestServer: setsockopt(SO_REUSEADDR) failed: " << strerror(errno);
    }

    // Bind socket
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR << "RestServer: Failed to bind to port " << port_ << ": " << strerror(errno);
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Listen
    if (listen(server_fd_, 64) < 0) {
        LOG_ERROR << "RestServer: Failed to listen: " << strerror(errno);
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    signal(SIGPIPE, SIG_IGN);

    running_ = true;
    worker_pool_ = std::make_unique<ThreadPool>(16);
    server_thread_ = std::thread(&RestServer::ServerLoop, this);

    LOG_INFO << "RestServer successfully started on http://0.0.0.0:" << port_ 
             << " (Web Root: " << web_root_ << ")";
    return true;
}

void RestServer::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    LOG_INFO << "RestServer: Shutting down HTTP server...";

    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    if (worker_pool_) {
        worker_pool_->Shutdown();
        worker_pool_.reset();
    }

    LOG_INFO << "RestServer: Cleanly stopped.";
}

bool RestServer::IsRunning() const {
    return running_;
}

void RestServer::ServerLoop() {
    struct pollfd pfd{};
    pfd.fd = server_fd_;
    pfd.events = POLLIN;

    while (running_) {
        int ret = poll(&pfd, 1, 500); // 500ms timeout
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ret > 0 && (pfd.revents & POLLIN)) {
            struct sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(server_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
            if (client_fd >= 0) {
                if (worker_pool_) {
                    worker_pool_->Enqueue([this, client_fd]() {
                        this->HandleClient(client_fd);
                    });
                } else {
                    close(client_fd);
                }
            }
        }
    }
}

void RestServer::HandleClient(int client_fd) {
    char buffer[8192];
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        close(client_fd);
        return;
    }

    buffer[bytes_read] = '\0';
    std::string request(buffer);

    std::istringstream req_stream(request);
    std::string method, raw_uri, http_version;
    req_stream >> method >> raw_uri >> http_version;

    if (method.empty() || raw_uri.empty()) {
        close(client_fd);
        return;
    }

    // Separate URI path and query string
    std::string path = raw_uri;
    std::string query;
    size_t q_pos = raw_uri.find('?');
    if (q_pos != std::string::npos) {
        path = raw_uri.substr(0, q_pos);
        query = raw_uri.substr(q_pos + 1);
    }

    // CORS preflight OPTIONS
    if (method == "OPTIONS") {
        std::string cors_header = 
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
            "Content-Length: 0\r\n\r\n";
        send(client_fd, cors_header.c_str(), cors_header.size(), 0);
        close(client_fd);
        return;
    }

    // =========================================================================
    // REST API ENDPOINTS
    // =========================================================================
    if (path.rfind("/api/", 0) == 0) {
        if (path == "/api/status") {
            json resp;
            resp["device_name"] = "NVR_Main";
            resp["version"] = "4.0.0";
            resp["status"] = "ONLINE";
            resp["free_storage_bytes"] = RetentionManager::Instance().GetFreeDiskSpaceBytes("./recordings");
            resp["total_segments"] = SegmentIndex::Instance().GetTotalSegmentCount();
            resp["total_storage_bytes"] = SegmentIndex::Instance().GetTotalRecordingSizeBytes();

            struct sysinfo si;
            if (sysinfo(&si) == 0) {
                uint64_t unit = si.mem_unit ? si.mem_unit : 1;
                resp["ram_total_bytes"] = (uint64_t)si.totalram * unit;
                resp["ram_free_bytes"] = (uint64_t)si.freeram * unit;
                resp["ram_used_bytes"] = (uint64_t)(si.totalram - si.freeram) * unit;
                resp["uptime_seconds"] = si.uptime;
            }

            double loadavg[3];
            if (getloadavg(loadavg, 3) != -1) {
                resp["cpu_load_1m"] = loadavg[0];
                resp["cpu_load_5m"] = loadavg[1];
            }

            auto cameras = CameraManager::Instance().GetCameras();
            json cam_arr = json::array();
            for (const auto& c : cameras) {
                json item;
                item["id"] = c.id;
                item["name"] = c.name;
                item["rtsp_url"] = c.rtsp_url;
                item["enabled"] = c.enabled;
                item["online"] = CameraManager::Instance().IsCameraOnline(c.id);
                item["recording"] = RecordingScheduler::Instance().IsChannelRecording(c.id);
                item["frames"] = RecordingScheduler::Instance().GetChannelFrameCount(c.id);
                item["bytes"] = RecordingScheduler::Instance().GetChannelBytesWritten(c.id);
                cam_arr.push_back(item);
            }
            resp["cameras"] = cam_arr;

            SendResponse(client_fd, 200, "OK", "application/json", resp.dump());
            close(client_fd);
            return;
        }

        if (path == "/api/cameras") {
            auto cameras = CameraManager::Instance().GetCameras();
            json arr = json::array();
            for (const auto& c : cameras) {
                json item;
                item["id"] = c.id;
                item["name"] = c.name;
                item["rtsp_url"] = c.rtsp_url;
                item["sub_rtsp_url"] = c.sub_rtsp_url;
                item["enabled"] = c.enabled;
                item["record_mode"] = static_cast<int>(c.record_mode);
                item["online"] = CameraManager::Instance().IsCameraOnline(c.id);
                arr.push_back(item);
            }
            SendResponse(client_fd, 200, "OK", "application/json", arr.dump());
            close(client_fd);
            return;
        }

        if (path == "/api/recordings") {
            // Parse channel_id, start_ms, end_ms from query
            int channel_id = 1;
            int64_t start_ms = 0;
            int64_t end_ms = 9999999999999LL;

            // Simple query parameter extraction
            if (!query.empty()) {
                std::istringstream q_stream(query);
                std::string pair;
                while (std::getline(q_stream, pair, '&')) {
                    size_t eq = pair.find('=');
                    if (eq != std::string::npos) {
                        std::string k = pair.substr(0, eq);
                        std::string v = pair.substr(eq + 1);
                        if (k == "channel_id") channel_id = std::stoi(v);
                        else if (k == "start_ms") start_ms = std::stoll(v);
                        else if (k == "end_ms") end_ms = std::stoll(v);
                    }
                }
            }

            auto segments = SegmentIndex::Instance().QuerySegments(channel_id, start_ms, end_ms);
            json arr = json::array();
            for (const auto& seg : segments) {
                json item;
                item["id"] = seg.id;
                item["channel_id"] = seg.channel_id;
                item["file_path"] = seg.file_path;
                item["start_time_ms"] = seg.start_time_ms;
                item["end_time_ms"] = seg.end_time_ms;
                item["duration_ms"] = seg.duration_ms;
                item["file_size_bytes"] = seg.file_size_bytes;
                item["frame_count"] = seg.frame_count;
                item["codec"] = CodecToString(seg.codec);
                arr.push_back(item);
            }

            SendResponse(client_fd, 200, "OK", "application/json", arr.dump());
            close(client_fd);
            return;
        }

        if (path == "/api/login") {
            json resp;
            resp["success"] = true;
            resp["user"] = "admin";
            resp["role"] = "Super Admin";
            resp["token"] = "nvr_auth_token_secret_session_v4";
            SendResponse(client_fd, 200, "OK", "application/json", resp.dump());
            close(client_fd);
            return;
        }

        // Live MJPEG Stream Endpoint
        if (path == "/api/live/stream") {
            int channel_id = 1;
            if (!query.empty()) {
                std::istringstream q_stream(query);
                std::string pair;
                while (std::getline(q_stream, pair, '&')) {
                    size_t eq = pair.find('=');
                    if (eq != std::string::npos) {
                        std::string k = pair.substr(0, eq);
                        std::string v = pair.substr(eq + 1);
                        if (k == "channel_id" || k == "channel" || k == "id") {
                            try { channel_id = std::stoi(v); } catch (...) {}
                        }
                    }
                }
            }

            // Spawn dedicated streaming thread; client_fd is owned and closed by HandleLiveStream
            std::thread(&RestServer::HandleLiveStream, this, client_fd, channel_id).detach();
            return;
        }

        // Live Snapshot Endpoint
        if (path == "/api/live/snapshot" || path == "/api/live/frame") {
            int channel_id = 1;
            if (!query.empty()) {
                std::istringstream q_stream(query);
                std::string pair;
                while (std::getline(q_stream, pair, '&')) {
                    size_t eq = pair.find('=');
                    if (eq != std::string::npos) {
                        std::string k = pair.substr(0, eq);
                        std::string v = pair.substr(eq + 1);
                        if (k == "channel_id" || k == "channel" || k == "id") {
                            try { channel_id = std::stoi(v); } catch (...) {}
                        }
                    }
                }
            }

            int target_tile = -1;
            for (int t = 0; t < 16; ++t) {
                if (QmlVideoBridge::Instance().GetTileChannelId(t) == channel_id) {
                    target_tile = t;
                    break;
                }
            }
            if (target_tile < 0) {
                target_tile = (channel_id > 0) ? (channel_id - 1) : 0;
                LiveController::Instance().ActivateChannel(channel_id, target_tile);
            }

            DecodedFramePtr frame = QmlVideoBridge::Instance().GetTileFrame(target_tile);
            if (frame && frame->data.data() && frame->width > 0 && frame->height > 0) {
                uint32_t w = frame->width;
                uint32_t h = frame->height;
                size_t num_pixels = static_cast<size_t>(w) * h;
                std::vector<uint8_t> rgb_buffer(num_pixels * 3);
                const uint8_t* src = frame->data.data();
                uint32_t stride = (frame->stride > 0) ? frame->stride : (w * 4);
                if (frame->format == PixelFormat::RGBA32) {
                    for (uint32_t y = 0; y < h; ++y) {
                        const uint8_t* row_src = src + y * stride;
                        uint8_t* row_dst = rgb_buffer.data() + y * (w * 3);
                        for (uint32_t x = 0; x < w; ++x) {
                            row_dst[x * 3 + 0] = row_src[x * 4 + 0];
                            row_dst[x * 3 + 1] = row_src[x * 4 + 1];
                            row_dst[x * 3 + 2] = row_src[x * 4 + 2];
                        }
                    }
                } else if (frame->format == PixelFormat::RGB24) {
                    uint32_t stride_rgb = (frame->stride > 0) ? frame->stride : (w * 3);
                    for (uint32_t y = 0; y < h; ++y) {
                        std::memcpy(rgb_buffer.data() + y * (w * 3), src + y * stride_rgb, w * 3);
                    }
                }

                std::vector<uint8_t> jpeg_bytes;
                jpeg_bytes.reserve(65536);
                bool enc_ok = TooJpeg::writeJpeg([&jpeg_bytes](uint8_t b) {
                    jpeg_bytes.push_back(b);
                }, rgb_buffer.data(), static_cast<unsigned short>(w), static_cast<unsigned short>(h), true, 75);

                if (enc_ok && !jpeg_bytes.empty()) {
                    SendResponse(client_fd, 200, "OK", "image/jpeg", std::string(reinterpret_cast<const char*>(jpeg_bytes.data()), jpeg_bytes.size()));
                    close(client_fd);
                    return;
                }
            }

            SendResponse(client_fd, 404, "Not Found", "application/json", "{\"error\":\"No frame available\"}");
            close(client_fd);
            return;
        }

        // Live Telemetry / Grid State
        if (path == "/api/live/state") {
            std::string state_json = QmlVideoBridge::Instance().GetGridStateJson();
            SendResponse(client_fd, 200, "OK", "application/json", state_json);
            close(client_fd);
            return;
        }

        // Live Layout / Control Command
        if (path == "/api/live/command") {
            std::string body;
            size_t body_pos = request.find("\r\n\r\n");
            if (body_pos != std::string::npos) {
                body = request.substr(body_pos + 4);
            }
            bool ok = QmlVideoBridge::Instance().ExecuteCommandJson(body);
            json resp;
            resp["success"] = ok;
            SendResponse(client_fd, 200, "OK", "application/json", resp.dump());
            close(client_fd);
            return;
        }

        SendResponse(client_fd, 404, "Not Found", "application/json", "{\"error\":\"Unknown API endpoint\"}");
        close(client_fd);
        return;
    }

    // =========================================================================
    // STATIC WEB ASSETS (web/index.html, css, js)
    // =========================================================================
    if (path == "/") {
        path = "/index.html";
    }

    // Path traversal check
    if (path.find("..") != std::string::npos) {
        SendResponse(client_fd, 403, "Forbidden", "text/plain", "Forbidden");
        close(client_fd);
        return;
    }

    std::string file_path = web_root_ + path;
    ServeFile(client_fd, file_path);
    close(client_fd);
}

void RestServer::ServeFile(int client_fd, const std::string& file_path) {
    if (!fs::exists(file_path) || fs::is_directory(file_path)) {
        SendResponse(client_fd, 404, "Not Found", "text/plain", "404 Not Found");
        return;
    }

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        SendResponse(client_fd, 500, "Internal Server Error", "text/plain", "500 Could not open file");
        return;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string mime = GetMimeType(file_path);

    SendResponse(client_fd, 200, "OK", mime, content);
}

std::string RestServer::GetMimeType(const std::string& path) const {
    std::string ext;
    size_t dot_pos = path.rfind('.');
    if (dot_pos != std::string::npos) {
        ext = path.substr(dot_pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".mp4") return "video/mp4";
    if (ext == ".m3u8") return "application/vnd.apple.mpegurl";
    if (ext == ".ts") return "video/mp2t";
    if (ext == ".woff") return "font/woff";
    if (ext == ".woff2") return "font/woff2";

    return "application/octet-stream";
}

void RestServer::SendResponse(int client_fd, int status_code, const std::string& status_text, 
                              const std::string& content_type, const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;

    std::string resp_str = response.str();
    const char* ptr = resp_str.data();
    size_t remaining = resp_str.size();
    while (remaining > 0) {
        ssize_t sent = send(client_fd, ptr, remaining, 0);
        if (sent <= 0) break;
        ptr += sent;
        remaining -= sent;
    }
}

void RestServer::HandleLiveStream(int client_fd, int channel_id) {
    std::string header = 
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate, pre-check=0, post-check=0, max-age=0\r\n"
        "Pragma: no-cache\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n";
    
    ssize_t sent = send(client_fd, header.data(), header.size(), MSG_NOSIGNAL);
    if (sent <= 0) {
        close(client_fd);
        return;
    }

    // Ensure the channel is active in LiveController
    int target_tile = -1;
    for (int t = 0; t < 16; ++t) {
        if (QmlVideoBridge::Instance().GetTileChannelId(t) == channel_id) {
            target_tile = t;
            break;
        }
    }
    if (target_tile < 0) {
        target_tile = (channel_id > 0) ? (channel_id - 1) : 0;
        LiveController::Instance().ActivateChannel(channel_id, target_tile);
    }

    std::vector<uint8_t> rgb_buffer;
    std::vector<uint8_t> jpeg_bytes;
    jpeg_bytes.reserve(65536);

    uint64_t last_revision = 0;

    while (running_) {
        // Detect client socket disconnect
        struct pollfd pfd{};
        pfd.fd = client_fd;
        pfd.events = POLLIN;
        int poll_res = poll(&pfd, 1, 0);
        if (poll_res > 0) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLRDHUP)) {
                break;
            }
            char test_buf[16];
            ssize_t r = recv(client_fd, test_buf, sizeof(test_buf), MSG_DONTWAIT | MSG_PEEK);
            if (r == 0) {
                break; // client closed socket
            }
        }

        // Dynamically find tile assigned to this channel
        for (int t = 0; t < 16; ++t) {
            if (QmlVideoBridge::Instance().GetTileChannelId(t) == channel_id) {
                target_tile = t;
                break;
            }
        }

        uint64_t current_revision = QmlVideoBridge::Instance().GetTileRevision(target_tile);
        if (current_revision != last_revision && current_revision > 0) {
            DecodedFramePtr frame = QmlVideoBridge::Instance().GetTileFrame(target_tile);
            if (frame && frame->data.data() && frame->width > 0 && frame->height > 0) {
                uint32_t w = frame->width;
                uint32_t h = frame->height;
                size_t num_pixels = static_cast<size_t>(w) * h;

                if (rgb_buffer.size() != num_pixels * 3) {
                    rgb_buffer.resize(num_pixels * 3);
                }

                const uint8_t* src = frame->data.data();
                uint32_t stride = (frame->stride > 0) ? frame->stride : (w * 4);
                if (frame->format == PixelFormat::RGBA32) {
                    for (uint32_t y = 0; y < h; ++y) {
                        const uint8_t* row_src = src + y * stride;
                        uint8_t* row_dst = rgb_buffer.data() + y * (w * 3);
                        for (uint32_t x = 0; x < w; ++x) {
                            row_dst[x * 3 + 0] = row_src[x * 4 + 0];
                            row_dst[x * 3 + 1] = row_src[x * 4 + 1];
                            row_dst[x * 3 + 2] = row_src[x * 4 + 2];
                        }
                    }
                } else if (frame->format == PixelFormat::RGB24) {
                    uint32_t stride_rgb = (frame->stride > 0) ? frame->stride : (w * 3);
                    for (uint32_t y = 0; y < h; ++y) {
                        std::memcpy(rgb_buffer.data() + y * (w * 3), src + y * stride_rgb, w * 3);
                    }
                }

                jpeg_bytes.clear();
                bool enc_ok = TooJpeg::writeJpeg([&jpeg_bytes](uint8_t b) {
                    jpeg_bytes.push_back(b);
                }, rgb_buffer.data(), static_cast<unsigned short>(w), static_cast<unsigned short>(h), true, 75);

                if (enc_ok && !jpeg_bytes.empty()) {
                    std::string part_hdr = 
                        "--frame\r\n"
                        "Content-Type: image/jpeg\r\n"
                        "Content-Length: " + std::to_string(jpeg_bytes.size()) + "\r\n\r\n";
                    
                    if (send(client_fd, part_hdr.data(), part_hdr.size(), MSG_NOSIGNAL) <= 0) {
                        break;
                    }
                    if (send(client_fd, jpeg_bytes.data(), jpeg_bytes.size(), MSG_NOSIGNAL) <= 0) {
                        break;
                    }
                    if (send(client_fd, "\r\n", 2, MSG_NOSIGNAL) <= 0) {
                        break;
                    }
                    last_revision = current_revision;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    close(client_fd);
}

} // namespace nvr
