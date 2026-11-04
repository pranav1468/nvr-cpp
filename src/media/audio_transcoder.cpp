#include "nvr/media/audio_transcoder.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"

#include <dlfcn.h>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace nvr {

namespace {

// Standard ITU-T G.711 A-law to 16-bit linear PCM conversion
inline int16_t AlawToPcm(uint8_t a_val) {
    a_val ^= 0x55;
    int t = (a_val & 0x0F) << 4;
    int seg = (a_val & 0x70) >> 4;
    switch (seg) {
        case 0: t += 8; break;
        case 1: t += 0x108; break;
        default: t += 0x108; t <<= (seg - 1); break;
    }
    return (a_val & 0x80) ? t : -t;
}

// Standard ITU-T G.711 mu-law to 16-bit linear PCM conversion
inline int16_t UlawToPcm(uint8_t u_val) {
    u_val = ~u_val;
    int t = ((u_val & 0x0F) << 3) + 0x84;
    t <<= (u_val & 0x70) >> 4;
    return (u_val & 0x80) ? (0x84 - t) : (t - 0x84);
}

// Construct standard 7-byte ADTS header for AAC-LC access units
inline void BuildAdtsHeader(uint8_t* adts, size_t aac_payload_size, uint8_t freq_idx, uint8_t channels) {
    size_t full_len = aac_payload_size + 7;
    adts[0] = 0xFF;
    adts[1] = 0xF1; // MPEG-4, Layer 0, No protection (16-bit syncword 0xFFF1)
    adts[2] = static_cast<uint8_t>((1 << 6) | ((freq_idx & 0x0F) << 2) | ((channels >> 2) & 1));
    adts[3] = static_cast<uint8_t>(((channels & 3) << 6) | ((full_len >> 11) & 0x03));
    adts[4] = static_cast<uint8_t>((full_len >> 3) & 0xFF);
    adts[5] = static_cast<uint8_t>(((full_len & 7) << 5) | 0x1F);
    adts[6] = 0xFC;
}

struct AVOption {
    const char* name;
    const char* help;
    int offset;
    int type;
};

} // namespace

AudioTranscoder::AudioTranscoder(int channel_id, StreamType stream_type, PacketCallback output_callback)
    : channel_id_(channel_id), stream_type_(stream_type), output_callback_(std::move(output_callback)) {
    pcm_fifo_.reserve(4096);
}

AudioTranscoder::~AudioTranscoder() {
    CloseEncoder();
}

void AudioTranscoder::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseEncoder();
    pcm_fifo_.clear();
    has_pts_ = false;
    pts_us_ = 0;
    total_samples_encoded_ = 0;
}

void AudioTranscoder::CloseEncoder() {
    if (codec_ctx_ && avcodec_lib_) {
        auto free_ctx = reinterpret_cast<void(*)(void**)>(dlsym(avcodec_lib_, "avcodec_free_context"));
        if (free_ctx) {
            free_ctx(&codec_ctx_);
        }
        codec_ctx_ = nullptr;
    }

    if (av_pkt_ && avcodec_lib_) {
        auto pkt_unref = reinterpret_cast<void(*)(void*)>(dlsym(avcodec_lib_, "av_packet_unref"));
        auto pkt_free = reinterpret_cast<void(*)(void**)>(dlsym(avcodec_lib_, "av_packet_free"));
        if (pkt_unref) pkt_unref(av_pkt_);
        if (pkt_free) pkt_free(&av_pkt_);
        av_pkt_ = nullptr;
    }

    if (av_frame_ && avutil_lib_) {
        auto frame_free = reinterpret_cast<void(*)(void**)>(dlsym(avutil_lib_, "av_frame_free"));
        if (frame_free) frame_free(&av_frame_);
        av_frame_ = nullptr;
    }

    if (avcodec_lib_) {
        dlclose(avcodec_lib_);
        avcodec_lib_ = nullptr;
    }
    if (avutil_lib_) {
        dlclose(avutil_lib_);
        avutil_lib_ = nullptr;
    }
    encoder_initialized_ = false;
}

bool AudioTranscoder::InitializeEncoder(uint32_t sample_rate, uint8_t channels) {
    if (encoder_initialized_) return true;

    sample_rate_ = (sample_rate > 0) ? sample_rate : 8000;
    channels_ = (channels > 0) ? channels : 1;

    const char* codec_libs[] = {
        "libavcodec.so.61", "libavcodec.so.60", "libavcodec.so.59", "libavcodec.so.58", "libavcodec.so"
    };
    for (const char* name : codec_libs) {
        avcodec_lib_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (avcodec_lib_) break;
    }

    const char* util_libs[] = {
        "libavutil.so.59", "libavutil.so.58", "libavutil.so.57", "libavutil.so.56", "libavutil.so"
    };
    for (const char* name : util_libs) {
        avutil_lib_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (avutil_lib_) break;
    }

    if (!avcodec_lib_ || !avutil_lib_) {
        LOG_WARN << "[Channel " << channel_id_ << "] AudioTranscoder: libavcodec/libavutil not available for in-process AAC encoding";
        return false;
    }

    auto find_encoder = reinterpret_cast<void*(*)(int)>(dlsym(avcodec_lib_, "avcodec_find_encoder"));
    auto alloc_ctx = reinterpret_cast<void*(*)(const void*)>(dlsym(avcodec_lib_, "avcodec_alloc_context3"));
    auto open2 = reinterpret_cast<int(*)(void*, const void*, void**)>(dlsym(avcodec_lib_, "avcodec_open2"));
    auto pkt_alloc = reinterpret_cast<void*(*)()>(dlsym(avcodec_lib_, "av_packet_alloc"));
    auto frame_alloc = reinterpret_cast<void*(*)()>(dlsym(avutil_lib_, "av_frame_alloc"));
    auto frame_get_buf = reinterpret_cast<int(*)(void*, int)>(dlsym(avutil_lib_, "av_frame_get_buffer"));
    auto opt_next = reinterpret_cast<const AVOption*(*)(void*, const AVOption*)>(dlsym(avutil_lib_, "av_opt_next"));

    if (!find_encoder || !alloc_ctx || !open2 || !pkt_alloc || !frame_alloc || !frame_get_buf) {
        LOG_WARN << "[Channel " << channel_id_ << "] AudioTranscoder: Required FFmpeg codec symbols not found";
        CloseEncoder();
        return false;
    }

    constexpr int kCodecIdAac = 86018; // AV_CODEC_ID_AAC
    void* enc = find_encoder(kCodecIdAac);
    if (!enc) {
        LOG_WARN << "[Channel " << channel_id_ << "] AudioTranscoder: AV_CODEC_ID_AAC encoder not registered in libavcodec";
        CloseEncoder();
        return false;
    }

    codec_ctx_ = alloc_ctx(enc);
    if (!codec_ctx_) {
        CloseEncoder();
        return false;
    }

    // Determine field offsets dynamically from AVOption table for cross-version compatibility
    int off_bit_rate = 0x38;
    int off_sample_rate = 0x190;
    int off_channels = 0x194;
    int off_sample_fmt = 0x198;
    int off_channel_layout = 0x1b0;

    if (opt_next) {
        const AVOption* opt = nullptr;
        while ((opt = opt_next(codec_ctx_, opt)) != nullptr) {
            if (opt->name) {
                if (std::strcmp(opt->name, "b") == 0) off_bit_rate = opt->offset;
                else if (std::strcmp(opt->name, "ar") == 0) off_sample_rate = opt->offset;
                else if (std::strcmp(opt->name, "ac") == 0 && opt->type == 1) off_channels = opt->offset;
                else if (std::strcmp(opt->name, "channel_layout") == 0) off_channel_layout = opt->offset;
            }
        }
    }

    char* ctx_ptr = reinterpret_cast<char*>(codec_ctx_);
    *reinterpret_cast<int64_t*>(ctx_ptr + off_bit_rate) = 32000; // 32 kbps mono
    *reinterpret_cast<int*>(ctx_ptr + 0x64) = 1; // time_base.num
    *reinterpret_cast<int*>(ctx_ptr + 0x68) = static_cast<int>(sample_rate_); // time_base.den
    *reinterpret_cast<int*>(ctx_ptr + off_sample_rate) = static_cast<int>(sample_rate_);
    *reinterpret_cast<int*>(ctx_ptr + off_channels) = static_cast<int>(channels_);
    *reinterpret_cast<int*>(ctx_ptr + off_sample_fmt) = 8; // AV_SAMPLE_FMT_FLTP
    *reinterpret_cast<uint64_t*>(ctx_ptr + off_channel_layout) = (channels_ == 2) ? 3ULL : 4ULL; // STEREO(3) or MONO(4)

    int ret = open2(codec_ctx_, enc, nullptr);
    if (ret != 0) {
        LOG_WARN << "[Channel " << channel_id_ << "] AudioTranscoder: avcodec_open2 failed for AAC (ret=" << ret << ")";
        CloseEncoder();
        return false;
    }

    av_frame_ = frame_alloc();
    av_pkt_ = pkt_alloc();
    if (!av_frame_ || !av_pkt_) {
        CloseEncoder();
        return false;
    }

    char* f_ptr = reinterpret_cast<char*>(av_frame_);
    *reinterpret_cast<int*>(f_ptr + 0x70) = 1024; // nb_samples
    *reinterpret_cast<int*>(f_ptr + 0x74) = 8;    // format = AV_SAMPLE_FMT_FLTP
    *reinterpret_cast<uint64_t*>(f_ptr + 0x110) = (channels_ == 2) ? 3ULL : 4ULL; // channel_layout
    *reinterpret_cast<int*>(f_ptr + 0x118) = static_cast<int>(channels_);
    *reinterpret_cast<int*>(f_ptr + 0x11c) = static_cast<int>(sample_rate_);

    ret = frame_get_buf(av_frame_, 0);
    if (ret != 0) {
        LOG_WARN << "[Channel " << channel_id_ << "] AudioTranscoder: av_frame_get_buffer failed (ret=" << ret << ")";
        CloseEncoder();
        return false;
    }

    encoder_initialized_ = true;
    LOG_INFO << "[Channel " << channel_id_ << "] AudioTranscoder: Initialized in-process AAC-LC encoder (" 
             << sample_rate_ << " Hz, " << static_cast<int>(channels_) << " ch, 32 kbps)";
    return true;
}

void AudioTranscoder::ProcessPacket(const MediaPacketPtr& packet) {
    if (!packet || packet->data.empty()) return;

    // Passthrough if stream is already encoded in AAC
    if (packet->codec == CodecType::AAC) {
        if (output_callback_) {
            output_callback_(packet);
        }
        return;
    }

    // Only transcode G.711 PCMA and PCMU
    if (packet->codec != CodecType::PCMA && packet->codec != CodecType::PCMU) {
        if (output_callback_) {
            output_callback_(packet);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    uint32_t in_rate = (packet->sample_rate > 0) ? packet->sample_rate : 8000;
    uint8_t in_ch = (packet->channels > 0) ? packet->channels : 1;

    if (!encoder_initialized_) {
        if (!InitializeEncoder(in_rate, in_ch)) {
            // If encoder cannot be loaded, fallback to raw packet pass
            if (output_callback_) output_callback_(packet);
            return;
        }
    }

    if (!has_pts_) {
        pts_us_ = packet->pts_us > 0 ? packet->pts_us : time_utils::MonotonicUs();
        has_pts_ = true;
    }

    // Decode G.711 bytes to normalized float [-1.0f, 1.0f]
    const uint8_t* src = packet->data.data();
    size_t len = packet->data.size();
    bool is_alaw = (packet->codec == CodecType::PCMA);

    for (size_t i = 0; i < len; ++i) {
        int16_t s16 = is_alaw ? AlawToPcm(src[i]) : UlawToPcm(src[i]);
        float flt = static_cast<float>(s16) / 32768.0f;
        pcm_fifo_.push_back(flt);
    }

    // AAC-LC frames are fixed at 1024 samples per frame
    constexpr size_t kFrameSize = 1024;
    while (pcm_fifo_.size() >= kFrameSize) {
        EncodeFrame(pcm_fifo_.data(), kFrameSize);
        pcm_fifo_.erase(pcm_fifo_.begin(), pcm_fifo_.begin() + kFrameSize);
    }
}

void AudioTranscoder::EncodeFrame(const float* samples, size_t count) {
    if (!encoder_initialized_ || !codec_ctx_ || !av_frame_ || !av_pkt_) return;

    auto send_frame = reinterpret_cast<int(*)(void*, const void*)>(dlsym(avcodec_lib_, "avcodec_send_frame"));
    auto receive_packet = reinterpret_cast<int(*)(void*, void*)>(dlsym(avcodec_lib_, "avcodec_receive_packet"));
    auto pkt_unref = reinterpret_cast<void(*)(void*)>(dlsym(avcodec_lib_, "av_packet_unref"));

    if (!send_frame || !receive_packet || !pkt_unref) return;

    float* data0 = *reinterpret_cast<float**>(av_frame_);
    std::memcpy(data0, samples, count * sizeof(float));

    int64_t current_pts = pts_us_ + static_cast<int64_t>((total_samples_encoded_ * 1000000ULL) / sample_rate_);
    *reinterpret_cast<int64_t*>(reinterpret_cast<char*>(av_frame_) + 0x88) = total_samples_encoded_; // frame pts in samples
    total_samples_encoded_ += count;

    int ret = send_frame(codec_ctx_, av_frame_);
    if (ret != 0) return;

    while (receive_packet(codec_ctx_, av_pkt_) == 0) {
        char* pkt_ptr = reinterpret_cast<char*>(av_pkt_);
        int pkt_size = *reinterpret_cast<int*>(pkt_ptr + 32);
        uint8_t* pkt_data = *reinterpret_cast<uint8_t**>(pkt_ptr + 24);

        if (pkt_size > 0 && pkt_data) {
            uint8_t freq_idx = 11; // 8000 Hz
            switch (sample_rate_) {
                case 96000: freq_idx = 0; break;
                case 88200: freq_idx = 1; break;
                case 64000: freq_idx = 2; break;
                case 48000: freq_idx = 3; break;
                case 44100: freq_idx = 4; break;
                case 32000: freq_idx = 5; break;
                case 24000: freq_idx = 6; break;
                case 22050: freq_idx = 7; break;
                case 16000: freq_idx = 8; break;
                case 12000: freq_idx = 9; break;
                case 11025: freq_idx = 10; break;
                case 8000:  freq_idx = 11; break;
                default:    freq_idx = 11; break;
            }

            auto aac_pkt = std::make_shared<MediaPacket>();
            aac_pkt->channel_id = channel_id_;
            aac_pkt->stream_type = stream_type_;
            aac_pkt->codec = CodecType::AAC;
            aac_pkt->media_type = MediaType::AUDIO;
            aac_pkt->is_keyframe = true;
            aac_pkt->sample_rate = sample_rate_;
            aac_pkt->channels = channels_;
            aac_pkt->pts_us = current_pts;
            aac_pkt->dts_us = current_pts;
            aac_pkt->wall_time_ms = time_utils::WallTimeMs();

            // Wrap AAC raw frame with standard 7-byte ADTS header for universal demuxing
            aac_pkt->data.resize(7 + pkt_size);
            BuildAdtsHeader(aac_pkt->data.data(), pkt_size, freq_idx, channels_);
            std::memcpy(aac_pkt->data.data() + 7, pkt_data, pkt_size);

            if (output_callback_) {
                output_callback_(aac_pkt);
            }
        }
        pkt_unref(av_pkt_);
    }
}

void AudioTranscoder::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!encoder_initialized_) return;

    if (!pcm_fifo_.empty()) {
        size_t pad = 1024 - (pcm_fifo_.size() % 1024);
        if (pad > 0 && pad < 1024) {
            pcm_fifo_.insert(pcm_fifo_.end(), pad, 0.0f);
        }
        while (pcm_fifo_.size() >= 1024) {
            EncodeFrame(pcm_fifo_.data(), 1024);
            pcm_fifo_.erase(pcm_fifo_.begin(), pcm_fifo_.begin() + 1024);
        }
    }

    if (codec_ctx_ && avcodec_lib_) {
        auto send_frame = reinterpret_cast<int(*)(void*, const void*)>(dlsym(avcodec_lib_, "avcodec_send_frame"));
        auto receive_packet = reinterpret_cast<int(*)(void*, void*)>(dlsym(avcodec_lib_, "avcodec_receive_packet"));
        auto pkt_unref = reinterpret_cast<void(*)(void*)>(dlsym(avcodec_lib_, "av_packet_unref"));

        if (send_frame && receive_packet && pkt_unref) {
            send_frame(codec_ctx_, nullptr); // flush signal
            while (receive_packet(codec_ctx_, av_pkt_) == 0) {
                char* pkt_ptr = reinterpret_cast<char*>(av_pkt_);
                int pkt_size = *reinterpret_cast<int*>(pkt_ptr + 32);
                uint8_t* pkt_data = *reinterpret_cast<uint8_t**>(pkt_ptr + 24);
                if (pkt_size > 0 && pkt_data) {
                    auto aac_pkt = std::make_shared<MediaPacket>();
                    aac_pkt->channel_id = channel_id_;
                    aac_pkt->stream_type = stream_type_;
                    aac_pkt->codec = CodecType::AAC;
                    aac_pkt->media_type = MediaType::AUDIO;
                    aac_pkt->is_keyframe = true;
                    aac_pkt->sample_rate = sample_rate_;
                    aac_pkt->channels = channels_;
                    aac_pkt->pts_us = pts_us_ + static_cast<int64_t>((total_samples_encoded_ * 1000000ULL) / sample_rate_);
                    aac_pkt->dts_us = aac_pkt->pts_us;
                    aac_pkt->wall_time_ms = time_utils::WallTimeMs();

                    uint8_t freq_idx = (sample_rate_ == 8000) ? 11 : 8;
                    aac_pkt->data.resize(7 + pkt_size);
                    BuildAdtsHeader(aac_pkt->data.data(), pkt_size, freq_idx, channels_);
                    std::memcpy(aac_pkt->data.data() + 7, pkt_data, pkt_size);

                    if (output_callback_) output_callback_(aac_pkt);
                }
                pkt_unref(av_pkt_);
            }
        }
    }
}

} // namespace nvr

