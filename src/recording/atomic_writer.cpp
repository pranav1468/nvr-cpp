#include "nvr/recording/atomic_writer.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>
#include <cstring>
#include <algorithm>

namespace nvr {

namespace {

inline void PutU32(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

inline void PutU16(std::vector<uint8_t>& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

inline void PutU64(std::vector<uint8_t>& buf, uint64_t val) {
    PutU32(buf, static_cast<uint32_t>((val >> 32) & 0xFFFFFFFF));
    PutU32(buf, static_cast<uint32_t>(val & 0xFFFFFFFF));
}

inline void PutFourCC(std::vector<uint8_t>& buf, const char* fcc) {
    buf.push_back(static_cast<uint8_t>(fcc[0]));
    buf.push_back(static_cast<uint8_t>(fcc[1]));
    buf.push_back(static_cast<uint8_t>(fcc[2]));
    buf.push_back(static_cast<uint8_t>(fcc[3]));
}

inline void UpdateBoxSize(std::vector<uint8_t>& buf, size_t offset) {
    uint32_t size = static_cast<uint32_t>(buf.size() - offset);
    buf[offset]     = static_cast<uint8_t>((size >> 24) & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>((size >> 16) & 0xFF);
    buf[offset + 2] = static_cast<uint8_t>((size >> 8) & 0xFF);
    buf[offset + 3] = static_cast<uint8_t>(size & 0xFF);
}

inline bool WriteAll(int fd, const void* data, size_t size) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, ptr + written, size - written);
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

// Simple bit reader for SPS parsing
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint32_t ReadBits(size_t num_bits) {
        uint32_t res = 0;
        for (size_t i = 0; i < num_bits; ++i) {
            if (byte_offset_ >= size_) return res;
            uint8_t bit = (data_[byte_offset_] >> (7 - bit_offset_)) & 0x01;
            res = (res << 1) | bit;
            bit_offset_++;
            if (bit_offset_ == 8) {
                bit_offset_ = 0;
                byte_offset_++;
                // Skip emulation prevention bytes 0x00 0x00 0x03
                if (byte_offset_ + 2 < size_ && data_[byte_offset_] == 0x03 &&
                    data_[byte_offset_ - 1] == 0x00 && data_[byte_offset_ - 2] == 0x00) {
                    byte_offset_++;
                }
            }
        }
        return res;
    }

    uint32_t ReadExponentialGolomb() {
        size_t leading_zeros = 0;
        while (ReadBits(1) == 0 && leading_zeros < 32) {
            leading_zeros++;
        }
        if (leading_zeros == 0) return 0;
        uint32_t suffix = ReadBits(leading_zeros);
        return (1U << leading_zeros) - 1 + suffix;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t byte_offset_{0};
    size_t bit_offset_{0};
};

} // namespace

AtomicWriter::AtomicWriter(int channel_id, const std::string& output_directory)
    : channel_id_(channel_id), output_dir_(output_directory) {
    try {
        std::filesystem::create_directories(output_dir_);
    } catch (...) {}
}

AtomicWriter::~AtomicWriter() {
    if (is_active_) {
        AbortSegment();
    }
}

void AtomicWriter::CleanOrphanedTmpFiles(const std::string& directory) {
    try {
        if (!std::filesystem::exists(directory)) return;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file() && entry.path().extension() == ".tmp") {
                LOG_WARN << "Startup cleanup: Removing orphaned temporary segment: " << entry.path();
                std::filesystem::remove(entry.path());
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "Error cleaning orphaned tmp files: " << e.what();
    }
}

bool AtomicWriter::StartSegment(int64_t start_wall_time_ms) {
    if (is_active_) {
        AbortSegment();
    }

    start_time_ms_ = start_wall_time_ms;
    std::string date_str = time_utils::FormatDateOnly(start_wall_time_ms);
    std::filesystem::path target_dir = std::filesystem::path(output_dir_) / date_str;
    std::error_code ec;
    std::filesystem::create_directories(target_dir, ec);

    std::string timestamp_str = time_utils::FormatTimestampCompact(start_wall_time_ms);
    std::string base_name = "cam" + std::to_string(channel_id_) + "_" + timestamp_str;

    tmp_file_path_ = (target_dir / (base_name + ".tmp")).string();
    final_file_path_ = (target_dir / (base_name + ".mp4")).string();

    fd_ = open(tmp_file_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
        LOG_ERROR << "Failed to open temporary recording file: " << tmp_file_path_;
        return false;
    }

    is_active_ = true;
    frame_count_ = 0;
    keyframe_count_ = 0;
    total_bytes_written_ = 0;
    fragment_sequence_ = 1;
    base_decode_time_ = 0;
    last_pts_us_ = 0;
    has_header_written_ = false;
    pending_samples_.clear();
    pending_mdat_bytes_.clear();

    if (audio_configured_) {
        has_audio_ = true;
        audio_codec_ = configured_audio_codec_;
        audio_sample_rate_ = configured_audio_sample_rate_;
        audio_channels_ = configured_audio_channels_;
    } else {
        has_audio_ = false;
        audio_codec_ = CodecType::UNKNOWN;
        audio_sample_rate_ = 8000;
        audio_channels_ = 1;
    }
    audio_base_decode_time_ = 0;
    last_audio_pts_us_ = 0;
    pending_audio_samples_.clear();
    pending_audio_mdat_bytes_.clear();

    LOG_INFO << "[Channel " << channel_id_ << "] Started atomic segment: " << tmp_file_path_;
    return true;
}

void AtomicWriter::ConfigureAudio(CodecType codec, uint32_t sample_rate, uint8_t channels) {
    audio_configured_ = true;
    configured_audio_codec_ = codec;
    configured_audio_sample_rate_ = sample_rate;
    configured_audio_channels_ = channels;
    has_audio_ = true;
    audio_codec_ = codec;
    audio_sample_rate_ = sample_rate;
    audio_channels_ = channels;
}

void AtomicWriter::ParseSps(const uint8_t* data, size_t size) {
    if (size < 4) return;
    BitReader reader(data + 1, size - 1);
    uint32_t profile_idc = reader.ReadBits(8);
    reader.ReadBits(8); // constraint flags
    reader.ReadBits(8); // level idc
    reader.ReadExponentialGolomb(); // seq_parameter_set_id

    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
        profile_idc == 86 || profile_idc == 118 || profile_idc == 128) {
        uint32_t chroma_format_idc = reader.ReadExponentialGolomb();
        if (chroma_format_idc == 3) {
            reader.ReadBits(1); // separate_colour_plane_flag
        }
        reader.ReadExponentialGolomb(); // bit_depth_luma_minus8
        reader.ReadExponentialGolomb(); // bit_depth_chroma_minus8
        reader.ReadBits(1); // qpprime_y_zero_transform_bypass_flag
        uint32_t seq_scaling_matrix_present = reader.ReadBits(1);
        if (seq_scaling_matrix_present) {
            size_t count = (chroma_format_idc != 3) ? 8 : 12;
            for (size_t i = 0; i < count; ++i) {
                if (reader.ReadBits(1)) { // seq_scaling_list_present_flag
                    size_t last_scale = 8, next_scale = 8;
                    size_t size_list = (i < 6) ? 16 : 64;
                    for (size_t j = 0; j < size_list; ++j) {
                        if (next_scale != 0) {
                            int32_t delta = static_cast<int32_t>(reader.ReadExponentialGolomb());
                            next_scale = (last_scale + static_cast<size_t>(delta) + 256) % 256;
                        }
                        last_scale = (next_scale == 0) ? last_scale : next_scale;
                    }
                }
            }
        }
    }

    reader.ReadExponentialGolomb(); // log2_max_frame_num_minus4
    uint32_t pic_order_cnt_type = reader.ReadExponentialGolomb();
    if (pic_order_cnt_type == 0) {
        reader.ReadExponentialGolomb(); // log2_max_pic_order_cnt_lsb_minus4
    } else if (pic_order_cnt_type == 1) {
        reader.ReadBits(1); // delta_pic_order_always_zero_flag
        reader.ReadExponentialGolomb(); // offset_for_non_ref_pic
        reader.ReadExponentialGolomb(); // offset_for_top_to_bottom_field
        uint32_t num_ref_frames_in_pic_order_cnt_cycle = reader.ReadExponentialGolomb();
        for (uint32_t i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; ++i) {
            reader.ReadExponentialGolomb();
        }
    }

    reader.ReadExponentialGolomb(); // max_num_ref_frames
    reader.ReadBits(1); // gaps_in_frame_num_value_allowed_flag
    uint32_t pic_width_in_mbs_minus1 = reader.ReadExponentialGolomb();
    uint32_t pic_height_in_map_units_minus1 = reader.ReadExponentialGolomb();
    uint32_t frame_mbs_only_flag = reader.ReadBits(1);
    if (!frame_mbs_only_flag) {
        reader.ReadBits(1); // mb_adaptive_frame_field_flag
    }
    reader.ReadBits(1); // direct_8x8_inference_flag

    uint32_t frame_cropping_flag = reader.ReadBits(1);
    uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (frame_cropping_flag) {
        crop_left = reader.ReadExponentialGolomb();
        crop_right = reader.ReadExponentialGolomb();
        crop_top = reader.ReadExponentialGolomb();
        crop_bottom = reader.ReadExponentialGolomb();
    }

    width_ = (pic_width_in_mbs_minus1 + 1) * 16 - (crop_left + crop_right) * 2;
    height_ = (2 - frame_mbs_only_flag) * (pic_height_in_map_units_minus1 + 1) * 16 - (crop_top + crop_bottom) * 2;

    LOG_INFO << "[Channel " << channel_id_ << "] Parsed SPS video resolution: " 
             << width_ << "x" << height_;
}

void AtomicWriter::WriteFtyp() {
    std::vector<uint8_t> buf;
    PutU32(buf, 0); // placeholder size
    PutFourCC(buf, "ftyp");
    PutFourCC(buf, "isom"); // major brand
    PutU32(buf, 0x00000200); // minor version
    PutFourCC(buf, "isom");
    PutFourCC(buf, "iso2");
    PutFourCC(buf, "avc1");
    PutFourCC(buf, "mp41");
    UpdateBoxSize(buf, 0);

    WriteAll(fd_, buf.data(), buf.size());
    total_bytes_written_ += buf.size();
}

void AtomicWriter::WriteMoov() {
    std::vector<uint8_t> buf;
    size_t moov_start = buf.size();
    PutU32(buf, 0); // moov size
    PutFourCC(buf, "moov");

    // mvhd
    size_t mvhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "mvhd");
    PutU32(buf, 0); // version 0, flags 0
    PutU32(buf, 0); // creation_time
    PutU32(buf, 0); // modification_time
    PutU32(buf, timescale_); // timescale 90000
    PutU32(buf, 0); // duration (0 for fmp4)
    PutU32(buf, 0x00010000); // rate 1.0
    PutU16(buf, 0x0100); // volume 1.0
    PutU16(buf, 0); // reserved
    PutU32(buf, 0); PutU32(buf, 0); // reserved[2]
    // Matrix structure (unity matrix)
    PutU32(buf, 0x00010000); PutU32(buf, 0); PutU32(buf, 0);
    PutU32(buf, 0); PutU32(buf, 0x00010000); PutU32(buf, 0);
    PutU32(buf, 0); PutU32(buf, 0); PutU32(buf, 0x40000000);
    for (int i = 0; i < 6; ++i) PutU32(buf, 0); // pre_defined[6]
    PutU32(buf, has_audio_ ? 3 : 2); // next_track_ID
    UpdateBoxSize(buf, mvhd_start);

    // trak
    size_t trak_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "trak");

    // tkhd
    size_t tkhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "tkhd");
    PutU32(buf, 0x00000003); // version 0, flags: track_enabled | track_in_movie
    PutU32(buf, 0); PutU32(buf, 0); // creation / mod
    PutU32(buf, 1); // track_ID = 1
    PutU32(buf, 0); // reserved
    PutU32(buf, 0); // duration
    PutU32(buf, 0); PutU32(buf, 0); // reserved[2]
    PutU16(buf, 0); // layer
    PutU16(buf, 0); // alternate_group
    PutU16(buf, 0); // volume
    PutU16(buf, 0); // reserved
    // Unity matrix
    PutU32(buf, 0x00010000); PutU32(buf, 0); PutU32(buf, 0);
    PutU32(buf, 0); PutU32(buf, 0x00010000); PutU32(buf, 0);
    PutU32(buf, 0); PutU32(buf, 0); PutU32(buf, 0x40000000);
    PutU32(buf, width_ << 16);  // width (16.16 fixed point)
    PutU32(buf, height_ << 16); // height
    UpdateBoxSize(buf, tkhd_start);

    // mdia
    size_t mdia_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "mdia");

    // mdhd
    size_t mdhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "mdhd");
    PutU32(buf, 0);
    PutU32(buf, 0); PutU32(buf, 0);
    PutU32(buf, timescale_);
    PutU32(buf, 0);
    PutU16(buf, 0x55C4); // language 'und'
    PutU16(buf, 0);
    UpdateBoxSize(buf, mdhd_start);

    // hdlr
    size_t hdlr_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "hdlr");
    PutU32(buf, 0);
    PutU32(buf, 0);
    PutFourCC(buf, "vide");
    PutU32(buf, 0); PutU32(buf, 0); PutU32(buf, 0);
    const char* hdlr_name = "VideoHandler";
    for (size_t i = 0; i <= strlen(hdlr_name); ++i) buf.push_back(static_cast<uint8_t>(hdlr_name[i]));
    UpdateBoxSize(buf, hdlr_start);

    // minf
    size_t minf_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "minf");

    // vmhd
    size_t vmhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "vmhd");
    PutU32(buf, 0x00000001); // flags
    PutU16(buf, 0); PutU16(buf, 0); PutU16(buf, 0); PutU16(buf, 0);
    UpdateBoxSize(buf, vmhd_start);

    // dinf & dref
    size_t dinf_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "dinf");
    size_t dref_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "dref");
    PutU32(buf, 0);
    PutU32(buf, 1); // entry_count
    PutU32(buf, 12); // url box size
    PutFourCC(buf, "url ");
    PutU32(buf, 0x00000001); // flags: self-contained
    UpdateBoxSize(buf, dref_start);
    UpdateBoxSize(buf, dinf_start);

    // stbl
    size_t stbl_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "stbl");

    // stsd
    size_t stsd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "stsd");
    PutU32(buf, 0);
    PutU32(buf, 1); // entry_count

    // avc1 box
    size_t avc1_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, (codec_ == CodecType::H265) ? "hvc1" : "avc1");
    for (int i = 0; i < 6; ++i) buf.push_back(0); // reserved
    PutU16(buf, 1); // data_reference_index
    PutU16(buf, 0); PutU16(buf, 0); // pre_defined, reserved
    for (int i = 0; i < 3; ++i) PutU32(buf, 0); // pre_defined[3]
    PutU16(buf, static_cast<uint16_t>(width_));
    PutU16(buf, static_cast<uint16_t>(height_));
    PutU32(buf, 0x00480000); // 72 dpi
    PutU32(buf, 0x00480000);
    PutU32(buf, 0); // reserved
    PutU16(buf, 1); // frame_count
    for (int i = 0; i < 32; ++i) buf.push_back(0); // compressorname
    PutU16(buf, 0x0018); // depth 24
    PutU16(buf, 0xFFFF); // pre_defined -1

    // avcC or hvcC box
    if (codec_ == CodecType::H265) {
        size_t hvcc_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "hvcC");
        buf.push_back(1); // configurationVersion
        buf.push_back(0x01); // general_profile_space=0, tier_flag=0, profile_idc=1
        PutU32(buf, 0x60000000); // compatibility flags
        for (int k = 0; k < 6; ++k) buf.push_back(0); // constraint flags
        buf.push_back(93); // level_idc 3.1
        PutU16(buf, 0xF000); // min_spatial_segmentation_idc
        buf.push_back(0xFC); // parallelismType = 0
        buf.push_back(0xFD); // chromaFormat = 1 (4:2:0)
        buf.push_back(0xF8); // bitDepthLumaMinus8 = 0 (8-bit)
        buf.push_back(0xF8); // bitDepthChromaMinus8 = 0 (8-bit)
        PutU16(buf, 0); // avgFrameRate
        buf.push_back(0x0F); // constantFrameRate=0, lengthSizeMinusOne=3 (4 bytes)

        uint8_t num_arrays = 0;
        if (!vps_bytes_.empty()) num_arrays++;
        if (!sps_bytes_.empty()) num_arrays++;
        if (!pps_bytes_.empty()) num_arrays++;
        buf.push_back(num_arrays);

        auto write_nal_array = [&](uint8_t nal_type, const std::vector<uint8_t>& data) {
            if (data.empty()) return;
            buf.push_back(0x80 | (nal_type & 0x3F));
            PutU16(buf, 1);
            PutU16(buf, static_cast<uint16_t>(data.size()));
            buf.insert(buf.end(), data.begin(), data.end());
        };

        write_nal_array(32, vps_bytes_);
        write_nal_array(33, sps_bytes_);
        write_nal_array(34, pps_bytes_);

        UpdateBoxSize(buf, hvcc_start);
    } else {
        if (!sps_bytes_.empty() && !pps_bytes_.empty()) {
            size_t avcc_start = buf.size();
            PutU32(buf, 0);
            PutFourCC(buf, "avcC");
            buf.push_back(1); // configurationVersion
            buf.push_back(sps_bytes_[1]); // AVCProfileIndication
            buf.push_back(sps_bytes_[2]); // profile_compatibility
            buf.push_back(sps_bytes_[3]); // AVCLevelIndication
            buf.push_back(0xFF); // lengthSizeMinusOne = 3 (4 bytes)
            buf.push_back(0xE1); // numOfSequenceParameterSets = 1
            PutU16(buf, static_cast<uint16_t>(sps_bytes_.size()));
            buf.insert(buf.end(), sps_bytes_.begin(), sps_bytes_.end());
            buf.push_back(1); // numOfPictureParameterSets = 1
            PutU16(buf, static_cast<uint16_t>(pps_bytes_.size()));
            buf.insert(buf.end(), pps_bytes_.begin(), pps_bytes_.end());
            UpdateBoxSize(buf, avcc_start);
        }
    }
    UpdateBoxSize(buf, avc1_start);
    UpdateBoxSize(buf, stsd_start);

    // Dummy empty stts, stsc, stsz, stco
    size_t empty_stts = buf.size();
    PutU32(buf, 0); PutFourCC(buf, "stts"); PutU32(buf, 0); PutU32(buf, 0);
    UpdateBoxSize(buf, empty_stts);

    size_t empty_stsc = buf.size();
    PutU32(buf, 0); PutFourCC(buf, "stsc"); PutU32(buf, 0); PutU32(buf, 0);
    UpdateBoxSize(buf, empty_stsc);

    size_t empty_stsz = buf.size();
    PutU32(buf, 0); PutFourCC(buf, "stsz"); PutU32(buf, 0); PutU32(buf, 0); PutU32(buf, 0);
    UpdateBoxSize(buf, empty_stsz);

    size_t empty_stco = buf.size();
    PutU32(buf, 0); PutFourCC(buf, "stco"); PutU32(buf, 0); PutU32(buf, 0);
    UpdateBoxSize(buf, empty_stco);

    UpdateBoxSize(buf, stbl_start);
    UpdateBoxSize(buf, minf_start);
    UpdateBoxSize(buf, mdia_start);
    UpdateBoxSize(buf, trak_start);

    // Track 2 (audio) trak
    if (has_audio_) {
        size_t audio_trak_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "trak");

        // tkhd
        size_t audio_tkhd_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "tkhd");
        PutU32(buf, 0x00000003); // enabled | in_movie
        PutU32(buf, 0); PutU32(buf, 0); // creation / mod
        PutU32(buf, 2); // track_ID = 2
        PutU32(buf, 0); // reserved
        PutU32(buf, 0); // duration = 0
        PutU32(buf, 0); PutU32(buf, 0); // reserved[2]
        PutU16(buf, 0); // layer
        PutU16(buf, 0); // alternate_group
        PutU16(buf, 0x0100); // volume = 1.0 (audio)
        PutU16(buf, 0); // reserved
        // Unity matrix
        PutU32(buf, 0x00010000); PutU32(buf, 0); PutU32(buf, 0);
        PutU32(buf, 0); PutU32(buf, 0x00010000); PutU32(buf, 0);
        PutU32(buf, 0); PutU32(buf, 0); PutU32(buf, 0x40000000);
        PutU32(buf, 0); // width = 0
        PutU32(buf, 0); // height = 0
        UpdateBoxSize(buf, audio_tkhd_start);

        // mdia
        size_t audio_mdia_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "mdia");

        // mdhd
        size_t audio_mdhd_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "mdhd");
        PutU32(buf, 0);
        PutU32(buf, 0); PutU32(buf, 0);
        PutU32(buf, audio_sample_rate_); // timescale
        PutU32(buf, 0); // duration = 0
        PutU16(buf, 0x55C4); // language 'und'
        PutU16(buf, 0);
        UpdateBoxSize(buf, audio_mdhd_start);

        // hdlr
        size_t audio_hdlr_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "hdlr");
        PutU32(buf, 0);
        PutU32(buf, 0);
        PutFourCC(buf, "soun");
        PutU32(buf, 0); PutU32(buf, 0); PutU32(buf, 0);
        const char* audio_hdlr_name = "SoundHandler";
        for (size_t i = 0; i <= strlen(audio_hdlr_name); ++i) buf.push_back(static_cast<uint8_t>(audio_hdlr_name[i]));
        UpdateBoxSize(buf, audio_hdlr_start);

        // minf
        size_t audio_minf_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "minf");

        // smhd
        size_t smhd_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "smhd");
        PutU32(buf, 0); // version 0, flags 0
        PutU16(buf, 0); // balance = 0
        PutU16(buf, 0); // reserved
        UpdateBoxSize(buf, smhd_start);

        // dinf & dref
        size_t audio_dinf_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "dinf");
        size_t audio_dref_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "dref");
        PutU32(buf, 0);
        PutU32(buf, 1);
        PutU32(buf, 12);
        PutFourCC(buf, "url ");
        PutU32(buf, 0x00000001);
        UpdateBoxSize(buf, audio_dref_start);
        UpdateBoxSize(buf, audio_dinf_start);

        // stbl
        size_t audio_stbl_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "stbl");

        // stsd
        size_t audio_stsd_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "stsd");
        PutU32(buf, 0);
        PutU32(buf, 1); // entry_count = 1

        size_t audio_entry_start = buf.size();
        PutU32(buf, 0);
        if (audio_codec_ == CodecType::PCMA) {
            PutFourCC(buf, "alaw");
        } else if (audio_codec_ == CodecType::PCMU) {
            PutFourCC(buf, "ulaw");
        } else {
            PutFourCC(buf, "mp4a");
        }
        for (int i = 0; i < 6; ++i) buf.push_back(0); // reserved
        PutU16(buf, 1); // data_reference_index = 1
        PutU16(buf, 0); // sound_version = 0
        PutU16(buf, 0); // reserved
        PutU32(buf, 0); // reserved2
        PutU16(buf, static_cast<uint16_t>(audio_channels_)); // channelcount
        PutU16(buf, 16); // samplesize (16-bit)
        PutU16(buf, 0); // pre_defined
        PutU16(buf, 0); // reserved3
        PutU32(buf, audio_sample_rate_ << 16); // 16.16 samplerate

        if (audio_codec_ == CodecType::AAC) {
            // Write esds box for AAC
            size_t esds_start = buf.size();
            PutU32(buf, 0);
            PutFourCC(buf, "esds");
            PutU32(buf, 0); // version 0, flags 0

            // Sampling frequency index (ISO/IEC 14496-3)
            uint8_t freq_idx = 4; // default 44100
            switch (audio_sample_rate_) {
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
                default:    freq_idx = 3; break;
            }
            uint8_t asc[2];
            asc[0] = static_cast<uint8_t>((2 << 3) | (freq_idx >> 1));
            asc[1] = static_cast<uint8_t>(((freq_idx & 1) << 7) | (audio_channels_ << 3));

            // ES_Descriptor tag 0x03
            buf.push_back(0x03);
            buf.push_back(25); // tag length
            PutU16(buf, 2); // ES_ID = 2
            buf.push_back(0); // priority = 0

            // DecoderConfigDescriptor tag 0x04
            buf.push_back(0x04);
            buf.push_back(17); // tag length
            buf.push_back(0x40); // objectTypeIndication = Audio ISO/IEC 14496-3 AAC
            buf.push_back(0x15); // streamType = Audio (5<<2 | 1)
            buf.push_back(0x00); buf.push_back(0x00); buf.push_back(0x00); // bufferSizeDB = 0
            PutU32(buf, 128000); // maxBitrate
            PutU32(buf, 128000); // avgBitrate

            // DecoderSpecificInfo tag 0x05
            buf.push_back(0x05);
            buf.push_back(2); // length = 2 bytes
            buf.push_back(asc[0]);
            buf.push_back(asc[1]);

            // SLConfigDescriptor tag 0x06
            buf.push_back(0x06);
            buf.push_back(1); // length = 1
            buf.push_back(0x02); // predefined = 2

            UpdateBoxSize(buf, esds_start);
        }

        UpdateBoxSize(buf, audio_entry_start);
        UpdateBoxSize(buf, audio_stsd_start);

        // Dummy empty stts, stsc, stsz, stco for audio
        size_t audio_empty_stts = buf.size();
        PutU32(buf, 0); PutFourCC(buf, "stts"); PutU32(buf, 0); PutU32(buf, 0);
        UpdateBoxSize(buf, audio_empty_stts);

        size_t audio_empty_stsc = buf.size();
        PutU32(buf, 0); PutFourCC(buf, "stsc"); PutU32(buf, 0); PutU32(buf, 0);
        UpdateBoxSize(buf, audio_empty_stsc);

        size_t audio_empty_stsz = buf.size();
        PutU32(buf, 0); PutFourCC(buf, "stsz"); PutU32(buf, 0); PutU32(buf, 0); PutU32(buf, 0);
        UpdateBoxSize(buf, audio_empty_stsz);

        size_t audio_empty_stco = buf.size();
        PutU32(buf, 0); PutFourCC(buf, "stco"); PutU32(buf, 0); PutU32(buf, 0);
        UpdateBoxSize(buf, audio_empty_stco);

        UpdateBoxSize(buf, audio_stbl_start);
        UpdateBoxSize(buf, audio_minf_start);
        UpdateBoxSize(buf, audio_mdia_start);
        UpdateBoxSize(buf, audio_trak_start);
    }

    // mvex box
    size_t mvex_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "mvex");

    size_t trex_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "trex");
    PutU32(buf, 0);
    PutU32(buf, 1); // track_ID = 1
    PutU32(buf, 1); // default_sample_description_index = 1
    PutU32(buf, 3600); // default_sample_duration (90000/25 = 3600)
    PutU32(buf, 0); // default_sample_size
    PutU32(buf, 0x00010000); // default_sample_flags
    UpdateBoxSize(buf, trex_start);

    if (has_audio_) {
        size_t audio_trex_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "trex");
        PutU32(buf, 0);
        PutU32(buf, 2); // track_ID = 2
        PutU32(buf, 1); // default_sample_description_index = 1
        PutU32(buf, (audio_codec_ == CodecType::AAC) ? 1024 : 160);
        PutU32(buf, 0);
        PutU32(buf, 0x02000000); // sync sample flags
        UpdateBoxSize(buf, audio_trex_start);
    }

    UpdateBoxSize(buf, mvex_start);

    UpdateBoxSize(buf, moov_start);

    WriteAll(fd_, buf.data(), buf.size());
    total_bytes_written_ += buf.size();
    has_header_written_ = true;
}

void AtomicWriter::WriteMoof(const std::vector<SampleEntry>& video_samples, uint64_t video_base_decode_time, uint32_t video_mdat_size,
                             const std::vector<SampleEntry>& audio_samples, uint64_t audio_base_decode_time, uint32_t audio_mdat_size) {
    (void)audio_mdat_size;
    std::vector<uint8_t> buf;
    size_t moof_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "moof");

    // mfhd
    size_t mfhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "mfhd");
    PutU32(buf, 0);
    PutU32(buf, fragment_sequence_++);
    UpdateBoxSize(buf, mfhd_start);

    // Track 1 (video) traf
    size_t video_traf_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "traf");

    // tfhd
    size_t video_tfhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "tfhd");
    PutU32(buf, 0x020000); // default-base-is-moof
    PutU32(buf, 1); // track_ID = 1
    UpdateBoxSize(buf, video_tfhd_start);

    // tfdt
    size_t video_tfdt_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "tfdt");
    PutU32(buf, 0x01000000); // version 1
    PutU64(buf, video_base_decode_time);
    UpdateBoxSize(buf, video_tfdt_start);

    // trun
    size_t video_trun_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "trun");

    bool has_cto = false;
    for (const auto& s : video_samples) {
        if (s.composition_time_offset != 0) {
            has_cto = true;
            break;
        }
    }

    uint32_t trun_flags = has_cto ? 0x000F01 : 0x000701;
    PutU32(buf, trun_flags);
    PutU32(buf, static_cast<uint32_t>(video_samples.size()));
    size_t video_data_offset_pos = buf.size();
    PutU32(buf, 0); // placeholder data_offset

    for (const auto& s : video_samples) {
        PutU32(buf, s.duration);
        PutU32(buf, s.size);
        uint32_t sample_flags = s.is_keyframe ? 0x02000000 : 0x01010000;
        PutU32(buf, sample_flags);
        if (has_cto) {
            PutU32(buf, static_cast<uint32_t>(s.composition_time_offset));
        }
    }
    UpdateBoxSize(buf, video_trun_start);
    UpdateBoxSize(buf, video_traf_start);

    // Track 2 (audio) traf
    size_t audio_data_offset_pos = 0;
    bool write_audio_traf = has_audio_ && !audio_samples.empty();
    if (write_audio_traf) {
        size_t audio_traf_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "traf");

        // tfhd
        size_t audio_tfhd_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "tfhd");
        PutU32(buf, 0x020000); // default-base-is-moof
        PutU32(buf, 2); // track_ID = 2
        UpdateBoxSize(buf, audio_tfhd_start);

        // tfdt
        size_t audio_tfdt_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "tfdt");
        PutU32(buf, 0x01000000); // version 1
        PutU64(buf, audio_base_decode_time);
        UpdateBoxSize(buf, audio_tfdt_start);

        // trun
        size_t audio_trun_start = buf.size();
        PutU32(buf, 0);
        PutFourCC(buf, "trun");
        PutU32(buf, 0x000701); // data-offset | duration | size | flags
        PutU32(buf, static_cast<uint32_t>(audio_samples.size()));
        audio_data_offset_pos = buf.size();
        PutU32(buf, 0); // placeholder audio data_offset

        for (const auto& s : audio_samples) {
            PutU32(buf, s.duration);
            PutU32(buf, s.size);
            PutU32(buf, 0x02000000); // sync sample flags
        }
        UpdateBoxSize(buf, audio_trun_start);
        UpdateBoxSize(buf, audio_traf_start);
    }

    UpdateBoxSize(buf, moof_start);

    uint32_t moof_size = static_cast<uint32_t>(buf.size() - moof_start);
    uint32_t video_data_offset = moof_size + 8; // mdat header is 8 bytes
    buf[video_data_offset_pos]     = static_cast<uint8_t>((video_data_offset >> 24) & 0xFF);
    buf[video_data_offset_pos + 1] = static_cast<uint8_t>((video_data_offset >> 16) & 0xFF);
    buf[video_data_offset_pos + 2] = static_cast<uint8_t>((video_data_offset >> 8) & 0xFF);
    buf[video_data_offset_pos + 3] = static_cast<uint8_t>(video_data_offset & 0xFF);

    if (write_audio_traf) {
        uint32_t audio_data_offset = moof_size + 8 + video_mdat_size;
        buf[audio_data_offset_pos]     = static_cast<uint8_t>((audio_data_offset >> 24) & 0xFF);
        buf[audio_data_offset_pos + 1] = static_cast<uint8_t>((audio_data_offset >> 16) & 0xFF);
        buf[audio_data_offset_pos + 2] = static_cast<uint8_t>((audio_data_offset >> 8) & 0xFF);
        buf[audio_data_offset_pos + 3] = static_cast<uint8_t>(audio_data_offset & 0xFF);
    }

    WriteAll(fd_, buf.data(), buf.size());
    total_bytes_written_ += buf.size();
}

void AtomicWriter::WriteMdat(const std::vector<uint8_t>& video_payload, const std::vector<uint8_t>& audio_payload) {
    uint32_t total_payload = static_cast<uint32_t>(video_payload.size() + (has_audio_ ? audio_payload.size() : 0));
    std::vector<uint8_t> mdat_hdr;
    PutU32(mdat_hdr, total_payload + 8);
    PutFourCC(mdat_hdr, "mdat");
    WriteAll(fd_, mdat_hdr.data(), mdat_hdr.size());
    total_bytes_written_ += mdat_hdr.size();

    if (!video_payload.empty()) {
        WriteAll(fd_, video_payload.data(), video_payload.size());
        total_bytes_written_ += video_payload.size();
    }
    if (has_audio_ && !audio_payload.empty()) {
        WriteAll(fd_, audio_payload.data(), audio_payload.size());
        total_bytes_written_ += audio_payload.size();
    }
}

void AtomicWriter::FlushCurrentFragment() {
    if (pending_samples_.empty() && pending_audio_samples_.empty()) {
        return;
    }

    if (!has_header_written_) {
        WriteFtyp();
        WriteMoov();
    }

    WriteMoof(pending_samples_, base_decode_time_, static_cast<uint32_t>(pending_mdat_bytes_.size()),
              pending_audio_samples_, audio_base_decode_time_, static_cast<uint32_t>(pending_audio_mdat_bytes_.size()));

    WriteMdat(pending_mdat_bytes_, pending_audio_mdat_bytes_);

    for (const auto& s : pending_samples_) {
        base_decode_time_ += s.duration;
    }
    for (const auto& s : pending_audio_samples_) {
        audio_base_decode_time_ += s.duration;
    }

    pending_samples_.clear();
    pending_mdat_bytes_.clear();
    pending_audio_samples_.clear();
    pending_audio_mdat_bytes_.clear();
}

bool AtomicWriter::WritePacket(const MediaPacketPtr& packet) {
    if (!is_active_ || !packet || packet->data.empty()) {
        return false;
    }

    if (packet->IsAudio()) {
        if (!has_audio_) {
            has_audio_ = true;
            audio_codec_ = packet->codec;
            audio_sample_rate_ = (packet->codec == CodecType::AAC) ? 48000 : 8000;
            audio_channels_ = 1;
        }

        uint32_t audio_sample_dur = (audio_codec_ == CodecType::AAC) ? 1024 : 160;
        if (last_audio_pts_us_ > 0 && packet->pts_us > last_audio_pts_us_) {
            int64_t diff_us = packet->pts_us - last_audio_pts_us_;
            audio_sample_dur = static_cast<uint32_t>((diff_us * audio_sample_rate_) / 1000000LL);
            if (audio_sample_dur == 0) {
                audio_sample_dur = (audio_codec_ == CodecType::AAC) ? 1024 : 160;
            }
        }
        last_audio_pts_us_ = packet->pts_us;

        SampleEntry audio_entry;
        audio_entry.size = static_cast<uint32_t>(packet->data.size());
        audio_entry.duration = audio_sample_dur;
        audio_entry.is_keyframe = true;
        audio_entry.pts = static_cast<uint64_t>(packet->pts_us);
        audio_entry.composition_time_offset = 0;

        pending_audio_samples_.push_back(audio_entry);
        pending_audio_mdat_bytes_.insert(pending_audio_mdat_bytes_.end(), packet->data.begin(), packet->data.end());
        return true;
    }

    codec_ = packet->codec;
    const uint8_t* ptr = packet->data.data();
    size_t len = packet->data.size();

    // Scan for Annex-B start codes and parse parameter sets (SPS, PPS)
    std::vector<std::pair<const uint8_t*, size_t>> nals;
    size_t i = 0;
    while (i + 3 < len) {
        if (ptr[i] == 0 && ptr[i+1] == 0 && (ptr[i+2] == 1 || (ptr[i+2] == 0 && i + 3 < len && ptr[i+3] == 1))) {
            size_t start_code_len = (ptr[i+2] == 1) ? 3 : 4;
            size_t nal_start = i + start_code_len;
            size_t next = nal_start;
            while (next + 2 < len) {
                if (ptr[next] == 0 && ptr[next+1] == 0 && (ptr[next+2] == 1 || (next + 3 < len && ptr[next+2] == 0 && ptr[next+3] == 1))) {
                    break;
                }
                next++;
            }
            if (next + 2 >= len) {
                next = len;
            }
            if (next > nal_start) {
                nals.push_back({ptr + nal_start, next - nal_start});
            }
            i = next;
        } else {
            i++;
        }
    }

    // If no start codes found, treat whole buffer as single NAL
    if (nals.empty()) {
        nals.push_back({ptr, len});
    }

    uint32_t frame_sample_size = 0;
    std::vector<uint8_t> frame_bytes;

    for (const auto& [nal_data, nal_len] : nals) {
        if (nal_len == 0) continue;
        if (codec_ == CodecType::H264) {
            uint8_t nal_type = nal_data[0] & 0x1F;
            if (nal_type == 7) { // SPS
                sps_bytes_.assign(nal_data, nal_data + nal_len);
                ParseSps(nal_data, nal_len);
            } else if (nal_type == 8) { // PPS
                pps_bytes_.assign(nal_data, nal_data + nal_len);
            } else if (nal_type == 1 || nal_type == 5 || nal_type == 6) { // Slice or SEI
                PutU32(frame_bytes, static_cast<uint32_t>(nal_len));
                frame_bytes.insert(frame_bytes.end(), nal_data, nal_data + nal_len);
                frame_sample_size += static_cast<uint32_t>(4 + nal_len);
            }
        } else if (codec_ == CodecType::H265) {
            uint8_t nal_type = (nal_data[0] >> 1) & 0x3F;
            if (nal_type == 32) { // VPS
                vps_bytes_.assign(nal_data, nal_data + nal_len);
            } else if (nal_type == 33) { // SPS
                sps_bytes_.assign(nal_data, nal_data + nal_len);
            } else if (nal_type == 34) { // PPS
                pps_bytes_.assign(nal_data, nal_data + nal_len);
            } else if (nal_type <= 31 || nal_type == 39 || nal_type == 40) { // VCL Slices or SEI
                PutU32(frame_bytes, static_cast<uint32_t>(nal_len));
                frame_bytes.insert(frame_bytes.end(), nal_data, nal_data + nal_len);
                frame_sample_size += static_cast<uint32_t>(4 + nal_len);
            }
        }
    }

    if (frame_sample_size == 0) {
        return true; // Parameter-set only packet consumed
    }

    // Calculate duration in 90000Hz units
    uint32_t sample_dur = 3600; // default 25fps (90000 / 25 = 3600)
    if (last_pts_us_ > 0 && packet->pts_us > last_pts_us_) {
        int64_t diff_us = packet->pts_us - last_pts_us_;
        sample_dur = static_cast<uint32_t>((diff_us * 90) / 1000);
        if (sample_dur < 900 || sample_dur > 18000) {
            sample_dur = 3600; // bound sanity
        }
    }
    last_pts_us_ = packet->pts_us;

    SampleEntry sample;
    sample.size = frame_sample_size;
    sample.duration = sample_dur;
    sample.is_keyframe = packet->is_keyframe;
    sample.pts = static_cast<uint64_t>(packet->pts_us);
    if (packet->dts_us > 0 && packet->pts_us != packet->dts_us) {
        int64_t cto_us = packet->pts_us - packet->dts_us;
        sample.composition_time_offset = static_cast<int32_t>((cto_us * 90) / 1000);
    } else {
        sample.composition_time_offset = 0;
    }

    pending_samples_.push_back(sample);
    pending_mdat_bytes_.insert(pending_mdat_bytes_.end(), frame_bytes.begin(), frame_bytes.end());

    frame_count_++;
    if (packet->is_keyframe) {
        keyframe_count_++;
    }

    // Emit fragment at GOP boundaries or every ~1 second (30 frames)
    if ((packet->is_keyframe && pending_samples_.size() >= 25) || pending_samples_.size() >= 30) {
        FlushCurrentFragment();
    }

    return true;
}

bool AtomicWriter::FinalizeSegment(SegmentMetadata& out_meta) {
    if (!is_active_) {
        return false;
    }

    FlushCurrentFragment();

    if (fd_ >= 0) {
        // Two-phase commit: flush to disk surface
        fdatasync(fd_);
        close(fd_);
        fd_ = -1;
    }

    // Atomic filesystem commit: rename .tmp -> .mp4
    int res = rename(tmp_file_path_.c_str(), final_file_path_.c_str());
    if (res != 0) {
        LOG_ERROR << "Failed to atomically rename temporary file " << tmp_file_path_
                  << " to " << final_file_path_ << ": " << strerror(errno);
        is_active_ = false;
        return false;
    }

    int64_t end_time_ms = time_utils::WallTimeMs();
    int64_t duration_ms = end_time_ms - start_time_ms_;

    out_meta.channel_id = channel_id_;
    out_meta.file_path = final_file_path_;
    out_meta.start_time_ms = start_time_ms_;
    out_meta.end_time_ms = end_time_ms;
    out_meta.duration_ms = duration_ms;
    out_meta.file_size_bytes = total_bytes_written_;
    out_meta.frame_count = frame_count_;
    out_meta.keyframe_count = keyframe_count_;
    out_meta.is_locked = false;
    out_meta.codec = codec_;
    out_meta.width = width_;
    out_meta.height = height_;
    out_meta.fps = (duration_ms > 0) ? static_cast<uint32_t>((frame_count_ * 1000) / duration_ms) : 25;
    out_meta.has_audio = has_audio_;
    out_meta.audio_codec = audio_codec_;
    out_meta.audio_sample_rate = audio_sample_rate_;
    out_meta.audio_channels = audio_channels_;

    LOG_INFO << "[Channel " << channel_id_ << "] Successfully finalized atomic segment: "
             << final_file_path_ << " (" << (total_bytes_written_ / 1024) << " KB, "
             << frame_count_ << " frames, " << duration_ms << " ms"
             << (has_audio_ ? ", with audio" : ", video only") << ")";

    is_active_ = false;
    return true;
}

void AtomicWriter::AbortSegment() {
    if (!is_active_) return;
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    if (std::filesystem::exists(tmp_file_path_)) {
        std::filesystem::remove(tmp_file_path_);
    }
    is_active_ = false;
    LOG_WARN << "[Channel " << channel_id_ << "] Aborted and removed partial segment: " << tmp_file_path_;
}

bool AtomicWriter::IsActive() const {
    return is_active_;
}

int64_t AtomicWriter::GetSegmentStartTimeMs() const {
    return start_time_ms_;
}

uint32_t AtomicWriter::GetFrameCount() const {
    return frame_count_;
}

uint64_t AtomicWriter::GetCurrentBytesWritten() const {
    return total_bytes_written_ + pending_mdat_bytes_.size();
}

} // namespace nvr
