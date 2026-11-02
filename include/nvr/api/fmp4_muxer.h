#pragma once

#include "nvr/common/types.h"
#include <cstdint>
#include <vector>
#include <string>

namespace nvr {

class Fmp4Muxer {
public:
    struct NalUnit {
        const uint8_t* data{nullptr};
        size_t size{0};
        uint8_t type{0};
        uint8_t nal_type{0};
    };

    struct HevcSpsMeta {
        uint8_t  configuration_version{1};
        uint8_t  general_profile_space{0};
        bool     general_tier_flag{false};
        uint8_t  general_profile_idc{1};
        uint32_t general_profile_compatibility_flags{0x60000000U};
        uint8_t  general_constraint_indicator_flags[6]{0x90, 0, 0, 0, 0, 0};
        uint8_t  general_level_idc{93};
        uint8_t  chroma_format_idc{1};
        uint8_t  bit_depth_luma_minus8{0};
        uint8_t  bit_depth_chroma_minus8{0};
        uint32_t width{0};
        uint32_t height{0};
    };

    // Extract all NAL units from an Annex-B byte buffer (codec-aware)
    static std::vector<NalUnit> ParseAnnexBNals(const uint8_t* data, size_t size, bool is_h265);
    static std::vector<NalUnit> ParseAnnexBNals(const uint8_t* data, size_t size, CodecType codec);

    // Extract SPS and PPS from Annex-B data (H.264)
    static bool ExtractParameterSets(const uint8_t* data, size_t size, 
                                     std::vector<uint8_t>& out_sps, 
                                     std::vector<uint8_t>& out_pps);

    // Extract VPS, SPS and PPS from Annex-B data (H.265)
    static bool ExtractParameterSetsH265(const uint8_t* data, size_t size,
                                         std::vector<uint8_t>& out_vps,
                                         std::vector<uint8_t>& out_sps,
                                         std::vector<uint8_t>& out_pps);

    // Parse H.264 SPS to extract width, height, profile, level
    static bool ParseSpsDimensions(const uint8_t* sps, size_t size, 
                                   uint32_t& out_w, uint32_t& out_h);

    // Parse H.265 SPS to extract width, height
    static bool ParseH265SpsDimensions(const uint8_t* sps, size_t size,
                                       uint32_t& out_w, uint32_t& out_h);

    // Parse H.265 SPS to extract full metadata (RBSP de-emulated)
    static HevcSpsMeta ParseHevcSpsMeta(const uint8_t* sps, size_t len);

    // Build RFC 6381 MIME codec strings
    static std::string BuildHevcCodecString(const uint8_t* sps, size_t len, bool use_hvc1 = false);
    static std::string BuildAvcCodecString(const uint8_t* sps, size_t len);

    // Build ISO/IEC 14496-15 configuration records (hvcC / avcC)
    static std::vector<uint8_t> BuildHvcCRecord(
        const std::vector<uint8_t>& vps,
        const std::vector<uint8_t>& sps,
        const std::vector<uint8_t>& pps);
    static std::vector<uint8_t> BuildAvcCRecord(
        const std::vector<uint8_t>& sps,
        const std::vector<uint8_t>& pps);

    // Convert Annex-B NAL stream to AVCC/HVCC format (4-byte length prefix per NAL)
    static std::vector<uint8_t> AnnexBToAvcc(const uint8_t* data, size_t size, bool is_h265);

    // Generate ISO 14496-12 initialization segment for H.264 (ftyp + moov with avc1/avcC)
    static std::vector<uint8_t> BuildInitSegment(const std::vector<uint8_t>& sps,
                                                 const std::vector<uint8_t>& pps,
                                                 uint32_t width, uint32_t height,
                                                 uint32_t timescale = 90000);

    // Generate ISO 14496-12 initialization segment for H.265 (ftyp + moov with hvc1/hvcC)
    static std::vector<uint8_t> BuildInitSegmentH265(const std::vector<uint8_t>& vps,
                                                     const std::vector<uint8_t>& sps,
                                                     const std::vector<uint8_t>& pps,
                                                     uint32_t width, uint32_t height,
                                                     uint32_t timescale = 90000);

    // Generate ISO 14496-12 media segment (moof + mdat)
    static std::vector<uint8_t> BuildMediaSegment(uint32_t sequence_number,
                                                  uint64_t pts_timescale,
                                                  uint32_t duration_timescale,
                                                  bool is_keyframe,
                                                  const uint8_t* avcc_data,
                                                  size_t avcc_size);
};

} // namespace nvr
