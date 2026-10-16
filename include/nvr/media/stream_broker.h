#pragma once

#include "nvr/common/types.h"
#include <functional>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <memory>

namespace nvr {

using PacketCallback = std::function<void(const MediaPacketPtr&)>;

class StreamBroker {
public:
    static StreamBroker& Instance();

    using SubscriptionId = uint64_t;

    SubscriptionId Subscribe(int channel_id, StreamType stream_type, PacketCallback callback);

    void Unsubscribe(SubscriptionId sub_id);

    void Publish(const MediaPacketPtr& packet);

    size_t GetSubscriberCount(int channel_id, StreamType stream_type) const;

private:
    StreamBroker() = default;
    ~StreamBroker() = default;
    StreamBroker(const StreamBroker&) = delete;
    StreamBroker& operator=(const StreamBroker&) = delete;

    static inline uint32_t MakeTopicKey(int channel_id, StreamType stream_type) {
        return (static_cast<uint32_t>(channel_id) << 1) | (stream_type == StreamType::MAIN ? 1u : 0u);
    }

    struct SubEntry {
        SubscriptionId id{0};
        PacketCallback callback;
    };

    using SubscriberList = std::vector<SubEntry>;
    using SubscriberListPtr = std::shared_ptr<const SubscriberList>;

    mutable std::shared_mutex rw_mutex_;
    SubscriptionId next_sub_id_{1};

    // Topic-bucketed routing table with immutable shared vectors for zero-copy, lock-free dispatch
    std::unordered_map<uint32_t, SubscriberListPtr> topic_subscribers_;
    // Reverse map to enable O(1) unsubscription by SubscriptionId
    std::unordered_map<SubscriptionId, uint32_t> sub_to_topic_;
};

} // namespace nvr
