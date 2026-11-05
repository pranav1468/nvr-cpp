#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlPropertyMap>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QQuickImageProvider>
#include <QDir>
#include <QTimer>
#include <QDebug>
#include <iostream>
#include <algorithm>
#include <vector>
#include <string>
#include <memory>
#include <filesystem>
#include <unistd.h>

#include "nvr/common/types.h"
#include "nvr/common/logger.h"
#include "nvr/common/config_loader.h"
#include "nvr/database/database_manager.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/recording/atomic_writer.h"
#include "nvr/recording/recording_scheduler.h"
#include "nvr/storage/retention_manager.h"
#include "nvr/live/live_controller.h"
#include "nvr/live/qml_video_bridge.h"
#include "nvr/live/live_bridge_controller.h"
#include "nvr/ui/nvr_bridge.h"
#include "nvr/api/rest_server.h"

namespace {

class LiveImageProvider : public QQuickImageProvider {
public:
    LiveImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override {
        Q_UNUSED(requestedSize);
        QStringList parts = id.split('/');
        int tileIndex = 0;
        int channel_id = 0;

        if (parts.value(0) == "ch" || parts.value(0) == "channel") {
            channel_id = parts.value(1).toInt();
            tileIndex = std::max(0, channel_id - 1);
        } else {
            tileIndex = parts.value(0).toInt();
            channel_id = nvr::QmlVideoBridge::Instance().GetTileChannelId(tileIndex);
            if (channel_id <= 0) channel_id = tileIndex + 1;
        }

        auto frame = nvr::QmlVideoBridge::Instance().GetTileFrame(tileIndex);

        if (!frame || !frame->HasData() || frame->width == 0 || frame->height == 0) {
            auto latest = nvr::LiveController::Instance().GetLatestFrame(channel_id);
            if (latest && latest->HasData() && latest->width > 0 && latest->height > 0) {
                frame = latest;
            }
        }

        auto get_fallback = [size]() {
            QImage fallback(320, 180, QImage::Format_RGB32);
            fallback.fill(QColor(15, 23, 42));
            if (size) *size = fallback.size();
            return fallback;
        };

        if (frame && frame->HasData() && frame->width > 0 && frame->height > 0) {
            if (size) *size = QSize(frame->width, frame->height);

            // Zero-copy wrapping for G2D hardware converted BGRx (Qt native Format_RGB32)
            if (frame->format == nvr::PixelFormat::BGRX32) {
                auto* holder = new nvr::DecodedFramePtr(frame);
                return QImage(
                    const_cast<uint8_t*>(frame->GetDataPtr()),
                    frame->width, frame->height,
                    frame->stride > 0 ? frame->stride : frame->width * 4,
                    QImage::Format_RGB32,
                    [](void* info) { delete static_cast<nvr::DecodedFramePtr*>(info); },
                    holder
                );
            } else if (frame->format == nvr::PixelFormat::RGB24) {
                auto* holder = new nvr::DecodedFramePtr(frame);
                return QImage(
                    const_cast<uint8_t*>(frame->GetDataPtr()),
                    frame->width, frame->height,
                    frame->stride > 0 ? frame->stride : frame->width * 3,
                    QImage::Format_RGB888,
                    [](void* info) { delete static_cast<nvr::DecodedFramePtr*>(info); },
                    holder
                );
            } else if (frame->format == nvr::PixelFormat::RGBA32) {
                auto* holder = new nvr::DecodedFramePtr(frame);
                return QImage(
                    const_cast<uint8_t*>(frame->GetDataPtr()),
                    frame->width, frame->height,
                    frame->stride > 0 ? frame->stride : frame->width * 4,
                    QImage::Format_RGBA8888,
                    [](void* info) { delete static_cast<nvr::DecodedFramePtr*>(info); },
                    holder
                );
            } else if (frame->format == nvr::PixelFormat::RGB16) {
                auto* holder = new nvr::DecodedFramePtr(frame);
                return QImage(
                    const_cast<uint8_t*>(frame->GetDataPtr()),
                    frame->width, frame->height,
                    frame->stride > 0 ? frame->stride : frame->width * 2,
                    QImage::Format_RGB16,
                    [](void* info) { delete static_cast<nvr::DecodedFramePtr*>(info); },
                    holder
                );
            } else if (frame->format == nvr::PixelFormat::NV12) {
                int w = frame->width;
                int h = frame->height;
                int stride = frame->stride > 0 ? frame->stride : w;
                size_t min_req = (static_cast<size_t>(stride) * h * 3) / 2;
                if (frame->GetSizeBytes() < min_req) {
                    return get_fallback();
                }

                // 100% Native full resolution conversion without CPU downscaling
                int out_w = w;
                int out_h = h;
                if (size) *size = QSize(out_w, out_h);

                const uint8_t* y_plane = frame->GetDataPtr();
                const uint8_t* uv_plane = y_plane + (static_cast<size_t>(stride) * h);
                QImage qimg(out_w, out_h, QImage::Format_RGB32);

                for (int y = 0; y < h; ++y) {
                    uint32_t* scanline = reinterpret_cast<uint32_t*>(qimg.scanLine(y));
                    const uint8_t* y_row = y_plane + (static_cast<size_t>(y) * stride);
                    const uint8_t* uv_row = uv_plane + (static_cast<size_t>(y >> 1) * stride);
                    for (int x = 0; x < w; x += 2) {
                        int u_val = uv_row[x] - 128;
                        int v_val = uv_row[x + 1] - 128;

                        int ruv = (1436 * v_val) >> 10;
                        int guv = (352 * u_val + 731 * v_val) >> 10;
                        int buv = (1815 * u_val) >> 10;

                        int y0 = y_row[x];
                        int r0 = std::clamp(y0 + ruv, 0, 255);
                        int g0 = std::clamp(y0 - guv, 0, 255);
                        int b0 = std::clamp(y0 + buv, 0, 255);
                        scanline[x] = 0xff000000u | (static_cast<uint32_t>(r0) << 16) | (static_cast<uint32_t>(g0) << 8) | static_cast<uint32_t>(b0);

                        if (x + 1 < w) {
                            int y1 = y_row[x + 1];
                            int r1 = std::clamp(y1 + ruv, 0, 255);
                            int g1 = std::clamp(y1 - guv, 0, 255);
                            int b1 = std::clamp(y1 + buv, 0, 255);
                            scanline[x + 1] = 0xff000000u | (static_cast<uint32_t>(r1) << 16) | (static_cast<uint32_t>(g1) << 8) | static_cast<uint32_t>(b1);
                        }
                    }
                }
                return qimg;
            } else if (frame->format == nvr::PixelFormat::YUV420P) {
                int w = frame->width;
                int h = frame->height;
                int stride = frame->stride > 0 ? frame->stride : w;
                int uv_stride = stride / 2;
                size_t min_req = (static_cast<size_t>(stride) * h) + 2 * (static_cast<size_t>(uv_stride) * (h / 2));
                if (frame->GetSizeBytes() < min_req) {
                    return get_fallback();
                }
                const uint8_t* y_plane = frame->GetDataPtr();
                const uint8_t* u_plane = y_plane + (static_cast<size_t>(stride) * h);
                const uint8_t* v_plane = u_plane + (static_cast<size_t>(uv_stride) * (h / 2));
                QImage qimg(w, h, QImage::Format_RGB32);

                for (int y = 0; y < h; ++y) {
                    uint32_t* scanline = reinterpret_cast<uint32_t*>(qimg.scanLine(y));
                    const uint8_t* y_row = y_plane + (static_cast<size_t>(y) * stride);
                    const uint8_t* u_row = u_plane + (static_cast<size_t>(y >> 1) * uv_stride);
                    const uint8_t* v_row = v_plane + (static_cast<size_t>(y >> 1) * uv_stride);
                    for (int x = 0; x < w; x += 2) {
                        int uv_idx = x >> 1;
                        int u_val = u_row[uv_idx] - 128;
                        int v_val = v_row[uv_idx] - 128;

                        int ruv = (1436 * v_val) >> 10;
                        int guv = (352 * u_val + 731 * v_val) >> 10;
                        int buv = (1815 * u_val) >> 10;

                        int y0 = y_row[x];
                        int r0 = std::clamp(y0 + ruv, 0, 255);
                        int g0 = std::clamp(y0 - guv, 0, 255);
                        int b0 = std::clamp(y0 + buv, 0, 255);
                        scanline[x] = 0xff000000u | (static_cast<uint32_t>(r0) << 16) | (static_cast<uint32_t>(g0) << 8) | static_cast<uint32_t>(b0);

                        if (x + 1 < w) {
                            int y1 = y_row[x + 1];
                            int r1 = std::clamp(y1 + ruv, 0, 255);
                            int g1 = std::clamp(y1 - guv, 0, 255);
                            int b1 = std::clamp(y1 + buv, 0, 255);
                            scanline[x + 1] = 0xff000000u | (static_cast<uint32_t>(r1) << 16) | (static_cast<uint32_t>(g1) << 8) | static_cast<uint32_t>(b1);
                        }
                    }
                }
                return qimg;
            }
        }

        return get_fallback();
    }
};

} // namespace

int main(int argc, char *argv[]) {
    // 1. Establish RHI graphics backend; adapt EGL/GLES2 for embedded i.MX board or Wayland
    if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND"))
        qputenv("QSG_RHI_BACKEND", "opengl");

    bool is_embedded_imx = (access("/dev/mxc_g2d", F_OK) == 0);
    bool is_explicit_wayland = (qEnvironmentVariable("QT_QPA_PLATFORM") == "wayland");
    if (is_embedded_imx || is_explicit_wayland) {
        if (qEnvironmentVariableIsEmpty("GST_GL_PLATFORM"))
            qputenv("GST_GL_PLATFORM", "egl");
        if (qEnvironmentVariableIsEmpty("GST_GL_API"))
            qputenv("GST_GL_API", "gles2");
    }
    qunsetenv("GST_GL_WINDOW");

    QQuickStyle::setStyle("Basic");

    QGuiApplication app(argc, argv);
    app.setApplicationName("Mactra NVR");
    app.setOrganizationName("Mactra");

    // 2. Initialize in-process NVR background core
    std::string config_path = "config/nvr_config.json";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--config=", 0) == 0) {
            config_path = arg.substr(9);
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    nvr::NvrConfig config = nvr::LoadNvrConfig(config_path);

    std::error_code ec;
    std::filesystem::create_directories(config.storage.recording_path, ec);
    nvr::AtomicWriter::CleanOrphanedTmpFiles(config.storage.recording_path);

    // Initialize SQLite Database Engine
    std::filesystem::create_directories("config", ec);
    std::string db_path = "config/nvr.db";
    if (!nvr::DatabaseManager::Instance().Initialize(db_path)) {
        LOG_ERROR << "Failed to initialize SQLite database at " << db_path;
    }

    // Load persisted user-defined storage path if configured in SQLite
    std::string saved_storage_path = nvr::DatabaseManager::Instance().GetSetting("storage_recording_path");
    if (!saved_storage_path.empty()) {
        config.storage.recording_path = saved_storage_path;
        LOG_INFO << "Loaded user-configured recording storage path from database: " << saved_storage_path;
    } else {
        nvr::DatabaseManager::Instance().SetSetting("storage_recording_path", config.storage.recording_path);
    }

    nvr::RetentionManager::Instance().Configure(config.storage);
    nvr::RetentionManager::Instance().Start();

    nvr::RecordingScheduler::Instance().Configure(config.storage);

    nvr::CameraManager::Instance().SetMaxChannels(config.system.max_channels);
    nvr::CameraManager::Instance().SetConfigFilePath(config_path);
    nvr::CameraManager::Instance().Initialize(config.cameras);

    for (const auto& cam : nvr::CameraManager::Instance().GetCameras()) {
        if (cam.enabled) {
            if (cam.record_mode != nvr::RecordMode::DISABLED) {
                nvr::RecordingScheduler::Instance().StartChannelRecording(cam.id, cam.record_mode);
            }
            if (cam.record_mode != nvr::RecordMode::DISABLED || cam.sub_rtsp_url.empty()) {
                nvr::CameraManager::Instance().StartCamera(cam.id);
            }
            nvr::CameraManager::Instance().StartSubStream(cam.id);
        }
    }

    // Connect LiveController directly to QmlVideoBridge display backend
    nvr::LiveController::Instance().SetDisplayBackend(
        std::shared_ptr<nvr::IDisplayBackend>(&nvr::QmlVideoBridge::Instance(), [](nvr::IDisplayBackend*){})
    );
    nvr::LiveController::Instance().Configure(config.live);
    nvr::LiveController::Instance().SetLayout(nvr::LiveGridLayout::GRID_4);
    nvr::LiveController::Instance().Start();

    if (config.api.enabled) {
        nvr::RestServer::Instance().Configure(config.api);
        nvr::RestServer::Instance().Start();
    }

    // 3. Setup QML Engine and dynamic hardware video bridge
    QQmlApplicationEngine engine;
    engine.addImageProvider("live", new LiveImageProvider());

    QQmlPropertyMap liveBridge;
    liveBridge.insert("hasHardwareGlSink", nvr::QmlVideoBridge::Instance().IsQml6GlSinkSupported());
    for (int i = 0; i < 8; ++i) {
        liveBridge.insert(QString("rev%1").arg(i), QVariant::fromValue<qulonglong>(0));
        liveBridge.insert(QString("online%1").arg(i), nvr::CameraManager::Instance().IsCameraOnline(i + 1));
    }

    // 40ms UI batching pump (25 Hz) ensures worker threads never flood the Qt event queue
    QTimer uiPump;
    uiPump.setTimerType(Qt::CoarseTimer);
    uiPump.setInterval(40);
    uint64_t pumpTick = 0;
    QObject::connect(&uiPump, &QTimer::timeout, [&liveBridge, &pumpTick]() {
        ++pumpTick;
        bool checkOnline = (pumpTick % 12 == 0);
        for (int i = 0; i < 8; ++i) {
            uint64_t rev = nvr::QmlVideoBridge::Instance().GetTileRevision(i);
            QString revKey = QString("rev%1").arg(i);
            bool revChanged = (liveBridge.value(revKey).toULongLong() != rev);
            if (revChanged) {
                liveBridge.insert(revKey, QVariant::fromValue<qulonglong>(rev));

                auto frame = nvr::QmlVideoBridge::Instance().GetTileFrame(i);
                if (frame && frame->height > 0) {
                    QString prefix = (frame->stream_type == nvr::StreamType::MAIN) ? "MAIN" : "SUB";
                    QString resStr;
                    if (frame->height >= 2160) resStr = prefix + " 4K";
                    else if (frame->height >= 1440) resStr = prefix + " 2K";
                    else resStr = QString("%1 %2p").arg(prefix).arg(frame->height);

                    QString resKey = QString("res%1").arg(i);
                    if (liveBridge.value(resKey).toString() != resStr) {
                        liveBridge.insert(resKey, resStr);
                    }
                }
            }

            if (checkOnline || revChanged) {
                int ch = nvr::QmlVideoBridge::Instance().GetTileChannelId(i);
                if (ch <= 0) ch = i + 1;
                bool online = nvr::CameraManager::Instance().IsCameraOnline(ch);
                QString onKey = QString("online%1").arg(i);
                if (liveBridge.value(onKey).toBool() != online) {
                    liveBridge.insert(onKey, online);
                }
            }
        }
    });
    uiPump.start();

    nvr::LiveBridgeController liveBridgeController;
    nvr::NvrBridge nvrBridge;
    engine.rootContext()->setContextProperty("LiveBridge", &liveBridge);
    engine.rootContext()->setContextProperty("LiveBridgeController", &liveBridgeController);
    engine.rootContext()->setContextProperty("NvrBridge", &nvrBridge);

    // Register import paths for local QML modules and system Qt6 modules
    QString currentDir = QDir::currentPath();
    engine.addImportPath(currentDir + "/ui");
    engine.addImportPath("/usr/lib/x86_64-linux-gnu/qt6/qml");

    bool captureMode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--capture") {
            captureMode = true;
        }
    }
    nvrBridge.setCaptureMode(captureMode);

    const QUrl url(QUrl::fromLocalFile(currentDir + "/ui/Main.qml"));

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url, captureMode](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl) {
            std::cerr << "Failed to load QML root object from " << objUrl.toString().toStdString() << std::endl;
            QCoreApplication::exit(-1);
        } else {
            std::cout << "Local UI loaded and running successfully." << std::endl;
            QQuickWindow *win = qobject_cast<QQuickWindow*>(obj);
            if (win && captureMode) {
                // Wait for camera streams to establish and initial frames to decode
                QTimer::singleShot(3000, win, [win, obj]() {
                    const char* env_dir = std::getenv("UI_CAPTURE_DIR");
                    std::string out_dir = env_dir ? (std::string(env_dir) + "/") : "artifacts/";
                    std::filesystem::create_directories(out_dir);

                    QImage img = win->grabWindow();
                    img.save(QString::fromStdString(out_dir + "local_ui_live_4cam.png"));
                    std::cout << "[UI Capture] 4-CAM grid screenshot saved to " << out_dir << std::endl;

                    // Trigger Single-Camera Fullscreen for Camera 2 (MAIN stream 1080p)
                    std::cout << "[UI Capture] Entering fullscreen for Camera 2..." << std::endl;
                    QMetaObject::invokeMethod(obj, "toggleFullscreen", Q_ARG(QVariant, 2));

                    QTimer::singleShot(2500, win, [win, obj, out_dir]() {
                        QImage img2 = win->grabWindow();
                        img2.save(QString::fromStdString(out_dir + "local_ui_fullscreen_ch2.png"));
                        std::cout << "[UI Capture] Camera 2 Fullscreen screenshot saved to " << out_dir << std::endl;

                        // Exit fullscreen back to 4-CAM grid
                        std::cout << "[UI Capture] Exiting fullscreen back to 4-CAM grid..." << std::endl;
                        QMetaObject::invokeMethod(obj, "toggleFullscreen", Q_ARG(QVariant, 2));

                        QTimer::singleShot(2000, win, [win, out_dir]() {
                            QImage img3 = win->grabWindow();
                            img3.save(QString::fromStdString(out_dir + "local_ui_live_restored_4cam.png"));
                            std::cout << "[UI Capture] Restored 4-CAM grid screenshot saved to " << out_dir << std::endl;

                            QTimer::singleShot(500, []() {
                                std::cout << "[UI Capture] All live verification captures complete. Exiting." << std::endl;
                                QCoreApplication::quit();
                            });
                        });
                    });
                });
            }
        }
    }, Qt::QueuedConnection);

    engine.load(url);

    const int exitCode = app.exec();

    // 4. Graceful subsystem shutdown on application exit
    nvr::RestServer::Instance().Stop();
    nvr::LiveController::Instance().Stop();
    nvr::CameraManager::Instance().StopAll();
    nvr::RecordingScheduler::Instance().StopAll();
    nvr::RetentionManager::Instance().Stop();
    nvr::DatabaseManager::Instance().Close();

    return exitCode;
}
