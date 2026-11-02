#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace nvr {

struct DiscoveredCamera {
    std::string ip_address;
    int port{80};
    std::string xaddr;
    std::string endpoint_reference;
    std::string scopes;
    std::string name;
    std::string hardware;
};

struct OnvifDeviceInfo {
    std::string manufacturer;
    std::string model;
    std::string firmware_version;
    std::string serial_number;
    std::string hardware_id;
};

struct OnvifProfileInfo {
    std::string token;
    std::string name;
    std::string rtsp_uri;
    std::string snapshot_uri;
    std::string video_source_token;
    std::string video_encoder_token;
    std::string encoding{"H264"};
    int width{0};
    int height{0};
    int fps{0};
    int bitrate_kbps{0};
    bool is_media2{false};
};

struct OnvifVideoResolution {
    int width{0};
    int height{0};
};

struct OnvifVideoEncoderConfig {
    std::string token;
    std::string name;
    std::string encoding{"H264"}; // "H264" or "H265"
    int width{1920};
    int height{1080};
    int fps{25};
    int bitrate_kbps{4000};
    int gov_length{50}; // GOP length
    std::string h264_profile{"Main"}; // "Baseline", "Main", "High"
    bool is_media2{false};
};

struct OnvifVideoEncoderOptions {
    std::vector<std::string> supported_encodings;
    std::vector<OnvifVideoResolution> available_resolutions;
    int min_fps{1};
    int max_fps{60};
    int min_bitrate_kbps{64};
    int max_bitrate_kbps{16384};
    int min_gov_length{1};
    int max_gov_length{300};
    bool is_media2{false};
};

struct OnvifNetworkInterface {
    std::string token;
    std::string name;
    std::string hw_address; // MAC address
    std::string ipv4_address;
    std::string subnet_mask;
    bool dhcp_enabled{false};
};

struct OnvifCameraDetails {
    std::string ip_address;
    int port{80};
    std::string xaddr;
    std::string media_xaddr;
    std::string media2_xaddr;
    int64_t clock_offset_ms{0};
    OnvifDeviceInfo device_info;
    std::string mac_address;
    std::vector<OnvifNetworkInterface> network_interfaces;
    std::vector<OnvifProfileInfo> profiles;
    std::string main_rtsp_url;
    std::string sub_rtsp_url;
    std::string snapshot_url;
};

class OnvifClient {
public:
    // Sends WS-Discovery UDP multicast (239.255.255.250:3702) across active IPv4 interfaces and collects responses
    static std::vector<DiscoveredCamera> DiscoverCameras(int timeout_ms = 2000, uint16_t multicast_port = 3702);

    // Connects to an ONVIF device, syncs UTC clock offset, queries DeviceInformation, Profiles, and resolves RTSP Stream and Snapshot URIs
    static bool ProbeCamera(const std::string& ip_or_xaddr, int port,
                            const std::string& username, const std::string& password,
                            OnvifCameraDetails& out_details, int timeout_ms = 3000);

    // Queries unauthenticated ONVIF Device Service GetSystemDateAndTime and returns (camera_utc_ms - nvr_utc_ms)
    static int64_t QueryCameraClockOffsetMs(const std::string& device_xaddr, int timeout_ms = 2000);

    // Sets camera SystemDateAndTime over ONVIF Device Service
    static bool SetSystemDateAndTime(const std::string& device_xaddr,
                                    const std::string& username, const std::string& password,
                                    int64_t target_utc_ms, const std::string& timezone_str = "UTC",
                                    int timeout_ms = 3000, int64_t clock_offset_ms = 0);

    // Remote camera reboot over ONVIF Device Service
    static bool RebootCamera(const std::string& device_xaddr,
                             const std::string& username, const std::string& password,
                             int timeout_ms = 3000, int64_t clock_offset_ms = 0);

    // Queries ONVIF Device Service GetDeviceInformation
    static bool GetDeviceInformation(const std::string& device_xaddr,
                                     const std::string& username, const std::string& password,
                                     OnvifDeviceInfo& out_info, int timeout_ms = 3000,
                                     int64_t clock_offset_ms = 0);

    // Queries ONVIF Media Service GetProfiles
    static bool GetProfiles(const std::string& media_xaddr,
                            const std::string& username, const std::string& password,
                            std::vector<OnvifProfileInfo>& out_profiles, int timeout_ms = 3000,
                            int64_t clock_offset_ms = 0, bool use_media2 = false);

    // Queries ONVIF Media Service GetSnapshotUri for direct hardware JPEG capture
    static std::string GetSnapshotUri(const std::string& media_xaddr, const std::string& profile_token,
                                      const std::string& username, const std::string& password,
                                      int timeout_ms = 3000, int64_t clock_offset_ms = 0,
                                      bool use_media2 = false);

    // Video Encoder Configuration (Media 1 & Media 2): basic codec, resolution, fps, bitrate
    static bool GetVideoEncoderConfig(const std::string& media_xaddr, const std::string& config_or_profile_token,
                                      const std::string& username, const std::string& password,
                                      OnvifVideoEncoderConfig& out_config, int timeout_ms = 3000,
                                      int64_t clock_offset_ms = 0, bool use_media2 = false);

    static bool GetVideoEncoderOptions(const std::string& media_xaddr, const std::string& config_or_profile_token,
                                       const std::string& username, const std::string& password,
                                       OnvifVideoEncoderOptions& out_options, int timeout_ms = 3000,
                                       int64_t clock_offset_ms = 0, bool use_media2 = false);

    static bool SetVideoEncoderConfig(const std::string& media_xaddr, const OnvifVideoEncoderConfig& config,
                                      const std::string& username, const std::string& password,
                                      int timeout_ms = 3000, int64_t clock_offset_ms = 0,
                                      bool use_media2 = false);

    // Queries ONVIF Media Service GetStreamUri for a given profile token
    static std::string GetStreamUri(const std::string& media_xaddr, const std::string& profile_token,
                                    const std::string& username, const std::string& password,
                                    int timeout_ms = 3000, int64_t clock_offset_ms = 0,
                                    bool use_media2 = false);

    // Camera Hardware Network Interface Discovery
    static bool GetNetworkInterfaces(const std::string& device_xaddr,
                                     const std::string& username, const std::string& password,
                                     std::vector<OnvifNetworkInterface>& out_interfaces,
                                     int timeout_ms = 3000, int64_t clock_offset_ms = 0);

    // Clears cached HTTP Digest Auth sessions and SOAP capability states
    static void ClearAuthCache();
};

} // namespace nvr
