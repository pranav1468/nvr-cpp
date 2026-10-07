#pragma once

#include "nvr/common/types.h"
#include <string>
#include <vector>
#include <fstream>
#include <cstdint>
#include <memory>

namespace nvr {

class AtomicWriter {
public:
    AtomicWriter(int channel_id, const std::string& output_directory);
    ~AtomicWriter();

    bool StartSegment(int64_t start_wall_time_ms);

    bool WritePacket(const MediaPacketPtr& packet);

    bool FinalizeSegment(SegmentMetadata& out_meta);

    void AbortSegment();

    bool IsActive() const;
    int64_t GetSegmentStartTimeMs() const;
    uint32_t GetFrameCount() const;
    uint64_t GetCurrentBytesWritten() const;
    bool HasAudio() const { return has_audio_; }
    CodecType GetAudioCodec() const { return audio_codec_; }
    void ConfigureAudio(CodecType codec, uint32_t sample_rate = 8000, uint8_t channels = 1);

    static void CleanOrphanedTmpFiles(const std::string& directory);

private:
    struct SampleEntry {
        uint32_t size{0};
        uint32_t duration{0};
        bool is_keyframe{false};
        uint64_t pts{0};
        int32_t composition_time_offset{0};
    };

    void FlushCurrentFragment();

    void WriteFtyp();
    void WriteMoov();
    void WriteMoof(const std::vector<SampleEntry>& video_samples, uint64_t video_base_decode_time, uint32_t video_mdat_size,
                   const std::vector<SampleEntry>& audio_samples, uint64_t audio_base_decode_time, uint32_t audio_mdat_size);
    void WriteMdat(const std::vector<uint8_t>& video_payload, const std::vector<uint8_t>& audio_payload);

    void ParseSps(const uint8_t* data, size_t size);
    void ParseHevcSps(const uint8_t* data, size_t size);

    int channel_id_{0};
    std::string output_dir_;
    std::string tmp_file_path_;
    std::string final_file_path_;
    int fd_{-1};
    bool is_active_{false};

    int64_t start_time_ms_{0};
    int64_t segment_start_pts_us_{-1};
    int64_t last_pts_us_{0};
    uint64_t base_decode_time_{0};
    uint32_t frame_count_{0};
    uint32_t keyframe_count_{0};
    uint64_t total_bytes_written_{0};

    uint32_t fragment_sequence_{1};
    std::vector<SampleEntry> pending_samples_;
    std::vector<uint8_t> pending_mdat_bytes_;

    std::vector<uint8_t> sps_bytes_;
    std::vector<uint8_t> pps_bytes_;
    std::vector<uint8_t> vps_bytes_;
    bool has_header_written_{false};

    uint32_t width_{1920};
    uint32_t height_{1080};
    uint32_t timescale_{90000};
    CodecType codec_{CodecType::H264};

    // Audio track support
    bool has_audio_{false};
    CodecType audio_codec_{CodecType::UNKNOWN};
    uint32_t audio_sample_rate_{8000};
    uint8_t audio_channels_{1};
    bool audio_configured_{false};
    CodecType configured_audio_codec_{CodecType::UNKNOWN};
    uint32_t configured_audio_sample_rate_{8000};
    uint8_t configured_audio_channels_{1};
    uint64_t audio_base_decode_time_{0};
    int64_t last_audio_pts_us_{0};
    std::vector<SampleEntry> pending_audio_samples_;
    std::vector<uint8_t> pending_audio_mdat_bytes_;
};

} // namespace nvr
