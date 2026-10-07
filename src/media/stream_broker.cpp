#include "nvr/media/stream_broker.h"

namespace nvr {

StreamBroker& StreamBroker::Instance() {
    static StreamBroker instance;
    return instance;
}

StreamBroker::SubscriptionId StreamBroker::Subscribe(int channel_id, StreamType stream_type, PacketCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    SubscriptionId id = next_sub_id_++;
    subscriptions_[id] = Subscription{id, channel_id, stream_type, std::move(callback)};
    return id;
}

void StreamBroker::Unsubscribe(SubscriptionId sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscriptions_.erase(sub_id);
}

void StreamBroker::Publish(const MediaPacketPtr& packet) {
    if (!packet) {
        return;
    }

    std::vector<PacketCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, sub] : subscriptions_) {
            if (sub.channel_id == packet->channel_id && sub.stream_type == packet->stream_type) {
                callbacks.push_back(sub.callback);
            }
        }
    }

    for (const auto& cb : callbacks) {
        cb(packet);
    }
}

size_t StreamBroker::GetSubscriberCount(int channel_id, StreamType stream_type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [id, sub] : subscriptions_) {
        if (sub.channel_id == channel_id && sub.stream_type == stream_type) {
            count++;
        }
    }
    return count;
}

} // namespace nvr
