#include "nvr/media/stream_broker.h"

namespace nvr {

StreamBroker& StreamBroker::Instance() {
    static StreamBroker instance;
    return instance;
}

StreamBroker::SubscriptionId StreamBroker::Subscribe(int channel_id, StreamType stream_type, PacketCallback callback) {
    std::unique_lock<std::shared_mutex> w_lock(rw_mutex_);
    SubscriptionId id = next_sub_id_++;
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);

    auto it = topic_subscribers_.find(topic_key);
    auto new_list = std::make_shared<SubscriberList>(
        (it != topic_subscribers_.end() && it->second) ? *it->second : SubscriberList{}
    );
    new_list->push_back(SubEntry{id, std::move(callback)});

    topic_subscribers_[topic_key] = std::move(new_list);
    sub_to_topic_[id] = topic_key;
    return id;
}

void StreamBroker::Unsubscribe(SubscriptionId sub_id) {
    std::unique_lock<std::shared_mutex> w_lock(rw_mutex_);
    auto it_topic = sub_to_topic_.find(sub_id);
    if (it_topic == sub_to_topic_.end()) {
        return;
    }
    uint32_t topic_key = it_topic->second;
    sub_to_topic_.erase(it_topic);

    auto it = topic_subscribers_.find(topic_key);
    if (it != topic_subscribers_.end() && it->second) {
        auto new_list = std::make_shared<SubscriberList>();
        new_list->reserve(it->second->size() > 0 ? it->second->size() - 1 : 0);
        for (const auto& entry : *it->second) {
            if (entry.id != sub_id) {
                new_list->push_back(entry);
            }
        }
        if (new_list->empty()) {
            topic_subscribers_.erase(it);
        } else {
            it->second = std::move(new_list);
        }
    }
}

void StreamBroker::Publish(const MediaPacketPtr& packet) {
    if (!packet) {
        return;
    }

    uint32_t topic_key = MakeTopicKey(packet->channel_id, packet->stream_type);
    SubscriberListPtr subs;
    {
        std::shared_lock<std::shared_mutex> r_lock(rw_mutex_);
        auto it = topic_subscribers_.find(topic_key);
        if (it != topic_subscribers_.end()) {
            subs = it->second;
        }
    }

    if (subs) {
        for (const auto& entry : *subs) {
            entry.callback(packet);
        }
    }
}

size_t StreamBroker::GetSubscriberCount(int channel_id, StreamType stream_type) const {
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);
    std::shared_lock<std::shared_mutex> r_lock(rw_mutex_);
    auto it = topic_subscribers_.find(topic_key);
    if (it != topic_subscribers_.end() && it->second) {
        return it->second->size();
    }
    return 0;
}

} // namespace nvr
