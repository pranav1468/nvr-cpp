#include "nvr/ingress/stream_session.h"
#include "nvr/media/stream_broker.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <cstring>
#include <chrono>

namespace nvr {

namespace {

// Base64 encoder for HTTP/RTSP basic authentication
std::string Base64Encode(const std::string& in) {
    static const char* kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (uint8_t c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(kChars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(kChars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

} // namespace

StreamSession::StreamSession(int channel_id, StreamType stream_type, const std::string& rtsp_url)
    : channel_id_(channel_id), stream_type_(stream_type), rtsp_url_(rtsp_url) {
    depacketizer_ = std::make_unique<RtpDepacketizer>(
        channel_id_, stream_type_, CodecType::H264,
        [](const MediaPacketPtr& pkt) {
            StreamBroker::Instance().Publish(pkt);
        }
    );
}

StreamSession::~StreamSession() {
    Stop();
}

void StreamSession::Start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_thread_ = std::thread(&StreamSession::WorkerLoop, this);
    LOG_INFO << "[Channel " << channel_id_ << "] Started RTSP session for " << rtsp_url_;
}

void StreamSession::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    DisconnectSocket();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    LOG_INFO << "[Channel " << channel_id_ << "] Stopped RTSP session";
}

bool StreamSession::IsConnected() const {
    return (state_ == SessionState::STREAMING);
}

SessionState StreamSession::GetState() const {
    return state_;
}

std::string StreamSession::GetUrl() const {
    return rtsp_url_;
}

bool StreamSession::ParseRtspUrl(const std::string& url, std::string& host, int& port, std::string& path, std::string& auth_header) {
    // Format: rtsp://[user:pass@]host[:port][/path]
    std::string prefix = "rtsp://";
    if (url.rfind(prefix, 0) != 0) {
        return false;
    }

    std::string rem = url.substr(prefix.length());
    size_t at_pos = rem.find('@');
    if (at_pos != std::string::npos) {
        std::string user_pass = rem.substr(0, at_pos);
        auth_header = "Authorization: Basic " + Base64Encode(user_pass) + "\r\n";
        rem = rem.substr(at_pos + 1);
    } else {
        auth_header.clear();
    }

    size_t slash_pos = rem.find('/');
    std::string host_port;
    if (slash_pos != std::string::npos) {
        host_port = rem.substr(0, slash_pos);
        path = rem.substr(slash_pos);
    } else {
        host_port = rem;
        path = "/";
    }

    size_t colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos) {
        host = host_port.substr(0, colon_pos);
        port = std::stoi(host_port.substr(colon_pos + 1));
    } else {
        host = host_port;
        port = 554;
    }

    return true;
}

bool StreamSession::ConnectSocket() {
    DisconnectSocket();

    std::string host, path;
    int port = 554;
    if (!ParseRtspUrl(rtsp_url_, host, port, path, auth_header_)) {
        LOG_ERROR << "[Channel " << channel_id_ << "] Malformed RTSP URL: " << rtsp_url_;
        return false;
    }

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        LOG_ERROR << "[Channel " << channel_id_ << "] Failed to resolve host: " << host;
        return false;
    }

    sock_fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_fd_ < 0) {
        freeaddrinfo(res);
        return false;
    }

    // Set non-blocking for connection timeout
    int flags = fcntl(sock_fd_, F_GETFL, 0);
    fcntl(sock_fd_, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(sock_fd_, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (ret < 0 && errno != EINPROGRESS) {
        DisconnectSocket();
        return false;
    }

    if (ret < 0) {
        struct pollfd pfd{};
        pfd.fd = sock_fd_;
        pfd.events = POLLOUT;
        int poll_ret = poll(&pfd, 1, 4000); // 4.0s timeout
        if (poll_ret <= 0) {
            DisconnectSocket();
            return false;
        }

        int sock_err = 0;
        socklen_t len = sizeof(sock_err);
        getsockopt(sock_fd_, SOL_SOCKET, SO_ERROR, &sock_err, &len);
        if (sock_err != 0) {
            DisconnectSocket();
            return false;
        }
    }

    // Restore blocking with receive timeout
    fcntl(sock_fd_, F_SETFL, flags);
    struct timeval tv{};
    tv.tv_sec = 4;
    tv.tv_usec = 0;
    setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    cseq_ = 1;
    session_id_.clear();
    state_ = SessionState::CONNECTING;
    return true;
}

void StreamSession::DisconnectSocket() {
    if (sock_fd_ >= 0) {
        close(sock_fd_);
        sock_fd_ = -1;
    }
    state_ = SessionState::DISCONNECTED;
    if (depacketizer_) {
        depacketizer_->Reset();
    }
}

bool StreamSession::ReadExact(uint8_t* buffer, size_t length, int timeout_ms) {
    size_t total = 0;
    while (total < length && running_) {
        struct pollfd pfd{};
        pfd.fd = sock_fd_;
        pfd.events = POLLIN;
        int p = poll(&pfd, 1, timeout_ms);
        if (p <= 0) {
            return false;
        }
        ssize_t n = recv(sock_fd_, buffer + total, length - total, 0);
        if (n <= 0) {
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return (total == length);
}

bool StreamSession::ReadRtspResponse(std::string& response, int timeout_ms) {
    response.clear();
    char c = 0;
    auto start_time = time_utils::MonotonicMs();

    while (running_ && (time_utils::MonotonicMs() - start_time < timeout_ms)) {
        if (!ReadExact(reinterpret_cast<uint8_t*>(&c), 1, 500)) {
            continue;
        }
        response.push_back(c);
        if (response.size() >= 4 && response.substr(response.size() - 4) == "\r\n\r\n") {
            // Check for Content-Length
            size_t cl_pos = response.find("Content-Length: ");
            if (cl_pos == std::string::npos) cl_pos = response.find("content-length: ");
            if (cl_pos != std::string::npos) {
                size_t val_start = cl_pos + 16;
                size_t val_end = response.find("\r\n", val_start);
                int content_len = std::stoi(response.substr(val_start, val_end - val_start));
                if (content_len > 0) {
                    std::vector<uint8_t> body(content_len);
                    if (ReadExact(body.data(), content_len, timeout_ms)) {
                        response.append(reinterpret_cast<char*>(body.data()), content_len);
                    }
                }
            }
            return true;
        }
    }
    return false;
}

bool StreamSession::SendRtspOptions() {
    std::ostringstream oss;
    oss << "OPTIONS " << rtsp_url_ << " RTSP/1.0\r\n"
        << "CSeq: " << cseq_++ << "\r\n"
        << "User-Agent: NVR_Core/1.0\r\n"
        << auth_header_
        << "\r\n";

    std::string req = oss.str();
    if (send(sock_fd_, req.c_str(), req.size(), 0) <= 0) return false;

    std::string resp;
    if (!ReadRtspResponse(resp) || resp.find("200 OK") == std::string::npos) {
        LOG_WARN << "[Channel " << channel_id_ << "] RTSP OPTIONS failed: " << resp;
        return false;
    }
    state_ = SessionState::OPTIONS_SENT;
    return true;
}

void StreamSession::ParseSdp(const std::string& sdp, std::string& track_control, CodecType& codec, int& payload_type) {
    track_control.clear();
    codec = CodecType::H264;
    payload_type = 96;

    std::istringstream stream(sdp);
    std::string line;
    bool in_video = false;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind("m=video", 0) == 0) {
            in_video = true;
            std::istringstream iss(line);
            std::string m, port, proto, pt;
            iss >> m >> port >> proto >> pt;
            if (!pt.empty()) {
                try { payload_type = std::stoi(pt); } catch (...) {}
            }
        } else if (line.rfind("m=", 0) == 0 && in_video) {
            in_video = false;
        }

        if (in_video) {
            if (line.find("H265") != std::string::npos || line.find("h265") != std::string::npos ||
                line.find("HEVC") != std::string::npos || line.find("hevc") != std::string::npos) {
                codec = CodecType::H265;
            }
            if (line.rfind("a=control:", 0) == 0) {
                track_control = line.substr(10);
            }
        }
    }

    if (track_control.empty()) {
        track_control = "trackID=1";
    }
}

bool StreamSession::SendRtspDescribe() {
    std::ostringstream oss;
    oss << "DESCRIBE " << rtsp_url_ << " RTSP/1.0\r\n"
        << "CSeq: " << cseq_++ << "\r\n"
        << "Accept: application/sdp\r\n"
        << "User-Agent: NVR_Core/1.0\r\n"
        << auth_header_
        << "\r\n";

    std::string req = oss.str();
    if (send(sock_fd_, req.c_str(), req.size(), 0) <= 0) return false;

    std::string resp;
    if (!ReadRtspResponse(resp) || resp.find("200 OK") == std::string::npos) {
        LOG_WARN << "[Channel " << channel_id_ << "] RTSP DESCRIBE failed: " << resp;
        return false;
    }

    std::string track_control;
    CodecType codec = CodecType::H264;
    int payload_type = 96;
    ParseSdp(resp, track_control, codec, payload_type);

    depacketizer_ = std::make_unique<RtpDepacketizer>(
        channel_id_, stream_type_, codec,
        [](const MediaPacketPtr& pkt) {
            StreamBroker::Instance().Publish(pkt);
        }
    );

    state_ = SessionState::DESCRIBE_SENT;
    return SendRtspSetup(track_control);
}

bool StreamSession::SendRtspSetup(const std::string& track_control) {
    std::string setup_url = rtsp_url_;
    if (track_control.rfind("rtsp://", 0) == 0) {
        setup_url = track_control;
    } else {
        if (!setup_url.empty() && setup_url.back() != '/' && !track_control.empty() && track_control.front() != '/') {
            setup_url += "/";
        }
        setup_url += track_control;
    }

    std::ostringstream oss;
    oss << "SETUP " << setup_url << " RTSP/1.0\r\n"
        << "CSeq: " << cseq_++ << "\r\n"
        << "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
        << "User-Agent: NVR_Core/1.0\r\n"
        << auth_header_
        << "\r\n";

    std::string req = oss.str();
    if (send(sock_fd_, req.c_str(), req.size(), 0) <= 0) return false;

    std::string resp;
    if (!ReadRtspResponse(resp) || resp.find("200 OK") == std::string::npos) {
        LOG_WARN << "[Channel " << channel_id_ << "] RTSP SETUP failed: " << resp;
        return false;
    }

    size_t sess_pos = resp.find("Session: ");
    if (sess_pos == std::string::npos) sess_pos = resp.find("session: ");
    if (sess_pos != std::string::npos) {
        size_t val_start = sess_pos + 9;
        size_t val_end = resp.find_first_of(";\r\n", val_start);
        session_id_ = resp.substr(val_start, val_end - val_start);
    }

    state_ = SessionState::SETUP_SENT;
    return SendRtspPlay();
}

bool StreamSession::SendRtspPlay() {
    std::ostringstream oss;
    oss << "PLAY " << rtsp_url_ << " RTSP/1.0\r\n"
        << "CSeq: " << cseq_++ << "\r\n"
        << "Session: " << session_id_ << "\r\n"
        << "Range: npt=0.000-\r\n"
        << "User-Agent: NVR_Core/1.0\r\n"
        << auth_header_
        << "\r\n";

    std::string req = oss.str();
    if (send(sock_fd_, req.c_str(), req.size(), 0) <= 0) return false;

    std::string resp;
    if (!ReadRtspResponse(resp) || resp.find("200 OK") == std::string::npos) {
        LOG_WARN << "[Channel " << channel_id_ << "] RTSP PLAY failed: " << resp;
        return false;
    }

    state_ = SessionState::STREAMING;
    LOG_INFO << "[Channel " << channel_id_ << "] RTSP PLAY confirmed! Interleaved streaming active.";
    return true;
}

bool StreamSession::SendRtspKeepAlive() {
    if (sock_fd_ < 0 || session_id_.empty()) return false;
    std::ostringstream oss;
    oss << "GET_PARAMETER " << rtsp_url_ << " RTSP/1.0\r\n"
        << "CSeq: " << cseq_++ << "\r\n"
        << "Session: " << session_id_ << "\r\n"
        << "User-Agent: NVR_Core/1.0\r\n"
        << auth_header_
        << "\r\n";

    std::string req = oss.str();
    return (send(sock_fd_, req.c_str(), req.size(), 0) > 0);
}

void StreamSession::WorkerLoop() {
    int backoff_sec = 1;
    auto last_keepalive = time_utils::MonotonicSec();

    while (running_) {
        if (!ConnectSocket() || !SendRtspOptions() || !SendRtspDescribe()) {
            LOG_WARN << "[Channel " << channel_id_ << "] RTSP handshake failed. Retrying in " 
                     << backoff_sec << "s...";
            state_ = SessionState::ERROR_BACKOFF;
            DisconnectSocket();
            for (int i = 0; i < backoff_sec * 10 && running_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            backoff_sec = std::min(backoff_sec * 2, 30);
            continue;
        }

        backoff_sec = 1; // Reset backoff on successful connect
        last_keepalive = time_utils::MonotonicSec();

        // Streaming reception loop for interleaved framing: '$' <channel> <length_hi> <length_lo> <data...>
        std::vector<uint8_t> rtp_buf(65536);
        while (running_ && state_ == SessionState::STREAMING) {
            uint8_t magic = 0;
            if (!ReadExact(&magic, 1, 3000)) {
                // Check if time for keepalive
                if (time_utils::MonotonicSec() - last_keepalive >= 25) {
                    SendRtspKeepAlive();
                    last_keepalive = time_utils::MonotonicSec();
                }
                continue;
            }

            if (magic != 0x24) { // '$' magic byte
                continue;
            }

            uint8_t hdr[3];
            if (!ReadExact(hdr, 3, 2000)) break;

            uint8_t channel = hdr[0];
            uint16_t length = (static_cast<uint16_t>(hdr[1]) << 8) | hdr[2];

            if (rtp_buf.size() < length) {
                rtp_buf.resize(length);
            }

            if (!ReadExact(rtp_buf.data(), length, 2000)) break;

            if (channel == 0 && length >= 12) { // RTP Channel
                // Parse RTP header
                uint8_t b0 = rtp_buf[0];
                uint8_t b1 = rtp_buf[1];
                bool marker_bit = (b1 & 0x80) != 0;
                uint16_t seq_num = (static_cast<uint16_t>(rtp_buf[2]) << 8) | rtp_buf[3];
                uint32_t rtp_timestamp = (static_cast<uint32_t>(rtp_buf[4]) << 24) |
                                         (static_cast<uint32_t>(rtp_buf[5]) << 16) |
                                         (static_cast<uint32_t>(rtp_buf[6]) << 8)  |
                                         rtp_buf[7];

                size_t csrc_count = b0 & 0x0F;
                size_t payload_offset = 12 + (csrc_count * 4);

                // Check extension bit
                if ((b0 & 0x10) != 0 && payload_offset + 4 <= length) {
                    uint16_t ext_len = (static_cast<uint16_t>(rtp_buf[payload_offset + 2]) << 8) |
                                       rtp_buf[payload_offset + 3];
                    payload_offset += 4 + (ext_len * 4);
                }

                if (payload_offset < length) {
                    depacketizer_->ProcessRtpPacket(
                        rtp_buf.data() + payload_offset,
                        length - payload_offset,
                        rtp_timestamp,
                        seq_num,
                        marker_bit
                    );
                }
            }

            if (time_utils::MonotonicSec() - last_keepalive >= 25) {
                SendRtspKeepAlive();
                last_keepalive = time_utils::MonotonicSec();
            }
        }

        DisconnectSocket();
    }
}

} // namespace nvr
