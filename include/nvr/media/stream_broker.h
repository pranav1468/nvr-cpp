#pragma once

#include "nvr/common/types.h"
#include <functional>
#include <vector>
#include <unordered_map>
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

    struct Subscription {
        SubscriptionId id;
        int channel_id;
        StreamType stream_type;
        PacketCallback callback;
    };

    mutable std::mutex mutex_;
    SubscriptionId next_sub_id_{1};
    std::unordered_map<SubscriptionId, Subscription> subscriptions_;
};

} // namespace nvr
