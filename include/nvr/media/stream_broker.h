#pragma once

#include "nvr/common/types.h"
#include <functional>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <memory>

#include <deque>

namespace nvr {

using PacketCallback = std::function<void(const MediaPacketPtr&)>;

class StreamBroker {
public:
    static StreamBroker& Instance();

    using SubscriptionId = uint64_t;

    SubscriptionId Subscribe(int channel_id, StreamType stream_type, PacketCallback callback, int replay_preroll_seconds = 0);

    void Unsubscribe(SubscriptionId sub_id);

    void Publish(const MediaPacketPtr& packet);

    size_t GetSubscriberCount(int channel_id, StreamType stream_type) const;

    // Rolling In-Memory Pre-Roll Ring Buffer (5-10s keyframe-aligned packets)
    void SetPreRollBufferDuration(int channel_id, StreamType stream_type, int duration_seconds,
                                  size_t max_bytes = 8 * 1024 * 1024, size_t max_packets = 300);
    void EnablePreRollBuffer(int channel_id, StreamType stream_type, bool enable);
    bool IsPreRollBufferEnabled(int channel_id, StreamType stream_type) const;

    std::vector<MediaPacketPtr> GetPreRollPackets(int channel_id, StreamType stream_type,
                                                  int duration_seconds = 5, bool keyframe_aligned = true) const;

    size_t GetRingBufferPacketCount(int channel_id, StreamType stream_type) const;
    size_t GetRingBufferBytes(int channel_id, StreamType stream_type) const;
    int64_t GetRingBufferDurationMs(int channel_id, StreamType stream_type) const;
    void ClearRingBuffer(int channel_id, StreamType stream_type);

    bool HasCachedParameters(int channel_id, StreamType stream_type) const;

    CodecType GetCachedCodec(int channel_id, StreamType stream_type) const;
    bool GetCachedAudioParams(int channel_id, StreamType stream_type, CodecType& codec, uint32_t& sample_rate, uint8_t& channels) const;
    bool GetCachedParameterSets(int channel_id, StreamType stream_type,
                                std::vector<uint8_t>& vps,
                                std::vector<uint8_t>& sps,
                                std::vector<uint8_t>& pps) const;
    size_t GetClientBufferMemoryBytes() const;

private:
    StreamBroker() = default;
    ~StreamBroker() = default;
    StreamBroker(const StreamBroker&) = delete;
    StreamBroker& operator=(const StreamBroker&) = delete;

    static inline uint32_t MakeTopicKey(int channel_id, StreamType stream_type) {
        return (static_cast<uint32_t>(channel_id) << 4) | (static_cast<uint32_t>(stream_type) & 0x0Fu);
    }

    struct SubEntry {
        SubscriptionId id{0};
        PacketCallback callback;
    };

    struct TopicParamCache {
        CodecType codec{CodecType::UNKNOWN};
        std::vector<uint8_t> vps;
        std::vector<uint8_t> sps;
        std::vector<uint8_t> pps;
        CodecType audio_codec{CodecType::UNKNOWN};
        uint32_t audio_sample_rate{0};
        uint8_t audio_channels{0};
    };

    struct RingBufferEntry {
        bool enabled{true};
        int duration_seconds{10};
        size_t max_bytes{8 * 1024 * 1024}; // 8 MB bound per topic
        size_t max_packets{300};           // ~10s @ 30 fps
        std::deque<MediaPacketPtr> packets;
        size_t current_bytes{0};
    };

    using SubscriberList = std::vector<SubEntry>;
    using SubscriberListPtr = std::shared_ptr<const SubscriberList>;

    mutable std::shared_mutex rw_mutex_;
    mutable std::mutex ring_buffer_mutex_;
    SubscriptionId next_sub_id_{1};

    // Topic-bucketed routing table with immutable shared vectors for zero-copy, lock-free dispatch
    std::unordered_map<uint32_t, SubscriberListPtr> topic_subscribers_;
    // Reverse map to enable O(1) unsubscription by SubscriptionId
    std::unordered_map<SubscriptionId, uint32_t> sub_to_topic_;
    // Per-topic cached parameter sets (VPS/SPS/PPS) replayed to late-joining subscribers
    std::unordered_map<uint32_t, TopicParamCache> topic_param_cache_;
    // Per-topic rolling in-memory ring buffer for pre-alarm event buffering
    std::unordered_map<uint32_t, RingBufferEntry> topic_ring_buffers_;
};

} // namespace nvr
