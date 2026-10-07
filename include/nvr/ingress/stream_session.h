#pragma once

#include "nvr/common/types.h"
#include "nvr/ingress/rtp_depacketizer.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

namespace nvr {

class StreamSession {
public:
    StreamSession(int channel_id, StreamType stream_type, const std::string& rtsp_url);
    ~StreamSession();

    void Start();
    void Stop();

    bool IsConnected() const;
    SessionState GetState() const;
    std::string GetUrl() const;

private:
    void WorkerLoop();

    bool ConnectSocket();
    void DisconnectSocket();

    bool SendRtspOptions();
    bool SendRtspDescribe();
    bool SendRtspSetup(const std::string& track_control, int rtp_channel = 0, int rtcp_channel = 1);
    bool SendRtspPlay();
    bool SendRtspKeepAlive();

    bool ParseRtspUrl(const std::string& url, std::string& host, int& port, std::string& path, std::string& auth_header);
    bool ReadRtspResponse(std::string& response, int timeout_ms = 4000);
    bool ReadExact(uint8_t* buffer, size_t length, int timeout_ms = 4000);

    void ParseSdp(const std::string& sdp, std::string& track_control, CodecType& codec, int& payload_type);

    int channel_id_{0};
    StreamType stream_type_{StreamType::MAIN};
    std::string rtsp_url_;

    std::atomic<bool> running_{false};
    std::atomic<SessionState> state_{SessionState::DISCONNECTED};
    std::thread worker_thread_;

    int sock_fd_{-1};
    int cseq_{1};
    std::string session_id_;
    std::string auth_header_;
    std::unique_ptr<RtpDepacketizer> depacketizer_;
    std::unique_ptr<RtpDepacketizer> audio_depacketizer_;

    bool has_audio_track_{false};
    std::string audio_track_control_;
    CodecType audio_codec_{CodecType::UNKNOWN};
    int audio_payload_type_{-1};
    uint32_t audio_clock_rate_{8000};
    uint8_t audio_channels_{1};
};

} // namespace nvr
