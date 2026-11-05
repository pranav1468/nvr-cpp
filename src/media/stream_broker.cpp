#include "nvr/media/stream_broker.h"

namespace nvr {

StreamBroker& StreamBroker::Instance() {
    static StreamBroker instance;
    return instance;
}

StreamBroker::SubscriptionId StreamBroker::Subscribe(int channel_id, StreamType stream_type, PacketCallback callback, int replay_preroll_seconds) {
    SubscriptionId id = 0;
    MediaPacketPtr replay_param_pkt;
    PacketCallback cb_copy = callback;
    {
        std::unique_lock<std::shared_mutex> w_lock(rw_mutex_);
        id = next_sub_id_++;
        uint32_t topic_key = MakeTopicKey(channel_id, stream_type);

        auto it = topic_subscribers_.find(topic_key);
        auto new_list = std::make_shared<SubscriberList>(
            (it != topic_subscribers_.end() && it->second) ? *it->second : SubscriberList{}
        );
        new_list->push_back(SubEntry{id, std::move(callback)});

        topic_subscribers_[topic_key] = std::move(new_list);
        sub_to_topic_[id] = topic_key;

        auto pit = topic_param_cache_.find(topic_key);
        if (pit != topic_param_cache_.end() && (!pit->second.sps.empty() || !pit->second.pps.empty() || !pit->second.vps.empty())) {
            replay_param_pkt = std::make_shared<MediaPacket>();
            replay_param_pkt->channel_id = channel_id;
            replay_param_pkt->stream_type = stream_type;
            replay_param_pkt->codec = pit->second.codec;
            if (replay_param_pkt->codec == CodecType::UNKNOWN) {
                if (!pit->second.vps.empty()) {
                    replay_param_pkt->codec = CodecType::H265;
                } else if (!pit->second.sps.empty()) {
                    replay_param_pkt->codec = DetectNalCodecFromParamSets(pit->second.sps.data(), pit->second.sps.size());
                }
            }
            replay_param_pkt->is_keyframe = false;
            replay_param_pkt->pts_us = 0;
            replay_param_pkt->dts_us = 0;
            auto append_nal = [&](const std::vector<uint8_t>& nal) {
                if (nal.empty()) return;
                replay_param_pkt->data.push_back(0x00);
                replay_param_pkt->data.push_back(0x00);
                replay_param_pkt->data.push_back(0x00);
                replay_param_pkt->data.push_back(0x01);
                replay_param_pkt->data.insert(replay_param_pkt->data.end(), nal.begin(), nal.end());
            };
            if (replay_param_pkt->codec == CodecType::H265) {
                append_nal(pit->second.vps);
            }
            append_nal(pit->second.sps);
            append_nal(pit->second.pps);
        }
    }

    if (replay_preroll_seconds > 0 && cb_copy) {
        auto preroll = GetPreRollPackets(channel_id, stream_type, replay_preroll_seconds, true);
        for (const auto& p : preroll) {
            cb_copy(p);
        }
    } else if (replay_param_pkt && !replay_param_pkt->data.empty() && cb_copy) {
        cb_copy(replay_param_pkt);
    }
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

    if (packet->HasParameterSets()) {
        std::unique_lock<std::shared_mutex> w_lock(rw_mutex_);
        auto& cache = topic_param_cache_[topic_key];
        const auto& d = packet->data;
        CodecType detected = (packet->codec != CodecType::UNKNOWN) 
            ? packet->codec 
            : DetectNalCodecFromParamSets(d.data(), d.size());

        if (cache.codec != CodecType::UNKNOWN && detected != CodecType::UNKNOWN && cache.codec != detected) {
            cache.vps.clear();
            cache.sps.clear();
            cache.pps.clear();
        }
        if (detected != CodecType::UNKNOWN) {
            cache.codec = detected;
            const_cast<MediaPacket*>(packet.get())->codec = detected;
        }
        if (cache.codec == CodecType::H264) {
            cache.vps.clear();
        }

        CodecType eff_codec = (cache.codec != CodecType::UNKNOWN) ? cache.codec : detected;
        size_t i = 0;
        while (i + 3 < d.size()) {
            if (d[i] == 0x00 && d[i + 1] == 0x00 &&
                (d[i + 2] == 0x01 || (d[i + 2] == 0x00 && i + 3 < d.size() && d[i + 3] == 0x01))) {
                size_t sc_len = (d[i + 2] == 0x01) ? 3 : 4;
                size_t nal_start = i + sc_len;
                size_t next = nal_start;
                while (next + 2 < d.size()) {
                    if (d[next] == 0x00 && d[next + 1] == 0x00 &&
                        (d[next + 2] == 0x01 || (next + 3 < d.size() && d[next + 2] == 0x00 && d[next + 3] == 0x01))) {
                        break;
                    }
                    ++next;
                }
                if (next + 2 >= d.size()) next = d.size();
                if (next > nal_start) {
                    uint8_t byte0 = d[nal_start];
                    if ((byte0 & 0x80) == 0) {
                        if (eff_codec == CodecType::H265 || (eff_codec == CodecType::UNKNOWN && next > nal_start + 1 && IsValidH265NalHeader(byte0, d[nal_start + 1]))) {
                            if (next > nal_start + 1) {
                                uint8_t nt = (byte0 >> 1) & 0x3F;
                                if (nt == 32) { cache.vps.assign(d.begin() + nal_start, d.begin() + next); cache.codec = CodecType::H265; }
                                else if (nt == 33) { cache.sps.assign(d.begin() + nal_start, d.begin() + next); cache.codec = CodecType::H265; }
                                else if (nt == 34) { cache.pps.assign(d.begin() + nal_start, d.begin() + next); cache.codec = CodecType::H265; }
                            }
                        } else if (eff_codec == CodecType::H264 || (eff_codec == CodecType::UNKNOWN && ((byte0 >> 5) & 0x03) != 0)) {
                            uint8_t nt = byte0 & 0x1F;
                            uint8_t nal_ref_idc = (byte0 >> 5) & 0x03;
                            if (nt == 7 && nal_ref_idc != 0) { cache.sps.assign(d.begin() + nal_start, d.begin() + next); cache.codec = CodecType::H264; cache.vps.clear(); }
                            else if (nt == 8 && nal_ref_idc != 0) { cache.pps.assign(d.begin() + nal_start, d.begin() + next); cache.codec = CodecType::H264; cache.vps.clear(); }
                        }
                    }
                }
                i = next;
            } else {
                ++i;
            }
        }
    }
    SubscriberListPtr subs;
    bool need_audio_cache_update = false;
    {
        std::shared_lock<std::shared_mutex> r_lock(rw_mutex_);
        if (packet->IsAudio()) {
            auto pit = topic_param_cache_.find(topic_key);
            if (pit == topic_param_cache_.end() ||
                pit->second.audio_codec != packet->codec ||
                (packet->sample_rate > 0 && pit->second.audio_sample_rate != packet->sample_rate) ||
                (packet->channels > 0 && pit->second.audio_channels != packet->channels)) {
                need_audio_cache_update = true;
            }
        }
        auto it = topic_subscribers_.find(topic_key);
        if (it != topic_subscribers_.end()) {
            subs = it->second;
        }
    }

    if (packet->is_keyframe && packet->IsVideo()) {
        std::unique_lock<std::shared_mutex> w_lock(rw_mutex_);
        topic_param_cache_[topic_key].last_keyframe = packet;
    }

    if (need_audio_cache_update) {
        std::unique_lock<std::shared_mutex> w_lock(rw_mutex_);
        auto& cache = topic_param_cache_[topic_key];
        cache.audio_codec = packet->codec;
        if (packet->sample_rate > 0) cache.audio_sample_rate = packet->sample_rate;
        if (packet->channels > 0) cache.audio_channels = packet->channels;
    }

    // Update rolling in-memory ring buffer for pre-alarm event capture
    {
        std::lock_guard<std::mutex> r_lock(ring_buffer_mutex_);
        auto& rb = topic_ring_buffers_[topic_key];
        if (rb.enabled) {
            rb.packets.push_back(packet);
            rb.current_bytes += packet->data.size();

            auto calc_dur_ms = [&]() -> int64_t {
                if (rb.packets.empty()) return 0;
                if (rb.packets.back()->pts_us >= 0 && rb.packets.front()->pts_us >= 0) {
                    return (rb.packets.back()->pts_us - rb.packets.front()->pts_us) / 1000;
                }
                if (rb.packets.back()->wall_time_ms >= 0 && rb.packets.front()->wall_time_ms >= 0) {
                    return rb.packets.back()->wall_time_ms - rb.packets.front()->wall_time_ms;
                }
                return 0;
            };

            while (rb.packets.size() > 1) {
                int64_t dur_ms = calc_dur_ms();
                bool needs_prune = (rb.packets.size() > rb.max_packets) ||
                                   (rb.current_bytes > rb.max_bytes) ||
                                   (dur_ms > static_cast<int64_t>(rb.duration_seconds) * 1000);
                if (!needs_prune) break;

                // Locate next video keyframe to evict complete expired GOP chunks
                size_t next_kf_idx = 0;
                for (size_t k = 1; k < rb.packets.size(); ++k) {
                    if (rb.packets[k]->is_keyframe && rb.packets[k]->IsVideo()) {
                        next_kf_idx = k;
                        break;
                    }
                }

                if (next_kf_idx == 0) {
                    // Only 1 keyframe exists in buffer.
                    // If hard limits breached, evict oldest to enforce memory ceiling
                    if (rb.current_bytes > rb.max_bytes || rb.packets.size() > rb.max_packets) {
                        if (rb.current_bytes >= rb.packets.front()->data.size()) {
                            rb.current_bytes -= rb.packets.front()->data.size();
                        } else {
                            rb.current_bytes = 0;
                        }
                        rb.packets.pop_front();
                    }
                    break;
                }

                // Evict oldest GOP up to next_kf_idx
                for (size_t k = 0; k < next_kf_idx; ++k) {
                    if (rb.current_bytes >= rb.packets.front()->data.size()) {
                        rb.current_bytes -= rb.packets.front()->data.size();
                    } else {
                        rb.current_bytes = 0;
                    }
                    rb.packets.pop_front();
                }
            }
        }
    }

    if (subs) {
        for (const auto& entry : *subs) {
            entry.callback(packet);
        }
    }
}

void StreamBroker::SetPreRollBufferDuration(int channel_id, StreamType stream_type, int duration_seconds,
                                            size_t max_bytes, size_t max_packets) {
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);
    std::lock_guard<std::mutex> r_lock(ring_buffer_mutex_);
    auto& rb = topic_ring_buffers_[topic_key];
    rb.duration_seconds = std::max(1, duration_seconds);
    rb.max_bytes = std::max<size_t>(1024 * 1024, max_bytes);
    rb.max_packets = std::max<size_t>(30, max_packets);
}

void StreamBroker::EnablePreRollBuffer(int channel_id, StreamType stream_type, bool enable) {
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);
    std::lock_guard<std::mutex> r_lock(ring_buffer_mutex_);
    topic_ring_buffers_[topic_key].enabled = enable;
    if (!enable) {
        topic_ring_buffers_[topic_key].packets.clear();
        topic_ring_buffers_[topic_key].current_bytes = 0;
    }
}

bool StreamBroker::IsPreRollBufferEnabled(int channel_id, StreamType stream_type) const {
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);
    std::lock_guard<std::mutex> r_lock(ring_buffer_mutex_);
    auto it = topic_ring_buffers_.find(topic_key);
    return (it != topic_ring_buffers_.end()) ? it->second.enabled : true;
}

std::vector<MediaPacketPtr> StreamBroker::GetPreRollPackets(int channel_id, StreamType stream_type,
                                                            int duration_seconds, bool keyframe_aligned) const {
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);
    std::vector<MediaPacketPtr> result;
    if (duration_seconds <= 0) return result;

    {
        std::lock_guard<std::mutex> r_lock(ring_buffer_mutex_);
        auto it = topic_ring_buffers_.find(topic_key);
        if (it == topic_ring_buffers_.end() || it->second.packets.empty()) {
            return result;
        }

        const auto& pkts = it->second.packets;
        int64_t target_dur_ms = static_cast<int64_t>(duration_seconds) * 1000;
        int64_t latest_ts_ms = 0;
        if (pkts.back()->pts_us >= 0) {
            latest_ts_ms = pkts.back()->pts_us / 1000;
        } else if (pkts.back()->wall_time_ms >= 0) {
            latest_ts_ms = pkts.back()->wall_time_ms;
        }

        size_t start_idx = 0;
        for (int i = static_cast<int>(pkts.size()) - 1; i >= 0; --i) {
            int64_t pkt_ts_ms = (pkts[i]->pts_us >= 0) ? (pkts[i]->pts_us / 1000) : pkts[i]->wall_time_ms;
            if (latest_ts_ms - pkt_ts_ms >= target_dur_ms) {
                start_idx = static_cast<size_t>(i);
                break;
            }
        }

        if (keyframe_aligned) {
            bool kf_found = false;
            for (int i = static_cast<int>(start_idx); i >= 0; --i) {
                if (pkts[i]->is_keyframe && pkts[i]->IsVideo()) {
                    start_idx = static_cast<size_t>(i);
                    kf_found = true;
                    break;
                }
            }
            if (!kf_found) {
                for (size_t i = start_idx; i < pkts.size(); ++i) {
                    if (pkts[i]->is_keyframe && pkts[i]->IsVideo()) {
                        start_idx = i;
                        kf_found = true;
                        break;
                    }
                }
            }
            if (!kf_found) {
                return result; // No keyframe found in buffer
            }

            // Include any parameter sets (SPS/PPS/VPS) that directly precede this keyframe
            while (start_idx > 0 && pkts[start_idx - 1]->IsVideo() &&
                   (pkts[start_idx - 1]->HasParameterSets() || !pkts[start_idx - 1]->HasVclSlice())) {
                --start_idx;
            }
        }

        result.reserve(pkts.size() - start_idx + 1);
        for (size_t i = start_idx; i < pkts.size(); ++i) {
            result.push_back(pkts[i]);
        }
        if (!result.empty() && result.front()->HasParameterSets()) {
            const_cast<MediaPacket*>(result.front().get())->is_keyframe = true;
        }
    }

    // Prepend cached parameter sets (VPS/SPS/PPS) if first packet lacks them
    if (!result.empty() && result.front()->IsVideo() && !result.front()->HasParameterSets()) {
        std::shared_lock<std::shared_mutex> p_lock(rw_mutex_);
        auto it_p = topic_param_cache_.find(topic_key);
        if (it_p != topic_param_cache_.end()) {
            const auto& c = it_p->second;
            if (!c.sps.empty() && !c.pps.empty()) {
                auto make_param_pkt = [&](const std::vector<uint8_t>& nal) -> MediaPacketPtr {
                    if (nal.empty()) return nullptr;
                    auto p = std::make_shared<MediaPacket>();
                    p->channel_id = channel_id;
                    p->stream_type = stream_type;
                    p->codec = (c.codec != CodecType::UNKNOWN) ? c.codec : result.front()->codec;
                    p->is_keyframe = true;
                    p->pts_us = result.front()->pts_us;
                    p->dts_us = result.front()->dts_us;
                    p->wall_time_ms = result.front()->wall_time_ms;
                    p->data.push_back(0x00);
                    p->data.push_back(0x00);
                    p->data.push_back(0x00);
                    p->data.push_back(0x01);
                    p->data.insert(p->data.end(), nal.begin(), nal.end());
                    return p;
                };

                std::vector<MediaPacketPtr> param_pkts;
                if (result.front()->codec == CodecType::H265 && !c.vps.empty()) {
                    if (auto p = make_param_pkt(c.vps)) param_pkts.push_back(p);
                }
                if (auto p = make_param_pkt(c.sps)) param_pkts.push_back(p);
                if (auto p = make_param_pkt(c.pps)) param_pkts.push_back(p);

                result.insert(result.begin(), param_pkts.begin(), param_pkts.end());
            }
        }
    }

    return result;
}

size_t StreamBroker::GetRingBufferPacketCount(int channel_id, StreamType stream_type) const {
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);
    std::lock_guard<std::mutex> r_lock(ring_buffer_mutex_);
    auto it = topic_ring_buffers_.find(topic_key);
    return (it != topic_ring_buffers_.end()) ? it->second.packets.size() : 0;
}

size_t StreamBroker::GetRingBufferBytes(int channel_id, StreamType stream_type) const {
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);
    std::lock_guard<std::mutex> r_lock(ring_buffer_mutex_);
    auto it = topic_ring_buffers_.find(topic_key);
    return (it != topic_ring_buffers_.end()) ? it->second.current_bytes : 0;
}

int64_t StreamBroker::GetRingBufferDurationMs(int channel_id, StreamType stream_type) const {
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);
    std::lock_guard<std::mutex> r_lock(ring_buffer_mutex_);
    auto it = topic_ring_buffers_.find(topic_key);
    if (it == topic_ring_buffers_.end() || it->second.packets.empty()) return 0;
    const auto& pkts = it->second.packets;
    if (pkts.back()->pts_us >= 0 && pkts.front()->pts_us >= 0) {
        return (pkts.back()->pts_us - pkts.front()->pts_us) / 1000;
    }
    if (pkts.back()->wall_time_ms >= 0 && pkts.front()->wall_time_ms >= 0) {
        return pkts.back()->wall_time_ms - pkts.front()->wall_time_ms;
    }
    return 0;
}

void StreamBroker::ClearRingBuffer(int channel_id, StreamType stream_type) {
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);
    std::lock_guard<std::mutex> r_lock(ring_buffer_mutex_);
    auto it = topic_ring_buffers_.find(topic_key);
    if (it != topic_ring_buffers_.end()) {
        it->second.packets.clear();
        it->second.current_bytes = 0;
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

bool StreamBroker::HasCachedParameters(int channel_id, StreamType stream_type) const {
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);
    std::shared_lock<std::shared_mutex> r_lock(rw_mutex_);
    auto it = topic_param_cache_.find(topic_key);
    if (it != topic_param_cache_.end()) {
        const auto& c = it->second;
        if (c.codec == CodecType::H265) {
            return !c.vps.empty() && !c.sps.empty() && !c.pps.empty();
        } else if (c.codec == CodecType::H264) {
            return !c.sps.empty() && !c.pps.empty();
        }
        if (!c.vps.empty()) {
            return !c.sps.empty() && !c.pps.empty();
        }
    }
    return false;
}

CodecType StreamBroker::GetCachedCodec(int channel_id, StreamType stream_type) const {
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);
    std::shared_lock<std::shared_mutex> r_lock(rw_mutex_);
    auto it = topic_param_cache_.find(topic_key);
    if (it != topic_param_cache_.end()) {
        if (it->second.codec != CodecType::UNKNOWN) {
            return it->second.codec;
        }
        if (!it->second.vps.empty()) {
            return CodecType::H265;
        }
        if (!it->second.sps.empty()) {
            return DetectNalCodecFromParamSets(it->second.sps.data(), it->second.sps.size());
        }
    }
    return CodecType::UNKNOWN;
}

bool StreamBroker::GetCachedAudioParams(int channel_id, StreamType stream_type, CodecType& codec, uint32_t& sample_rate, uint8_t& channels) const {
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);
    std::shared_lock<std::shared_mutex> r_lock(rw_mutex_);
    auto it = topic_param_cache_.find(topic_key);
    if (it != topic_param_cache_.end() && it->second.audio_codec != CodecType::UNKNOWN) {
        codec = it->second.audio_codec;
        sample_rate = it->second.audio_sample_rate;
        channels = it->second.audio_channels;
        return true;
    }
    return false;
}

bool StreamBroker::GetCachedParameterSets(int channel_id, StreamType stream_type,
                                          std::vector<uint8_t>& vps,
                                          std::vector<uint8_t>& sps,
                                          std::vector<uint8_t>& pps) const {
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);
    std::shared_lock<std::shared_mutex> r_lock(rw_mutex_);
    auto it = topic_param_cache_.find(topic_key);
    if (it == topic_param_cache_.end()) {
        return false;
    }
    vps = it->second.vps;
    sps = it->second.sps;
    pps = it->second.pps;
    return (!sps.empty() && !pps.empty());
}

MediaPacketPtr StreamBroker::GetLatestKeyframe(int channel_id, StreamType stream_type) const {
    uint32_t topic_key = MakeTopicKey(channel_id, stream_type);
    {
        std::lock_guard<std::mutex> r_lock(ring_buffer_mutex_);
        auto rb_it = topic_ring_buffers_.find(topic_key);
        if (rb_it != topic_ring_buffers_.end() && !rb_it->second.packets.empty()) {
            for (auto it = rb_it->second.packets.rbegin(); it != rb_it->second.packets.rend(); ++it) {
                if (*it && (*it)->IsVideo()) {
                    return (*it)->is_keyframe ? *it : nullptr;
                }
            }
        }
    }
    return nullptr;
}

size_t StreamBroker::GetClientBufferMemoryBytes() const {
    size_t total_bytes = 0;
    {
        std::shared_lock<std::shared_mutex> r_lock(rw_mutex_);
        for (const auto& kv : topic_param_cache_) {
            total_bytes += kv.second.vps.capacity() +
                           kv.second.sps.capacity() +
                           kv.second.pps.capacity();
        }
        for (const auto& kv : topic_subscribers_) {
            if (kv.second) {
                total_bytes += sizeof(SubscriberList) + kv.second->capacity() * sizeof(SubEntry);
            }
        }
    }
    {
        std::lock_guard<std::mutex> r_lock(ring_buffer_mutex_);
        for (const auto& kv : topic_ring_buffers_) {
            total_bytes += sizeof(RingBufferEntry) + kv.second.current_bytes;
        }
    }
    return total_bytes;
}

} // namespace nvr
