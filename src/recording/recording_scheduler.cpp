#include "nvr/recording/recording_scheduler.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"

namespace nvr {

RecordingScheduler& RecordingScheduler::Instance() {
    static RecordingScheduler instance;
    return instance;
}

RecordingScheduler::~RecordingScheduler() {
    StopAll();
}

void RecordingScheduler::Configure(const StorageConfig& storage_config) {
    std::lock_guard<std::mutex> lock(mutex_);
    storage_config_ = storage_config;
}

void RecordingScheduler::SetStoragePath(const std::string& new_path) {
    if (new_path.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    storage_config_.recording_path = new_path;
    std::error_code ec;
    std::filesystem::create_directories(new_path, ec);
    for (auto& pair : channels_) {
        if (pair.second && pair.second->segmenter) {
            pair.second->segmenter->SetOutputDirectory(new_path);
        }
    }
    LOG_INFO << "RecordingScheduler: Storage path updated to " << new_path << " across all active channels";
}

std::string RecordingScheduler::GetStoragePath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return storage_config_.recording_path.empty() ? "./recordings" : storage_config_.recording_path;
}

bool RecordingScheduler::StartChannelRecording(int channel_id, RecordMode mode) {
    std::shared_ptr<ChannelRecordState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (channels_.find(channel_id) != channels_.end()) {
            LOG_WARN << "[Channel " << channel_id << "] Recording already active";
            return true;
        }

        if (mode == RecordMode::DISABLED) {
            LOG_INFO << "[Channel " << channel_id << "] Recording mode is DISABLED";
            return true;
        }

        auto segmenter = std::make_shared<Segmenter>(
            channel_id, 
            storage_config_.recording_path, 
            storage_config_.segment_duration_seconds
        );

        state = std::make_shared<ChannelRecordState>();
        state->channel_id = channel_id;
        state->mode = mode;
        state->segmenter = std::move(segmenter);
        state->running = true;
        state->last_motion_time = std::chrono::steady_clock::now();
        state->last_event_time = std::chrono::steady_clock::now();

        auto ev_it = channel_event_states_.find(channel_id);
        if (ev_it != channel_event_states_.end()) {
            state->event_active = ev_it->second.first;
            state->current_event_type = ev_it->second.second;
        }

        channels_[channel_id] = state;
    }

    // Enable rolling in-memory ring buffer in StreamBroker for pre-alarm buffering
    StreamBroker::Instance().EnablePreRollBuffer(channel_id, StreamType::MAIN, true);
    StreamBroker::Instance().SetPreRollBufferDuration(channel_id, StreamType::MAIN, state->preroll_seconds);

    // Check if audio parameters are cached from stream broker and configure segmenter
    CodecType cached_acodec = CodecType::UNKNOWN;
    uint32_t cached_asrate = 8000;
    uint8_t cached_ach = 1;
    if (StreamBroker::Instance().GetCachedAudioParams(channel_id, StreamType::MAIN, cached_acodec, cached_asrate, cached_ach) &&
        cached_acodec != CodecType::UNKNOWN) {
        state->segmenter->ConfigureAudio(cached_acodec, cached_asrate, cached_ach);
    }

    // Start dedicated recording worker thread (isolated from network ingress)
    state->worker_thread = std::thread(&RecordingScheduler::ChannelWorkerLoop, this, state);

    // Subscribe to compressed MAIN stream packets from the StreamBroker (outside mutex_ to avoid self-deadlock)
    state->subscription_id = StreamBroker::Instance().Subscribe(
        channel_id, 
        StreamType::MAIN,
        [this, channel_id](const MediaPacketPtr& packet) {
            EnqueuePacket(channel_id, packet);
        }
    );

    LOG_INFO << "[Channel " << channel_id << "] Started async recording scheduler (Mode: " 
             << static_cast<int>(mode) << ", Target: " << storage_config_.recording_path << ")";
    return true;
}

void RecordingScheduler::StopChannelRecording(int channel_id) {
    std::shared_ptr<ChannelRecordState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) {
            return;
        }
        state = it->second;
        channels_.erase(it);
    }

    if (state) {
        StreamBroker::Instance().Unsubscribe(state->subscription_id);
        StreamBroker::Instance().EnablePreRollBuffer(channel_id, StreamType::MAIN, false);
        StreamBroker::Instance().ClearRingBuffer(channel_id, StreamType::MAIN);
        state->running = false;
        state->queue_cv.notify_all();
        if (state->worker_thread.joinable()) {
            state->worker_thread.join();
        }
        if (state->segmenter) {
            state->segmenter->FlushAndStop();
        }
        {
            std::lock_guard<std::mutex> p_lock(state->preroll_mutex);
            state->preroll_buffer.clear();
            state->preroll_bytes = 0;
        }
        LOG_INFO << "[Channel " << channel_id << "] Stopped recording";
    }
}

void RecordingScheduler::StopAll() {
    std::vector<std::shared_ptr<ChannelRecordState>> to_stop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [ch_id, state] : channels_) {
            to_stop.push_back(state);
        }
        channels_.clear();
    }

    for (auto& state : to_stop) {
        StreamBroker::Instance().Unsubscribe(state->subscription_id);
        StreamBroker::Instance().EnablePreRollBuffer(state->channel_id, StreamType::MAIN, false);
        StreamBroker::Instance().ClearRingBuffer(state->channel_id, StreamType::MAIN);
        state->running = false;
        state->queue_cv.notify_all();
        if (state->worker_thread.joinable()) {
            state->worker_thread.join();
        }
        if (state->segmenter) {
            state->segmenter->FlushAndStop();
        }
        {
            std::lock_guard<std::mutex> p_lock(state->preroll_mutex);
            state->preroll_buffer.clear();
            state->preroll_bytes = 0;
        }
    }
    LOG_INFO << "Stopped all active recording channels";
}

bool RecordingScheduler::IsChannelRecording(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    return (it != channels_.end() && it->second->segmenter && it->second->segmenter->IsRecording());
}

bool RecordingScheduler::IsChannelActive(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return channels_.find(channel_id) != channels_.end();
}

uint32_t RecordingScheduler::GetChannelFrameCount(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end() && it->second->segmenter) {
        return it->second->segmenter->GetCurrentFrameCount();
    }
    return 0;
}

uint64_t RecordingScheduler::GetChannelBytesWritten(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end() && it->second->segmenter) {
        return it->second->segmenter->GetCurrentBytesWritten();
    }
    return 0;
}

uint64_t RecordingScheduler::GetChannelDroppedPackets(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        return it->second->dropped_packets_count.load();
    }
    return 0;
}

uint64_t RecordingScheduler::GetChannelPrerollPacketCount(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        std::lock_guard<std::mutex> p_lock(it->second->preroll_mutex);
        return it->second->preroll_buffer.size();
    }
    return 0;
}

uint64_t RecordingScheduler::GetChannelPrerollBytes(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        std::lock_guard<std::mutex> p_lock(it->second->preroll_mutex);
        return it->second->preroll_bytes;
    }
    return 0;
}

void RecordingScheduler::SetPostRollSeconds(int channel_id, int seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        it->second->post_roll_seconds = std::max(0, seconds);
    }
}

void RecordingScheduler::SetPreRollSeconds(int channel_id, int seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        it->second->preroll_seconds = std::max(0, seconds);
        StreamBroker::Instance().SetPreRollBufferDuration(channel_id, StreamType::MAIN, it->second->preroll_seconds);
    }
}

void RecordingScheduler::SetMotionEvent(int channel_id, bool motion_active) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        it->second->motion_active = motion_active;
        if (motion_active) {
            it->second->has_motion_occurred = true;
            it->second->last_motion_time = std::chrono::steady_clock::now();
        }
        it->second->queue_cv.notify_one();
    }
}

void RecordingScheduler::SetEventActive(int channel_id, const std::string& event_type, bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    channel_event_states_[channel_id] = {active, event_type};
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        it->second->event_active = active;
        if (active) {
            it->second->has_event_occurred = true;
            it->second->last_event_time = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> ev_lock(it->second->event_mutex);
            it->second->current_event_type = event_type;
        }
        it->second->queue_cv.notify_one();
    }
}

void RecordingScheduler::TriggerEventRecording(int channel_id, const std::string& event_type, int pre_roll_sec, int post_roll_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    channel_event_states_[channel_id] = {true, event_type};
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        it->second->has_event_occurred = true;
        it->second->last_event_time = std::chrono::steady_clock::now();
        if (post_roll_sec > 0) {
            it->second->post_roll_seconds = post_roll_sec;
        }
        if (pre_roll_sec > 0) {
            it->second->preroll_seconds = pre_roll_sec;
            StreamBroker::Instance().SetPreRollBufferDuration(channel_id, StreamType::MAIN, pre_roll_sec);
        }
        {
            std::lock_guard<std::mutex> ev_lock(it->second->event_mutex);
            it->second->current_event_type = event_type;
        }
        it->second->queue_cv.notify_one();
    }
}

bool RecordingScheduler::IsEventActive(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        if (it->second->event_active) return true;
        if (it->second->has_event_occurred) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - it->second->last_event_time).count();
            return (elapsed_sec < it->second->post_roll_seconds);
        }
        return false;
    }
    auto ev_it = channel_event_states_.find(channel_id);
    if (ev_it != channel_event_states_.end()) {
        return ev_it->second.first;
    }
    return false;
}

std::string RecordingScheduler::GetCurrentEventType(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        std::lock_guard<std::mutex> ev_lock(it->second->event_mutex);
        return it->second->current_event_type;
    }
    auto ev_it = channel_event_states_.find(channel_id);
    if (ev_it != channel_event_states_.end()) {
        return ev_it->second.second;
    }
    return "";
}


void RecordingScheduler::EnqueuePacket(int channel_id, const MediaPacketPtr& packet) {
    if (!packet) return;

    std::shared_ptr<ChannelRecordState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = channels_.find(channel_id);
        if (it != channels_.end()) {
            state = it->second;
        }
    }

    if (!state || !state->running) return;

    std::lock_guard<std::mutex> q_lock(state->queue_mutex);

    // Recording queue overflow protection (128 compressed packets budget)
    if (state->input_queue.size() >= 128) {
        if (!state->needs_keyframe_resync) {
            state->needs_keyframe_resync = true;
            LOG_WARN << "[Channel " << channel_id << "] Recording queue overflow (size=" 
                     << state->input_queue.size() << "); dropping packets and awaiting IDR keyframe";
        }
        state->dropped_packets_count++;

        // Drop incoming non-keyframe video packet immediately to prevent orphaned frames
        if (packet->IsVideo() && !packet->is_keyframe) {
            return;
        }

        // If keyframe arrived or audio, make room in bounded queue
        while (state->input_queue.size() >= 128) {
            state->input_queue.pop_front();
            state->dropped_packets_count++;
        }
    }

    // If awaiting keyframe realignment after packet drop
    if (state->needs_keyframe_resync && packet->IsVideo()) {
        if (!packet->is_keyframe) {
            state->dropped_packets_count++;
            return;
        }
        // Recovered IDR keyframe boundary!
        state->needs_keyframe_resync = false;
    }

    state->input_queue.push_back(packet);
    state->queue_cv.notify_one();
}

void RecordingScheduler::FlushChannel(int channel_id) {
    std::shared_ptr<ChannelRecordState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = channels_.find(channel_id);
        if (it != channels_.end()) {
            state = it->second;
        }
    }

    if (state) {
        while (true) {
            {
                std::lock_guard<std::mutex> q_lock(state->queue_mutex);
                if (state->input_queue.empty()) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (state->segmenter) {
            state->segmenter->FlushAndStop();
        }
    }
}

void RecordingScheduler::ChannelWorkerLoop(std::shared_ptr<ChannelRecordState> state) {
    auto last_packet_time = std::chrono::steady_clock::now();

    while (state->running) {
        MediaPacketPtr packet;
        {
            std::unique_lock<std::mutex> lock(state->queue_mutex);
            state->queue_cv.wait_for(lock, std::chrono::milliseconds(100), [&]() {
                return !state->running || !state->input_queue.empty();
            });

            if (!state->running && state->input_queue.empty()) {
                break;
            }

            if (!state->input_queue.empty()) {
                packet = state->input_queue.front();
                state->input_queue.pop_front();
            }
        }

        if (!packet) {
            auto now = std::chrono::steady_clock::now();
            auto stall_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_packet_time).count();

            // Check if active recording has stalled (> 5000ms without packets)
            if (state->segmenter && state->segmenter->IsRecording() && stall_elapsed_ms >= 5000) {
                LOG_WARN << "[Channel " << state->channel_id
                         << "] Recording packet stall detected (" << stall_elapsed_ms
                         << " ms without packets). Finalizing active segment.";
                state->segmenter->FlushAndStop();
                state->needs_keyframe_resync = true;
                state->is_in_motion_recording = false;
                state->has_motion_occurred = false;
                state->is_in_event_recording = false;
                state->has_event_occurred = false;
            } else if (state->mode == RecordMode::MOTION_ONLY && state->is_in_motion_recording) {
                // Periodic check for motion post-roll expiration
                auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - state->last_motion_time).count();
                if (!state->motion_active && elapsed_sec >= state->post_roll_seconds) {
                    state->is_in_motion_recording = false;
                    if (state->segmenter) {
                        state->segmenter->FlushAndStop();
                    }
                }
            } else if (state->mode == RecordMode::EVENT_ONLY && state->is_in_event_recording) {
                // Periodic check for event post-roll expiration
                auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - state->last_event_time).count();
                if (!state->event_active && elapsed_sec >= state->post_roll_seconds) {
                    state->is_in_event_recording = false;
                    if (state->segmenter) {
                        state->segmenter->FlushAndStop();
                    }
                }
            }
            continue;
        }

        last_packet_time = std::chrono::steady_clock::now();

        // Realignment check: if recovery active, skip orphaned non-keyframes until IDR keyframe
        if (state->needs_keyframe_resync && packet->IsVideo()) {
            if (!packet->is_keyframe) {
                state->dropped_packets_count++;
                continue;
            }
            state->needs_keyframe_resync = false;
        }

        // Helper lambda: flush pre-roll packets from StreamBroker (or local buffer) to segmenter
        auto flush_preroll_packets = [&](std::shared_ptr<ChannelRecordState>& s) {
            // 1. First retrieve rolling keyframe-aligned pre-roll packets from StreamBroker
            auto preroll_pkts = StreamBroker::Instance().GetPreRollPackets(
                s->channel_id, StreamType::MAIN, s->preroll_seconds, true
            );
            if (!preroll_pkts.empty()) {
                for (const auto& p : preroll_pkts) {
                    s->segmenter->PushPacket(p);
                }
                std::lock_guard<std::mutex> p_lock(s->preroll_mutex);
                s->preroll_buffer.clear();
                s->preroll_bytes = 0;
                return;
            }

            // 2. Fallback to local preroll buffer if StreamBroker pre-roll buffer was empty
            std::lock_guard<std::mutex> p_lock(s->preroll_mutex);
            size_t kf_idx = s->preroll_buffer.size();
            for (size_t i = 0; i < s->preroll_buffer.size(); ++i) {
                if (s->preroll_buffer[i]->is_keyframe && s->preroll_buffer[i]->IsVideo()) {
                    kf_idx = i;
                    break;
                }
            }
            if (kf_idx < s->preroll_buffer.size()) {
                for (size_t i = kf_idx; i < s->preroll_buffer.size(); ++i) {
                    s->segmenter->PushPacket(s->preroll_buffer[i]);
                }
            }
            s->preroll_buffer.clear();
            s->preroll_bytes = 0;
        };

        // Helper lambda: store packet in local preroll buffer with bounded GOP pruning
        auto store_preroll_packet = [&](std::shared_ptr<ChannelRecordState>& s, const MediaPacketPtr& pkt) {
            std::lock_guard<std::mutex> p_lock(s->preroll_mutex);
            if (s->preroll_buffer.empty() && (!pkt->is_keyframe || !pkt->IsVideo())) {
                return;
            }
            s->preroll_buffer.push_back(pkt);
            s->preroll_bytes += pkt->data.size();

            auto calc_dur_ms = [&]() -> int64_t {
                if (s->preroll_buffer.empty()) return 0;
                if (s->preroll_buffer.back()->pts_us >= 0 && s->preroll_buffer.front()->pts_us >= 0) {
                    return (s->preroll_buffer.back()->pts_us - s->preroll_buffer.front()->pts_us) / 1000;
                }
                if (s->preroll_buffer.back()->wall_time_ms >= 0 && s->preroll_buffer.front()->wall_time_ms >= 0) {
                    return s->preroll_buffer.back()->wall_time_ms - s->preroll_buffer.front()->wall_time_ms;
                }
                return 0;
            };

            while (!s->preroll_buffer.empty()) {
                int64_t dur_ms = calc_dur_ms();
                bool needs_prune = (s->preroll_buffer.size() > s->max_preroll_packets) ||
                                  (s->preroll_bytes > s->max_preroll_bytes) ||
                                  (dur_ms > static_cast<int64_t>(s->preroll_seconds) * 1000);
                if (!needs_prune) break;

                size_t next_kf_idx = 0;
                for (size_t i = 1; i < s->preroll_buffer.size(); ++i) {
                    if (s->preroll_buffer[i]->is_keyframe && s->preroll_buffer[i]->IsVideo()) {
                        next_kf_idx = i;
                        break;
                    }
                }
                if (next_kf_idx == 0) {
                    if (s->preroll_bytes > s->max_preroll_bytes ||
                        s->preroll_buffer.size() > s->max_preroll_packets) {
                        s->preroll_buffer.clear();
                        s->preroll_bytes = 0;
                    }
                    break;
                }
                for (size_t i = 0; i < next_kf_idx; ++i) {
                    if (s->preroll_bytes >= s->preroll_buffer.front()->data.size()) {
                        s->preroll_bytes -= s->preroll_buffer.front()->data.size();
                    } else {
                        s->preroll_bytes = 0;
                    }
                    s->preroll_buffer.pop_front();
                }
            }
        };

        if (state->mode == RecordMode::CONTINUOUS || state->mode == RecordMode::SCHEDULED) {
            if (state->segmenter) {
                state->segmenter->PushPacket(packet);
            }
        } else if (state->mode == RecordMode::MOTION_ONLY) {
            auto now = std::chrono::steady_clock::now();
            if (state->motion_active) {
                state->has_motion_occurred = true;
                state->last_motion_time = now;
            }
            bool should_record = state->motion_active;
            if (!should_record && state->has_motion_occurred) {
                auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - state->last_motion_time).count();
                if (elapsed_sec < state->post_roll_seconds) {
                    should_record = true;
                } else {
                    state->has_motion_occurred = false;
                }
            }

            if (should_record) {
                if (!state->is_in_motion_recording) {
                    state->is_in_motion_recording = true;
                    flush_preroll_packets(state);
                }
                state->segmenter->PushPacket(packet);
            } else {
                if (state->is_in_motion_recording) {
                    state->is_in_motion_recording = false;
                    if (state->segmenter) {
                        state->segmenter->FlushAndStop();
                    }
                }
                store_preroll_packet(state, packet);
            }
        } else if (state->mode == RecordMode::EVENT_ONLY) {
            auto now = std::chrono::steady_clock::now();
            if (state->event_active) {
                state->has_event_occurred = true;
                state->last_event_time = now;
            }
            bool should_record = state->event_active;
            if (!should_record && state->has_event_occurred) {
                auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - state->last_event_time).count();
                if (elapsed_sec < state->post_roll_seconds) {
                    should_record = true;
                } else {
                    state->has_event_occurred = false;
                }
            }

            if (should_record) {
                if (!state->is_in_event_recording) {
                    state->is_in_event_recording = true;
                    flush_preroll_packets(state);
                }
                state->segmenter->PushPacket(packet);
            } else {
                if (state->is_in_event_recording) {
                    state->is_in_event_recording = false;
                    if (state->segmenter) {
                        state->segmenter->FlushAndStop();
                    }
                }
                store_preroll_packet(state, packet);
            }
        }
    }
}

nlohmann::json RecordingScheduler::GetChannelSchedule(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channel_schedules_.find(channel_id);
    if (it != channel_schedules_.end()) {
        auto sched = it->second;
        sched["is_active"] = (channels_.find(channel_id) != channels_.end());
        return sched;
    }

    nlohmann::json def;
    def["channel_id"] = channel_id;
    auto ch_it = channels_.find(channel_id);
    bool active = (ch_it != channels_.end());
    def["enabled"] = active;
    def["is_active"] = active;
    def["mode"] = active ? RecordModeToString(ch_it->second->mode) : "CONTINUOUS";
    def["pre_roll_sec"] = active ? ch_it->second->preroll_seconds : 5;
    def["post_roll_sec"] = active ? ch_it->second->post_roll_seconds : 10;
    def["intervals"] = nlohmann::json::array();
    return def;
}

bool RecordingScheduler::SetChannelSchedule(int channel_id, const nlohmann::json& payload) {
    RecordMode mode = RecordMode::CONTINUOUS;
    bool enabled = true;
    int pre_roll = 5;
    int post_roll = 10;

    if (payload.contains("mode")) {
        if (payload["mode"].is_number()) {
            mode = static_cast<RecordMode>(payload["mode"].get<int>());
        } else if (payload["mode"].is_string()) {
            std::string s = payload["mode"].get<std::string>();
            if (s == "motion" || s == "motion_only" || s == "2" || s == "MOTION_ONLY") mode = RecordMode::MOTION_ONLY;
            else if (s == "scheduled" || s == "3" || s == "SCHEDULED") mode = RecordMode::SCHEDULED;
            else if (s == "event" || s == "event_only" || s == "4" || s == "EVENT_ONLY") mode = RecordMode::EVENT_ONLY;
            else if (s == "disabled" || s == "0" || s == "DISABLED") mode = RecordMode::DISABLED;
            else mode = RecordMode::CONTINUOUS;
        }
    }
    if (payload.contains("enabled") && payload["enabled"].is_boolean()) {
        enabled = payload["enabled"].get<bool>();
    }
    if (payload.contains("pre_roll_sec") && payload["pre_roll_sec"].is_number()) {
        pre_roll = payload["pre_roll_sec"].get<int>();
    }
    if (payload.contains("post_roll_sec") && payload["post_roll_sec"].is_number()) {
        post_roll = payload["post_roll_sec"].get<int>();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        nlohmann::json sched = payload;
        sched["channel_id"] = channel_id;
        sched["mode"] = RecordModeToString(mode);
        sched["enabled"] = enabled;
        sched["pre_roll_sec"] = pre_roll;
        sched["post_roll_sec"] = post_roll;
        channel_schedules_[channel_id] = sched;
    }

    if (enabled && mode != RecordMode::DISABLED) {
        if (IsChannelRecording(channel_id)) {
            StopChannelRecording(channel_id);
        }
        StartChannelRecording(channel_id, mode);
        SetPreRollSeconds(channel_id, pre_roll);
        SetPostRollSeconds(channel_id, post_roll);
    } else {
        if (IsChannelRecording(channel_id)) {
            StopChannelRecording(channel_id);
        }
    }

    return true;
}

} // namespace nvr
