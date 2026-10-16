#include "nvr/api/fmp4_muxer.h"
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
                if (byte_offset_ >= 2 && byte_offset_ < size_ && data_[byte_offset_] == 0x03 &&
                    data_[byte_offset_ - 1] == 0x00 && data_[byte_offset_ - 2] == 0x00) {
                    byte_offset_++;
                }
            }
        }
        return res;
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

std::vector<Fmp4Muxer::NalUnit> Fmp4Muxer::ParseAnnexBNals(const uint8_t* data, size_t size) {
    std::vector<NalUnit> nals;
    if (!data || size < 4) return nals;

    size_t i = 0;
    std::vector<size_t> start_offsets;

    while (i + 3 <= size) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                start_offsets.push_back(i + 3);
                i += 3;
                continue;
            } else if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                start_offsets.push_back(i + 4);
                i += 4;
                continue;
            }
        }
        ++i;
    }

    for (size_t idx = 0; idx < start_offsets.size(); ++idx) {
        size_t start = start_offsets[idx];
        size_t end = (idx + 1 < start_offsets.size()) ? start_offsets[idx + 1] : size;

        // Trim trailing zeros of the preceding start code
        while (end > start && data[end - 1] == 0) {
            --end;
        }

        if (end > start) {
            NalUnit nu;
            nu.data = data + start;
            nu.size = end - start;
            nu.type = nu.data[0] & 0x1F;
            nals.push_back(nu);
        }
    }

    return nals;
}

bool Fmp4Muxer::ExtractParameterSets(const uint8_t* data, size_t size, 
                                     std::vector<uint8_t>& out_sps, 
                                     std::vector<uint8_t>& out_pps) {
    auto nals = ParseAnnexBNals(data, size);
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
                    size_t sc = (i < 6) ? 16 : 64;
                    for (size_t j = 0; j < sc; ++j) {
                        reader.ReadExpGolomb();
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

std::vector<uint8_t> Fmp4Muxer::AnnexBToAvcc(const uint8_t* data, size_t size) {
    std::vector<uint8_t> avcc;
    auto nals = ParseAnnexBNals(data, size);
    for (const auto& nal : nals) {
        // Exclude in-band SPS/PPS from AVCC sample payloads (already stored in avcC)
        if (nal.type == 7 || nal.type == 8 || nal.type == 9) continue;
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
    PutU32(buf, 2); // next_track_ID
    UpdateBoxSize(buf, mvhd_start);

    // trak
    size_t trak_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "trak");

    // tkhd
    size_t tkhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "tkhd");
    PutU32(buf, 0x00000003); // track_enabled | track_in_movie
    PutU32(buf, 0); PutU32(buf, 0);
    PutU32(buf, 1); // track_ID = 1
    PutU32(buf, 0);
    PutU32(buf, 0); // duration
    PutU32(buf, 0); PutU32(buf, 0);
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
    PutU32(buf, 0x00000001);
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
    buf.push_back(1); // configurationVersion
    buf.push_back(sps.size() > 1 ? sps[1] : 0x42); // AVCProfileIndication
    buf.push_back(sps.size() > 2 ? sps[2] : 0xE0); // profile_compatibility
    buf.push_back(sps.size() > 3 ? sps[3] : 0x1E); // AVCLevelIndication
    buf.push_back(0xFF); // lengthSizeMinusOne (4-byte NAL size)
    buf.push_back(0xE1); // numOfSequenceParameterSets = 1
    PutU16(buf, static_cast<uint16_t>(sps.size()));
    buf.insert(buf.end(), sps.begin(), sps.end());
    buf.push_back(1);    // numOfPictureParameterSets = 1
    PutU16(buf, static_cast<uint16_t>(pps.size()));
    buf.insert(buf.end(), pps.begin(), pps.end());
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

    // mvex
    size_t mvex_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "mvex");
    size_t trex_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "trex");
    PutU32(buf, 0);
    PutU32(buf, 1); // track_ID = 1
    PutU32(buf, 1); // default_sample_description_index = 1
    PutU32(buf, 3600); // default_sample_duration (for 25fps)
    PutU32(buf, 0); // default_sample_size
    PutU32(buf, 0x00010000); // default_sample_flags (depends on nothing)
    UpdateBoxSize(buf, trex_start);
    UpdateBoxSize(buf, mvex_start);

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
    PutU32(buf, 0);
    PutU32(buf, sequence_number);
    UpdateBoxSize(buf, mfhd_start);

    // traf
    size_t traf_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "traf");

    // tfhd
    size_t tfhd_start = buf.size();
    PutU32(buf, 0);
    PutFourCC(buf, "tfhd");
    PutU32(buf, 0x020000); // default-base-is-moof
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
