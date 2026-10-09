#include "nvr/recording/atomic_writer.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <map>

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

// Deep ISOBMFF Box Parsing Structures and Helpers
struct BoxHeader {
    uint64_t size{0};
    uint32_t header_size{8};
    char type[5]{0};
    uint64_t offset{0};
};

inline uint32_t ReadU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline uint64_t ReadU64BE(const uint8_t* p) {
    return (static_cast<uint64_t>(ReadU32BE(p)) << 32) |
           static_cast<uint64_t>(ReadU32BE(p + 4));
}

inline bool ReadBoxHeader(std::ifstream& file, uint64_t file_size, BoxHeader& box) {
    uint64_t pos = static_cast<uint64_t>(file.tellg());
    box.offset = pos;
    if (pos + 8 > file_size) return false;

    uint8_t hdr[8];
    if (!file.read(reinterpret_cast<char*>(hdr), 8)) return false;

    uint32_t s32 = ReadU32BE(hdr);
    std::memcpy(box.type, hdr + 4, 4);
    box.type[4] = '\0';
    box.header_size = 8;

    if (s32 == 1) {
        if (pos + 16 > file_size) return false;
        uint8_t large[8];
        if (!file.read(reinterpret_cast<char*>(large), 8)) return false;
        box.size = ReadU64BE(large);
        box.header_size = 16;
        if (box.size < 16) return false;
    } else if (s32 == 0) {
        box.size = file_size - pos;
    } else if (s32 < 8) {
        return false;
    } else {
        box.size = s32;
    }

    if (pos + box.size > file_size) {
        return false;
    }
    return true;
}

struct MemBox {
    char type[5]{0};
    size_t header_size{8};
    size_t payload_offset{8};
    size_t total_size{0};
    const uint8_t* payload{nullptr};
    size_t payload_size{0};
};

inline bool ParseMemBoxes(const uint8_t* data, size_t size, std::vector<MemBox>& out_boxes) {
    size_t offset = 0;
    while (offset + 8 <= size) {
        uint32_t s32 = ReadU32BE(data + offset);
        char type[5];
        std::memcpy(type, data + offset + 4, 4);
        type[4] = '\0';

        size_t total_box_size = s32;
        size_t hdr_size = 8;
        if (s32 == 1) {
            if (offset + 16 > size) return false;
            total_box_size = static_cast<size_t>(ReadU64BE(data + offset + 8));
            hdr_size = 16;
            if (total_box_size < 16) return false;
        } else if (s32 == 0) {
            total_box_size = size - offset;
        } else if (s32 < 8) {
            return false;
        }

        if (offset + total_box_size > size) return false;

        MemBox b;
        std::memcpy(b.type, type, 5);
        b.header_size = hdr_size;
        b.payload_offset = offset + hdr_size;
        b.total_size = total_box_size;
        b.payload = data + offset + hdr_size;
        b.payload_size = total_box_size - hdr_size;
        out_boxes.push_back(b);

        offset += total_box_size;
    }
    return (offset == size);
}

inline const MemBox* FindBox(const std::vector<MemBox>& boxes, const char* type) {
    for (const auto& b : boxes) {
        if (std::strcmp(b.type, type) == 0) return &b;
    }
    return nullptr;
}

// Simple bit reader for SPS parsing
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    void SkipBits(size_t num_bits) {
        for (size_t i = 0; i < num_bits; ++i) {
            if (byte_offset_ >= size_) return;
            bit_offset_++;
            if (bit_offset_ == 8) {
                bit_offset_ = 0;
                byte_offset_++;
                if (byte_offset_ >= 2 && byte_offset_ < size_ && data_[byte_offset_] == 0x03 &&
                    data_[byte_offset_ - 1] == 0x00 && data_[byte_offset_ - 2] == 0x00) {
                    byte_offset_++;
                }
            }
        }
    }

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
                if (byte_offset_ >= 2 && byte_offset_ < size_ && data_[byte_offset_] == 0x03 &&
                    data_[byte_offset_ - 1] == 0x00 && data_[byte_offset_ - 2] == 0x00) {
                    byte_offset_++;
                }
            }
        }
        return res;
    }

    uint32_t ReadExponentialGolomb() {
        size_t leading_zeros = 0;
        while (ReadBits(1) == 0 && leading_zeros < 31) {
            leading_zeros++;
        }
        if (leading_zeros == 0) return 0;
        if (leading_zeros >= 31) return 0;
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
    has_io_error_ = false;
    frame_count_ = 0;
    keyframe_count_ = 0;
    total_bytes_written_ = 0;
    fragment_sequence_ = 1;
    segment_start_pts_us_ = -1;
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

void AtomicWriter::ParseHevcSps(const uint8_t* data, size_t size) {
    if (size < 4) return;
    BitReader reader(data + 2, size - 2);

    reader.ReadBits(4); // sps_video_parameter_set_id
    uint32_t max_sub_layers_minus1 = reader.ReadBits(3);
    reader.ReadBits(1); // sps_temporal_id_nesting_flag

    // profile_tier_level
    reader.ReadBits(2);  // general_profile_space
    reader.ReadBits(1);  // general_tier_flag
    reader.ReadBits(5);  // general_profile_idc
    reader.ReadBits(32); // general_profile_compatibility_flags
    reader.SkipBits(48); // general_constraint_indicator_flags
    reader.ReadBits(8);  // general_level_idc

    std::vector<bool> sub_layer_profile_present(max_sub_layers_minus1);
    std::vector<bool> sub_layer_level_present(max_sub_layers_minus1);
    for (uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
        sub_layer_profile_present[i] = (reader.ReadBits(1) != 0);
        sub_layer_level_present[i] = (reader.ReadBits(1) != 0);
    }
    if (max_sub_layers_minus1 > 0) {
        for (uint32_t i = max_sub_layers_minus1; i < 8; ++i) {
            reader.SkipBits(2);
        }
    }
    for (uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
        if (sub_layer_profile_present[i]) {
            reader.SkipBits(88);
        }
        if (sub_layer_level_present[i]) {
            reader.SkipBits(8);
        }
    }

    reader.ReadExponentialGolomb(); // sps_seq_parameter_set_id
    uint32_t chroma_format_idc = reader.ReadExponentialGolomb();
    if (chroma_format_idc == 3) {
        reader.ReadBits(1); // separate_colour_plane_flag
    }

    uint32_t pic_width = reader.ReadExponentialGolomb();
    uint32_t pic_height = reader.ReadExponentialGolomb();

    uint32_t conformance_window_flag = reader.ReadBits(1);
    uint32_t conf_left = 0, conf_right = 0, conf_top = 0, conf_bottom = 0;
    if (conformance_window_flag) {
        conf_left = reader.ReadExponentialGolomb();
        conf_right = reader.ReadExponentialGolomb();
        conf_top = reader.ReadExponentialGolomb();
        conf_bottom = reader.ReadExponentialGolomb();
    }

    uint32_t sub_width_c = (chroma_format_idc == 1 || chroma_format_idc == 2) ? 2 : 1;
    uint32_t sub_height_c = (chroma_format_idc == 1) ? 2 : 1;

    if (pic_width > 0 && pic_height > 0) {
        width_ = pic_width - (conf_left + conf_right) * sub_width_c;
        height_ = pic_height - (conf_top + conf_bottom) * sub_height_c;
        LOG_INFO << "[Channel " << channel_id_ << "] Parsed HEVC SPS video resolution: " 
                 << width_ << "x" << height_;
    }
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

    if (!WriteAll(fd_, buf.data(), buf.size())) {
        has_io_error_ = true;
        LOG_ERROR << "[Channel " << channel_id_ << "] Failed to write ftyp box";
        return;
    }
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
        } else {
            size_t avcc_start = buf.size();
            PutU32(buf, 0);
            PutFourCC(buf, "avcC");
            buf.push_back(1);    // configurationVersion
            buf.push_back(0x64); // profile Main (100)
            buf.push_back(0x00);
            buf.push_back(0x28); // level 4.0
            buf.push_back(0xFF); // 4-byte NAL length
            buf.push_back(0xE0); // 0 SPS
            buf.push_back(0x00); // 0 PPS
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

    if (!WriteAll(fd_, buf.data(), buf.size())) {
        has_io_error_ = true;
        LOG_ERROR << "[Channel " << channel_id_ << "] Failed to write moov box";
        return;
    }
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

    if (!WriteAll(fd_, buf.data(), buf.size())) {
        has_io_error_ = true;
        LOG_ERROR << "[Channel " << channel_id_ << "] Failed to write moof box";
        return;
    }
    total_bytes_written_ += buf.size();
}

void AtomicWriter::WriteMdat(const std::vector<uint8_t>& video_payload, const std::vector<uint8_t>& audio_payload) {
    uint32_t total_payload = static_cast<uint32_t>(video_payload.size() + (has_audio_ ? audio_payload.size() : 0));
    std::vector<uint8_t> mdat_hdr;
    PutU32(mdat_hdr, total_payload + 8);
    PutFourCC(mdat_hdr, "mdat");
    if (!WriteAll(fd_, mdat_hdr.data(), mdat_hdr.size())) {
        has_io_error_ = true;
        LOG_ERROR << "[Channel " << channel_id_ << "] Failed to write mdat box header";
        return;
    }
    total_bytes_written_ += mdat_hdr.size();

    if (!video_payload.empty()) {
        if (!WriteAll(fd_, video_payload.data(), video_payload.size())) {
            has_io_error_ = true;
            LOG_ERROR << "[Channel " << channel_id_ << "] Failed to write video mdat payload";
            return;
        }
        total_bytes_written_ += video_payload.size();
    }
    if (has_audio_ && !audio_payload.empty()) {
        if (!WriteAll(fd_, audio_payload.data(), audio_payload.size())) {
            has_io_error_ = true;
            LOG_ERROR << "[Channel " << channel_id_ << "] Failed to write audio mdat payload";
            return;
        }
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

        if (segment_start_pts_us_ < 0) {
            segment_start_pts_us_ = packet->pts_us;
        }

        // Intrinsic nominal sample duration for this audio format:
        // AAC-LC access units are fixed at 1024 PCM samples; G.711 is 1 byte per sample (8kHz).
        uint32_t nominal_dur = (audio_codec_ == CodecType::AAC) ? 1024 : 
            (packet->data.empty() ? 160 : static_cast<uint32_t>(packet->data.size()));
        int64_t nominal_dur_us = (static_cast<int64_t>(nominal_dur) * 1000000LL) / audio_sample_rate_;
        if (nominal_dur_us <= 0) nominal_dur_us = 20000;

        // Gap detection: If a gap > 3x nominal duration occurs (network stall, packet drop, silence suppression),
        // flush any pending fragment first so the gap is placed between fragments on the container timeline.
        bool is_gap = (last_audio_pts_us_ > 0) && ((packet->pts_us - last_audio_pts_us_) > (3 * nominal_dur_us));
        if (is_gap) {
            if (!pending_audio_samples_.empty() || !pending_samples_.empty()) {
                FlushCurrentFragment();
            }
        }

        // Re-anchor audio_base_decode_time_ on the first sample of a segment or across gaps
        if (last_audio_pts_us_ == 0 || is_gap) {
            int64_t offset_us = packet->pts_us - segment_start_pts_us_;
            if (offset_us < 0) offset_us = 0;
            audio_base_decode_time_ = static_cast<uint64_t>((offset_us * static_cast<int64_t>(audio_sample_rate_)) / 1000000LL);
        }

        if (last_audio_pts_us_ == 0 || packet->pts_us > last_audio_pts_us_) {
            last_audio_pts_us_ = packet->pts_us;
        }

        SampleEntry audio_entry;
        audio_entry.size = static_cast<uint32_t>(packet->data.size());
        audio_entry.duration = nominal_dur;
        audio_entry.is_keyframe = true;
        audio_entry.pts = static_cast<uint64_t>(packet->pts_us);
        audio_entry.composition_time_offset = 0;

        pending_audio_samples_.push_back(audio_entry);
        pending_audio_mdat_bytes_.insert(pending_audio_mdat_bytes_.end(), packet->data.begin(), packet->data.end());
        if (pending_audio_samples_.size() >= 50) {
            FlushCurrentFragment();
        }
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
                ParseHevcSps(nal_data, nal_len);
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

    if (segment_start_pts_us_ < 0) {
        segment_start_pts_us_ = packet->pts_us;
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
    if ((packet->is_keyframe && pending_samples_.size() >= 25) || 
        pending_samples_.size() >= 30 || 
        pending_audio_samples_.size() >= 50) {
        FlushCurrentFragment();
    }

    return true;
}

bool AtomicWriter::ValidateMp4File(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR << "MP4 validation failed: cannot open file " << path;
        return false;
    }

    file.seekg(0, std::ios::end);
    std::streampos fsize_pos = file.tellg();
    if (fsize_pos < 32) {
        LOG_ERROR << "MP4 validation failed: file size " << fsize_pos << " bytes is smaller than minimum header";
        return false;
    }
    uint64_t file_size = static_cast<uint64_t>(fsize_pos);
    file.seekg(0, std::ios::beg);

    // 1. First box MUST be 'ftyp'
    BoxHeader ftyp_box;
    if (!ReadBoxHeader(file, file_size, ftyp_box) || std::strcmp(ftyp_box.type, "ftyp") != 0) {
        LOG_ERROR << "MP4 validation failed: missing ftyp box at head of file: " << path;
        return false;
    }
    if (ftyp_box.size < 16) {
        LOG_ERROR << "MP4 validation failed: malformed ftyp box size: " << ftyp_box.size;
        return false;
    }

    uint64_t current_pos = ftyp_box.offset + ftyp_box.size;
    bool found_ftyp = true;
    bool found_moov = false;
    bool has_mvex = false;
    bool found_video_trak = false;
    std::vector<uint32_t> declared_tracks;

    uint32_t expected_fragment_seq = 1;
    uint32_t fragment_count = 0;
    uint64_t total_sample_count = 0;
    std::map<uint32_t, uint64_t> last_decode_times;

    while (current_pos < file_size) {
        file.seekg(current_pos);
        BoxHeader box;
        if (!ReadBoxHeader(file, file_size, box)) {
            LOG_ERROR << "MP4 validation failed: invalid box header at offset " << current_pos << " in " << path;
            return false;
        }

        if (std::strcmp(box.type, "moov") == 0) {
            if (found_moov) {
                LOG_ERROR << "MP4 validation failed: multiple moov boxes found in " << path;
                return false;
            }
            if (box.size < 16 || box.size > 10 * 1024 * 1024) {
                LOG_ERROR << "MP4 validation failed: invalid moov box size " << box.size << " in " << path;
                return false;
            }

            std::vector<uint8_t> moov_payload(box.size - box.header_size);
            if (!file.read(reinterpret_cast<char*>(moov_payload.data()), moov_payload.size())) {
                LOG_ERROR << "MP4 validation failed: could not read moov payload in " << path;
                return false;
            }

            std::vector<MemBox> moov_children;
            if (!ParseMemBoxes(moov_payload.data(), moov_payload.size(), moov_children)) {
                LOG_ERROR << "MP4 validation failed: corrupted sub-boxes in moov in " << path;
                return false;
            }

            const MemBox* mvhd = FindBox(moov_children, "mvhd");
            if (!mvhd || mvhd->payload_size < 16) {
                LOG_ERROR << "MP4 validation failed: missing or malformed mvhd box in " << path;
                return false;
            }
            uint8_t mvhd_ver = mvhd->payload[0];
            uint32_t timescale = (mvhd_ver == 1) ? ((mvhd->payload_size >= 24) ? ReadU32BE(mvhd->payload + 20) : 0)
                                                 : ReadU32BE(mvhd->payload + 12);
            if (timescale == 0) {
                LOG_ERROR << "MP4 validation failed: invalid zero timescale in mvhd in " << path;
                return false;
            }

            for (const auto& child : moov_children) {
                if (std::strcmp(child.type, "trak") == 0) {
                    std::vector<MemBox> trak_children;
                    if (!ParseMemBoxes(child.payload, child.payload_size, trak_children)) continue;

                    const MemBox* tkhd = FindBox(trak_children, "tkhd");
                    if (!tkhd || tkhd->payload_size < 20) continue;
                    uint8_t tkhd_ver = tkhd->payload[0];
                    uint32_t track_id = (tkhd_ver == 1) ? ((tkhd->payload_size >= 28) ? ReadU32BE(tkhd->payload + 20) : 0)
                                                        : ReadU32BE(tkhd->payload + 12);
                    if (track_id == 0) continue;
                    declared_tracks.push_back(track_id);

                    const MemBox* mdia = FindBox(trak_children, "mdia");
                    if (!mdia) continue;
                    std::vector<MemBox> mdia_children;
                    if (!ParseMemBoxes(mdia->payload, mdia->payload_size, mdia_children)) continue;

                    const MemBox* hdlr = FindBox(mdia_children, "hdlr");
                    if (!hdlr || hdlr->payload_size < 12) continue;
                    char handler[5]{0};
                    std::memcpy(handler, hdlr->payload + 8, 4);

                    const MemBox* minf = FindBox(mdia_children, "minf");
                    if (!minf) continue;
                    std::vector<MemBox> minf_children;
                    if (!ParseMemBoxes(minf->payload, minf->payload_size, minf_children)) continue;

                    const MemBox* stbl = FindBox(minf_children, "stbl");
                    if (!stbl) continue;
                    std::vector<MemBox> stbl_children;
                    if (!ParseMemBoxes(stbl->payload, stbl->payload_size, stbl_children)) continue;

                    const MemBox* stsd = FindBox(stbl_children, "stsd");
                    if (!stsd || stsd->payload_size < 8) continue;
                    uint32_t entry_count = ReadU32BE(stsd->payload + 4);
                    if (entry_count == 0) continue;

                    if (std::strcmp(handler, "vide") == 0) {
                        std::vector<MemBox> stsd_entries;
                        if (ParseMemBoxes(stsd->payload + 8, stsd->payload_size - 8, stsd_entries)) {
                            for (const auto& entry : stsd_entries) {
                                if (std::strcmp(entry.type, "avc1") == 0 && entry.payload_size > 78) {
                                    std::vector<MemBox> avc1_children;
                                    if (ParseMemBoxes(entry.payload + 78, entry.payload_size - 78, avc1_children)) {
                                        const MemBox* avcc = FindBox(avc1_children, "avcC");
                                        if (avcc && avcc->payload_size >= 7 && avcc->payload[0] == 1) {
                                            uint8_t sps_count = avcc->payload[5] & 0x1F;
                                            if (sps_count > 0) {
                                                found_video_trak = true;
                                            }
                                        }
                                    }
                                } else if ((std::strcmp(entry.type, "hvc1") == 0 || std::strcmp(entry.type, "hev1") == 0) &&
                                           entry.payload_size > 78) {
                                    std::vector<MemBox> hvc1_children;
                                    if (ParseMemBoxes(entry.payload + 78, entry.payload_size - 78, hvc1_children)) {
                                        const MemBox* hvcc = FindBox(hvc1_children, "hvcC");
                                        if (hvcc && hvcc->payload_size >= 23 && hvcc->payload[0] == 1) {
                                            found_video_trak = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            const MemBox* mvex = FindBox(moov_children, "mvex");
            if (mvex) {
                has_mvex = true;
            }

            if (!found_video_trak) {
                LOG_ERROR << "MP4 validation failed: no valid video track with AVC/HEVC codec config found in " << path;
                return false;
            }

            found_moov = true;
            current_pos = box.offset + box.size;

        } else if (std::strcmp(box.type, "moof") == 0) {
            if (!found_moov) {
                LOG_ERROR << "MP4 validation failed: moof encountered before moov in " << path;
                return false;
            }

            uint64_t moof_offset = box.offset;
            uint64_t moof_size = box.size;
            std::vector<uint8_t> moof_payload(box.size - box.header_size);
            if (!file.read(reinterpret_cast<char*>(moof_payload.data()), moof_payload.size())) {
                LOG_ERROR << "MP4 validation failed: could not read moof payload in " << path;
                return false;
            }

            std::vector<MemBox> moof_children;
            if (!ParseMemBoxes(moof_payload.data(), moof_payload.size(), moof_children)) {
                LOG_ERROR << "MP4 validation failed: corrupted sub-boxes in moof in " << path;
                return false;
            }

            const MemBox* mfhd = FindBox(moof_children, "mfhd");
            if (!mfhd || mfhd->payload_size < 8) {
                LOG_ERROR << "MP4 validation failed: missing mfhd in moof in " << path;
                return false;
            }
            uint32_t seq = ReadU32BE(mfhd->payload + 4);
            if (seq != expected_fragment_seq) {
                LOG_ERROR << "MP4 validation failed: fragment sequence gap (expected "
                          << expected_fragment_seq << ", got " << seq << ") in " << path;
                return false;
            }
            expected_fragment_seq++;

            struct TrafInfo {
                uint32_t track_id{0};
                int32_t data_offset{0};
                bool has_data_offset{false};
                uint64_t sample_bytes{0};
                uint32_t sample_count{0};
            };
            std::vector<TrafInfo> trafs;
            uint64_t fragment_sample_bytes = 0;

            for (const auto& child : moof_children) {
                if (std::strcmp(child.type, "traf") == 0) {
                    std::vector<MemBox> traf_children;
                    if (!ParseMemBoxes(child.payload, child.payload_size, traf_children)) continue;

                    const MemBox* tfhd = FindBox(traf_children, "tfhd");
                    if (!tfhd || tfhd->payload_size < 8) continue;
                    uint32_t t_id = ReadU32BE(tfhd->payload + 4);

                    const MemBox* tfdt = FindBox(traf_children, "tfdt");
                    if (tfdt && tfdt->payload_size >= 8) {
                        uint8_t tfdt_ver = tfdt->payload[0];
                        uint64_t dtime = (tfdt_ver == 1) ? ((tfdt->payload_size >= 12) ? ReadU64BE(tfdt->payload + 4) : 0)
                                                         : ReadU32BE(tfdt->payload + 4);
                        if (last_decode_times.count(t_id) && dtime < last_decode_times[t_id]) {
                            LOG_ERROR << "MP4 validation failed: non-monotonic decode time in track "
                                      << t_id << " (" << dtime << " < " << last_decode_times[t_id] << ") in " << path;
                            return false;
                        }
                        last_decode_times[t_id] = dtime;
                    }

                    const MemBox* trun = FindBox(traf_children, "trun");
                    if (!trun || trun->payload_size < 8) {
                        LOG_ERROR << "MP4 validation failed: missing or empty trun in traf in " << path;
                        return false;
                    }
                    uint32_t trun_flags = (static_cast<uint32_t>(trun->payload[1]) << 16) |
                                          (static_cast<uint32_t>(trun->payload[2]) << 8) |
                                          static_cast<uint32_t>(trun->payload[3]);
                    uint32_t scount = ReadU32BE(trun->payload + 4);
                    if (scount == 0) {
                        LOG_ERROR << "MP4 validation failed: trun sample count is 0 in " << path;
                        return false;
                    }

                    size_t trun_pos = 8;
                    int32_t data_off = 0;
                    bool has_data_off = false;
                    if (trun_flags & 0x000001) { // data-offset-present
                        if (trun_pos + 4 > trun->payload_size) return false;
                        data_off = static_cast<int32_t>(ReadU32BE(trun->payload + trun_pos));
                        trun_pos += 4;
                        has_data_off = true;
                    }
                    if (trun_flags & 0x000004) { // first-sample-flags-present
                        if (trun_pos + 4 > trun->payload_size) return false;
                        trun_pos += 4;
                    }

                    uint64_t t_bytes = 0;
                    for (uint32_t s = 0; s < scount; ++s) {
                        if (trun_flags & 0x000100) { // duration
                            if (trun_pos + 4 > trun->payload_size) return false;
                            trun_pos += 4;
                        }
                        if (trun_flags & 0x000200) { // size
                            if (trun_pos + 4 > trun->payload_size) return false;
                            uint32_t s_sz = ReadU32BE(trun->payload + trun_pos);
                            if (s_sz == 0) {
                                LOG_ERROR << "MP4 validation failed: zero sample size in trun in " << path;
                                return false;
                            }
                            t_bytes += s_sz;
                            trun_pos += 4;
                        }
                        if (trun_flags & 0x000400) { // flags
                            if (trun_pos + 4 > trun->payload_size) return false;
                            trun_pos += 4;
                        }
                        if (trun_flags & 0x000800) { // composition time offset
                            if (trun_pos + 4 > trun->payload_size) return false;
                            trun_pos += 4;
                        }
                    }

                    TrafInfo ti;
                    ti.track_id = t_id;
                    ti.data_offset = data_off;
                    ti.has_data_offset = has_data_off;
                    ti.sample_bytes = t_bytes;
                    ti.sample_count = scount;
                    trafs.push_back(ti);
                    fragment_sample_bytes += t_bytes;
                }
            }

            if (trafs.empty()) {
                LOG_ERROR << "MP4 validation failed: moof contains no valid traf boxes in " << path;
                return false;
            }

            // Next box MUST be mdat!
            uint64_t next_box_offset = moof_offset + moof_size;
            file.seekg(next_box_offset);
            BoxHeader mdat_box;
            if (!ReadBoxHeader(file, file_size, mdat_box) || std::strcmp(mdat_box.type, "mdat") != 0) {
                LOG_ERROR << "MP4 validation failed: moof is not followed by mdat in " << path;
                return false;
            }

            uint64_t mdat_payload_size = mdat_box.size - mdat_box.header_size;
            if (mdat_payload_size != fragment_sample_bytes) {
                LOG_ERROR << "MP4 validation failed: mdat payload size (" << mdat_payload_size
                          << ") does not match sum of trun sample sizes (" << fragment_sample_bytes
                          << ") in " << path;
                return false;
            }

            uint64_t expected_data_off = moof_size + mdat_box.header_size;
            for (const auto& ti : trafs) {
                if (ti.has_data_offset) {
                    if (static_cast<uint64_t>(ti.data_offset) != expected_data_off) {
                        LOG_ERROR << "MP4 validation failed: trun data_offset " << ti.data_offset
                                  << " does not point to expected offset " << expected_data_off
                                  << " in " << path;
                        return false;
                    }
                    expected_data_off += ti.sample_bytes;
                }
                total_sample_count += ti.sample_count;
            }

            fragment_count++;
            current_pos = mdat_box.offset + mdat_box.size;

        } else if (std::strcmp(box.type, "mfra") == 0) {
            current_pos = box.offset + box.size;
        } else {
            // Permissible top-level boxes: skip, free, void, etc.
            current_pos = box.offset + box.size;
        }
    }

    if (current_pos != file_size) {
        LOG_ERROR << "MP4 validation failed: trailing unparsed bytes at end of file (parsed "
                  << current_pos << " of " << file_size << " bytes) in " << path;
        return false;
    }

    if (!found_ftyp || !found_moov) {
        LOG_ERROR << "MP4 validation failed: missing ftyp or moov box in " << path;
        return false;
    }

    if (has_mvex && (fragment_count == 0 || total_sample_count == 0)) {
        LOG_ERROR << "MP4 validation failed: fragmented MP4 has 0 valid fragments or 0 samples in " << path;
        return false;
    }

    LOG_DEBUG << "Deep MP4 validation PASSED for " << path << " ("
              << fragment_count << " fragments, " << total_sample_count << " samples verified)";
    return true;
}

bool AtomicWriter::FinalizeSegment(SegmentMetadata& out_meta) {
    if (!is_active_) {
        return false;
    }

    FlushCurrentFragment();

    if (fd_ >= 0) {
        // Two-phase commit: flush to disk surface
        if (fdatasync(fd_) != 0) {
            has_io_error_ = true;
            LOG_ERROR << "[Channel " << channel_id_ << "] fdatasync failed: " << strerror(errno);
        }
        close(fd_);
        fd_ = -1;
    }

    if (has_io_error_) {
        LOG_ERROR << "[Channel " << channel_id_ << "] Aborting segment commit due to disk I/O error";
        AbortSegment();
        return false;
    }

    if (frame_count_ == 0) {
        LOG_WARN << "[Channel " << channel_id_ << "] Aborting segment commit with 0 frames";
        AbortSegment();
        return false;
    }

    if (!ValidateMp4File(tmp_file_path_)) {
        LOG_ERROR << "[Channel " << channel_id_ << "] MP4 container validation failed on temporary file: " << tmp_file_path_;
        AbortSegment();
        return false;
    }

    // Atomic filesystem commit: rename .tmp -> .mp4
    int res = rename(tmp_file_path_.c_str(), final_file_path_.c_str());
    if (res != 0) {
        LOG_ERROR << "Failed to atomically rename temporary file " << tmp_file_path_
                  << " to " << final_file_path_ << ": " << strerror(errno);
        AbortSegment();
        return false;
    }

    int64_t duration_ms = 0;
    if (segment_start_pts_us_ >= 0 && last_pts_us_ > segment_start_pts_us_) {
        duration_ms = (last_pts_us_ - segment_start_pts_us_) / 1000;
    } else {
        int64_t end_time_ms = time_utils::WallTimeMs();
        duration_ms = end_time_ms - start_time_ms_;
    }
    if (duration_ms <= 0) duration_ms = 1;

    int64_t end_time_ms = start_time_ms_ + duration_ms;

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
