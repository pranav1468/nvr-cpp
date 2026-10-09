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

bool RecordingScheduler::StartChannelRecording(int channel_id, RecordMode mode) {
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

    auto state = std::make_shared<ChannelRecordState>();
    state->channel_id = channel_id;
    state->mode = mode;
    state->segmenter = std::move(segmenter);
    state->running = true;
    state->last_motion_time = std::chrono::steady_clock::now();

    // Start dedicated recording worker thread (isolated from network ingress)
    state->worker_thread = std::thread(&RecordingScheduler::ChannelWorkerLoop, this, state);

    // Subscribe to compressed MAIN stream packets from the StreamBroker
    state->subscription_id = StreamBroker::Instance().Subscribe(
        channel_id, 
        StreamType::MAIN,
        [this, channel_id](const MediaPacketPtr& packet) {
            EnqueuePacket(channel_id, packet);
        }
    );

    channels_[channel_id] = state;

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

void RecordingScheduler::SetSchedule(int channel_id, const std::vector<ScheduleTimeWindow>& schedule) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        it->second->schedule = schedule;
    }
}

bool RecordingScheduler::IsScheduleActiveNow(int channel_id, int64_t now_ms) const {
    std::vector<ScheduleTimeWindow> sched;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        sched = it->second->schedule;
    }

    if (sched.empty()) {
        return true; // Unrestricted 24/7 if no specific rules configured
    }

    time_t sec = static_cast<time_t>(now_ms / 1000);
    struct tm tm_buf;
    localtime_r(&sec, &tm_buf);
    int cur_wday = tm_buf.tm_wday; // 0=Sun, 1=Mon...
    int cur_mins = tm_buf.tm_hour * 60 + tm_buf.tm_min;

    for (const auto& w : sched) {
        int start_mins = w.start_hour * 60 + w.start_minute;
        int end_mins = w.end_hour * 60 + w.end_minute;

        if (start_mins == end_mins) {
            // Full 24-hour day window
            if ((w.days_mask & (1 << cur_wday)) != 0) {
                return true;
            }
        } else if (start_mins < end_mins) {
            // Same-day window (e.g., 08:00 to 18:00)
            if ((w.days_mask & (1 << cur_wday)) != 0) {
                if (cur_mins >= start_mins && cur_mins < end_mins) {
                    return true;
                }
            }
        } else {
            // Overnight window crossing midnight (e.g., 22:00 to 06:00)
            // 1. Evening portion on scheduled day (>= start_mins)
            if ((w.days_mask & (1 << cur_wday)) != 0 && cur_mins >= start_mins) {
                return true;
            }
            // 2. Early morning portion following scheduled day (< end_mins)
            int prev_wday = (cur_wday + 6) % 7;
            if ((w.days_mask & (1 << prev_wday)) != 0 && cur_mins < end_mins) {
                return true;
            }
        }
    }
    return false;
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
            // Periodic check for motion post-roll expiration
            if (state->mode == RecordMode::MOTION_ONLY && state->is_in_motion_recording) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - state->last_motion_time).count();
                if (!state->motion_active && elapsed_sec >= state->post_roll_seconds) {
                    state->is_in_motion_recording = false;
                    if (state->segmenter) {
                        state->segmenter->FlushAndStop();
                    }
                }
            }
            continue;
        }

        // Realignment check: if recovery active, skip orphaned non-keyframes until IDR keyframe
        if (state->needs_keyframe_resync && packet->IsVideo()) {
            if (!packet->is_keyframe) {
                state->dropped_packets_count++;
                continue;
            }
            state->needs_keyframe_resync = false;
        }

        if (state->mode == RecordMode::CONTINUOUS) {
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
                    // Flush pre-roll buffer from the oldest keyframe forward
                    std::lock_guard<std::mutex> p_lock(state->preroll_mutex);
                    size_t kf_idx = state->preroll_buffer.size();
                    for (size_t i = 0; i < state->preroll_buffer.size(); ++i) {
                        if (state->preroll_buffer[i]->is_keyframe && state->preroll_buffer[i]->IsVideo()) {
                            kf_idx = i;
                            break;
                        }
                    }
                    if (kf_idx < state->preroll_buffer.size()) {
                        for (size_t i = kf_idx; i < state->preroll_buffer.size(); ++i) {
                            state->segmenter->PushPacket(state->preroll_buffer[i]);
                        }
                    }
                    state->preroll_buffer.clear();
                    state->preroll_bytes = 0;
                }
                state->segmenter->PushPacket(packet);
            } else {
                if (state->is_in_motion_recording) {
                    state->is_in_motion_recording = false;
                    if (state->segmenter) {
                        state->segmenter->FlushAndStop();
                    }
                }

                std::lock_guard<std::mutex> p_lock(state->preroll_mutex);
                // If buffer is empty, only accept a video keyframe to guarantee GOP alignment
                if (state->preroll_buffer.empty() && (!packet->is_keyframe || !packet->IsVideo())) {
                    continue;
                }

                state->preroll_buffer.push_back(packet);
                state->preroll_bytes += packet->data.size();

                // Prune preroll_buffer to satisfy bounds:
                // 1) Time duration <= preroll_seconds
                // 2) preroll_bytes <= max_preroll_bytes
                // 3) preroll_buffer.size() <= max_preroll_packets
                // Pruning discards complete expired GOPs up to the next IDR keyframe
                auto calc_duration_ms = [&]() -> int64_t {
                    if (state->preroll_buffer.empty()) return 0;
                    if (state->preroll_buffer.back()->pts_us > 0 && state->preroll_buffer.front()->pts_us > 0) {
                        return (state->preroll_buffer.back()->pts_us - state->preroll_buffer.front()->pts_us) / 1000;
                    }
                    if (state->preroll_buffer.back()->wall_time_ms > 0 && state->preroll_buffer.front()->wall_time_ms > 0) {
                        return state->preroll_buffer.back()->wall_time_ms - state->preroll_buffer.front()->wall_time_ms;
                    }
                    return 0;
                };

                while (!state->preroll_buffer.empty()) {
                    int64_t dur_ms = calc_duration_ms();
                    bool needs_prune = (state->preroll_buffer.size() > state->max_preroll_packets) ||
                                      (state->preroll_bytes > state->max_preroll_bytes) ||
                                      (dur_ms > static_cast<int64_t>(state->preroll_seconds) * 1000);
                    if (!needs_prune) {
                        break;
                    }

                    // Locate next keyframe to discard the oldest GOP as a complete chunk
                    size_t next_kf_idx = 0;
                    for (size_t i = 1; i < state->preroll_buffer.size(); ++i) {
                        if (state->preroll_buffer[i]->is_keyframe && state->preroll_buffer[i]->IsVideo()) {
                            next_kf_idx = i;
                            break;
                        }
                    }

                    if (next_kf_idx == 0) {
                        // Only one keyframe exists in buffer.
                        // If hard byte or packet limits were breached, flush entire buffer to preserve memory budget
                        if (state->preroll_bytes > state->max_preroll_bytes ||
                            state->preroll_buffer.size() > state->max_preroll_packets) {
                            state->preroll_buffer.clear();
                            state->preroll_bytes = 0;
                        }
                        break;
                    }

                    // Discard oldest GOP up to next_kf_idx
                    for (size_t i = 0; i < next_kf_idx; ++i) {
                        if (state->preroll_bytes >= state->preroll_buffer.front()->data.size()) {
                            state->preroll_bytes -= state->preroll_buffer.front()->data.size();
                        } else {
                            state->preroll_bytes = 0;
                        }
                        state->preroll_buffer.pop_front();
                    }
                }
            }
        } else if (state->mode == RecordMode::SCHEDULED) {
            int64_t now_ms = (packet->wall_time_ms > 0) ? packet->wall_time_ms : time_utils::WallTimeMs();
            bool active = IsScheduleActiveNow(state->channel_id, now_ms);
            if (active) {
                state->segmenter->PushPacket(packet);
            } else {
                if (state->segmenter && state->segmenter->IsRecording()) {
                    state->segmenter->FlushAndStop();
                }
            }
        }
    }
}

} // namespace nvr
