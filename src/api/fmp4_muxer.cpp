#include "nvr/api/fmp4_muxer.h"
#include <cstring>
#include <cstdio>
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

inline std::vector<uint8_t> StripEmulationPrevention(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        if (i + 2 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 3) {
            out.push_back(0);
            out.push_back(0);
            i += 2; // Skip 0x03 emulation prevention byte
        } else {
            out.push_back(data[i]);
        }
    }
    return out;
}

inline uint8_t ExtractNalType(uint8_t byte0, CodecType codec) {
    if (codec == CodecType::H265) {
        return (byte0 >> 1) & 0x3F;
    }
    return byte0 & 0x1F;
}

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint32_t ReadBit() {
        return ReadBits(1);
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
            }
        }
        return res;
    }

    uint32_t ReadUE() {
        return ReadExpGolomb();
    }

    uint32_t ReadExpGolomb() {
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

std::vector<Fmp4Muxer::NalUnit> Fmp4Muxer::ParseAnnexBNals(const uint8_t* data, size_t size, CodecType codec) {
    std::vector<NalUnit> nals;
    if (!data || size < 4) return nals;

    struct StartCodeSpan {
        size_t prefix_start;
        size_t nal_start;
    };
    std::vector<StartCodeSpan> spans;
    size_t i = 0;
    while (i + 3 <= size) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                spans.push_back({i, i + 3});
                i += 3;
                continue;
            }
            if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) {
                spans.push_back({i, i + 4});
                i += 4;
                continue;
            }
        }
        ++i;
    }

    for (size_t idx = 0; idx < spans.size(); ++idx) {
        size_t start = spans[idx].nal_start;
        size_t end = (idx + 1 < spans.size()) ? spans[idx + 1].prefix_start : size;
        while (end > start && data[end - 1] == 0) {
            --end;
        }
        if (end <= start) continue;
        uint8_t t = ExtractNalType(data[start], codec);
        nals.push_back(NalUnit{data + start, end - start, t, t});
    }
    return nals;
}

std::vector<Fmp4Muxer::NalUnit> Fmp4Muxer::ParseAnnexBNals(const uint8_t* data, size_t size, bool is_h265) {
    return ParseAnnexBNals(data, size, is_h265 ? CodecType::H265 : CodecType::H264);
}

bool Fmp4Muxer::ExtractParameterSets(const uint8_t* data, size_t size, 
                                     std::vector<uint8_t>& out_sps, 
                                     std::vector<uint8_t>& out_pps) {
    auto nals = ParseAnnexBNals(data, size, false);
    bool found_sps = false;
    bool found_pps = false;

    for (const auto& nal : nals) {
        if (nal.type == 7 && !found_sps) { // SPS
            out_sps.assign(nal.data, nal.data + nal.size);
            found_sps = true;
        } else if (nal.type == 8 && !found_pps) { // PPS
            out_pps.assign(nal.data, nal.data + nal.size);
            found_pps = true;
        }
    }
    return found_sps && found_pps;
}

bool Fmp4Muxer::ExtractParameterSetsH265(const uint8_t* data, size_t size,
                                         std::vector<uint8_t>& out_vps,
                                         std::vector<uint8_t>& out_sps,
                                         std::vector<uint8_t>& out_pps) {
    auto nals = ParseAnnexBNals(data, size, true);
    bool found_vps = false;
    bool found_sps = false;
    bool found_pps = false;

    for (const auto& nal : nals) {
        if (nal.type == 32 && !found_vps) { // VPS
            out_vps.assign(nal.data, nal.data + nal.size);
            found_vps = true;
        } else if (nal.type == 33 && !found_sps) { // SPS
            out_sps.assign(nal.data, nal.data + nal.size);
            found_sps = true;
        } else if (nal.type == 34 && !found_pps) { // PPS
            out_pps.assign(nal.data, nal.data + nal.size);
            found_pps = true;
        }
    }
    return found_vps && found_sps && found_pps;
}

Fmp4Muxer::HevcSpsMeta Fmp4Muxer::ParseHevcSpsMeta(const uint8_t* sps, size_t len) {
    HevcSpsMeta meta;
    if (!sps || len < 6) return meta;

    // Strip 0x03 emulation prevention bytes after the 2-byte HEVC NAL header
    std::vector<uint8_t> rbsp = StripEmulationPrevention(sps + 2, len - 2);
    if (rbsp.size() < 13) return meta;

    BitReader br(rbsp.data(), rbsp.size());
    br.ReadBits(4); // sps_video_parameter_set_id
    uint32_t max_sub_layers_minus1 = br.ReadBits(3);
    br.ReadBit();   // sps_temporal_id_nesting_flag

    // profile_tier_level(1, max_sub_layers_minus1)
    meta.general_profile_space = static_cast<uint8_t>(br.ReadBits(2));
    meta.general_tier_flag     = (br.ReadBit() != 0);
    meta.general_profile_idc   = static_cast<uint8_t>(br.ReadBits(5));
    meta.general_profile_compatibility_flags = br.ReadBits(32);
    for (int i = 0; i < 6; ++i) {
        meta.general_constraint_indicator_flags[i] = static_cast<uint8_t>(br.ReadBits(8));
    }
    meta.general_level_idc = static_cast<uint8_t>(br.ReadBits(8));

    std::vector<uint8_t> sub_layer_profile_present(max_sub_layers_minus1, 0);
    std::vector<uint8_t> sub_layer_level_present(max_sub_layers_minus1, 0);
    for (uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
        sub_layer_profile_present[i] = static_cast<uint8_t>(br.ReadBit());
        sub_layer_level_present[i]   = static_cast<uint8_t>(br.ReadBit());
    }
    if (max_sub_layers_minus1 > 0) {
        for (uint32_t i = max_sub_layers_minus1; i < 8; ++i) {
            br.ReadBits(2); // reserved_zero_2bits
        }
    }
    for (uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
        if (sub_layer_profile_present[i]) {
            br.ReadBits(32);
            br.ReadBits(32);
            br.ReadBits(24);
        }
        if (sub_layer_level_present[i]) {
            br.ReadBits(8);
        }
    }

    br.ReadUE(); // sps_seq_parameter_set_id
    uint32_t chroma_format_idc = br.ReadUE();
    meta.chroma_format_idc = static_cast<uint8_t>(chroma_format_idc);
    if (chroma_format_idc == 3) {
        br.ReadBit(); // separate_colour_plane_flag
    }
    uint32_t pic_width_in_luma_samples  = br.ReadUE();
    uint32_t pic_height_in_luma_samples = br.ReadUE();
    uint32_t conformance_window_flag    = br.ReadBit();
    uint32_t conf_win_left_offset   = 0;
    uint32_t conf_win_right_offset  = 0;
    uint32_t conf_win_top_offset    = 0;
    uint32_t conf_win_bottom_offset = 0;
    if (conformance_window_flag) {
        conf_win_left_offset   = br.ReadUE();
        conf_win_right_offset  = br.ReadUE();
        conf_win_top_offset    = br.ReadUE();
        conf_win_bottom_offset = br.ReadUE();
    }
    meta.bit_depth_luma_minus8   = static_cast<uint8_t>(br.ReadUE());
    meta.bit_depth_chroma_minus8 = static_cast<uint8_t>(br.ReadUE());

    uint32_t sub_width_c  = (chroma_format_idc == 1 || chroma_format_idc == 2) ? 2 : 1;
    uint32_t sub_height_c = (chroma_format_idc == 1) ? 2 : 1;
    uint32_t crop_x = sub_width_c * (conf_win_left_offset + conf_win_right_offset);
    uint32_t crop_y = sub_height_c * (conf_win_top_offset + conf_win_bottom_offset);

    if (pic_width_in_luma_samples > crop_x && pic_height_in_luma_samples > crop_y) {
        meta.width  = pic_width_in_luma_samples - crop_x;
        meta.height = pic_height_in_luma_samples - crop_y;
    } else {
        meta.width  = pic_width_in_luma_samples;
        meta.height = pic_height_in_luma_samples;
    }
    return meta;
}

bool Fmp4Muxer::ParseH265SpsDimensions(const uint8_t* sps, size_t size, 
                                       uint32_t& out_w, uint32_t& out_h) {
    auto meta = ParseHevcSpsMeta(sps, size);
    if (meta.width > 0 && meta.height > 0) {
        out_w = meta.width;
        out_h = meta.height;
        return true;
    }
    return false;
}

bool Fmp4Muxer::ParseSpsDimensions(const uint8_t* sps, size_t size, 
                                   uint32_t& out_w, uint32_t& out_h) {
    if (!sps || size < 4) return false;
    BitReader reader(sps + 1, size - 1);
    uint32_t profile_idc = reader.ReadBits(8);
    reader.ReadBits(8); // constraint flags
    reader.ReadBits(8); // level idc
    reader.ReadExpGolomb(); // seq_parameter_set_id

    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
        profile_idc == 86 || profile_idc == 118 || profile_idc == 128) {
        uint32_t chroma_format_idc = reader.ReadExpGolomb();
        if (chroma_format_idc == 3) {
            reader.ReadBits(1); // separate_colour_plane_flag
        }
        reader.ReadExpGolomb(); // bit_depth_luma_minus8
        reader.ReadExpGolomb(); // bit_depth_chroma_minus8
        reader.ReadBits(1);     // qpprime_y_zero_transform_bypass_flag
        uint32_t seq_scaling_matrix_present = reader.ReadBits(1);
        if (seq_scaling_matrix_present) {
            size_t count = (chroma_format_idc != 3) ? 8 : 12;
            for (size_t i = 0; i < count; ++i) {
                if (reader.ReadBits(1)) {
                    size_t last_scale = 8, next_scale = 8;
                    size_t size_list = (i < 6) ? 16 : 64;
                    for (size_t j = 0; j < size_list; ++j) {
                        if (next_scale != 0) {
                            int32_t delta = static_cast<int32_t>(reader.ReadExpGolomb());
                            next_scale = (last_scale + static_cast<size_t>(delta) + 256) % 256;
                        }
                        last_scale = (next_scale == 0) ? last_scale : next_scale;
                    }
                }
            }
        }
    }

    reader.ReadExpGolomb(); // log2_max_frame_num_minus4
    uint32_t pic_order_cnt_type = reader.ReadExpGolomb();
    if (pic_order_cnt_type == 0) {
        reader.ReadExpGolomb(); // log2_max_pic_order_cnt_lsb_minus4
    } else if (pic_order_cnt_type == 1) {
        reader.ReadBits(1); // delta_pic_order_always_zero_flag
        reader.ReadExpGolomb(); // offset_for_non_ref_pic
        reader.ReadExpGolomb(); // offset_for_top_to_bottom_field
        uint32_t num_ref_frames_in_pic_order_cnt_cycle = reader.ReadExpGolomb();
        for (uint32_t i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; ++i) {
            reader.ReadExpGolomb();
        }
    }

    reader.ReadExpGolomb(); // max_num_ref_frames
    reader.ReadBits(1);     // gaps_in_frame_num_value_allowed_flag

    uint32_t pic_width_in_mbs_minus1 = reader.ReadExpGolomb();
    uint32_t pic_height_in_map_units_minus1 = reader.ReadExpGolomb();
    uint32_t frame_mbs_only_flag = reader.ReadBits(1);

    if (!frame_mbs_only_flag) {
        reader.ReadBits(1); // mb_adaptive_frame_field_flag
    }
    reader.ReadBits(1); // direct_8x8_inference_flag

    uint32_t frame_cropping_flag = reader.ReadBits(1);
    uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (frame_cropping_flag) {
        crop_left = reader.ReadExpGolomb();
        crop_right = reader.ReadExpGolomb();
        crop_top = reader.ReadExpGolomb();
        crop_bottom = reader.ReadExpGolomb();
    }

    uint32_t w = ((pic_width_in_mbs_minus1 + 1) * 16) - ((crop_left + crop_right) * 2);
    uint32_t h = ((2 - frame_mbs_only_flag) * (pic_height_in_map_units_minus1 + 1) * 16) - ((crop_top + crop_bottom) * 2);

    if (w > 0 && h > 0) {
        out_w = w;
        out_h = h;
        return true;
    }
    return false;
}

std::string Fmp4Muxer::BuildHevcCodecString(const uint8_t* sps, size_t len, bool use_hvc1) {
    HevcSpsMeta meta = ParseHevcSpsMeta(sps, len);
    // Reverse bits of general_profile_compatibility_flags per ISO/IEC 14496-15 Section E.3
    uint32_t compat_reversed = 0;
    for (int i = 0; i < 32; ++i) {
        if (meta.general_profile_compatibility_flags & (1U << i)) {
            compat_reversed |= (1U << (31 - i));
        }
    }
    if (compat_reversed == 0) compat_reversed = 0x60;

    static const char* kSpacePrefix[4] = {"", "A", "B", "C"};
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s.%s%u.%X.%c%u",
                  use_hvc1 ? "hvc1" : "hev1",
                  kSpacePrefix[meta.general_profile_space & 0x03],
                  static_cast<unsigned>(meta.general_profile_idc),
                  compat_reversed,
                  meta.general_tier_flag ? 'H' : 'L',
                  static_cast<unsigned>(meta.general_level_idc));
    std::string result(buf);

    // Append non-zero trailing constraint bytes in hex per ISO/IEC 14496-15 E.3
    int last_nonzero = -1;
    for (int i = 5; i >= 0; --i) {
        if (meta.general_constraint_indicator_flags[i] != 0) {
            last_nonzero = i;
            break;
        }
    }
    for (int i = 0; i <= last_nonzero; ++i) {
        char cbuf[8];
        std::snprintf(cbuf, sizeof(cbuf), ".%02X",
                      static_cast<unsigned>(meta.general_constraint_indicator_flags[i]));
        result += cbuf;
    }
    return result;
}

std::string Fmp4Muxer::BuildAvcCodecString(const uint8_t* sps, size_t len) {
    uint8_t profile_idc  = (sps && len > 1) ? sps[1] : 0x42;
    uint8_t profile_comp = (sps && len > 2) ? sps[2] : 0xE0;
    uint8_t level_idc    = (sps && len > 3) ? sps[3] : 0x1E;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "avc1.%02X%02X%02X",
                  static_cast<unsigned>(profile_idc),
                  static_cast<unsigned>(profile_comp),
                  static_cast<unsigned>(level_idc));
    return std::string(buf);
}

std::vector<uint8_t> Fmp4Muxer::BuildHvcCRecord(
    const std::vector<uint8_t>& vps,
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps) {
    HevcSpsMeta meta = ParseHevcSpsMeta(sps.data(), sps.size());
    std::vector<uint8_t> hvcc;
    hvcc.push_back(1); // configurationVersion = 1
    uint8_t byte1 = static_cast<uint8_t>(
        ((meta.general_profile_space & 0x03) << 6) |
        (meta.general_tier_flag ? 0x20 : 0x00) |
        (meta.general_profile_idc & 0x1F));
    hvcc.push_back(byte1);
    PutU32(hvcc, meta.general_profile_compatibility_flags);
    for (int i = 0; i < 6; ++i) hvcc.push_back(meta.general_constraint_indicator_flags[i]);
    hvcc.push_back(meta.general_level_idc);
    PutU16(hvcc, 0xF000); // min_spatial_segmentation_idc = 0
    hvcc.push_back(0xFC); // parallelismType = 0
    hvcc.push_back(static_cast<uint8_t>(0xFC | (meta.chroma_format_idc & 0x03)));
    hvcc.push_back(static_cast<uint8_t>(0xF8 | (meta.bit_depth_luma_minus8 & 0x07)));
    hvcc.push_back(static_cast<uint8_t>(0xF8 | (meta.bit_depth_chroma_minus8 & 0x07)));
    PutU16(hvcc, 0);      // avgFrameRate = 0
    hvcc.push_back(0x0F); // constantFrameRate=0, numTemporalLayers=1, temporalIdNested=1, lengthSizeMinusOne=3

    uint8_t num_arrays = 0;
    if (!vps.empty()) ++num_arrays;
    if (!sps.empty()) ++num_arrays;
    if (!pps.empty()) ++num_arrays;
    hvcc.push_back(num_arrays);

    auto append_array = [&](uint8_t nal_type, const std::vector<uint8_t>& nal) {
        if (nal.empty()) return;
        hvcc.push_back(static_cast<uint8_t>(0x80 | (nal_type & 0x3F)));
        PutU16(hvcc, 1);
        PutU16(hvcc, static_cast<uint16_t>(nal.size()));
        hvcc.insert(hvcc.end(), nal.begin(), nal.end());
    };
    append_array(32, vps);
    append_array(33, sps);
    append_array(34, pps);
    return hvcc;
}

std::vector<uint8_t> Fmp4Muxer::BuildAvcCRecord(
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps) {
    uint8_t profile_idc  = (sps.size() > 1) ? sps[1] : 0x64;
    uint8_t profile_comp = (sps.size() > 2) ? sps[2] : 0x00;
    uint8_t level_idc    = (sps.size() > 3) ? sps[3] : 0x28;

    std::vector<uint8_t> avcc;
    avcc.push_back(1);
    avcc.push_back(profile_idc);
    avcc.push_back(profile_comp);
    avcc.push_back(level_idc);
    avcc.push_back(0xFF); // 4-byte NAL length prefix
    avcc.push_back(0xE1); // 1 SPS
    PutU16(avcc, static_cast<uint16_t>(sps.size()));
    avcc.insert(avcc.end(), sps.begin(), sps.end());
    avcc.push_back(1);    // 1 PPS
    PutU16(avcc, static_cast<uint16_t>(pps.size()));
    avcc.insert(avcc.end(), pps.begin(), pps.end());
    return avcc;
}

std::vector<uint8_t> Fmp4Muxer::AnnexBToAvcc(const uint8_t* data, size_t size, bool is_h265) {
    std::vector<uint8_t> avcc;
    auto nals = ParseAnnexBNals(data, size, is_h265);
    for (const auto& nal : nals) {
        if (is_h265) {
            if (nal.type == 32 || nal.type == 33 || nal.type == 34 || nal.type == 35) continue;
        } else {
            if (nal.type == 7 || nal.type == 8 || nal.type == 9) continue;
        }
        PutU32(avcc, static_cast<uint32_t>(nal.size));
        avcc.insert(avcc.end(), nal.data, nal.data + nal.size);
    }
    return avcc;
}

std::vector<uint8_t> Fmp4Muxer::BuildInitSegment(const std::vector<uint8_t>& sps,
                                                 const std::vector<uint8_t>& pps,
                                                 uint32_t width, uint32_t height,
                                                 uint32_t timescale) {
    std::vector<uint8_t> buf;

    // 1. ftyp box
    size_t ftyp_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "ftyp");
    PutFourCC(buf, "isom"); // major brand
    PutU32(buf, 0x00000200); // minor version
    PutFourCC(buf, "isom");
    PutFourCC(buf, "iso6");
    PutFourCC(buf, "avc1");
    PutFourCC(buf, "mp41");
    UpdateBoxSize(buf, ftyp_start);

    // 2. moov box
    size_t moov_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "moov");

    // mvhd
    size_t mvhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "mvhd");
    PutU32(buf, 0); // version 0, flags 0
    PutU32(buf, 0); // creation_time
    PutU32(buf, 0); // modification_time
    PutU32(buf, timescale); // timescale 90000
    PutU32(buf, 0); // duration 0
    PutU32(buf, 0x00010000); // rate 1.0
    PutU16(buf, 0x0100);     // volume 1.0
    PutU16(buf, 0);          // reserved
    PutU32(buf, 0); PutU32(buf, 0); // reserved[2]
    // Matrix structure
    PutU32(buf, 0x00010000); PutU32(buf, 0); PutU32(buf, 0);
    PutU32(buf, 0); PutU32(buf, 0x00010000); PutU32(buf, 0);
    PutU32(buf, 0); PutU32(buf, 0); PutU32(buf, 0x40000000);
    for (int i = 0; i < 6; ++i) PutU32(buf, 0); // pre_defined[6]
    PutU32(buf, 2); // next_track_ID = 2
    UpdateBoxSize(buf, mvhd_start);

    // mvex
    size_t mvex_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "mvex");
    // trex
    size_t trex_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "trex");
    PutU32(buf, 0); // version 0, flags 0
    PutU32(buf, 1); // track_ID = 1
    PutU32(buf, 1); // default_sample_description_index = 1
    PutU32(buf, 0); // default_sample_duration = 0
    PutU32(buf, 0); // default_sample_size = 0
    PutU32(buf, 0); // default_sample_flags = 0
    UpdateBoxSize(buf, trex_start);
    UpdateBoxSize(buf, mvex_start);

    // trak
    size_t trak_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "trak");

    // tkhd
    size_t tkhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "tkhd");
    PutU32(buf, 0x00000007); // flags: TRACK_ENABLED | TRACK_IN_MOVIE | TRACK_IN_PREVIEW
    PutU32(buf, 0); // creation_time
    PutU32(buf, 0); // modification_time
    PutU32(buf, 1); // track_ID = 1
    PutU32(buf, 0); // reserved
    PutU32(buf, 0); // duration = 0
    PutU32(buf, 0); PutU32(buf, 0); // reserved[2]
    PutU16(buf, 0); // layer
    PutU16(buf, 0); // alternate_group
    PutU16(buf, 0); // volume
    PutU16(buf, 0);
    // Matrix
    PutU32(buf, 0x00010000); PutU32(buf, 0); PutU32(buf, 0);
    PutU32(buf, 0); PutU32(buf, 0x00010000); PutU32(buf, 0);
    PutU32(buf, 0); PutU32(buf, 0); PutU32(buf, 0x40000000);
    PutU32(buf, width << 16);
    PutU32(buf, height << 16);
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
    PutU32(buf, timescale);
    PutU32(buf, 0);
    PutU16(buf, 0x55C4); // 'und'
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
    for (size_t k = 0; k < std::strlen(hdlr_name) + 1; ++k) buf.push_back(hdlr_name[k]);
    UpdateBoxSize(buf, hdlr_start);

    // minf
    size_t minf_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "minf");

    // vmhd
    size_t vmhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "vmhd");
    PutU32(buf, 0x00000001); // flags = 1
    PutU16(buf, 0); // graphicsmode
    PutU16(buf, 0); PutU16(buf, 0); PutU16(buf, 0); // opcolor
    UpdateBoxSize(buf, vmhd_start);

    // dinf
    size_t dinf_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "dinf");
    size_t dref_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "dref");
    PutU32(buf, 0);
    PutU32(buf, 1); // entry_count = 1
    size_t url_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "url ");
    PutU32(buf, 0x00000001); // self-contained
    UpdateBoxSize(buf, url_start);
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
    PutU32(buf, 1); // entry_count = 1

    // avc1
    size_t avc1_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "avc1");
    for (int i = 0; i < 6; ++i) buf.push_back(0); // reserved
    PutU16(buf, 1); // data_reference_index
    PutU16(buf, 0); PutU16(buf, 0); // pre_defined, reserved
    for (int i = 0; i < 3; ++i) PutU32(buf, 0); // pre_defined[3]
    PutU16(buf, static_cast<uint16_t>(width));
    PutU16(buf, static_cast<uint16_t>(height));
    PutU32(buf, 0x00480000); // 72 dpi horiz
    PutU32(buf, 0x00480000); // 72 dpi vert
    PutU32(buf, 0); // reserved
    PutU16(buf, 1); // frame_count = 1
    for (int i = 0; i < 32; ++i) buf.push_back(0); // compressorname
    PutU16(buf, 0x0018); // depth 24-bit
    PutU16(buf, 0xFFFF); // pre_defined = -1

    // avcC
    size_t avcc_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "avcC");
    std::vector<uint8_t> avcc_payload = BuildAvcCRecord(sps, pps);
    buf.insert(buf.end(), avcc_payload.begin(), avcc_payload.end());
    UpdateBoxSize(buf, avcc_start);

    UpdateBoxSize(buf, avc1_start);
    UpdateBoxSize(buf, stsd_start);

    // Empty stts, stsc, stsz, stco
    size_t stts_start = buf.size(); PutU32(buf, 0); PutFourCC(buf, "stts"); PutU32(buf, 0); PutU32(buf, 0); UpdateBoxSize(buf, stts_start);
    size_t stsc_start = buf.size(); PutU32(buf, 0); PutFourCC(buf, "stsc"); PutU32(buf, 0); PutU32(buf, 0); UpdateBoxSize(buf, stsc_start);
    size_t stsz_start = buf.size(); PutU32(buf, 0); PutFourCC(buf, "stsz"); PutU32(buf, 0); PutU32(buf, 0); PutU32(buf, 0); UpdateBoxSize(buf, stsz_start);
    size_t stco_start = buf.size(); PutU32(buf, 0); PutFourCC(buf, "stco"); PutU32(buf, 0); PutU32(buf, 0); UpdateBoxSize(buf, stco_start);

    UpdateBoxSize(buf, stbl_start);
    UpdateBoxSize(buf, minf_start);
    UpdateBoxSize(buf, mdia_start);
    UpdateBoxSize(buf, trak_start);
    UpdateBoxSize(buf, moov_start);

    return buf;
}

std::vector<uint8_t> Fmp4Muxer::BuildInitSegmentH265(const std::vector<uint8_t>& vps,
                                                         const std::vector<uint8_t>& sps,
                                                         const std::vector<uint8_t>& pps,
                                                         uint32_t width, uint32_t height,
                                                         uint32_t timescale) {
    std::vector<uint8_t> buf;

    if (width == 0 || height == 0) {
        ParseH265SpsDimensions(sps.data(), sps.size(), width, height);
    }

    // 1. ftyp box
    size_t ftyp_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "ftyp");
    PutFourCC(buf, "iso6"); // major brand
    PutU32(buf, 0x00000200); // minor version
    PutFourCC(buf, "isom");
    PutFourCC(buf, "iso6");
    PutFourCC(buf, "hvc1");
    PutFourCC(buf, "mp41");
    UpdateBoxSize(buf, ftyp_start);

    // 2. moov box
    size_t moov_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "moov");

    // mvhd
    size_t mvhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "mvhd");
    PutU32(buf, 0); // version 0, flags 0
    PutU32(buf, 0); // creation_time
    PutU32(buf, 0); // modification_time
    PutU32(buf, timescale); // timescale 90000
    PutU32(buf, 0); // duration 0
    PutU32(buf, 0x00010000); // rate 1.0
    PutU16(buf, 0x0100);     // volume 1.0
    PutU16(buf, 0);          // reserved
    PutU32(buf, 0); PutU32(buf, 0); // reserved[2]
    // Matrix structure
    PutU32(buf, 0x00010000); PutU32(buf, 0); PutU32(buf, 0);
    PutU32(buf, 0); PutU32(buf, 0x00010000); PutU32(buf, 0);
    PutU32(buf, 0); PutU32(buf, 0); PutU32(buf, 0x40000000);
    for (int i = 0; i < 6; ++i) PutU32(buf, 0); // pre_defined[6]
    PutU32(buf, 2); // next_track_ID = 2
    UpdateBoxSize(buf, mvhd_start);

    // mvex
    size_t mvex_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "mvex");
    // trex
    size_t trex_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "trex");
    PutU32(buf, 0); // version 0, flags 0
    PutU32(buf, 1); // track_ID = 1
    PutU32(buf, 1); // default_sample_description_index = 1
    PutU32(buf, 0); // default_sample_duration = 0
    PutU32(buf, 0); // default_sample_size = 0
    PutU32(buf, 0); // default_sample_flags = 0
    UpdateBoxSize(buf, trex_start);
    UpdateBoxSize(buf, mvex_start);

    // trak
    size_t trak_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "trak");

    // tkhd
    size_t tkhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "tkhd");
    PutU32(buf, 0x00000007); // flags: TRACK_ENABLED | TRACK_IN_MOVIE | TRACK_IN_PREVIEW
    PutU32(buf, 0); // creation_time
    PutU32(buf, 0); // modification_time
    PutU32(buf, 1); // track_ID = 1
    PutU32(buf, 0); // reserved
    PutU32(buf, 0); // duration = 0
    PutU32(buf, 0); PutU32(buf, 0); // reserved[2]
    PutU16(buf, 0); // layer
    PutU16(buf, 0); // alternate_group
    PutU16(buf, 0); // volume
    PutU16(buf, 0);
    // Matrix
    PutU32(buf, 0x00010000); PutU32(buf, 0); PutU32(buf, 0);
    PutU32(buf, 0); PutU32(buf, 0x00010000); PutU32(buf, 0);
    PutU32(buf, 0); PutU32(buf, 0); PutU32(buf, 0x40000000);
    PutU32(buf, width << 16);
    PutU32(buf, height << 16);
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
    PutU32(buf, timescale);
    PutU32(buf, 0);
    PutU16(buf, 0x55C4); // 'und'
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
    for (size_t k = 0; k < std::strlen(hdlr_name) + 1; ++k) buf.push_back(hdlr_name[k]);
    UpdateBoxSize(buf, hdlr_start);

    // minf
    size_t minf_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "minf");

    // vmhd
    size_t vmhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "vmhd");
    PutU32(buf, 0x00000001); // flags = 1
    PutU16(buf, 0); // graphicsmode
    PutU16(buf, 0); PutU16(buf, 0); PutU16(buf, 0); // opcolor
    UpdateBoxSize(buf, vmhd_start);

    // dinf
    size_t dinf_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "dinf");
    size_t dref_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "dref");
    PutU32(buf, 0);
    PutU32(buf, 1); // entry_count = 1
    size_t url_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "url ");
    PutU32(buf, 0x00000001); // self-contained
    UpdateBoxSize(buf, url_start);
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
    PutU32(buf, 1); // entry_count = 1

    // hvc1
    size_t hvc1_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "hvc1");
    for (int i = 0; i < 6; ++i) buf.push_back(0); // reserved
    PutU16(buf, 1); // data_reference_index
    PutU16(buf, 0); PutU16(buf, 0); // pre_defined, reserved
    for (int i = 0; i < 3; ++i) PutU32(buf, 0); // pre_defined[3]
    PutU16(buf, static_cast<uint16_t>(width));
    PutU16(buf, static_cast<uint16_t>(height));
    PutU32(buf, 0x00480000); // 72 dpi horiz
    PutU32(buf, 0x00480000); // 72 dpi vert
    PutU32(buf, 0); // reserved
    PutU16(buf, 1); // frame_count = 1
    for (int i = 0; i < 32; ++i) buf.push_back(0); // compressorname
    PutU16(buf, 0x0018); // depth 24-bit
    PutU16(buf, 0xFFFF); // pre_defined = -1

    // hvcC
    size_t hvcc_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "hvcC");
    std::vector<uint8_t> hvcc_payload = BuildHvcCRecord(vps, sps, pps);
    buf.insert(buf.end(), hvcc_payload.begin(), hvcc_payload.end());
    UpdateBoxSize(buf, hvcc_start);

    UpdateBoxSize(buf, hvc1_start);
    UpdateBoxSize(buf, stsd_start);

    // Empty static tables required by ISOBMFF
    size_t stts_start = buf.size(); PutU32(buf, 0); PutFourCC(buf, "stts"); PutU32(buf, 0); PutU32(buf, 0); UpdateBoxSize(buf, stts_start);
    size_t stsc_start = buf.size(); PutU32(buf, 0); PutFourCC(buf, "stsc"); PutU32(buf, 0); PutU32(buf, 0); UpdateBoxSize(buf, stsc_start);
    size_t stsz_start = buf.size(); PutU32(buf, 0); PutFourCC(buf, "stsz"); PutU32(buf, 0); PutU32(buf, 0); PutU32(buf, 0); UpdateBoxSize(buf, stsz_start);
    size_t stco_start = buf.size(); PutU32(buf, 0); PutFourCC(buf, "stco"); PutU32(buf, 0); PutU32(buf, 0); UpdateBoxSize(buf, stco_start);

    UpdateBoxSize(buf, stbl_start);
    UpdateBoxSize(buf, minf_start);
    UpdateBoxSize(buf, mdia_start);
    UpdateBoxSize(buf, trak_start);
    UpdateBoxSize(buf, moov_start);

    return buf;
}

std::vector<uint8_t> Fmp4Muxer::BuildMediaSegment(uint32_t sequence_number,
                                                  uint64_t pts_timescale,
                                                  uint32_t duration_timescale,
                                                  bool is_keyframe,
                                                  const uint8_t* avcc_data,
                                                  size_t avcc_size) {
    std::vector<uint8_t> buf;

    // 1. moof box
    size_t moof_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "moof");

    // mfhd
    size_t mfhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "mfhd");
    PutU32(buf, 0); // version 0, flags 0
    PutU32(buf, sequence_number);
    UpdateBoxSize(buf, mfhd_start);

    // traf
    size_t traf_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "traf");

    // tfhd
    // flags: default-base-is-moof(0x020000)
    size_t tfhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "tfhd");
    PutU32(buf, 0x00020000);
    PutU32(buf, 1); // track_ID = 1
    UpdateBoxSize(buf, tfhd_start);

    // tfdt
    size_t tfdt_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "tfdt");
    PutU32(buf, 0x01000000); // version 1 (64-bit baseMediaDecodeTime)
    PutU64(buf, pts_timescale);
    UpdateBoxSize(buf, tfdt_start);

    // trun
    // flags: data-offset(0x01) | sample-duration(0x100) | sample-size(0x200) | first-sample-flags(0x04) = 0x000305
    size_t trun_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "trun");
    PutU32(buf, 0x00000305);
    PutU32(buf, 1); // sample_count = 1

    size_t data_offset_pos = buf.size();
    PutU32(buf, 0); // data_offset (placeholder)

    // first_sample_flags: keyframe = 0x02000000 (no depends-on), non-keyframe = 0x01010000
    PutU32(buf, is_keyframe ? 0x02000000 : 0x01010000);
    PutU32(buf, duration_timescale);
    PutU32(buf, static_cast<uint32_t>(avcc_size));

    UpdateBoxSize(buf, trun_start);
    UpdateBoxSize(buf, traf_start);
    UpdateBoxSize(buf, moof_start);

    // Total moof size is now finalized!
    // data_offset in trun points to the beginning of mdat payload (moof_size + 8 bytes of mdat header)
    uint32_t moof_size = static_cast<uint32_t>(buf.size() - moof_start);
    uint32_t final_data_offset = moof_size + 8;
    buf[data_offset_pos]     = static_cast<uint8_t>((final_data_offset >> 24) & 0xFF);
    buf[data_offset_pos + 1] = static_cast<uint8_t>((final_data_offset >> 16) & 0xFF);
    buf[data_offset_pos + 2] = static_cast<uint8_t>((final_data_offset >> 8) & 0xFF);
    buf[data_offset_pos + 3] = static_cast<uint8_t>(final_data_offset & 0xFF);

    // 2. mdat box
    size_t mdat_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "mdat");
    if (avcc_data && avcc_size > 0) {
        buf.insert(buf.end(), avcc_data, avcc_data + avcc_size);
    }
    UpdateBoxSize(buf, mdat_start);

    return buf;
}

} // namespace nvr
