#include "nvr/ingress/stream_session.h"
#include "nvr/media/stream_broker.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"
#include "nvr/common/md5.h"

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

std::vector<uint8_t> Base64Decode(const std::string& in) {
    static const int8_t kLookup[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::vector<uint8_t> out;
    int val = 0, valb = -8;
    for (uint8_t c : in) {
        if (kLookup[c] == -1) continue;
        val = (val << 6) + kLookup[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
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
    {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        stop_cv_.notify_all();
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

bool StreamSession::ParseRtspUrl(const std::string& url, std::string& host, int& port, std::string& path,
                                 std::string& auth_header, std::string& username, std::string& password) {
    // Format: rtsp://[user:pass@]host[:port][/path]
    // host can be IPv4, hostname, or bracketed IPv6 [2001:db8::1]
    std::string prefix = "rtsp://";
    if (url.rfind(prefix, 0) != 0) {
        return false;
    }

    std::string rem = url.substr(prefix.length());
    size_t first_slash = rem.find('/');
    size_t search_end = (first_slash != std::string::npos) ? first_slash : rem.size();
    size_t at_pos = rem.rfind('@', search_end);
    if (at_pos != std::string::npos && at_pos < search_end) {
        std::string user_pass = rem.substr(0, at_pos);
        auth_header = "Authorization: Basic " + Base64Encode(user_pass) + "\r\n";
        size_t colon_user = user_pass.find(':');
        if (colon_user != std::string::npos) {
            username = user_pass.substr(0, colon_user);
            password = user_pass.substr(colon_user + 1);
        } else {
            username = user_pass;
            password.clear();
        }
        rem = rem.substr(at_pos + 1);
    } else {
        auth_header.clear();
        username.clear();
        password.clear();
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

    if (!host_port.empty() && host_port.front() == '[') {
        size_t close_bracket = host_port.find(']');
        if (close_bracket != std::string::npos) {
            host = host_port.substr(1, close_bracket - 1);
            if (close_bracket + 1 < host_port.size() && host_port[close_bracket + 1] == ':') {
                try {
                    port = std::stoi(host_port.substr(close_bracket + 2));
                } catch (...) {
                    port = 554;
                }
            } else {
                port = 554;
            }
        } else {
            host = host_port;
            port = 554;
        }
    } else {
        size_t colon_pos = host_port.rfind(':');
        if (colon_pos != std::string::npos) {
            host = host_port.substr(0, colon_pos);
            try {
                port = std::stoi(host_port.substr(colon_pos + 1));
            } catch (...) {
                port = 554;
            }
        } else {
            host = host_port;
            port = 554;
        }
    }

    return true;
}

bool StreamSession::ParseRtspUrl(const std::string& url, std::string& host, int& port, std::string& path, std::string& auth_header) {
    return ParseRtspUrl(url, host, port, path, auth_header, username_, password_);
}

void StreamSession::ParseDigestChallenge(const std::string& response, std::string& realm,
                                         std::string& nonce, std::string& opaque, std::string& qop) {
    size_t pos = response.find("WWW-Authenticate: Digest");
    if (pos == std::string::npos) pos = response.find("www-authenticate: digest");
    if (pos == std::string::npos) pos = response.find("WWW-Authenticate: digest");
    if (pos == std::string::npos) pos = response.find("www-authenticate: Digest");
    if (pos == std::string::npos) return;

    size_t end_line = response.find("\r\n", pos);
    std::string challenge = (end_line != std::string::npos) ?
        response.substr(pos, end_line - pos) : response.substr(pos);

    auto extract_param = [&](const std::string& key) -> std::string {
        size_t kpos = challenge.find(key + "=");
        if (kpos == std::string::npos) return "";
        size_t vstart = kpos + key.size() + 1;
        if (vstart >= challenge.size()) return "";
        if (challenge[vstart] == '"') {
            vstart++;
            size_t vend = challenge.find('"', vstart);
            if (vend != std::string::npos) {
                return challenge.substr(vstart, vend - vstart);
            }
        } else {
            size_t vend = challenge.find_first_of(", \r\n", vstart);
            return (vend != std::string::npos) ?
                challenge.substr(vstart, vend - vstart) : challenge.substr(vstart);
        }
        return "";
    };

    realm = extract_param("realm");
    nonce = extract_param("nonce");
    opaque = extract_param("opaque");
    qop = extract_param("qop");
}

void StreamSession::ParseDigestChallenge(const std::string& response) {
    ParseDigestChallenge(response, digest_realm_, digest_nonce_, digest_opaque_, digest_qop_);
}

bool StreamSession::BuildDigestAuthHeader(const std::string& username, const std::string& password,
                                          const std::string& realm, const std::string& nonce,
                                          const std::string& method, const std::string& uri,
                                          const std::string& qop, const std::string& opaque,
                                          std::string& out_header) {
    if (username.empty() || realm.empty() || nonce.empty()) {
        return false;
    }

    // HA1 = MD5(username:realm:password)
    std::string ha1 = crypto::ComputeMD5(username + ":" + realm + ":" + password);
    // HA2 = MD5(method:uri)
    std::string ha2 = crypto::ComputeMD5(method + ":" + uri);

    std::string response;
    std::ostringstream oss;
    oss << "Authorization: Digest username=\"" << username << "\", "
        << "realm=\"" << realm << "\", "
        << "nonce=\"" << nonce << "\", "
        << "uri=\"" << uri << "\", ";

    if (qop.find("auth") != std::string::npos) {
        std::string nc = "00000001";
        std::string cnonce = "0a4f113b";
        // response = MD5(HA1:nonce:nc:cnonce:qop:HA2)
        response = crypto::ComputeMD5(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":auth:" + ha2);
        oss << "qop=auth, nc=" << nc << ", cnonce=\"" << cnonce << "\", "
            << "response=\"" << response << "\"";
    } else {
        // response = MD5(HA1:nonce:HA2)
        response = crypto::ComputeMD5(ha1 + ":" + nonce + ":" + ha2);
        oss << "response=\"" << response << "\"";
    }

    if (!opaque.empty()) {
        oss << ", opaque=\"" << opaque << "\"";
    }
    oss << "\r\n";

    out_header = oss.str();
    return true;
}

bool StreamSession::BuildDigestAuthHeader(const std::string& method, const std::string& uri, std::string& out_header) {
    return BuildDigestAuthHeader(username_, password_, digest_realm_, digest_nonce_,
                                 method, uri, digest_qop_, digest_opaque_, out_header);
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
    hints.ai_family = AF_UNSPEC;
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
        int poll_ret = 0;
        int waited = 0;
        while (running_ && waited < 4000) {
            poll_ret = poll(&pfd, 1, 100);
            if (poll_ret != 0) break;
            waited += 100;
        }
        if (!running_ || poll_ret <= 0) {
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
        shutdown(sock_fd_, SHUT_RDWR);
        close(sock_fd_);
        sock_fd_ = -1;
    }
    state_ = SessionState::DISCONNECTED;
    if (depacketizer_) {
        depacketizer_->Reset();
    }
    if (audio_depacketizer_) {
        audio_depacketizer_->Reset();
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
                int content_len = 0;
                if (val_end != std::string::npos && val_end > val_start) {
                    try {
                        content_len = std::stoi(response.substr(val_start, val_end - val_start));
                    } catch (...) {
                        content_len = 0;
                    }
                }
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
    has_audio_track_ = false;
    audio_track_control_.clear();
    audio_codec_ = CodecType::UNKNOWN;
    audio_clock_rate_ = 8000;
    audio_channels_ = 1;

    std::istringstream stream(sdp);
    std::string line;
    bool in_video = false;
    bool in_audio = false;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind("m=video", 0) == 0) {
            in_video = true;
            in_audio = false;
            std::istringstream iss(line);
            std::string m, port, proto, pt;
            iss >> m >> port >> proto >> pt;
            if (!pt.empty()) {
                try { payload_type = std::stoi(pt); } catch (...) {}
            }
        } else if (line.rfind("m=audio", 0) == 0) {
            in_audio = true;
            in_video = false;
            has_audio_track_ = true;
            std::istringstream iss(line);
            std::string m, port, proto, pt;
            iss >> m >> port >> proto >> pt;
            if (!pt.empty()) {
                try {
                    audio_payload_type_ = std::stoi(pt);
                    if (audio_payload_type_ == 0) {
                        audio_codec_ = CodecType::PCMU;
                        audio_clock_rate_ = 8000;
                    } else if (audio_payload_type_ == 8) {
                        audio_codec_ = CodecType::PCMA;
                        audio_clock_rate_ = 8000;
                    }
                } catch (...) {}
            }
        } else if (line.rfind("m=", 0) == 0) {
            in_video = false;
            in_audio = false;
        }

        if (in_video) {
            if (line.find("H265") != std::string::npos || line.find("h265") != std::string::npos ||
                line.find("HEVC") != std::string::npos || line.find("hevc") != std::string::npos) {
                codec = CodecType::H265;
            }
            if (line.rfind("a=control:", 0) == 0) {
                track_control = line.substr(10);
            }
            size_t sps_pos = line.find("sprop-parameter-sets=");
            if (sps_pos != std::string::npos) {
                std::string params = line.substr(sps_pos + 21);
                size_t semi = params.find(';');
                if (semi != std::string::npos) params = params.substr(0, semi);
                std::stringstream ss(params);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    if (!item.empty()) {
                        auto raw = Base64Decode(item);
                        if (!raw.empty()) {
                            auto pkt = std::make_shared<MediaPacket>();
                            pkt->channel_id = channel_id_;
                            pkt->stream_type = stream_type_;
                            pkt->codec = CodecType::H264;
                            pkt->is_keyframe = true;
                            pkt->wall_time_ms = time_utils::WallTimeMs();
                            pkt->data = {0x00, 0x00, 0x00, 0x01};
                            pkt->data.insert(pkt->data.end(), raw.begin(), raw.end());
                            StreamBroker::Instance().Publish(pkt);
                        }
                    }
                }
            }
        } else if (in_audio) {
            if (line.rfind("a=control:", 0) == 0) {
                audio_track_control_ = line.substr(10);
            }
            if (line.find("PCMU") != std::string::npos || line.find("pcmu") != std::string::npos) {
                audio_codec_ = CodecType::PCMU;
                audio_clock_rate_ = 8000;
            } else if (line.find("PCMA") != std::string::npos || line.find("pcma") != std::string::npos) {
                audio_codec_ = CodecType::PCMA;
                audio_clock_rate_ = 8000;
            } else if (line.find("MPEG4-GENERIC") != std::string::npos || line.find("mpeg4-generic") != std::string::npos ||
                       line.find("MP4A-LATM") != std::string::npos || line.find("mp4a-latm") != std::string::npos) {
                audio_codec_ = CodecType::AAC;
                size_t slash1 = line.find('/');
                if (slash1 != std::string::npos) {
                    size_t slash2 = line.find('/', slash1 + 1);
                    std::string rate_str = (slash2 != std::string::npos) ?
                        line.substr(slash1 + 1, slash2 - slash1 - 1) : line.substr(slash1 + 1);
                    try { audio_clock_rate_ = std::stoi(rate_str); } catch (...) {}
                    if (slash2 != std::string::npos) {
                        try { audio_channels_ = static_cast<uint8_t>(std::stoi(line.substr(slash2 + 1))); } catch (...) {}
                    }
                }
            }
        }
    }

    if (track_control.empty()) {
        track_control = "trackID=1";
    }
    if (has_audio_track_ && audio_track_control_.empty()) {
        audio_track_control_ = "trackID=2";
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
    if (!ReadRtspResponse(resp)) return false;

    if (resp.find("401") != std::string::npos) {
        LOG_INFO << "[Channel " << channel_id_ << "] RTSP DESCRIBE received 401 Unauthorized, testing Digest auth challenge";
        ParseDigestChallenge(resp);
        if (!digest_nonce_.empty() && !username_.empty()) {
            BuildDigestAuthHeader("DESCRIBE", rtsp_url_, auth_header_);
            std::ostringstream retry_oss;
            retry_oss << "DESCRIBE " << rtsp_url_ << " RTSP/1.0\r\n"
                      << "CSeq: " << cseq_++ << "\r\n"
                      << "Accept: application/sdp\r\n"
                      << "User-Agent: NVR_Core/1.0\r\n"
                      << auth_header_
                      << "\r\n";
            std::string retry_req = retry_oss.str();
            if (send(sock_fd_, retry_req.c_str(), retry_req.size(), 0) <= 0) return false;
            if (!ReadRtspResponse(resp)) return false;
        }
    }

    if (resp.find("200 OK") == std::string::npos) {
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

    if (has_audio_track_ && audio_codec_ != CodecType::UNKNOWN) {
        audio_depacketizer_ = std::make_unique<RtpDepacketizer>(
            channel_id_, stream_type_, audio_codec_,
            [](const MediaPacketPtr& pkt) {
                StreamBroker::Instance().Publish(pkt);
            },
            audio_clock_rate_
        );
        LOG_INFO << "[Channel " << channel_id_ << "] Detected audio track: codec=" 
                 << CodecToString(audio_codec_) << " (" << audio_clock_rate_ << " Hz, "
                 << static_cast<int>(audio_channels_) << " ch)";
    }

    state_ = SessionState::DESCRIBE_SENT;
    if (!SendRtspSetup(track_control, 0, 1)) {
        return false;
    }

    if (has_audio_track_ && !audio_track_control_.empty() && audio_codec_ != CodecType::UNKNOWN) {
        if (!SendRtspSetup(audio_track_control_, 2, 3)) {
            LOG_WARN << "[Channel " << channel_id_ << "] Audio track SETUP failed, continuing video-only";
        }
    }

    return SendRtspPlay();
}

bool StreamSession::SendRtspSetup(const std::string& track_control, int rtp_channel, int rtcp_channel) {
    std::string setup_url = rtsp_url_;
    if (track_control.rfind("rtsp://", 0) == 0) {
        setup_url = track_control;
    } else {
        if (!setup_url.empty() && setup_url.back() != '/' && !track_control.empty() && track_control.front() != '/') {
            setup_url += "/";
        }
        setup_url += track_control;
    }

    if (!digest_nonce_.empty() && !username_.empty()) {
        BuildDigestAuthHeader("SETUP", setup_url, auth_header_);
    }

    std::ostringstream oss;
    oss << "SETUP " << setup_url << " RTSP/1.0\r\n"
        << "CSeq: " << cseq_++ << "\r\n"
        << "Transport: RTP/AVP/TCP;unicast;interleaved=" << rtp_channel << "-" << rtcp_channel << "\r\n";
    if (!session_id_.empty()) {
        oss << "Session: " << session_id_ << "\r\n";
    }
    oss << "User-Agent: NVR_Core/1.0\r\n"
        << auth_header_
        << "\r\n";

    std::string req = oss.str();
    if (send(sock_fd_, req.c_str(), req.size(), 0) <= 0) return false;

    std::string resp;
    if (!ReadRtspResponse(resp) || resp.find("200 OK") == std::string::npos) {
        LOG_WARN << "[Channel " << channel_id_ << "] RTSP SETUP failed: " << resp;
        return false;
    }

    if (session_id_.empty()) {
        size_t sess_pos = resp.find("Session: ");
        if (sess_pos == std::string::npos) sess_pos = resp.find("session: ");
        if (sess_pos != std::string::npos) {
            size_t val_start = sess_pos + 9;
            size_t val_end = resp.find_first_of(";\r\n", val_start);
            session_id_ = resp.substr(val_start, val_end - val_start);
        }
    }

    state_ = SessionState::SETUP_SENT;
    return true;
}

bool StreamSession::SendRtspPlay() {
    if (!digest_nonce_.empty() && !username_.empty()) {
        BuildDigestAuthHeader("PLAY", rtsp_url_, auth_header_);
    }

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
            {
                std::unique_lock<std::mutex> lock(stop_mutex_);
                stop_cv_.wait_for(lock, std::chrono::seconds(backoff_sec), [this] {
                    return !running_.load();
                });
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

            if ((channel == 0 || channel == 2) && length >= 12) { // Video (ch 0) or Audio (ch 2) RTP
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
                    if (channel == 0 && depacketizer_) {
                        depacketizer_->ProcessRtpPacket(
                            rtp_buf.data() + payload_offset,
                            length - payload_offset,
                            rtp_timestamp,
                            seq_num,
                            marker_bit
                        );
                    } else if (channel == 2 && audio_depacketizer_) {
                        audio_depacketizer_->ProcessRtpPacket(
                            rtp_buf.data() + payload_offset,
                            length - payload_offset,
                            rtp_timestamp,
                            seq_num,
                            marker_bit
                        );
                    }
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
