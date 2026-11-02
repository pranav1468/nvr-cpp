#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <new>
#include <cctype>
#include <algorithm>

namespace nvr {

enum class CodecType {
    UNKNOWN = 0,
    H264,
    H265,
    AAC,
    PCMU,
    PCMA
};

inline const char* CodecToString(CodecType codec) {
    switch (codec) {
        case CodecType::H264: return "H264";
        case CodecType::H265: return "H265";
        case CodecType::AAC:  return "AAC";
        case CodecType::PCMU: return "PCMU";
        case CodecType::PCMA: return "PCMA";
        default:              return "UNKNOWN";
    }
}

inline CodecType StringToCodec(const std::string& str) {
    if (str.empty()) return CodecType::UNKNOWN;
    std::string upper;
    upper.reserve(str.size());
    for (char c : str) {
        if (c != '.' && c != '-' && c != '_' && c != ' ') {
            upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    if (upper == "H264" || upper == "AVC" || upper == "AVC1" || upper == "AVC3") return CodecType::H264;
    if (upper == "H265" || upper == "HEVC" || upper == "HVC1" || upper == "HEV1") return CodecType::H265;
    if (upper == "AAC" || upper == "MP4A") return CodecType::AAC;
    if (upper == "PCMU" || upper == "ULAW" || upper == "G711U") return CodecType::PCMU;
    if (upper == "PCMA" || upper == "ALAW" || upper == "G711A") return CodecType::PCMA;
    return CodecType::UNKNOWN;
}

enum class StreamType {
    MAIN = 0,
    SUB = 1
};

enum class RecordMode {
    DISABLED = 0,
    CONTINUOUS = 1,
    MOTION_ONLY = 2,
    SCHEDULED = 3,
    EVENT_ONLY = 4
};

inline const char* RecordModeToString(RecordMode mode) {
    switch (mode) {
        case RecordMode::DISABLED:    return "DISABLED";
        case RecordMode::CONTINUOUS:  return "CONTINUOUS";
        case RecordMode::MOTION_ONLY: return "MOTION_ONLY";
        case RecordMode::SCHEDULED:   return "SCHEDULED";
        case RecordMode::EVENT_ONLY:  return "EVENT_ONLY";
        default:                      return "UNKNOWN";
    }
}

enum class SessionState {
    DISCONNECTED = 0,
    CONNECTING,
    OPTIONS_SENT,
    DESCRIBE_SENT,
    SETUP_SENT,
    PLAYING,
    STREAMING,
    ERROR_BACKOFF
};

enum class MediaType {
    VIDEO = 0,
    AUDIO = 1
};

inline bool IsValidH265NalHeader(uint8_t b0, uint8_t b1) {
    return ((b0 & 0x80) == 0) &&
           ((b0 & 0x01) == 0) &&
           (((b1 >> 3) & 0x1F) == 0) &&
           ((b1 & 0x07) != 0);
}

inline CodecType DetectNalCodecFromParamSets(const uint8_t* data, size_t size) {
    if (!data || size < 4) return CodecType::UNKNOWN;
    size_t i = 0;
    while (i + 3 < size) {
        if (data[i] == 0x00 && data[i + 1] == 0x00 &&
            (data[i + 2] == 0x01 || (data[i + 2] == 0x00 && i + 3 < size && data[i + 3] == 0x01))) {
            size_t nal_pos = i + ((data[i + 2] == 0x01) ? 3 : 4);
            if (nal_pos >= size) break;
            uint8_t b0 = data[nal_pos];
            if ((b0 & 0x80) == 0) {
                if (nal_pos + 1 < size) {
                    uint8_t b1 = data[nal_pos + 1];
                    uint8_t nt265 = (b0 >> 1) & 0x3F;
                    if (IsValidH265NalHeader(b0, b1) && (nt265 >= 32 && nt265 <= 34)) {
                        return CodecType::H265;
                    }
                }
                uint8_t nt264 = b0 & 0x1F;
                uint8_t nal_ref_idc = (b0 >> 5) & 0x03;
                if (nt264 == 7 && nal_ref_idc != 0) {
                    return CodecType::H264;
                }
            }
            i = nal_pos;
        } else {
            ++i;
        }
    }
    return CodecType::UNKNOWN;
}

struct MediaPacket {
    int channel_id{0};
    StreamType stream_type{StreamType::MAIN};
    MediaType media_type{MediaType::VIDEO};
    CodecType codec{CodecType::UNKNOWN};
    bool is_keyframe{false};
    uint32_t rtp_timestamp{0};
    int64_t pts_us{0};
    int64_t dts_us{0};
    int64_t wall_time_ms{0};
    uint16_t sequence_number{0};
    uint32_t sample_rate{0};
    uint8_t channels{0};
    std::vector<uint8_t> data;

    bool IsAudio() const {
        return media_type == MediaType::AUDIO ||
               codec == CodecType::AAC ||
               codec == CodecType::PCMU ||
               codec == CodecType::PCMA;
    }
    bool IsVideo() const {
        return !IsAudio();
    }

    bool HasParameterSets() const {
        if (IsAudio() || data.size() < 4) return false;
        CodecType eff = (codec != CodecType::UNKNOWN) ? codec : DetectNalCodecFromParamSets(data.data(), data.size());
        size_t i = 0;
        while (i + 3 < data.size()) {
            if (data[i] == 0x00 && data[i + 1] == 0x00 &&
                (data[i + 2] == 0x01 || (data[i + 2] == 0x00 && i + 3 < data.size() && data[i + 3] == 0x01))) {
                size_t nal_pos = i + ((data[i + 2] == 0x01) ? 3 : 4);
                if (nal_pos >= data.size()) break;
                uint8_t b0 = data[nal_pos];
                if ((b0 & 0x80) == 0) {
                    if (eff == CodecType::H265) {
                        if (nal_pos + 1 < data.size()) {
                            uint8_t b1 = data[nal_pos + 1];
                            uint8_t nt = (b0 >> 1) & 0x3F;
                            if (IsValidH265NalHeader(b0, b1) && (nt == 32 || nt == 33 || nt == 34)) return true;
                        }
                    } else if (eff == CodecType::H264) {
                        uint8_t nt = b0 & 0x1F;
                        uint8_t nal_ref_idc = (b0 >> 5) & 0x03;
                        if ((nt == 7 || nt == 8) && nal_ref_idc != 0) return true;
                    }
                }
                i = nal_pos;
            } else {
                ++i;
            }
        }
        return false;
    }
    bool HasVclSlice() const {
        if (IsAudio() || data.size() < 4) return false;
        CodecType eff = (codec != CodecType::UNKNOWN) ? codec : DetectNalCodecFromParamSets(data.data(), data.size());
        size_t i = 0;
        while (i + 3 < data.size()) {
            if (data[i] == 0x00 && data[i + 1] == 0x00 &&
                (data[i + 2] == 0x01 || (data[i + 2] == 0x00 && i + 3 < data.size() && data[i + 3] == 0x01))) {
                size_t nal_pos = i + ((data[i + 2] == 0x01) ? 3 : 4);
                if (nal_pos >= data.size()) break;
                uint8_t b0 = data[nal_pos];
                if ((b0 & 0x80) == 0) {
                    if (eff == CodecType::H265) {
                        if (nal_pos + 1 < data.size()) {
                            uint8_t b1 = data[nal_pos + 1];
                            uint8_t nt = (b0 >> 1) & 0x3F;
                            if (IsValidH265NalHeader(b0, b1) && nt <= 31) return true;
                        }
                    } else if (eff == CodecType::H264) {
                        uint8_t nt = b0 & 0x1F;
                        if (nt >= 1 && nt <= 5) return true;
                    } else {
                        uint8_t nt264 = b0 & 0x1F;
                        if (nt264 >= 1 && nt264 <= 5) return true;
                        if (nal_pos + 1 < data.size()) {
                            uint8_t b1 = data[nal_pos + 1];
                            uint8_t nt265 = (b0 >> 1) & 0x3F;
                            if (IsValidH265NalHeader(b0, b1) && nt265 <= 31) return true;
                        }
                    }
                }
                i = nal_pos;
            } else {
                ++i;
            }
        }
        return false;
    }
};

using MediaPacketPtr = std::shared_ptr<MediaPacket>;

enum class PixelFormat {
    NV12 = 0,
    YUV420P,
    RGB24,
    RGBA32,
    RGB16,
    UNKNOWN
};

inline const char* PixelFormatToString(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::NV12:    return "NV12";
        case PixelFormat::YUV420P: return "YUV420P";
        case PixelFormat::RGB24:   return "RGB24";
        case PixelFormat::RGBA32:  return "RGBA32";
        case PixelFormat::RGB16:   return "RGB16";
        default:                   return "UNKNOWN";
    }
}

enum class LiveGridLayout {
    CUSTOM = 0,
    SINGLE = 1,
    GRID_4 = 4,
    GRID_6 = 6,
    GRID_8 = 8,
    FULLSCREEN = 100
};

class AlignedByteBuffer {
public:
    static constexpr size_t kAlignment = 64;

    AlignedByteBuffer() noexcept = default;

    explicit AlignedByteBuffer(size_t size, uint8_t init_val = 0) {
        resize(size, init_val);
    }

    ~AlignedByteBuffer() {
        reset();
    }

    AlignedByteBuffer(const AlignedByteBuffer& other) {
        if (other.size_ > 0) {
            allocate(other.size_);
            std::memcpy(data_, other.data_, other.size_);
            size_ = other.size_;
        }
    }

    AlignedByteBuffer& operator=(const AlignedByteBuffer& other) {
        if (this != &other) {
            if (other.size_ == 0) {
                size_ = 0;
            } else {
                if (capacity_ < other.size_) {
                    reset();
                    allocate(other.size_);
                }
                std::memcpy(data_, other.data_, other.size_);
                size_ = other.size_;
            }
        }
        return *this;
    }

    AlignedByteBuffer(AlignedByteBuffer&& other) noexcept
        : data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    AlignedByteBuffer& operator=(AlignedByteBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    uint8_t* data() noexcept { return data_; }
    const uint8_t* data() const noexcept { return data_; }

    size_t size() const noexcept { return size_; }
    size_t capacity() const noexcept { return capacity_; }
    bool empty() const noexcept { return size_ == 0; }

    uint8_t& operator[](size_t idx) noexcept { return data_[idx]; }
    const uint8_t& operator[](size_t idx) const noexcept { return data_[idx]; }

    uint8_t* begin() noexcept { return data_; }
    const uint8_t* begin() const noexcept { return data_; }
    uint8_t* end() noexcept { return data_ ? (data_ + size_) : nullptr; }
    const uint8_t* end() const noexcept { return data_ ? (data_ + size_) : nullptr; }

    void clear() noexcept { size_ = 0; }

    void reserve(size_t new_cap) {
        if (new_cap <= capacity_) return;
        size_t aligned_cap = (new_cap + (kAlignment - 1)) & ~(kAlignment - 1);
        void* ptr = nullptr;
        if (posix_memalign(&ptr, kAlignment, aligned_cap) != 0) {
            throw std::bad_alloc();
        }
        uint8_t* new_data = static_cast<uint8_t*>(ptr);
        if (data_ && size_ > 0) {
            std::memcpy(new_data, data_, size_);
        }
        free(data_);
        data_ = new_data;
        capacity_ = aligned_cap;
    }

    void resize(size_t new_size, uint8_t val = 0) {
        if (new_size > capacity_) {
            reserve(new_size);
        }
        if (new_size > size_ && data_) {
            std::memset(data_ + size_, val, new_size - size_);
        }
        size_ = new_size;
    }

    void assign(size_t n, uint8_t val) {
        resize(n);
        if (data_ && n > 0) {
            std::memset(data_, val, n);
        }
    }

    void reset() noexcept {
        if (data_) {
            free(data_);
            data_ = nullptr;
        }
        size_ = 0;
        capacity_ = 0;
    }

private:
    void allocate(size_t size) {
        size_t aligned_cap = (size + (kAlignment - 1)) & ~(kAlignment - 1);
        void* ptr = nullptr;
        if (posix_memalign(&ptr, kAlignment, aligned_cap) != 0) {
            throw std::bad_alloc();
        }
        data_ = static_cast<uint8_t*>(ptr);
        capacity_ = aligned_cap;
    }

    uint8_t* data_{nullptr};
    size_t size_{0};
    size_t capacity_{0};
};

struct DecodedFrame {
    int channel_id{0};
    StreamType stream_type{StreamType::SUB};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    PixelFormat format{PixelFormat::NV12};
    int64_t pts_us{0};
    int64_t wall_time_ms{0};
    uint64_t frame_index{0};
    AlignedByteBuffer data;
    int dmabuf_fd{-1};

    size_t GetSizeBytes() const {
        return data.size();
    }
};

using DecodedFramePtr = std::shared_ptr<DecodedFrame>;

struct CameraConfig {
    int id{0};
    std::string name;
    std::string rtsp_url;
    std::string sub_rtsp_url;
    bool enabled{true};
    RecordMode record_mode{RecordMode::CONTINUOUS};
    int reconnect_delay_sec{3};
    std::string onvif_xaddr;
    std::string onvif_username;
    std::string onvif_password;
};

struct SegmentMetadata {
    int64_t id{0};
    int channel_id{0};
    std::string file_path;
    int64_t start_time_ms{0};
    int64_t end_time_ms{0};
    int64_t duration_ms{0};
    uint64_t file_size_bytes{0};
    uint32_t frame_count{0};
    uint32_t keyframe_count{0};
    bool is_locked{false};
    CodecType codec{CodecType::UNKNOWN};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t fps{25};
    bool has_audio{false};
    CodecType audio_codec{CodecType::UNKNOWN};
    uint32_t audio_sample_rate{8000};
    uint8_t audio_channels{1};
    std::string thumbnail_path;
};

struct StorageConfig {
    std::string recording_path{"./recordings"};
    int segment_duration_seconds{60};
    int min_free_space_mb{2048};
    int max_retention_days{30};
};

struct SystemConfig {
    std::string device_name{"NVR_Core"};
    std::string log_level{"info"};
    std::string log_dir{"logs"};
    int max_channels{8};
};

struct LiveConfig {
    int grid_layout{4};
    int max_queue_depth{2};
    bool drop_stale_frames{true};
};

struct AiConfig {
    bool enabled{false};
    int max_concurrent_models{2};
    double motion_threshold{0.05};
};

struct ApiConfig {
    bool enabled{true};
    int port{8080};
    std::string web_root{"./web"};
    std::string bind_address{"0.0.0.0"};
    bool enforce_https{false};
    bool enable_hsts{false};
    std::vector<std::string> cors_allowed_origins{"*"};
};

struct NvrConfig {
    SystemConfig system;
    StorageConfig storage;
    LiveConfig live;
    AiConfig ai;
    ApiConfig api;
    std::vector<CameraConfig> cameras;
};

} // namespace nvr
