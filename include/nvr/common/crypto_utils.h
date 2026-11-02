#pragma once

#include <string>

namespace nvr {
namespace crypto {

// Redact password from an RTSP URL (e.g. "rtsp://admin:secret123@192.168.1.10:554/live" -> "rtsp://admin:******@192.168.1.10:554/live")
inline std::string MaskRtspUrl(const std::string& url) {
    auto at_pos = url.find('@');
    if (at_pos == std::string::npos) return url;
    auto scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) return url;

    size_t user_start = scheme_pos + 3;
    auto colon_pos = url.find(':', user_start);
    if (colon_pos == std::string::npos || colon_pos >= at_pos) {
        return url; // No password in URL
    }

    return url.substr(0, colon_pos + 1) + "******" + url.substr(at_pos);
}

} // namespace crypto
} // namespace nvr
