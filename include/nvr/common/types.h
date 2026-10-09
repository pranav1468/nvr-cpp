#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <new>

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
    if (str == "H264") return CodecType::H264;
    if (str == "H265" || str == "HEVC") return CodecType::H265;
    if (str == "AAC") return CodecType::AAC;
    if (str == "PCMU") return CodecType::PCMU;
    if (str == "PCMA") return CodecType::PCMA;
    return CodecType::UNKNOWN;
}

enum class StreamType {
    MAIN = 0,
    SUB = 1
};

enum class NalType {
    UNKNOWN = 0,
    SLICE_NON_IDR,
    SLICE_IDR,
    SEI,
    SPS,
    PPS,
    VPS
};

enum class RecordMode {
    DISABLED = 0,
    CONTINUOUS = 1,
    MOTION_ONLY = 2,
    SCHEDULED = 3
};

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

struct MediaPacket {
    int channel_id{0};
    StreamType stream_type{StreamType::MAIN};
    MediaType media_type{MediaType::VIDEO};
    CodecType codec{CodecType::H264};
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
};

using MediaPacketPtr = std::shared_ptr<MediaPacket>;

enum class PixelFormat {
    NV12 = 0,
    YUV420P,
    RGB24,
    RGBA32,
    UNKNOWN
};

inline const char* PixelFormatToString(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::NV12:    return "NV12";
        case PixelFormat::YUV420P: return "YUV420P";
        case PixelFormat::RGB24:   return "RGB24";
        case PixelFormat::RGBA32:  return "RGBA32";
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
    CodecType codec{CodecType::H264};
    uint32_t width{1920};
    uint32_t height{1080};
    uint32_t fps{25};
    bool has_audio{false};
    CodecType audio_codec{CodecType::UNKNOWN};
    uint32_t audio_sample_rate{8000};
    uint8_t audio_channels{1};
};

struct StorageConfig {
    std::string recording_path{"./recordings"};
    std::string database_path{"./nvr_metadata.db"};
    int segment_duration_seconds{60};
    int min_free_space_mb{2048};
    int max_retention_days{30};
};

struct SystemConfig {
    std::string device_name{"NVR_Core"};
    std::string log_level{"info"};
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

struct NvrConfig {
    SystemConfig system;
    StorageConfig storage;
    LiveConfig live;
    AiConfig ai;
    std::vector<CameraConfig> cameras;
};

} // namespace nvr
