#pragma once

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
    };

    // Extract all NAL units from an Annex-B byte buffer
    static std::vector<NalUnit> ParseAnnexBNals(const uint8_t* data, size_t size);

    // Extract SPS and PPS from Annex-B data
    static bool ExtractParameterSets(const uint8_t* data, size_t size, 
                                     std::vector<uint8_t>& out_sps, 
                                     std::vector<uint8_t>& out_pps);

    // Parse SPS to extract width, height, profile, level
    static bool ParseSpsDimensions(const uint8_t* sps, size_t size, 
                                   uint32_t& out_w, uint32_t& out_h);

    // Convert Annex-B NAL stream to AVCC format (4-byte length prefix per NAL)
    static std::vector<uint8_t> AnnexBToAvcc(const uint8_t* data, size_t size);

    // Generate ISO 14496-12 initialization segment (ftyp + moov)
    static std::vector<uint8_t> BuildInitSegment(const std::vector<uint8_t>& sps,
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
