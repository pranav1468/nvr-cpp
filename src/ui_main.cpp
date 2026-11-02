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

        if (!frame || frame->data.empty() || frame->width == 0 || frame->height == 0) {
            auto latest = nvr::LiveController::Instance().GetLatestFrame(channel_id);
            if (latest && !latest->data.empty() && latest->width > 0 && latest->height > 0) {
                frame = latest;
            }
        }

        if (frame && !frame->data.empty() && frame->width > 0 && frame->height > 0) {
            if (size) *size = QSize(frame->width, frame->height);

            // Zero-memcpy wrapping for standard RGB/RGBA/RGB16 formats
            if (frame->format == nvr::PixelFormat::RGB24) {
                auto* holder = new nvr::DecodedFramePtr(frame);
                return QImage(
                    const_cast<uint8_t*>(frame->data.data()),
                    frame->width, frame->height,
                    frame->stride > 0 ? frame->stride : frame->width * 3,
                    QImage::Format_RGB888,
                    [](void* info) { delete static_cast<nvr::DecodedFramePtr*>(info); },
                    holder
                );
            } else if (frame->format == nvr::PixelFormat::RGBA32) {
                auto* holder = new nvr::DecodedFramePtr(frame);
                return QImage(
                    const_cast<uint8_t*>(frame->data.data()),
                    frame->width, frame->height,
                    frame->stride > 0 ? frame->stride : frame->width * 4,
                    QImage::Format_RGBA8888,
                    [](void* info) { delete static_cast<nvr::DecodedFramePtr*>(info); },
                    holder
                );
            } else if (frame->format == nvr::PixelFormat::RGB16) {
                auto* holder = new nvr::DecodedFramePtr(frame);
                return QImage(
                    const_cast<uint8_t*>(frame->data.data()),
                    frame->width, frame->height,
                    frame->stride > 0 ? frame->stride : frame->width * 2,
                    QImage::Format_RGB16,
                    [](void* info) { delete static_cast<nvr::DecodedFramePtr*>(info); },
                    holder
                );
            } else if (frame->format == nvr::PixelFormat::NV12) {
                // Fast integer fixed-point YUV->RGB conversion for desktop fallback without G2D
                int w = frame->width;
                int h = frame->height;
                const uint8_t* y_plane = frame->data.data();
                const uint8_t* uv_plane = y_plane + (w * h);
                QImage qimg(w, h, QImage::Format_RGB888);

                for (int y = 0; y < h; ++y) {
                    uchar* scanline = qimg.scanLine(y);
                    const uint8_t* y_row = y_plane + (y * w);
                    const uint8_t* uv_row = uv_plane + ((y >> 1) * w);
                    for (int x = 0; x < w; ++x) {
                        int y_val = y_row[x];
                        int uv_idx = x & ~1;
                        int u_val = uv_row[uv_idx] - 128;
                        int v_val = uv_row[uv_idx + 1] - 128;

                        int r = y_val + ((1436 * v_val) >> 10);
                        int g = y_val - ((352 * u_val + 731 * v_val) >> 10);
                        int b = y_val + ((1815 * u_val) >> 10);

                        scanline[x * 3 + 0] = static_cast<uchar>(std::clamp(r, 0, 255));
                        scanline[x * 3 + 1] = static_cast<uchar>(std::clamp(g, 0, 255));
                        scanline[x * 3 + 2] = static_cast<uchar>(std::clamp(b, 0, 255));
                    }
                }
                return qimg;
            } else if (frame->format == nvr::PixelFormat::YUV420P) {
                int w = frame->width;
                int h = frame->height;
                const uint8_t* y_plane = frame->data.data();
                const uint8_t* u_plane = y_plane + (w * h);
                const uint8_t* v_plane = u_plane + (w * h / 4);
                QImage qimg(w, h, QImage::Format_RGB888);

                for (int y = 0; y < h; ++y) {
                    uchar* scanline = qimg.scanLine(y);
                    const uint8_t* y_row = y_plane + (y * w);
                    const uint8_t* u_row = u_plane + ((y >> 1) * (w >> 1));
                    const uint8_t* v_row = v_plane + ((y >> 1) * (w >> 1));
                    for (int x = 0; x < w; ++x) {
                        int y_val = y_row[x];
                        int uv_idx = x >> 1;
                        int u_val = u_row[uv_idx] - 128;
                        int v_val = v_row[uv_idx] - 128;

                        int r = y_val + ((1436 * v_val) >> 10);
                        int g = y_val - ((352 * u_val + 731 * v_val) >> 10);
                        int b = y_val + ((1815 * u_val) >> 10);

                        scanline[x * 3 + 0] = static_cast<uchar>(std::clamp(r, 0, 255));
                        scanline[x * 3 + 1] = static_cast<uchar>(std::clamp(g, 0, 255));
                        scanline[x * 3 + 2] = static_cast<uchar>(std::clamp(b, 0, 255));
                    }
                }
                return qimg;
            }
        }

        // Standby background
        QImage fallback(320, 180, QImage::Format_RGB888);
        fallback.fill(QColor(15, 23, 42));
        if (size) *size = fallback.size();
        return fallback;
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

    nvr::RetentionManager::Instance().Configure(config.storage);
    nvr::RetentionManager::Instance().Start();

    nvr::RecordingScheduler::Instance().Configure(config.storage);

    nvr::CameraManager::Instance().SetMaxChannels(config.system.max_channels);
    nvr::CameraManager::Instance().SetConfigFilePath(config_path);
    nvr::CameraManager::Instance().Initialize(config.cameras);

    for (const auto& cam : nvr::CameraManager::Instance().GetCameras()) {
        if (cam.enabled) {
            nvr::RecordingScheduler::Instance().StartChannelRecording(cam.id, cam.record_mode);
            nvr::CameraManager::Instance().StartCamera(cam.id);
            nvr::CameraManager::Instance().StartSubStream(cam.id);
        }
    }

    // Connect LiveController directly to QmlVideoBridge display backend
    nvr::LiveController::Instance().SetDisplayBackend(
        std::shared_ptr<nvr::IDisplayBackend>(&nvr::QmlVideoBridge::Instance(), [](nvr::IDisplayBackend*){})
    );
    nvr::LiveController::Instance().Configure(config.live);
    if (config.live.grid_layout == 1) {
        nvr::LiveController::Instance().SetLayout(nvr::LiveGridLayout::SINGLE);
    } else if (config.live.grid_layout == 6) {
        nvr::LiveController::Instance().SetLayout(nvr::LiveGridLayout::GRID_6);
    } else if (config.live.grid_layout == 8) {
        nvr::LiveController::Instance().SetLayout(nvr::LiveGridLayout::GRID_8);
    } else {
        nvr::LiveController::Instance().SetLayout(nvr::LiveGridLayout::GRID_4);
    }
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

    // 20ms UI batching pump (50 Hz) ensures worker threads never flood the Qt event queue
    QTimer uiPump;
    uiPump.setTimerType(Qt::CoarseTimer);
    uiPump.setInterval(20);
    QObject::connect(&uiPump, &QTimer::timeout, [&liveBridge]() {
        for (int i = 0; i < 8; ++i) {
            uint64_t rev = nvr::QmlVideoBridge::Instance().GetTileRevision(i);
            QString revKey = QString("rev%1").arg(i);
            if (liveBridge.value(revKey).toULongLong() != rev) {
                liveBridge.insert(revKey, QVariant::fromValue<qulonglong>(rev));
            }

            int ch = nvr::QmlVideoBridge::Instance().GetTileChannelId(i);
            if (ch <= 0) ch = i + 1;
            bool online = nvr::CameraManager::Instance().IsCameraOnline(ch);
            QString onKey = QString("online%1").arg(i);
            if (liveBridge.value(onKey).toBool() != online) {
                liveBridge.insert(onKey, online);
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
                QTimer::singleShot(1500, win, [win, obj]() {
                    QImage img = win->grabWindow();
                    img.save("/home/mantra/.gemini/antigravity-ide/brain/401c5b32-abf8-4a83-aeeb-1c82c438014e/local_ui_live.png");
                    std::cout << "[UI Capture] Live screenshot saved." << std::endl;

                    obj->setProperty("currentRoute", "playback");

                    QTimer::singleShot(1200, win, [win, obj]() {
                        QImage img2 = win->grabWindow();
                        img2.save("/home/mantra/.gemini/antigravity-ide/brain/401c5b32-abf8-4a83-aeeb-1c82c438014e/local_ui_recordings.png");
                        std::cout << "[UI Capture] Playback screenshot saved." << std::endl;

                        obj->setProperty("currentRoute", "cameras");

                        QTimer::singleShot(1200, win, [win, obj]() {
                            QImage img3 = win->grabWindow();
                            img3.save("/home/mantra/.gemini/antigravity-ide/brain/401c5b32-abf8-4a83-aeeb-1c82c438014e/local_ui_cameras.png");
                            std::cout << "[UI Capture] Cameras screenshot saved." << std::endl;

                            obj->setProperty("currentRoute", "dashboard");

                            QTimer::singleShot(1200, win, [win, obj]() {
                                QImage img4 = win->grabWindow();
                                img4.save("/home/mantra/.gemini/antigravity-ide/brain/401c5b32-abf8-4a83-aeeb-1c82c438014e/local_ui_dashboard.png");
                                std::cout << "[UI Capture] Dashboard screenshot saved." << std::endl;

                                obj->setProperty("currentRoute", "system");

                                QTimer::singleShot(1200, win, [win, obj]() {
                                    QImage img5 = win->grabWindow();
                                    img5.save("/home/mantra/.gemini/antigravity-ide/brain/401c5b32-abf8-4a83-aeeb-1c82c438014e/local_ui_system.png");
                                    std::cout << "[UI Capture] System screenshot saved." << std::endl;

                                    obj->setProperty("currentRoute", "live");
                                    std::cout << "[UI Capture] Switched back to Live Matrix. All 5 pages captured." << std::endl;

                                    // Capture 7+1 Mode
                                    obj->setProperty("liveLayoutMode", "7plus1");
                                    QTimer::singleShot(1000, win, [win, obj]() {
                                        QImage img7 = win->grabWindow();
                                        img7.save("/home/mantra/.gemini/antigravity-ide/brain/401c5b32-abf8-4a83-aeeb-1c82c438014e/local_ui_live_7plus1.png");
                                        std::cout << "[UI Capture] 7+1 layout screenshot saved." << std::endl;

                                        // Capture 8-CAM Mode
                                        obj->setProperty("liveLayoutMode", "8");
                                        QTimer::singleShot(1000, win, [win, obj]() {
                                            QImage img8 = win->grabWindow();
                                            img8.save("/home/mantra/.gemini/antigravity-ide/brain/401c5b32-abf8-4a83-aeeb-1c82c438014e/local_ui_live_8cam.png");
                                            std::cout << "[UI Capture] 8-CAM layout screenshot saved." << std::endl;

                                            // Return to 4-CAM default
                                            obj->setProperty("liveLayoutMode", "4");

                                            QTimer::singleShot(500, []() {
                                                std::cout << "[UI Capture] All captures complete. Exiting." << std::endl;
                                                QCoreApplication::quit();
                                            });
                                        });
                                    });
                                });
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

    return exitCode;
}
