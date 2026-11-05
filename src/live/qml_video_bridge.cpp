#include "nvr/live/qml_video_bridge.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/recording/recording_scheduler.h"
#include "nvr/dispatcher/command_dispatcher.h"
#include "nvr/common/time_utils.h"
#include "nvr/common/logger.h"
#include "3rdparty/nlohmann/json.hpp"

#include <dlfcn.h>
#include <unistd.h>
#include <sstream>

using json = nlohmann::json;

namespace nvr {

namespace {

// Layout-compatible mirror of GstMapInfo (gst/gstmemory.h). GStreamer is loaded
// dynamically, so its headers are not available at build time.
struct GstMapInfoCompat {
    void* memory;
    int flags;
    uint8_t* data;
    size_t size;
    size_t maxsize;
    void* user_data[4];
    void* gst_reserved[4];
};

constexpr int kGstMapRead = 1;               // GST_MAP_READ
constexpr int kGstMemoryFlagReadonly = 2;    // GST_MEMORY_FLAG_READONLY
constexpr int kGstStateNull = 1;             // GST_STATE_NULL
constexpr int kGstStatePlaying = 4;          // GST_STATE_PLAYING

// Dynamic GStreamer runtime loader for NXP i.MX hardware acceleration
struct GstRuntimeShim {
    void* gst_lib{nullptr};
    void* app_lib{nullptr};
    void* gobject_lib{nullptr};
    void* alloc_lib{nullptr};

    int (*gst_init_check)(int*, char***, void**){nullptr};
    int (*gst_is_initialized)(){nullptr};
    void* (*gst_parse_launch)(const char*, void**){nullptr};
    int (*gst_element_set_state)(void*, int){nullptr};
    void* (*gst_bin_get_by_name)(void*, const char*){nullptr};
    void (*gst_object_unref)(void*){nullptr};
    void* (*gst_buffer_new_wrapped_full)(int, void*, size_t, size_t, size_t, void*, void (*)(void*)){nullptr};
    int (*gst_app_src_push_buffer)(void*, void*){nullptr};
    void* (*gst_element_factory_find)(const char*){nullptr};
    void (*g_object_set)(void*, const char*, ...){nullptr};

    void* (*gst_caps_from_string)(const char*){nullptr};
    void (*gst_caps_unref)(void*){nullptr};
    void (*gst_app_src_set_caps)(void*, const void*){nullptr};
    void* (*gst_buffer_new)(){nullptr};
    void (*gst_buffer_append_memory)(void*, void*){nullptr};

    // appsink pull + buffer mapping (G2D appsink conversion mode)
    void* (*gst_app_sink_try_pull_sample)(void*, uint64_t){nullptr};
    void* (*gst_sample_get_buffer)(void*){nullptr};
    void (*gst_mini_object_unref)(void*){nullptr};
    int (*gst_buffer_map)(void*, GstMapInfoCompat*, int){nullptr};
    void (*gst_buffer_unmap)(void*, GstMapInfoCompat*){nullptr};
    bool appsink_pull_supported{false};

    void* (*gst_dmabuf_allocator_new)(){nullptr};
    void* (*gst_dmabuf_allocator_alloc)(void*, int, size_t){nullptr};
    void* dmabuf_allocator{nullptr};
    bool dmabuf_supported{false};

    bool loaded{false};

    ~GstRuntimeShim() {
        if (dmabuf_allocator && gst_object_unref) {
            gst_object_unref(dmabuf_allocator);
            dmabuf_allocator = nullptr;
        }
        if (alloc_lib) dlclose(alloc_lib);
        if (gobject_lib) dlclose(gobject_lib);
        if (app_lib) dlclose(app_lib);
        if (gst_lib) dlclose(gst_lib);
    }

    bool Load() {
        if (loaded) return true;

        const char* gst_names[] = {
            "libgstreamer-1.0.so.0",
            "libgstreamer-1.0.so"
        };
        for (const char* name : gst_names) {
            gst_lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (gst_lib) break;
        }

        const char* app_names[] = {
            "libgstapp-1.0.so.0",
            "libgstapp-1.0.so"
        };
        for (const char* name : app_names) {
            app_lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (app_lib) break;
        }

        const char* gobject_names[] = {
            "libgobject-2.0.so.0",
            "libgobject-2.0.so"
        };
        for (const char* name : gobject_names) {
            gobject_lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (gobject_lib) break;
        }

        if (!gst_lib || !app_lib || !gobject_lib) {
            return false;
        }

        gst_init_check = reinterpret_cast<int(*)(int*, char***, void**)>(dlsym(gst_lib, "gst_init_check"));
        gst_is_initialized = reinterpret_cast<int(*)()>(dlsym(gst_lib, "gst_is_initialized"));
        gst_parse_launch = reinterpret_cast<void*(*)(const char*, void**)>(dlsym(gst_lib, "gst_parse_launch"));
        gst_element_set_state = reinterpret_cast<int(*)(void*, int)>(dlsym(gst_lib, "gst_element_set_state"));
        gst_bin_get_by_name = reinterpret_cast<void*(*)(void*, const char*)>(dlsym(gst_lib, "gst_bin_get_by_name"));
        gst_object_unref = reinterpret_cast<void(*)(void*)>(dlsym(gst_lib, "gst_object_unref"));
        gst_buffer_new_wrapped_full = reinterpret_cast<void*(*)(int, void*, size_t, size_t, size_t, void*, void (*)(void*))>(dlsym(gst_lib, "gst_buffer_new_wrapped_full"));
        gst_element_factory_find = reinterpret_cast<void*(*)(const char*)>(dlsym(gst_lib, "gst_element_factory_find"));
        gst_app_src_push_buffer = reinterpret_cast<int(*)(void*, void*)>(dlsym(app_lib, "gst_app_src_push_buffer"));
        g_object_set = reinterpret_cast<void(*)(void*, const char*, ...)>(dlsym(gobject_lib, "g_object_set"));

        gst_caps_from_string = reinterpret_cast<void*(*)(const char*)>(dlsym(gst_lib, "gst_caps_from_string"));
        gst_caps_unref = reinterpret_cast<void(*)(void*)>(dlsym(gst_lib, "gst_caps_unref"));
        gst_buffer_new = reinterpret_cast<void*(*)()>(dlsym(gst_lib, "gst_buffer_new"));
        gst_buffer_append_memory = reinterpret_cast<void(*)(void*, void*)>(dlsym(gst_lib, "gst_buffer_append_memory"));
        gst_app_src_set_caps = reinterpret_cast<void(*)(void*, const void*)>(dlsym(app_lib, "gst_app_src_set_caps"));

        gst_app_sink_try_pull_sample = reinterpret_cast<void*(*)(void*, uint64_t)>(dlsym(app_lib, "gst_app_sink_try_pull_sample"));
        gst_sample_get_buffer = reinterpret_cast<void*(*)(void*)>(dlsym(gst_lib, "gst_sample_get_buffer"));
        gst_mini_object_unref = reinterpret_cast<void(*)(void*)>(dlsym(gst_lib, "gst_mini_object_unref"));
        gst_buffer_map = reinterpret_cast<int(*)(void*, GstMapInfoCompat*, int)>(dlsym(gst_lib, "gst_buffer_map"));
        gst_buffer_unmap = reinterpret_cast<void(*)(void*, GstMapInfoCompat*)>(dlsym(gst_lib, "gst_buffer_unmap"));
        appsink_pull_supported = gst_app_sink_try_pull_sample && gst_sample_get_buffer &&
                                 gst_mini_object_unref && gst_buffer_map && gst_buffer_unmap;

        if (!gst_init_check || !gst_parse_launch || !gst_element_set_state ||
            !gst_bin_get_by_name || !gst_object_unref || !gst_buffer_new_wrapped_full ||
            !gst_app_src_push_buffer || !g_object_set) {
            return false;
        }

        // Attempt to load optional libgstallocators for hardware DMABUF zero-copy
        const char* alloc_names[] = {
            "libgstallocators-1.0.so.0",
            "libgstallocators-1.0.so"
        };
        for (const char* name : alloc_names) {
            alloc_lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (alloc_lib) break;
        }
        if (alloc_lib) {
            gst_dmabuf_allocator_new = reinterpret_cast<void*(*)()>(dlsym(alloc_lib, "gst_dmabuf_allocator_new"));
            gst_dmabuf_allocator_alloc = reinterpret_cast<void*(*)(void*, int, size_t)>(dlsym(alloc_lib, "gst_dmabuf_allocator_alloc"));
            if (gst_dmabuf_allocator_new && gst_dmabuf_allocator_alloc) {
                dmabuf_allocator = gst_dmabuf_allocator_new();
                dmabuf_supported = (dmabuf_allocator != nullptr);
            }
        }

        if (gst_is_initialized && !gst_is_initialized()) {
            void* err = nullptr;
            if (!gst_init_check(nullptr, nullptr, &err)) {
                return false;
            }
        } else if (!gst_is_initialized) {
            void* err = nullptr;
            if (!gst_init_check(nullptr, nullptr, &err)) {
                return false;
            }
        }

        loaded = true;
        return true;
    }
};

static GstRuntimeShim& GetGstShim() {
    static GstRuntimeShim shim;
    return shim;
}

} // namespace

// ---------------------------------------------------------------------------
// G2D appsink converter: NV12 (VPU output) -> BGRx on the G2D 2D engine.
//
// The worker thread pushes the decoded NV12 frame (wrapped, no copy) into
// appsrc and synchronously pulls the converted sample. The mapped output buffer
// is handed to Qt as a Format_RGB32 QImage without copying; the GstSample is
// released when the last DecodedFrame / QImage reference goes away.
// BGRx is chosen over RGB16 because it is Qt Quick's native upload layout:
// any other QImage format is converted again on the CPU before GPU upload.
// ---------------------------------------------------------------------------
struct QmlVideoBridge::G2dConverter {
    static constexpr uint64_t kPullTimeoutNs = 40ULL * 1000000ULL;  // one frame period at 25 fps
    static constexpr int kMaxConsecutiveFailures = 50;              // ~2 s of failed conversions

    std::mutex mtx;
    int tile_index{-1};
    void* pipeline{nullptr};
    void* appsrc{nullptr};
    void* appsink{nullptr};
    uint32_t width{0};
    uint32_t height{0};
    int consecutive_failures{0};
    bool disabled{false};
    bool first_success_logged{false};

    ~G2dConverter() { Teardown(); }

    void Teardown() {
        if (!pipeline) return;
        auto& shim = GetGstShim();
        shim.gst_element_set_state(pipeline, kGstStateNull);
        if (appsrc) shim.gst_object_unref(appsrc);
        if (appsink) shim.gst_object_unref(appsink);
        shim.gst_object_unref(pipeline);
        pipeline = appsrc = appsink = nullptr;
        width = height = 0;
    }

    bool Configure(uint32_t w, uint32_t h) {
        if (pipeline && w == width && h == height) return true;
        Teardown();

        std::ostringstream oss;
        oss << "appsrc name=src is-live=true format=time block=false "
            << "caps=\"video/x-raw,format=NV12,width=" << w << ",height=" << h << ",framerate=0/1\" ! "
            << "imxvideoconvert_g2d ! "
            << "video/x-raw,format=BGRx,width=" << w << ",height=" << h << " ! "
            << "appsink name=sink sync=false async=false max-buffers=1 drop=true "
            << "emit-signals=false enable-last-sample=false";
        const std::string desc = oss.str();

        auto& shim = GetGstShim();
        void* err = nullptr;
        void* pipe = shim.gst_parse_launch(desc.c_str(), &err);
        if (!pipe) {
            LOG_WARN << "[QmlVideoBridge] Tile " << tile_index << ": failed to build G2D converter: " << desc;
            return false;
        }
        void* src = shim.gst_bin_get_by_name(pipe, "src");
        void* sink = shim.gst_bin_get_by_name(pipe, "sink");
        if (!src || !sink) {
            if (src) shim.gst_object_unref(src);
            if (sink) shim.gst_object_unref(sink);
            shim.gst_object_unref(pipe);
            return false;
        }
        shim.gst_element_set_state(pipe, kGstStatePlaying);

        pipeline = pipe;
        appsrc = src;
        appsink = sink;
        width = w;
        height = h;
        LOG_INFO << "[QmlVideoBridge] Tile " << tile_index << ": G2D NV12->BGRx converter ready ("
                 << w << "x" << h << ", native resolution)";
        return true;
    }

    // Returns a BGRX32 frame backed by the mapped G2D output, or nullptr on failure.
    DecodedFramePtr Convert(const DecodedFramePtr& in) {
        auto& shim = GetGstShim();
        const uint32_t w = in->width;
        const uint32_t h = in->height;
        const size_t in_bytes = static_cast<size_t>(w) * h * 3 / 2;
        if (in->GetSizeBytes() < in_bytes) return nullptr;
        if (!Configure(w, h)) return nullptr;

        // Zero-copy input: GStreamer keeps the NV12 frame alive via the holder.
        auto* holder = new DecodedFramePtr(in);
        void* inbuf = shim.gst_buffer_new_wrapped_full(
            kGstMemoryFlagReadonly,
            const_cast<uint8_t*>(in->GetDataPtr()), in_bytes, 0, in_bytes,
            holder, [](void* p) { delete static_cast<DecodedFramePtr*>(p); });
        if (!inbuf) {
            delete holder;
            return nullptr;
        }
        if (shim.gst_app_src_push_buffer(appsrc, inbuf) != 0) {  // takes ownership
            return nullptr;
        }

        void* sample = shim.gst_app_sink_try_pull_sample(appsink, kPullTimeoutNs);
        if (!sample) return nullptr;

        struct MappedSample {
            void* sample{nullptr};
            void* buffer{nullptr};
            GstMapInfoCompat info{};
        };
        auto* mapped = new MappedSample();
        mapped->sample = sample;
        mapped->buffer = shim.gst_sample_get_buffer(sample);  // borrowed; the sample holds the ref
        const size_t out_bytes = static_cast<size_t>(w) * h * 4;
        if (!mapped->buffer || !shim.gst_buffer_map(mapped->buffer, &mapped->info, kGstMapRead)) {
            shim.gst_mini_object_unref(sample);
            delete mapped;
            return nullptr;
        }
        if (mapped->info.size < out_bytes) {
            shim.gst_buffer_unmap(mapped->buffer, &mapped->info);
            shim.gst_mini_object_unref(sample);
            delete mapped;
            return nullptr;
        }

        auto out = std::make_shared<DecodedFrame>();
        out->channel_id = in->channel_id;
        out->stream_type = in->stream_type;
        out->width = w;
        out->height = h;
        out->stride = w * 4;
        out->format = PixelFormat::BGRX32;
        out->pts_us = in->pts_us;
        out->wall_time_ms = in->wall_time_ms;
        out->frame_index = in->frame_index;
        out->external_data = mapped->info.data;
        out->external_size = mapped->info.size;
        out->external_owner = std::shared_ptr<void>(mapped, [](void* p) {
            auto* m = static_cast<MappedSample*>(p);
            auto& s = GetGstShim();
            s.gst_buffer_unmap(m->buffer, &m->info);
            s.gst_mini_object_unref(m->sample);
            delete m;
        });
        return out;
    }
};

bool QmlVideoBridge::IsG2dAppsinkModeAvailable() const {
    return hw_acceleration_enabled_ && g2d_gst_element_ && !qml6glsink_supported_ &&
           GetGstShim().appsink_pull_supported;
}

DecodedFramePtr QmlVideoBridge::ConvertFrameWithG2d(int tile_index, const DecodedFramePtr& frame) {
    // G2D input must be tightly packed NV12 (caps carry no stride information).
    if (frame->format != PixelFormat::NV12 || frame->width == 0 || frame->height == 0 ||
        (frame->width & 1) || (frame->height & 1) ||
        (frame->stride != 0 && frame->stride != frame->width)) {
        return nullptr;
    }

    std::shared_ptr<G2dConverter> conv;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsG2dAppsinkModeAvailable()) return nullptr;
        auto& slot = g2d_converters_[tile_index];
        if (!slot) {
            slot = std::make_shared<G2dConverter>();
            slot->tile_index = tile_index;
        }
        conv = slot;
    }

    // Conversion runs outside mutex_ so the GUI thread and other tiles never wait on G2D.
    std::lock_guard<std::mutex> conv_lock(conv->mtx);
    if (conv->disabled) return nullptr;

    DecodedFramePtr out = conv->Convert(frame);
    if (out) {
        conv->consecutive_failures = 0;
        if (!conv->first_success_logged) {
            conv->first_success_logged = true;
            LOG_INFO << "[QmlVideoBridge] Tile " << tile_index
                     << ": live frames now colour-converted on G2D (zero CPU copy into Qt)";
        }
        return out;
    }

    if (++conv->consecutive_failures >= G2dConverter::kMaxConsecutiveFailures) {
        conv->disabled = true;
        conv->Teardown();
        LOG_WARN << "[QmlVideoBridge] Tile " << tile_index << ": G2D conversion failed "
                 << conv->consecutive_failures
                 << " times in a row; falling back to CPU colour conversion for this tile";
    }
    return nullptr;
}

QmlVideoBridge::QmlVideoBridge() {
    InitGstRuntime();
}

QmlVideoBridge::~QmlVideoBridge() {
    Shutdown();
}

QmlVideoBridge& QmlVideoBridge::Instance() {
    static QmlVideoBridge instance;
    return instance;
}

void QmlVideoBridge::InitGstRuntime() {
    if (gst_initialized_) return;

    auto& shim = GetGstShim();
    if (!shim.Load()) {
        LOG_INFO << "[QmlVideoBridge] GStreamer runtime libraries not detected; using decoupled software frame bridge.";
        gst_initialized_ = true;
        g2d_supported_ = false;
        qml6glsink_supported_ = false;
        return;
    }

    gst_initialized_ = true;

    // Check if NXP Vivante G2D hardware converter element is available
    void* g2d_fact = shim.gst_element_factory_find("imxvideoconvert_g2d");
    if (g2d_fact) {
        g2d_supported_ = true;
        g2d_gst_element_ = true;
        shim.gst_object_unref(g2d_fact);
    } else {
        // Also check if NXP G2D kernel device node exists
        if (access("/dev/mxc_g2d", F_OK) == 0 || access("/dev/g2d", F_OK) == 0) {
            g2d_supported_ = true;
        }
    }

    // Check if Qt Quick 6 OpenGL sink element is available
    void* qml_sink_fact = shim.gst_element_factory_find("qml6glsink");
    if (qml_sink_fact) {
        qml6glsink_supported_ = true;
        shim.gst_object_unref(qml_sink_fact);
    }

    dmabuf_supported_ = shim.dmabuf_supported;

    if (g2d_supported_ && qml6glsink_supported_) {
        LOG_INFO << "[QmlVideoBridge] NXP i.MX G2D hardware converter and qml6glsink display sink successfully detected. Zero-copy rendering pipeline ready (dmabuf="
                 << (dmabuf_supported_ ? "yes" : "no") << ").";
    } else {
        LOG_INFO << "[QmlVideoBridge] Hardware G2D / qml6glsink elements not present on host (g2d=" 
                 << (g2d_supported_ ? "yes" : "no") << ", qml6glsink=" << (qml6glsink_supported_ ? "yes" : "no")
                 << ", dmabuf=" << (dmabuf_supported_ ? "yes" : "no")
                 << "); operating in decoupled software bridge mode.";
    }
}

bool QmlVideoBridge::Initialize(int window_width, int window_height) {
    std::lock_guard<std::mutex> lock(mutex_);
    width_ = window_width;
    height_ = window_height;
    tiles_.clear();
    for (auto& pair : hw_pipelines_) {
        CleanupTilePipeline(pair.second);
    }
    hw_pipelines_.clear();
    return true;
}

std::string QmlVideoBridge::BuildG2dPipelineDescription(int src_width, int src_height,
                                                      int dst_width, int dst_height,
                                                      const std::string& pixel_format) {
    if (src_width <= 0) src_width = 640;
    if (src_height <= 0) src_height = 360;
    if (dst_width <= 0) dst_width = 1920;
    if (dst_height <= 0) dst_height = 1080;

    std::string fmt = pixel_format.empty() ? "NV12" : pixel_format;

    std::string converter = Instance().IsG2dHardwareSupported() ? "imxvideoconvert_g2d" : "videoconvert";
    std::ostringstream oss;
    oss << "appsrc name=src is-live=true format=time do-timestamp=true "
        << "caps=\"video/x-raw,format=" << fmt << ",width=" << src_width << ",height=" << src_height << "\" ! "
        << "queue max-size-buffers=1 leaky=downstream ! "
        << converter << " ! "
        << "video/x-raw,format=RGBA,width=" << dst_width << ",height=" << dst_height << " ! "
        << "glupload ! "
        << "video/x-raw(memory:GLMemory),format=RGBA,width=" << dst_width << ",height=" << dst_height << ",texture-target=2D ! "
        << "qml6glsink name=sink";
    return oss.str();
}

bool QmlVideoBridge::AttachTileVideoSurface(int tile_index, void* qquick_surface_item) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& p = hw_pipelines_[tile_index];
    p.tile_index = tile_index;
    p.qquick_surface = qquick_surface_item;

    if (p.gst_sink && qquick_surface_item) {
        auto& shim = GetGstShim();
        if (shim.loaded && shim.g_object_set) {
            shim.g_object_set(p.gst_sink, "widget", qquick_surface_item, nullptr);
            LOG_INFO << "[QmlVideoBridge] Bound QML VideoSurface item to qml6glsink for tile " << tile_index;
        }
    }
    return true;
}

void QmlVideoBridge::DetachTileVideoSurface(int tile_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = hw_pipelines_.find(tile_index);
    if (it != hw_pipelines_.end()) {
        CleanupTilePipeline(it->second);
        hw_pipelines_.erase(it);
    }
}

bool QmlVideoBridge::StartTilePipeline(ImxG2dTilePipeline& p, int src_w, int src_h, int dst_w, int dst_h) {
    CleanupTilePipeline(p);

    p.src_width = src_w;
    p.src_height = src_h;
    p.dst_width = dst_w;
    p.dst_height = dst_h;
    p.pixel_format = "NV12";
    p.description = BuildG2dPipelineDescription(src_w, src_h, dst_w, dst_h);

    if (!IsG2dHardwareSupported() || !IsQml6GlSinkSupported()) {
        return false;
    }

    auto& shim = GetGstShim();
    if (!shim.Load()) return false;

    void* err = nullptr;
    void* pipe = shim.gst_parse_launch(p.description.c_str(), &err);
    if (!pipe) {
        LOG_WARN << "[QmlVideoBridge] Failed to construct G2D hardware pipeline for tile " << p.tile_index;
        return false;
    }

    void* appsrc = shim.gst_bin_get_by_name(pipe, "src");
    void* sink = shim.gst_bin_get_by_name(pipe, "sink");

    if (!appsrc || !sink) {
        LOG_WARN << "[QmlVideoBridge] Failed to resolve src/sink in G2D pipeline for tile " << p.tile_index;
        if (appsrc) shim.gst_object_unref(appsrc);
        if (sink) shim.gst_object_unref(sink);
        shim.gst_object_unref(pipe);
        return false;
    }

    if (p.qquick_surface) {
        shim.g_object_set(sink, "widget", p.qquick_surface, nullptr);
    }

    // Set pipeline to PLAYING state (GST_STATE_PLAYING = 4)
    shim.gst_element_set_state(pipe, 4);

    p.gst_pipeline = pipe;
    p.gst_appsrc = appsrc;
    p.gst_sink = sink;
    p.is_active = true;
    p.is_hw_accelerated = true;

    LOG_INFO << "[QmlVideoBridge] G2D glupload qml6glsink pipeline running for tile " << p.tile_index;
    return true;
}

void QmlVideoBridge::CleanupTilePipeline(ImxG2dTilePipeline& p) {
    if (p.gst_pipeline) {
        auto& shim = GetGstShim();
        if (shim.loaded) {
            shim.gst_element_set_state(p.gst_pipeline, 1); // GST_STATE_NULL
            if (p.gst_appsrc) shim.gst_object_unref(p.gst_appsrc);
            if (p.gst_sink) shim.gst_object_unref(p.gst_sink);
            shim.gst_object_unref(p.gst_pipeline);
        }
        p.gst_pipeline = nullptr;
        p.gst_appsrc = nullptr;
        p.gst_sink = nullptr;
    }
    p.is_active = false;
    p.is_hw_accelerated = false;
}

bool QmlVideoBridge::PushFrameToTilePipeline(int tile_index, const DecodedFramePtr& frame) {
    if (!frame) return false;

    auto it = hw_pipelines_.find(tile_index);
    if (it == hw_pipelines_.end() || !it->second.is_active) return false;

    auto& p = it->second;

    if (!p.gst_appsrc) return false;

    auto& shim = GetGstShim();
    if (!shim.loaded || !shim.gst_app_src_push_buffer) return false;

    // 1. Dynamic Caps Renegotiation:
    // If incoming frame resolution or format changes, reconfigure appsrc dynamically
    // without tearing down the pipeline or flashing black frames.
    std::string fmt = PixelFormatToString(frame->format);
    if (frame->width > 0 && frame->height > 0 &&
        (p.src_width != static_cast<int>(frame->width) ||
         p.src_height != static_cast<int>(frame->height) ||
         p.pixel_format != fmt)) {
        if (shim.gst_caps_from_string && shim.gst_app_src_set_caps && shim.gst_caps_unref) {
            std::string caps_str = "video/x-raw,format=" + fmt +
                                   ",width=" + std::to_string(frame->width) +
                                   ",height=" + std::to_string(frame->height);
            void* caps = shim.gst_caps_from_string(caps_str.c_str());
            if (caps) {
                shim.gst_app_src_set_caps(p.gst_appsrc, caps);
                shim.gst_caps_unref(caps);
                p.src_width = frame->width;
                p.src_height = frame->height;
                p.pixel_format = fmt;
                LOG_INFO << "[QmlVideoBridge] Dynamically renegotiated tile " << tile_index
                         << " input caps: " << caps_str;
            }
        }
    }

    void* buf = nullptr;

    // 2. Direct DMABUF Hardware Zero-Copy:
    // When the frame originates from V4L2 VPU hardware decoding with a valid dmabuf_fd,
    // import the file descriptor directly into GStreamer GLMemory without CPU copies.
    if (frame->dmabuf_fd >= 0 && shim.dmabuf_supported && shim.gst_buffer_new && shim.gst_buffer_append_memory) {
        int dup_fd = dup(frame->dmabuf_fd);
        if (dup_fd >= 0) {
            size_t frame_bytes = frame->GetSizeBytes();
            if (frame_bytes == 0 && frame->width > 0 && frame->height > 0) {
                frame_bytes = (static_cast<size_t>(frame->width) * frame->height * 3) / 2; // NV12
            }
            void* mem = shim.gst_dmabuf_allocator_alloc(shim.dmabuf_allocator, dup_fd, frame_bytes);
            if (mem) {
                buf = shim.gst_buffer_new();
                if (buf) {
                    shim.gst_buffer_append_memory(buf, mem);
                } else {
                    close(dup_fd);
                }
            } else {
                close(dup_fd);
            }
        }
    }

    // 3. Asynchronous Buffer Lifetime Refcount Safety:
    // Wrap CPU/allocated frame data with a GDestroyNotify callback that holds a shared_ptr
    // reference to the DecodedFrame. This guarantees that even if upstream threads drop or
    // reuse the frame, memory remains valid until GStreamer unrefs the buffer.
    if (!buf && !frame->data.empty() && shim.gst_buffer_new_wrapped_full) {
        auto* holder = new DecodedFramePtr(frame);
        buf = shim.gst_buffer_new_wrapped_full(
            2, // GST_MEMORY_FLAG_READONLY
            const_cast<uint8_t*>(frame->data.data()),
            frame->data.size(),
            0,
            frame->data.size(),
            holder,
            [](void* data) {
                delete static_cast<DecodedFramePtr*>(data);
            }
        );
        if (!buf) {
            delete holder;
            return false;
        }
    }

    if (!buf) return false;

    int ret = shim.gst_app_src_push_buffer(p.gst_appsrc, buf);
    return (ret == 0); // GST_FLOW_OK
}

void QmlVideoBridge::RenderTile(int tile_index, int channel_id, const DecodedFramePtr& frame) {
    if (!frame) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hw_acceleration_enabled_) {
            auto it = hw_pipelines_.find(tile_index);
            if (it != hw_pipelines_.end()) {
                auto& p = it->second;
                p.channel_id = channel_id;
                if (!p.is_active && p.qquick_surface != nullptr) {
                    int target_w = (p.dst_width > 0) ? p.dst_width : width_;
                    int target_h = (p.dst_height > 0) ? p.dst_height : height_;
                    StartTilePipeline(p, frame->width, frame->height, target_w, target_h);
                }
                if (p.is_active) {
                    p.frames_submitted++;
                    if (PushFrameToTilePipeline(tile_index, frame)) {
                        p.frames_rendered++;
                    } else {
                        p.frames_dropped++;
                    }
                }
            }
        }
    }

    // Hardware G2D offload: when qml6glsink is not attached but G2D is present on NXP i.MX,
    // convert NV12 to BGRx (Format_RGB32) on the hardware G2D engine outside Qt GUI thread.
    DecodedFramePtr effective_frame = frame;
    if (frame->format == PixelFormat::NV12 && IsG2dAppsinkModeAvailable()) {
        auto g2d_frame = ConvertFrameWithG2d(tile_index, frame);
        if (g2d_frame) {
            effective_frame = std::move(g2d_frame);
        }
    }

    // Always update software bridge and frame revision so decoupled viewers,
    // thumbnail generators, and QML view state bindings remain fully responsive.
    FrameRenderCallback cb;
    uint64_t rev = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& t = tiles_[tile_index];
        t.channel_id = channel_id;
        t.revision++;
        t.frame = effective_frame;
        rev = t.revision;
        cb = render_callback_;
    }

    if (cb) {
        cb(tile_index, channel_id, rev);
    }
}

void QmlVideoBridge::ClearTile(int tile_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    tiles_.erase(tile_index);
    g2d_converters_.erase(tile_index);
    auto it = hw_pipelines_.find(tile_index);
    if (it != hw_pipelines_.end()) {
        CleanupTilePipeline(it->second);
        hw_pipelines_.erase(it);
    }
}

void QmlVideoBridge::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : hw_pipelines_) {
        CleanupTilePipeline(pair.second);
    }
    hw_pipelines_.clear();
    g2d_converters_.clear();
    tiles_.clear();
}

const char* QmlVideoBridge::GetBackendName() const {
    return "QmlVideoBridge";
}

DecodedFramePtr QmlVideoBridge::GetTileFrame(int tile_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tiles_.find(tile_index);
    return (it != tiles_.end()) ? it->second.frame : nullptr;
}

uint64_t QmlVideoBridge::GetTileRevision(int tile_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tiles_.find(tile_index);
    return (it != tiles_.end()) ? it->second.revision : 0;
}

int QmlVideoBridge::GetTileChannelId(int tile_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tiles_.find(tile_index);
    return (it != tiles_.end()) ? it->second.channel_id : -1;
}

bool QmlVideoBridge::IsTileHardwareAccelerated(int tile_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = hw_pipelines_.find(tile_index);
    return (it != hw_pipelines_.end()) ? it->second.is_hw_accelerated : false;
}

QmlVideoBridge::G2dPipelineMetrics QmlVideoBridge::GetTileHardwareMetrics(int tile_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    G2dPipelineMetrics m;
    m.tile_index = tile_index;
    auto it = hw_pipelines_.find(tile_index);
    if (it != hw_pipelines_.end()) {
        const auto& p = it->second;
        m.channel_id = p.channel_id;
        m.frames_submitted = p.frames_submitted;
        m.frames_rendered = p.frames_rendered;
        m.frames_dropped = p.frames_dropped;
        m.is_active = p.is_active;
        m.hardware_accelerated = p.is_hw_accelerated;
        m.pipeline_description = p.description;
    }
    return m;
}

bool QmlVideoBridge::IsG2dHardwareSupported() const {
    return g2d_supported_;
}

bool QmlVideoBridge::IsQml6GlSinkSupported() const {
    return qml6glsink_supported_;
}

bool QmlVideoBridge::IsDmabufSupported() const {
    return dmabuf_supported_;
}

bool QmlVideoBridge::IsHardwareAccelerationEnabled() const {
    return hw_acceleration_enabled_;
}

void QmlVideoBridge::SetHardwareAccelerationEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    hw_acceleration_enabled_ = enabled;
}

void QmlVideoBridge::SetSessionToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_token_ = token;
}

std::string QmlVideoBridge::GetSessionToken() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_token_;
}

void QmlVideoBridge::SetLocalUserRole(UserRole role) {
    std::lock_guard<std::mutex> lock(mutex_);
    local_role_ = role;
}

UserRole QmlVideoBridge::GetLocalUserRole() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return local_role_;
}

void QmlVideoBridge::RequestLayout(int layout_count) {
    CommandEnvelope env;
    env.command_id = "ui_" + std::to_string(time_utils::MonotonicMs());
    env.action = "layout.set";
    env.source = CommandSource::UI_QML;
    env.auth_token = GetSessionToken();
    env.payload["count"] = layout_count;
    env.timestamp_ms = time_utils::WallTimeMs();
    CommandDispatcher::Instance().Dispatch(env);
}

void QmlVideoBridge::RequestFullscreen(int channel_id) {
    CommandEnvelope env;
    env.command_id = "ui_" + std::to_string(time_utils::MonotonicMs());
    env.action = "layout.fullscreen";
    env.source = CommandSource::UI_QML;
    env.auth_token = GetSessionToken();
    env.payload["channel_id"] = channel_id;
    env.timestamp_ms = time_utils::WallTimeMs();
    CommandDispatcher::Instance().Dispatch(env);
}

void QmlVideoBridge::RequestExitFullscreen() {
    CommandEnvelope env;
    env.command_id = "ui_" + std::to_string(time_utils::MonotonicMs());
    env.action = "layout.fullscreen";
    env.source = CommandSource::UI_QML;
    env.auth_token = GetSessionToken();
    env.payload["exit"] = true;
    env.timestamp_ms = time_utils::WallTimeMs();
    CommandDispatcher::Instance().Dispatch(env);
}

bool QmlVideoBridge::ExecuteCommandJson(const std::string& command_json) {
    try {
        auto j = json::parse(command_json);
        if (!j.is_object() || !j.contains("action") || !j["action"].is_string()) {
            return false;
        }

        CommandEnvelope env;
        env.command_id = "ui_" + std::to_string(time_utils::MonotonicMs());
        env.action = j["action"].get<std::string>();
        env.client_id = "QmlVideoBridge";
        env.source = CommandSource::UI_QML;
        env.auth_token = j.value("auth_token", GetSessionToken());
        env.payload = j;
        env.timestamp_ms = time_utils::WallTimeMs();

        auto res = CommandDispatcher::Instance().Dispatch(env);
        return res.success;
    } catch (const std::exception& e) {
        LOG_WARN << "QmlVideoBridge::ExecuteCommandJson error: " << e.what();
        return false;
    }
}

std::string QmlVideoBridge::GetGridStateJson() const {
    json j;
    bool is_fs = LiveController::Instance().IsFullscreen();
    j["is_fullscreen"] = is_fs;
    j["fullscreen_channel_id"] = LiveController::Instance().GetFullscreenChannelId();
    j["layout_count"] = LiveController::Instance().GetActiveTileCount();
    j["width"] = width_;
    j["height"] = height_;
    j["hardware_acceleration_supported"] = IsG2dHardwareSupported();
    j["hardware_acceleration_enabled"] = IsHardwareAccelerationEnabled();
    j["dmabuf_hardware_supported"] = IsDmabufSupported();
    j["g2d_pipeline_pattern"] = "imxvideoconvert_g2d ! glupload ! qml6glsink";

    auto metrics = LiveController::Instance().GetTileMetrics();
    json tiles_arr = json::array();

    if (!metrics.empty()) {
        for (const auto& m : metrics) {
            json tile_obj;
            tile_obj["tile_index"] = m.tile_index;
            tile_obj["channel_id"] = m.channel_id;
            CameraConfig cam_cfg;
            if (CameraManager::Instance().GetCamera(m.channel_id, cam_cfg) && !cam_cfg.name.empty()) {
                tile_obj["camera_name"] = cam_cfg.name;
            } else {
                tile_obj["camera_name"] = "Camera " + std::to_string(m.channel_id);
            }
            tile_obj["stream_type"] = (m.stream_type == StreamType::MAIN) ? "MAIN" : "SUB";
            tile_obj["width"] = m.width;
            tile_obj["height"] = m.height;
            tile_obj["stream_width"] = m.width;
            tile_obj["stream_height"] = m.height;
            tile_obj["enqueued"] = m.enqueued_frames;
            tile_obj["dropped"] = m.dropped_frames;
            tile_obj["dropped_frames"] = m.dropped_frames;
            tile_obj["rendered"] = m.rendered_frames;
            tile_obj["fps"] = m.estimated_fps;
            tile_obj["decoded_fps"] = m.estimated_fps;
            tile_obj["frame_revision"] = GetTileRevision(m.tile_index);
            tile_obj["is_recording"] = RecordingScheduler::Instance().IsChannelRecording(m.channel_id);

            tile_obj["hw_accelerated"] = IsTileHardwareAccelerated(m.tile_index);
            auto hw_m = GetTileHardwareMetrics(m.tile_index);
            tile_obj["hw_frames_submitted"] = hw_m.frames_submitted;
            tile_obj["hw_frames_rendered"] = hw_m.frames_rendered;
            if (!hw_m.pipeline_description.empty()) {
                tile_obj["hw_pipeline"] = hw_m.pipeline_description;
            }

            std::string status_str = "OFFLINE";
            switch (m.state) {
                case TileConnectionState::CONNECTING: status_str = "CONNECTING"; break;
                case TileConnectionState::STREAMING:  status_str = "STREAMING";  break;
                case TileConnectionState::STALLED:    status_str = "STALLED";    break;
                case TileConnectionState::OFFLINE:
                default:                              status_str = "OFFLINE";    break;
            }
            tile_obj["status"] = status_str;
            tile_obj["is_online"] = (m.state != TileConnectionState::OFFLINE);
            tiles_arr.push_back(tile_obj);
        }
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& pair : tiles_) {
            json tile_obj;
            tile_obj["tile_index"] = pair.first;
            tile_obj["channel_id"] = pair.second.channel_id;
            tile_obj["frame_revision"] = pair.second.revision;
            auto it_hw = hw_pipelines_.find(pair.first);
            if (it_hw != hw_pipelines_.end()) {
                tile_obj["hw_accelerated"] = it_hw->second.is_hw_accelerated;
                tile_obj["hw_frames_submitted"] = it_hw->second.frames_submitted;
                tile_obj["hw_frames_rendered"] = it_hw->second.frames_rendered;
                tile_obj["hw_pipeline"] = it_hw->second.description;
            } else {
                tile_obj["hw_accelerated"] = false;
                tile_obj["hw_frames_submitted"] = 0;
                tile_obj["hw_frames_rendered"] = 0;
            }
            tiles_arr.push_back(tile_obj);
        }
    }

    j["tiles"] = tiles_arr;
    return j.dump();
}

void QmlVideoBridge::SetFrameRenderCallback(FrameRenderCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    render_callback_ = std::move(cb);
}

} // namespace nvr
