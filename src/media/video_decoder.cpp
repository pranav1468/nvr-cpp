#include "nvr/media/video_decoder.h"
#include "nvr/media/buffer_allocator.h"
#include "nvr/common/logger.h"
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <algorithm>
#include <vector>
#include <dlfcn.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#ifndef V4L2_PIX_FMT_HEVC
#define V4L2_PIX_FMT_HEVC v4l2_fourcc('H', 'E', 'V', 'C')
#endif

namespace nvr {

namespace {

bool CopyNv12Frame(
    const uint8_t* src,
    size_t src_size,
    uint32_t width,
    uint32_t height,
    uint32_t src_stride,
    uint32_t dst_stride,
    uint8_t* dst,
    size_t dst_size)
{
    if (!src || !dst || width == 0 || height == 0 ||
        (width & 1) != 0 || (height & 1) != 0 ||
        src_stride < width || dst_stride < width) {
        return false;
    }

    uint64_t total_rows = static_cast<uint64_t>(height) + (height / 2);
    if (src_size < static_cast<uint64_t>(src_stride) * total_rows) {
        return false;
    }
    if (dst_size < static_cast<uint64_t>(dst_stride) * total_rows) {
        return false;
    }

    // Copy Y plane row-by-row
    for (uint32_t r = 0; r < height; ++r) {
        std::memcpy(dst + static_cast<size_t>(r) * dst_stride,
                    src + static_cast<size_t>(r) * src_stride,
                    width);
    }

    // Copy interleaved UV plane row-by-row
    const uint8_t* src_uv = src + static_cast<size_t>(height) * src_stride;
    uint8_t* dst_uv = dst + static_cast<size_t>(height) * dst_stride;
    uint32_t uv_rows = height / 2;
    for (uint32_t r = 0; r < uv_rows; ++r) {
        std::memcpy(dst_uv + static_cast<size_t>(r) * dst_stride,
                    src_uv + static_cast<size_t>(r) * src_stride,
                    width);
    }

    return true;
}

class BitstreamReader {
public:
    BitstreamReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint32_t ReadBits(size_t n) {
        uint32_t val = 0;
        for (size_t i = 0; i < n; ++i) {
            val = (val << 1) | ReadBit();
        }
        return val;
    }

    uint32_t ReadBit() {
        if (byte_offset_ >= size_) return 0;
        uint32_t bit = (data_[byte_offset_] >> (7 - bit_offset_)) & 1;
        bit_offset_++;
        if (bit_offset_ == 8) {
            bit_offset_ = 0;
            byte_offset_++;
        }
        return bit;
    }

    uint32_t ReadUE() {
        int zeros = 0;
        while (ReadBit() == 0 && byte_offset_ < size_) {
            zeros++;
            if (zeros >= 31) return 0;
        }
        if (zeros == 0) return 0;
        uint32_t val = ReadBits(zeros);
        return (1U << zeros) - 1 + val;
    }

private:
    const uint8_t* data_{nullptr};
    size_t size_{0};
    size_t byte_offset_{0};
    size_t bit_offset_{0};
};

bool ParseSpsDimensions(const uint8_t* data, size_t size, uint32_t& out_w, uint32_t& out_h) {
    if (size < 4) return false;
    // Strip emulation prevention bytes 0x000003
    std::vector<uint8_t> rbsp;
    rbsp.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        if (i + 2 < size && data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x03) {
            rbsp.push_back(0x00);
            rbsp.push_back(0x00);
            i += 2;
        } else {
            rbsp.push_back(data[i]);
        }
    }

    if (rbsp.size() < 4) return false;

    BitstreamReader reader(rbsp.data() + 1, rbsp.size() - 1);
    uint32_t profile_idc = reader.ReadBits(8);
    reader.ReadBits(16); // constraint flags + level_idc
    reader.ReadUE();     // seq_parameter_set_id

    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244) {
        uint32_t chroma_format_idc = reader.ReadUE();
        if (chroma_format_idc == 3) reader.ReadBit();
        reader.ReadUE(); // bit_depth_luma_minus8
        reader.ReadUE(); // bit_depth_chroma_minus8
        reader.ReadBit(); // qpprime_y_zero_transform_bypass_flag
        if (reader.ReadBit()) { // seq_scaling_matrix_present_flag
            for (int i = 0; i < ((chroma_format_idc != 3) ? 8 : 12); ++i) {
                if (reader.ReadBit()) {
                    int lastScale = 8, nextScale = 8;
                    int sizeOfScalingList = (i < 6) ? 16 : 64;
                    for (int j = 0; j < sizeOfScalingList; ++j) {
                        if (nextScale != 0) {
                            int delta_scale = reader.ReadUE();
                            nextScale = (lastScale + delta_scale) % 256;
                        }
                        lastScale = (nextScale == 0) ? lastScale : nextScale;
                    }
                }
            }
        }
    }

    reader.ReadUE(); // log2_max_frame_num_minus4
    uint32_t pic_order_cnt_type = reader.ReadUE();
    if (pic_order_cnt_type == 0) {
        reader.ReadUE(); // log2_max_pic_order_cnt_lsb_minus4
    } else if (pic_order_cnt_type == 1) {
        reader.ReadBit();
        reader.ReadUE();
        reader.ReadUE();
        uint32_t num_ref_frames_in_pic_order_cnt_cycle = reader.ReadUE();
        for (uint32_t i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; ++i) reader.ReadUE();
    }

    reader.ReadUE(); // max_num_ref_frames
    reader.ReadBit(); // gaps_in_frame_num_value_allowed_flag
    uint32_t pic_width_in_mbs_minus1 = reader.ReadUE();
    uint32_t pic_height_in_map_units_minus1 = reader.ReadUE();
    uint32_t frame_mbs_only_flag = reader.ReadBit();

    uint32_t w = (pic_width_in_mbs_minus1 + 1) * 16;
    uint32_t h = (2 - frame_mbs_only_flag) * ((pic_height_in_map_units_minus1 + 1) * 16);

    uint32_t frame_cropping_flag = reader.ReadBit();
    if (frame_cropping_flag) {
        uint32_t crop_left = reader.ReadUE();
        uint32_t crop_right = reader.ReadUE();
        uint32_t crop_top = reader.ReadUE();
        uint32_t crop_bottom = reader.ReadUE();
        uint32_t crop_x = (crop_left + crop_right) * 2;
        uint32_t crop_y = (crop_top + crop_bottom) * 2;
        if (crop_x < w && crop_y < h) {
            w -= crop_x;
            h -= crop_y;
        }
    }

    if (w > 0 && h > 0 && w <= 4096 && h <= 2160) {
        out_w = w;
        out_h = h;
        return true;
    }
    return false;
}

class SoftwareDecoderShim {
public:
    SoftwareDecoderShim() = default;
    ~SoftwareDecoderShim() { Close(); }

    bool Initialize(CodecType codec) {
        if (initialized_) return true;

        const char* codec_libs[] = {"libavcodec.so.58", "libavcodec.so"};
        for (const char* name : codec_libs) {
            avcodec_lib_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (avcodec_lib_) break;
        }
        const char* util_libs[] = {"libavutil.so.56", "libavutil.so"};
        for (const char* name : util_libs) {
            avutil_lib_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (avutil_lib_) break;
        }

        if (!avcodec_lib_ || !avutil_lib_) {
            Close();
            return false;
        }

        avcodec_version_ = reinterpret_cast<unsigned int(*)()>(dlsym(avcodec_lib_, "avcodec_version"));
        avutil_version_ = reinterpret_cast<unsigned int(*)()>(dlsym(avutil_lib_, "avutil_version"));
        if (!avcodec_version_ || !avutil_version_) {
            LOG_WARN << "[SoftwareDecoder] FFmpeg version functions are unavailable";
            Close();
            return false;
        }

        const unsigned int codec_major = avcodec_version_() >> 16;
        const unsigned int util_major = avutil_version_() >> 16;
        if (codec_major != 58 || util_major != 56) {
            LOG_WARN << "[SoftwareDecoder] Unsupported FFmpeg ABI: requires libavcodec 58 and libavutil 56 (found "
                     << codec_major << " / " << util_major << ")";
            Close();
            return false;
        }

        find_decoder_by_name_ = reinterpret_cast<const AVCodec*(*)(const char*)>(dlsym(avcodec_lib_, "avcodec_find_decoder_by_name"));
        alloc_context3_ = reinterpret_cast<AVCodecContext*(*)(const AVCodec*)>(dlsym(avcodec_lib_, "avcodec_alloc_context3"));
        open2_ = reinterpret_cast<int(*)(AVCodecContext*, const AVCodec*, void**)>(dlsym(avcodec_lib_, "avcodec_open2"));
        free_context_ = reinterpret_cast<void(*)(AVCodecContext**)>(dlsym(avcodec_lib_, "avcodec_free_context"));
        send_packet_ = reinterpret_cast<int(*)(AVCodecContext*, const AVPacket*)>(dlsym(avcodec_lib_, "avcodec_send_packet"));
        receive_frame_ = reinterpret_cast<int(*)(AVCodecContext*, AVFrame*)>(dlsym(avcodec_lib_, "avcodec_receive_frame"));
        packet_alloc_ = reinterpret_cast<AVPacket*(*)()>(dlsym(avcodec_lib_, "av_packet_alloc"));
        packet_free_ = reinterpret_cast<void(*)(AVPacket**)>(dlsym(avcodec_lib_, "av_packet_free"));
        packet_unref_ = reinterpret_cast<void(*)(AVPacket*)>(dlsym(avcodec_lib_, "av_packet_unref"));
        frame_alloc_ = reinterpret_cast<AVFrame*(*)()>(dlsym(avutil_lib_, "av_frame_alloc"));
        frame_free_ = reinterpret_cast<void(*)(AVFrame**)>(dlsym(avutil_lib_, "av_frame_free"));
        frame_unref_ = reinterpret_cast<void(*)(AVFrame*)>(dlsym(avutil_lib_, "av_frame_unref"));
        flush_buffers_ = reinterpret_cast<void(*)(AVCodecContext*)>(dlsym(avcodec_lib_, "avcodec_flush_buffers"));

        if (!find_decoder_by_name_ || !alloc_context3_ || !open2_ || !free_context_ ||
            !send_packet_ || !receive_frame_ || !frame_alloc_ || !frame_free_ ||
            !frame_unref_ || !packet_alloc_ || !packet_free_ || !packet_unref_) {
            Close();
            return false;
        }

        const char* codec_name = (codec == CodecType::H265) ? "hevc" : "h264";
        codec_ = find_decoder_by_name_(codec_name);
        if (!codec_) {
            Close();
            return false;
        }

        ctx_ = alloc_context3_(codec_);
        if (!ctx_ || open2_(ctx_, codec_, nullptr) != 0) {
            Close();
            return false;
        }

        pkt_ = packet_alloc_();
        frame_ = frame_alloc_();
        if (!pkt_ || !frame_) {
            Close();
            return false;
        }

        initialized_ = true;
        return true;
    }

    bool Decode(const uint8_t* data, size_t size, DecodedFrame& out_frame) {
        if (!initialized_ || !data || size == 0) return false;

        pkt_->data = const_cast<uint8_t*>(data);
        pkt_->size = static_cast<int>(size);

        int ret = send_packet_(ctx_, pkt_);
        pkt_->data = nullptr;
        pkt_->size = 0;
        if (packet_unref_) packet_unref_(pkt_);
        if (ret < 0) return false;

        ret = receive_frame_(ctx_, frame_);
        if (ret != 0) {
            if (frame_unref_) frame_unref_(frame_);
            return false;
        }

        if (frame_->width <= 0 || frame_->height <= 0 || frame_->width > 4096 || frame_->height > 2160 || !frame_->data[0]) {
            if (frame_unref_) frame_unref_(frame_);
            return false;
        }

        out_frame.width = frame_->width;
        out_frame.height = frame_->height;
        out_frame.stride = (frame_->width + 63) & ~size_t(63);
        size_t surface_size = BufferAllocator::CalculateSurfaceSize(out_frame.width, out_frame.height, 1, 1, 64);
        out_frame.data = BufferAllocator::Instance().Allocate(surface_size);
        if (out_frame.data.size() < surface_size) {
            out_frame.data.resize(surface_size, 0);
        }

        uint8_t* dst_y = out_frame.data.data();
        uint8_t* dst_uv = dst_y + (out_frame.stride * out_frame.height);

        bool copy_ok = false;
        if (frame_->format == AV_PIX_FMT_YUV420P && frame_->data[1] && frame_->data[2]) {
            for (int r = 0; r < frame_->height; ++r) {
                std::memcpy(dst_y + r * out_frame.stride, frame_->data[0] + r * frame_->linesize[0], frame_->width);
            }
            int uv_h = frame_->height / 2;
            int uv_w = frame_->width / 2;
            const uint8_t* src_u = frame_->data[1];
            const uint8_t* src_v = frame_->data[2];
            for (int r = 0; r < uv_h; ++r) {
                uint8_t* row_uv = dst_uv + r * out_frame.stride;
                const uint8_t* row_u = src_u + r * frame_->linesize[1];
                const uint8_t* row_v = src_v + r * frame_->linesize[2];
                for (int c = 0; c < uv_w; ++c) {
                    row_uv[2 * c] = row_u[c];
                    row_uv[2 * c + 1] = row_v[c];
                }
            }
            copy_ok = true;
        } else if (frame_->format == AV_PIX_FMT_NV12 && frame_->data[1]) {
            for (int r = 0; r < frame_->height; ++r) {
                std::memcpy(dst_y + r * out_frame.stride, frame_->data[0] + r * frame_->linesize[0], frame_->width);
            }
            int uv_h = frame_->height / 2;
            for (int r = 0; r < uv_h; ++r) {
                std::memcpy(dst_uv + r * out_frame.stride, frame_->data[1] + r * frame_->linesize[1], frame_->width);
            }
            copy_ok = true;
        }

        if (frame_unref_) frame_unref_(frame_);
        return copy_ok;
    }

    void Close() {
        if (frame_ && frame_free_) frame_free_(&frame_);
        if (pkt_ && packet_free_) packet_free_(&pkt_);
        if (ctx_ && free_context_) free_context_(&ctx_);
        if (avutil_lib_) { dlclose(avutil_lib_); avutil_lib_ = nullptr; }
        if (avcodec_lib_) { dlclose(avcodec_lib_); avcodec_lib_ = nullptr; }
        codec_ = nullptr;
        ctx_ = nullptr;
        pkt_ = nullptr;
        frame_ = nullptr;
        initialized_ = false;
    }

    void Flush() {
        if (initialized_ && ctx_ && flush_buffers_) {
            flush_buffers_(ctx_);
        }
    }

    bool IsAvailable() const { return initialized_; }

private:
    void* avcodec_lib_{nullptr};
    void* avutil_lib_{nullptr};
    const AVCodec* codec_{nullptr};
    AVCodecContext* ctx_{nullptr};
    AVPacket* pkt_{nullptr};
    AVFrame* frame_{nullptr};
    bool initialized_{false};

    unsigned int(*avcodec_version_)(){nullptr};
    unsigned int(*avutil_version_)(){nullptr};
    const AVCodec*(*find_decoder_by_name_)(const char*){nullptr};
    AVCodecContext*(*alloc_context3_)(const AVCodec*){nullptr};
    int(*open2_)(AVCodecContext*, const AVCodec*, void**){nullptr};
    void(*free_context_)(AVCodecContext**){nullptr};
    int(*send_packet_)(AVCodecContext*, const AVPacket*){nullptr};
    int(*receive_frame_)(AVCodecContext*, AVFrame*){nullptr};
    void(*flush_buffers_)(AVCodecContext*){nullptr};
    AVPacket*(*packet_alloc_)(){nullptr};
    void(*packet_free_)(AVPacket**){nullptr};
    void(*packet_unref_)(AVPacket*){nullptr};
    AVFrame*(*frame_alloc_)(){nullptr};
    void(*frame_free_)(AVFrame**){nullptr};
    void(*frame_unref_)(AVFrame*){nullptr};
};

struct V4l2MmapBuffer {
    void* start{nullptr};
    size_t length{0};
    int dmabuf_fd{-1};
    bool queued{false};
};

} // namespace

class VpuVideoDecoder : public IVideoDecoder {
public:
    VpuVideoDecoder(CodecType codec, uint32_t width, uint32_t height, bool prefer_vpu)
        : codec_(codec), width_(width), height_(height), prefer_vpu_(prefer_vpu) {}

    ~VpuVideoDecoder() override {
        Close();
    }

    bool Initialize(CodecType codec, uint32_t width, uint32_t height) override {
        codec_ = codec;
        if (width > 0) width_ = width;
        if (height > 0) height_ = height;

        // Probe for Linux V4L2 M2M hardware decoder device (NXP i.MX 95 VPU)
        if (prefer_vpu_) {
            const char* vpu_devs[] = {"/dev/video10", "/dev/video11", "/dev/video-dec0", "/dev/mxc_vpu"};
            for (const char* dev : vpu_devs) {
                if (access(dev, F_OK) == 0) {
                    int fd = open(dev, O_RDWR | O_NONBLOCK);
                    if (fd >= 0) {
                        struct v4l2_capability cap{};
                        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
                            uint32_t caps = cap.capabilities;
                            if (caps & V4L2_CAP_DEVICE_CAPS) {
                                caps = cap.device_caps;
                            }
                            if (caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) {
                                v4l2_fd_ = fd;
                                v4l2_device_path_ = dev;
                                is_hardware_ = true;
                                output_buf_type_ = (caps & V4L2_CAP_VIDEO_M2M_MPLANE) ?
                                    V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
                                capture_buf_type_ = (caps & V4L2_CAP_VIDEO_M2M_MPLANE) ?
                                    V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
                                
                                if (SetupV4l2Queues()) {
                                    LOG_INFO << "[VpuVideoDecoder] Hardware V4L2 M2M VPU device configured at " 
                                             << v4l2_device_path_;
                                    break;
                                }
                            }
                        }
                        CleanupV4l2Buffers();
                        close(fd);
                        v4l2_fd_ = -1;
                        is_hardware_ = false;
                    }
                }
            }
        }

        if (is_hardware_) {
            LOG_INFO << "[VpuVideoDecoder] Hardware V4L2 VPU streaming active at " << v4l2_device_path_;
        } else if (codec_ != CodecType::UNKNOWN) {
            sw_decoder_.Initialize(codec_);
            LOG_INFO << "[VpuVideoDecoder] Hardware V4L2 device not present, active engine: "
                     << (sw_decoder_.IsAvailable() ? "Software-FFmpeg" : "Aligned-Surface")
                     << " (" << CodecToString(codec_) << ")";
        }

        initialized_ = true;
        return true;
    }

    bool Decode(const MediaPacketPtr& packet, DecodedFramePtr& out_frame) override {
        if (!packet || packet->data.size() < 4 || packet->IsAudio()) {
            return false;
        }

        // Dynamically adopt packet's codec if unconfigured or changed
        if (packet->codec != CodecType::UNKNOWN && packet->codec != codec_) {
            codec_ = packet->codec;
            if (is_hardware_) {
                SetupV4l2Queues();
            } else {
                sw_decoder_.Close();
                sw_decoder_.Initialize(codec_);
            }
        }

        // Validate Annex-B start code
        const auto& d = packet->data;
        bool has_start_code = (d[0] == 0x00 && d[1] == 0x00 && (d[2] == 0x01 || (d.size() >= 4 && d[2] == 0x00 && d[3] == 0x01)));
        if (!has_start_code) {
            return false;
        }

        // Parse SPS if present to update dimensions dynamically
        if (packet->codec == CodecType::H264) {
            ExtractSpsAndConfigure(packet->data);
        }

        if (width_ == 0 || height_ == 0) {
            // Default to 1080p for MAIN, 360p for SUB if not yet parsed
            if (packet->stream_type == StreamType::MAIN) {
                width_ = 1920;
                height_ = 1080;
            } else {
                width_ = 640;
                height_ = 360;
            }
        }

        out_frame = std::make_shared<DecodedFrame>();
        out_frame->channel_id = packet->channel_id;
        out_frame->stream_type = packet->stream_type;
        out_frame->pts_us = packet->pts_us;
        out_frame->wall_time_ms = packet->wall_time_ms;
        out_frame->frame_index = frame_counter_++;
        out_frame->format = PixelFormat::NV12;

        // 1. Hardware V4L2 M2M decoding path
        if (is_hardware_ && v4l2_fd_ >= 0) {
            if (DecodeHardware(packet, out_frame)) {
                return true;
            }
        }

        // 2. Dynamic software decoding path
        if (sw_decoder_.IsAvailable()) {
            if (sw_decoder_.Decode(packet->data.data(), packet->data.size(), *out_frame)) {
                return true;
            }
        }

        // Decode failed on both hardware and software engines; do not report fake success
        return false;
    }

    void Flush() override {
        if (is_hardware_ && v4l2_fd_ >= 0) {
            if (output_streaming_) {
                ioctl(v4l2_fd_, VIDIOC_STREAMOFF, &output_buf_type_);
                output_streaming_ = false;
            }
            if (capture_streaming_) {
                ioctl(v4l2_fd_, VIDIOC_STREAMOFF, &capture_buf_type_);
                capture_streaming_ = false;
            }
            for (auto& b : output_buffers_) b.queued = false;
            for (size_t i = 0; i < capture_buffers_.size(); ++i) {
                struct v4l2_buffer qbuf{};
                qbuf.type = capture_buf_type_;
                qbuf.memory = V4L2_MEMORY_MMAP;
                qbuf.index = i;
                struct v4l2_plane qplanes[1]{};
                if (capture_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
                    qplanes[0].length = capture_buffers_[i].length;
                    qbuf.m.planes = qplanes;
                    qbuf.length = 1;
                }
                if (ioctl(v4l2_fd_, VIDIOC_QBUF, &qbuf) == 0) {
                    capture_buffers_[i].queued = true;
                }
            }
            if (ioctl(v4l2_fd_, VIDIOC_STREAMON, &output_buf_type_) == 0) {
                output_streaming_ = true;
            }
            if (ioctl(v4l2_fd_, VIDIOC_STREAMON, &capture_buf_type_) == 0) {
                capture_streaming_ = true;
            }
        }
        sw_decoder_.Flush();
    }

    void CleanupV4l2Buffers() {
        if (v4l2_fd_ >= 0) {
            if (output_streaming_) {
                ioctl(v4l2_fd_, VIDIOC_STREAMOFF, &output_buf_type_);
                output_streaming_ = false;
            }
            if (capture_streaming_) {
                ioctl(v4l2_fd_, VIDIOC_STREAMOFF, &capture_buf_type_);
                capture_streaming_ = false;
            }
        }
        for (auto& b : output_buffers_) {
            if (b.start && b.start != MAP_FAILED) {
                munmap(b.start, b.length);
                b.start = nullptr;
            }
        }
        output_buffers_.clear();
        for (auto& b : capture_buffers_) {
            if (b.dmabuf_fd >= 0) {
                close(b.dmabuf_fd);
                b.dmabuf_fd = -1;
            }
            if (b.start && b.start != MAP_FAILED) {
                munmap(b.start, b.length);
                b.start = nullptr;
            }
        }
        capture_buffers_.clear();

        // Release kernel buffer allocations via REQBUFS count = 0
        if (v4l2_fd_ >= 0) {
            struct v4l2_requestbuffers req_zero{};
            req_zero.count = 0;
            req_zero.memory = V4L2_MEMORY_MMAP;
            req_zero.type = output_buf_type_;
            ioctl(v4l2_fd_, VIDIOC_REQBUFS, &req_zero);

            req_zero.type = capture_buf_type_;
            ioctl(v4l2_fd_, VIDIOC_REQBUFS, &req_zero);
        }
    }

    void Close() override {
        CleanupV4l2Buffers();
        if (v4l2_fd_ >= 0) {
            close(v4l2_fd_);
            v4l2_fd_ = -1;
        }
        sw_decoder_.Close();
        is_hardware_ = false;
        initialized_ = false;
    }

    bool IsHardwareAccelerated() const override {
        return is_hardware_;
    }

    const char* GetName() const override {
        if (is_hardware_) return "V4L2-VPU-Hardware";
        if (sw_decoder_.IsAvailable()) return "FFmpeg-Software";
        return "VPU-Engine";
    }

    uint32_t GetWidth() const override { return width_; }
    uint32_t GetHeight() const override { return height_; }

private:
    bool DecodeHardware(const MediaPacketPtr& packet, DecodedFramePtr& out_frame) {
        uint32_t frame_w = (width_ > 0) ? width_ : (negotiated_width_ > 0 ? negotiated_width_ : 1920);
        uint32_t frame_h = (height_ > 0) ? height_ : (negotiated_height_ > 0 ? negotiated_height_ : 1080);
        frame_w = (frame_w & ~1U);
        frame_h = (frame_h & ~1U);
        uint32_t dst_stride = (frame_w + 63) & ~size_t(63);
        uint32_t src_stride = (negotiated_stride_ > 0) ? negotiated_stride_ : dst_stride;
        size_t surface_size = BufferAllocator::CalculateSurfaceSize(frame_w, frame_h, 1, 1, 64);

        int out_idx = -1;
        for (size_t i = 0; i < output_buffers_.size(); ++i) {
            if (!output_buffers_[i].queued) {
                out_idx = static_cast<int>(i);
                break;
            }
        }
        if (out_idx < 0) {
            struct v4l2_buffer dq{};
            dq.type = output_buf_type_;
            dq.memory = V4L2_MEMORY_MMAP;
            struct v4l2_plane dq_planes[1]{};
            if (output_buf_type_ == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
                dq.m.planes = dq_planes;
                dq.length = 1;
            }
            if (ioctl(v4l2_fd_, VIDIOC_DQBUF, &dq) == 0 && dq.index < output_buffers_.size()) {
                output_buffers_[dq.index].queued = false;
                out_idx = dq.index;
            }
        }

        if (out_idx >= 0 && output_buffers_[out_idx].start) {
            // Reject oversized compressed packets; never silently truncate encoded video
            if (packet->data.size() > output_buffers_[out_idx].length) {
                LOG_WARN << "[VpuVideoDecoder] Rejecting oversized compressed packet (" 
                         << packet->data.size() << " bytes > buffer limit " 
                         << output_buffers_[out_idx].length << " bytes)";
                return false;
            }
            size_t copy_bytes = packet->data.size();
            std::memcpy(output_buffers_[out_idx].start, packet->data.data(), copy_bytes);

            struct v4l2_buffer q{};
            q.type = output_buf_type_;
            q.memory = V4L2_MEMORY_MMAP;
            q.index = out_idx;
            q.bytesused = copy_bytes;
            // Associate packet PTS with output buffer so driver preserves timestamp
            q.timestamp.tv_sec = packet->pts_us / 1000000;
            q.timestamp.tv_usec = packet->pts_us % 1000000;
            struct v4l2_plane q_planes[1]{};
            if (output_buf_type_ == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
                q_planes[0].bytesused = copy_bytes;
                q_planes[0].length = output_buffers_[out_idx].length;
                q.m.planes = q_planes;
                q.length = 1;
            }
            if (ioctl(v4l2_fd_, VIDIOC_QBUF, &q) == 0) {
                output_buffers_[out_idx].queued = true;
            }
        }

        struct v4l2_buffer dq_cap{};
        dq_cap.type = capture_buf_type_;
        dq_cap.memory = V4L2_MEMORY_MMAP;
        struct v4l2_plane dq_cap_planes[1]{};
        if (capture_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            dq_cap.m.planes = dq_cap_planes;
            dq_cap.length = 1;
        }

        int dq_ret = ioctl(v4l2_fd_, VIDIOC_DQBUF, &dq_cap);
        if (dq_ret == 0 && dq_cap.index < capture_buffers_.size()) {
            uint32_t cap_idx = dq_cap.index;
            capture_buffers_[cap_idx].queued = false;

            out_frame->width = frame_w;
            out_frame->height = frame_h;
            out_frame->stride = dst_stride;
            out_frame->format = PixelFormat::NV12;
            // Safe buffer ownership: downstream consumes owned memory in out_frame->data.
            // dmabuf_fd is set to -1 to prevent premature reuse race conditions.
            out_frame->dmabuf_fd = -1;

            // Associate driver-propagated PTS from capture buffer
            int64_t cap_pts = static_cast<int64_t>(dq_cap.timestamp.tv_sec) * 1000000LL + dq_cap.timestamp.tv_usec;
            if (cap_pts > 0) {
                out_frame->pts_us = cap_pts;
            }

            out_frame->data = BufferAllocator::Instance().Allocate(surface_size);
            if (out_frame->data.size() < surface_size) {
                out_frame->data.resize(surface_size, 0);
            }

            bool copy_ok = CopyNv12Frame(
                static_cast<const uint8_t*>(capture_buffers_[cap_idx].start),
                capture_buffers_[cap_idx].length,
                frame_w,
                frame_h,
                src_stride,
                dst_stride,
                out_frame->data.data(),
                out_frame->data.size()
            );

            // Requeue capture buffer only AFTER copy has fully completed
            struct v4l2_buffer req_q{};
            req_q.type = capture_buf_type_;
            req_q.memory = V4L2_MEMORY_MMAP;
            req_q.index = cap_idx;
            struct v4l2_plane rq_planes[1]{};
            if (capture_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
                rq_planes[0].length = capture_buffers_[cap_idx].length;
                req_q.m.planes = rq_planes;
                req_q.length = 1;
            }
            if (ioctl(v4l2_fd_, VIDIOC_QBUF, &req_q) == 0) {
                capture_buffers_[cap_idx].queued = true;
            }
            return copy_ok;
        }

        if (dq_ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;
        }

        return false;
    }

    bool SetupV4l2Queues() {
        if (v4l2_fd_ < 0) return false;

        // Clean up previous buffers/streaming if re-configuring
        CleanupV4l2Buffers();

        // 1. Output queue format (bitstream input)
        struct v4l2_format fmt_out{};
        fmt_out.type = output_buf_type_;
        uint32_t pix_fmt = (codec_ == CodecType::H264) ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_HEVC;

        // Verify driver output queue actually advertises the requested codec
        bool has_codec_fmt = false;
        struct v4l2_fmtdesc fdesc{};
        fdesc.type = output_buf_type_;
        while (ioctl(v4l2_fd_, VIDIOC_ENUM_FMT, &fdesc) == 0) {
            if (fdesc.pixelformat == pix_fmt) {
                has_codec_fmt = true;
                break;
            }
            fdesc.index++;
        }
        if (!has_codec_fmt) {
            LOG_WARN << "[VpuVideoDecoder] Hardware V4L2 device does not support format " << CodecToString(codec_);
            CleanupV4l2Buffers();
            return false;
        }

        // Verify driver capture queue actually advertises NV12
        bool has_nv12 = false;
        struct v4l2_fmtdesc fdesc_cap{};
        fdesc_cap.type = capture_buf_type_;
        while (ioctl(v4l2_fd_, VIDIOC_ENUM_FMT, &fdesc_cap) == 0) {
            if (fdesc_cap.pixelformat == V4L2_PIX_FMT_NV12) {
                has_nv12 = true;
                break;
            }
            fdesc_cap.index++;
        }
        if (!has_nv12) {
            LOG_WARN << "[VpuVideoDecoder] Hardware V4L2 device does not support NV12 capture format";
            CleanupV4l2Buffers();
            return false;
        }

        if (output_buf_type_ == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            fmt_out.fmt.pix_mp.pixelformat = pix_fmt;
            fmt_out.fmt.pix_mp.width = (width_ > 0) ? width_ : 1920;
            fmt_out.fmt.pix_mp.height = (height_ > 0) ? height_ : 1080;
            fmt_out.fmt.pix_mp.num_planes = 1;
            fmt_out.fmt.pix_mp.plane_fmt[0].sizeimage = 1024 * 1024;
        } else {
            fmt_out.fmt.pix.pixelformat = pix_fmt;
            fmt_out.fmt.pix.width = (width_ > 0) ? width_ : 1920;
            fmt_out.fmt.pix.height = (height_ > 0) ? height_ : 1080;
            fmt_out.fmt.pix.sizeimage = 1024 * 1024;
        }

        if (ioctl(v4l2_fd_, VIDIOC_S_FMT, &fmt_out) < 0) {
            LOG_WARN << "[VpuVideoDecoder] Output VIDIOC_S_FMT failed";
            CleanupV4l2Buffers();
            return false;
        }

        // 2. Capture queue format (decoded NV12 frames)
        struct v4l2_format fmt_cap{};
        fmt_cap.type = capture_buf_type_;
        if (capture_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            fmt_cap.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
            fmt_cap.fmt.pix_mp.width = (width_ > 0) ? width_ : 1920;
            fmt_cap.fmt.pix_mp.height = (height_ > 0) ? height_ : 1080;
            fmt_cap.fmt.pix_mp.num_planes = 1;
        } else {
            fmt_cap.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
            fmt_cap.fmt.pix.width = (width_ > 0) ? width_ : 1920;
            fmt_cap.fmt.pix.height = (height_ > 0) ? height_ : 1080;
        }

        if (ioctl(v4l2_fd_, VIDIOC_S_FMT, &fmt_cap) < 0) {
            LOG_WARN << "[VpuVideoDecoder] Capture VIDIOC_S_FMT failed";
            CleanupV4l2Buffers();
            return false;
        }

        // Query driver negotiated format & stride
        struct v4l2_format gfmt_cap{};
        gfmt_cap.type = capture_buf_type_;
        if (ioctl(v4l2_fd_, VIDIOC_G_FMT, &gfmt_cap) == 0) {
            if (capture_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
                negotiated_width_ = gfmt_cap.fmt.pix_mp.width;
                negotiated_height_ = gfmt_cap.fmt.pix_mp.height;
                negotiated_stride_ = gfmt_cap.fmt.pix_mp.plane_fmt[0].bytesperline;
            } else {
                negotiated_width_ = gfmt_cap.fmt.pix.width;
                negotiated_height_ = gfmt_cap.fmt.pix.height;
                negotiated_stride_ = gfmt_cap.fmt.pix.bytesperline;
            }
        }
        if (negotiated_width_ == 0) negotiated_width_ = (width_ > 0) ? width_ : 1920;
        if (negotiated_height_ == 0) negotiated_height_ = (height_ > 0) ? height_ : 1080;
        if (negotiated_stride_ == 0) negotiated_stride_ = (negotiated_width_ + 63) & ~size_t(63);

        // 3. Request buffers for output queue
        struct v4l2_requestbuffers req_out{};
        req_out.count = 4;
        req_out.type = output_buf_type_;
        req_out.memory = V4L2_MEMORY_MMAP;
        if (ioctl(v4l2_fd_, VIDIOC_REQBUFS, &req_out) < 0 || req_out.count == 0) {
            LOG_WARN << "[VpuVideoDecoder] Output VIDIOC_REQBUFS failed";
            CleanupV4l2Buffers();
            return false;
        }

        output_buffers_.resize(req_out.count);
        for (uint32_t i = 0; i < req_out.count; ++i) {
            struct v4l2_buffer buf{};
            buf.type = output_buf_type_;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            struct v4l2_plane planes[1]{};
            if (output_buf_type_ == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
                buf.m.planes = planes;
                buf.length = 1;
            }
            if (ioctl(v4l2_fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                LOG_WARN << "[VpuVideoDecoder] Output VIDIOC_QUERYBUF failed for index " << i;
                CleanupV4l2Buffers();
                return false;
            }
            size_t length = (output_buf_type_ == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) ? planes[0].length : buf.length;
            off_t offset = (output_buf_type_ == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) ? planes[0].m.mem_offset : buf.m.offset;
            void* ptr = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, v4l2_fd_, offset);
            if (ptr == MAP_FAILED) {
                LOG_WARN << "[VpuVideoDecoder] Output mmap failed for index " << i;
                CleanupV4l2Buffers();
                return false;
            }
            output_buffers_[i].start = ptr;
            output_buffers_[i].length = length;
            output_buffers_[i].queued = false;
        }

        // 4. Request buffers for capture queue
        struct v4l2_requestbuffers req_cap{};
        req_cap.count = 4;
        req_cap.type = capture_buf_type_;
        req_cap.memory = V4L2_MEMORY_MMAP;
        if (ioctl(v4l2_fd_, VIDIOC_REQBUFS, &req_cap) < 0 || req_cap.count == 0) {
            LOG_WARN << "[VpuVideoDecoder] Capture VIDIOC_REQBUFS failed";
            CleanupV4l2Buffers();
            return false;
        }

        capture_buffers_.resize(req_cap.count);
        for (uint32_t i = 0; i < req_cap.count; ++i) {
            struct v4l2_buffer buf{};
            buf.type = capture_buf_type_;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            struct v4l2_plane planes[1]{};
            if (capture_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
                buf.m.planes = planes;
                buf.length = 1;
            }
            if (ioctl(v4l2_fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                LOG_WARN << "[VpuVideoDecoder] Capture VIDIOC_QUERYBUF failed for index " << i;
                CleanupV4l2Buffers();
                return false;
            }
            size_t length = (capture_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) ? planes[0].length : buf.length;
            off_t offset = (capture_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) ? planes[0].m.mem_offset : buf.m.offset;
            void* ptr = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, v4l2_fd_, offset);
            if (ptr == MAP_FAILED) {
                LOG_WARN << "[VpuVideoDecoder] Capture mmap failed for index " << i;
                CleanupV4l2Buffers();
                return false;
            }
            capture_buffers_[i].start = ptr;
            capture_buffers_[i].length = length;
            capture_buffers_[i].queued = false;
            capture_buffers_[i].dmabuf_fd = -1;

            // Initial queueing of capture buffers
            struct v4l2_buffer qbuf{};
            qbuf.type = capture_buf_type_;
            qbuf.memory = V4L2_MEMORY_MMAP;
            qbuf.index = i;
            struct v4l2_plane qplanes[1]{};
            if (capture_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
                qplanes[0].length = length;
                qbuf.m.planes = qplanes;
                qbuf.length = 1;
            }
            if (ioctl(v4l2_fd_, VIDIOC_QBUF, &qbuf) == 0) {
                capture_buffers_[i].queued = true;
            }
        }

        // 5. Start streaming on both queues independently
        if (ioctl(v4l2_fd_, VIDIOC_STREAMON, &output_buf_type_) == 0) {
            output_streaming_ = true;
        } else {
            LOG_WARN << "[VpuVideoDecoder] Output VIDIOC_STREAMON failed";
            CleanupV4l2Buffers();
            return false;
        }

        if (ioctl(v4l2_fd_, VIDIOC_STREAMON, &capture_buf_type_) == 0) {
            capture_streaming_ = true;
        } else {
            LOG_WARN << "[VpuVideoDecoder] Capture VIDIOC_STREAMON failed";
            CleanupV4l2Buffers();
            return false;
        }

        return true;
    }

private:
    void ExtractSpsAndConfigure(const std::vector<uint8_t>& data) {
        size_t i = 0;
        while (i + 4 < data.size()) {
            if (data[i] == 0x00 && data[i+1] == 0x00 &&
                ((data[i+2] == 0x01) || (data[i+2] == 0x00 && i + 3 < data.size() && data[i+3] == 0x01))) {
                size_t prefix_len = (data[i+2] == 0x01) ? 3 : 4;
                size_t nal_start = i + prefix_len;
                if (nal_start >= data.size()) break;
                uint8_t nal_type = data[nal_start] & 0x1F;
                if (nal_type == 7) { // SPS
                    size_t nal_end = data.size();
                    for (size_t j = nal_start + 1; j + 3 < data.size(); ++j) {
                        if (data[j] == 0x00 && data[j+1] == 0x00 &&
                            ((data[j+2] == 0x01) || (data[j+2] == 0x00 && data[j+3] == 0x01))) {
                            nal_end = j;
                            break;
                        }
                    }
                    uint32_t parsed_w = 0, parsed_h = 0;
                    if (ParseSpsDimensions(data.data() + nal_start, nal_end - nal_start, parsed_w, parsed_h)) {
                        width_ = parsed_w;
                        height_ = parsed_h;
                    }
                    break;
                }
                i = nal_start;
            } else {
                i++;
            }
        }
    }

    CodecType codec_{CodecType::H264};
    uint32_t width_{0};
    uint32_t height_{0};
    uint32_t negotiated_width_{0};
    uint32_t negotiated_height_{0};
    uint32_t negotiated_stride_{0};
    bool prefer_vpu_{true};
    bool is_hardware_{false};
    bool initialized_{false};
    std::string v4l2_device_path_;
    int v4l2_fd_{-1};
    uint32_t output_buf_type_{0};
    uint32_t capture_buf_type_{0};
    bool output_streaming_{false};
    bool capture_streaming_{false};
    uint64_t frame_counter_{0};

    std::vector<V4l2MmapBuffer> output_buffers_;
    std::vector<V4l2MmapBuffer> capture_buffers_;
    SoftwareDecoderShim sw_decoder_;
};

VideoDecoderPtr VideoDecoderFactory::Create(CodecType codec, uint32_t width, uint32_t height, bool prefer_vpu) {
    auto dec = std::make_unique<VpuVideoDecoder>(codec, width, height, prefer_vpu);
    dec->Initialize(codec, width, height);
    return dec;
}

} // namespace nvr

