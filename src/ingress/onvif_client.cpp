// File: src/ingress/onvif_client.cpp
#include "nvr/ingress/onvif_client.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <vector>

namespace nvr {

namespace {

const char* kMulticastIp = "239.255.255.250";

std::string TrimWhitespace(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string UrlDecodeString(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() &&
            std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            int hi = std::isdigit(static_cast<unsigned char>(in[i + 1]))
                         ? (in[i + 1] - '0')
                         : (std::tolower(static_cast<unsigned char>(in[i + 1])) - 'a' + 10);
            int lo = std::isdigit(static_cast<unsigned char>(in[i + 2]))
                         ? (in[i + 2] - '0')
                         : (std::tolower(static_cast<unsigned char>(in[i + 2])) - 'a' + 10);
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else if (in[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::string UrlEncodeUserInfoComponent(const std::string& in) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            oss << static_cast<char>(c);
        } else {
            oss << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return oss.str();
}

std::string XmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

std::string XmlUnescape(std::string s) {
    auto replace_all = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all("&amp;", "&");
    replace_all("&lt;", "<");
    replace_all("&gt;", ">");
    replace_all("&quot;", "\"");
    replace_all("&apos;", "'");
    replace_all("&#38;", "&");
    return TrimWhitespace(s);
}

std::string GenerateUuidV4() {
    std::random_device rd;
    std::mt19937_64 gen(rd() ^ static_cast<uint64_t>(time_utils::MonotonicUs()));
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFFu);

    uint32_t d0 = dist(gen);
    uint32_t d1 = dist(gen);
    uint32_t d2 = dist(gen);
    uint32_t d3 = dist(gen);

    d1 = (d1 & 0xFFFF0FFFu) | 0x00004000u; // Version 4
    d2 = (d2 & 0x3FFFFFFFu) | 0x80000000u; // Variant 1

    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(8) << d0 << "-"
        << std::setw(4) << (d1 >> 16) << "-"
        << std::setw(4) << (d1 & 0xFFFFu) << "-"
        << std::setw(4) << (d2 >> 16) << "-"
        << std::setw(4) << (d2 & 0xFFFFu)
        << std::setw(8) << d3;
    return oss.str();
}

std::string Base64EncodeBytes(const uint8_t* data, size_t len) {
    static const char* kLut = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kLut[(v >> 18) & 0x3F]);
        out.push_back(kLut[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kLut[(v >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kLut[v & 0x3F] : '=');
    }
    return out;
}

// Minimal RFC 3174 SHA-1 implementation for ONVIF WS-UsernameToken PasswordDigest
void Sha1Digest(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xEFCDAB89u;
    uint32_t h2 = 0x98BADCFEu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xC3D2E1F0u;

    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0x00);
    }
    uint64_t bit_len = static_cast<uint64_t>(len) * 8ULL;
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF));
    }

    auto rol = [](uint32_t v, int bits) -> uint32_t {
        return (v << bits) | (v >> (32 - bits));
    };

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[chunk + i * 4 + 0]) << 24) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 2]) << 8)  |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>((hs[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xFF);
    }
}

// RFC 1321 MD5 implementation for HTTP Digest Authentication (RFC 2617 / ONVIF Core Section 5.9)
std::string Md5Hex(const std::string& input) {
    static const uint32_t s[64] = {
        7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
        5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
        4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
        6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
    };
    static const uint32_t K[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
        0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
        0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
        0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
        0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
        0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
        0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
        0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
        0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
        0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
        0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
    };

    uint32_t a0 = 0x67452301u;
    uint32_t b0 = 0xefcdab89u;
    uint32_t c0 = 0x98badcfeu;
    uint32_t d0 = 0x10325476u;

    std::vector<uint8_t> msg(input.begin(), input.end());
    uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8ULL;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0x00);
    }
    for (int i = 0; i < 8; ++i) {
        msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFu));
    }

    auto leftrotate = [](uint32_t x, uint32_t c) -> uint32_t {
        return (x << c) | (x >> (32 - c));
    };

    for (size_t offset = 0; offset < msg.size(); offset += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; ++i) {
            M[i] = static_cast<uint32_t>(msg[offset + i * 4 + 0]) |
                   (static_cast<uint32_t>(msg[offset + i * 4 + 1]) << 8) |
                   (static_cast<uint32_t>(msg[offset + i * 4 + 2]) << 16) |
                   (static_cast<uint32_t>(msg[offset + i * 4 + 3]) << 24);
        }

        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t F = 0, g = 0;
            if (i < 16) {
                F = (B & C) | ((~B) & D);
                g = i;
            } else if (i < 32) {
                F = (D & B) | ((~D) & C);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                F = B ^ C ^ D;
                g = (3 * i + 5) % 16;
            } else {
                F = C ^ (B | (~D));
                g = (7 * i) % 16;
            }
            F = F + A + K[i] + M[g];
            A = D;
            D = C;
            C = B;
            B = B + leftrotate(F, s[i]);
        }
        a0 += A;
        b0 += B;
        c0 += C;
        d0 += D;
    }

    uint32_t digest[4] = {a0, b0, c0, d0};
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) {
        oss << std::setw(2) << (digest[i] & 0xFFu)
            << std::setw(2) << ((digest[i] >> 8) & 0xFFu)
            << std::setw(2) << ((digest[i] >> 16) & 0xFFu)
            << std::setw(2) << ((digest[i] >> 24) & 0xFFu);
    }
    return oss.str();
}

std::string ExtractDigestParam(const std::string& header, const std::string& key) {
    std::string lower_hdr = ToLowerAscii(header);
    std::string lower_key = ToLowerAscii(key) + "=";
    size_t pos = 0;
    while ((pos = lower_hdr.find(lower_key, pos)) != std::string::npos) {
        if (pos == 0 || !std::isalnum(static_cast<unsigned char>(lower_hdr[pos - 1]))) {
            size_t val_start = pos + lower_key.size();
            if (val_start < header.size() && header[val_start] == '"') {
                ++val_start;
                size_t val_end = header.find('"', val_start);
                if (val_end != std::string::npos) {
                    return header.substr(val_start, val_end - val_start);
                }
            } else {
                size_t val_end = header.find_first_of(", \r\n", val_start);
                return header.substr(val_start, val_end - val_start);
            }
        }
        pos += lower_key.size();
    }
    return "";
}

std::string BuildHttpAuthHeader(const std::string& www_authenticate,
                                const std::string& method,
                                const std::string& uri,
                                const std::string& username,
                                const std::string& password) {
    if (username.empty()) return "";
    std::string lower = ToLowerAscii(www_authenticate);
    if (lower.find("digest") != std::string::npos) {
        std::string realm = ExtractDigestParam(www_authenticate, "realm");
        std::string nonce = ExtractDigestParam(www_authenticate, "nonce");
        std::string opaque = ExtractDigestParam(www_authenticate, "opaque");
        std::string qop_list = ExtractDigestParam(www_authenticate, "qop");
        if (nonce.empty()) return "";

        bool use_qop_auth = false;
        if (!qop_list.empty()) {
            std::istringstream qss(qop_list);
            std::string tok;
            while (std::getline(qss, tok, ',')) {
                if (TrimWhitespace(tok) == "auth") {
                    use_qop_auth = true;
                    break;
                }
            }
        }

        std::string cnonce = GenerateUuidV4().substr(0, 8);
        std::string nc = "00000001";
        std::string ha1 = Md5Hex(username + ":" + realm + ":" + password);
        std::string ha2 = Md5Hex(method + ":" + uri);
        std::string response;
        if (use_qop_auth) {
            response = Md5Hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":auth:" + ha2);
        } else {
            response = Md5Hex(ha1 + ":" + nonce + ":" + ha2);
        }

        std::ostringstream auth;
        auth << "Digest username=\"" << username << "\", "
             << "realm=\"" << realm << "\", "
             << "nonce=\"" << nonce << "\", "
             << "uri=\"" << uri << "\", "
             << "response=\"" << response << "\"";
        if (use_qop_auth) {
            auth << ", qop=auth, nc=" << nc << ", cnonce=\"" << cnonce << "\"";
        }
        if (!opaque.empty()) {
            auth << ", opaque=\"" << opaque << "\"";
        }
        return auth.str();
    }
    if (lower.find("basic") != std::string::npos) {
        std::string up = username + ":" + password;
        return "Basic " + Base64EncodeBytes(reinterpret_cast<const uint8_t*>(up.data()), up.size());
    }
    return "";
}

struct DigestSession {
    std::string realm;
    std::string nonce;
    std::string opaque;
    bool use_qop_auth{false};
    uint32_t nc{0};
    int64_t last_active_ms{0};
};

struct HostCapabilitySession {
    bool use_soap11{false};
    bool soap_negotiated{false};
};

struct PersistentConnection {
    int sock{-1};
    int64_t last_active_ms{0};
};

static std::mutex s_onvif_session_mutex;
static std::unordered_map<std::string, DigestSession> s_digest_sessions;
static std::unordered_map<std::string, HostCapabilitySession> s_soap_sessions;
static std::unordered_map<std::string, PersistentConnection> s_persistent_conns;

static void ClearAuthSessionsInternal() {
    std::lock_guard<std::mutex> lock(s_onvif_session_mutex);
    s_digest_sessions.clear();
    s_soap_sessions.clear();
    for (auto& pair : s_persistent_conns) {
        if (pair.second.sock >= 0) {
            close(pair.second.sock);
        }
    }
    s_persistent_conns.clear();
}

std::string BuildDigestHeaderFromSession(DigestSession& session,
                                         const std::string& method,
                                         const std::string& uri,
                                         const std::string& username,
                                         const std::string& password) {
    if (session.realm.empty() || session.nonce.empty() || username.empty()) return "";

    session.nc++;
    std::ostringstream nc_oss;
    nc_oss << std::hex << std::setw(8) << std::setfill('0') << session.nc;
    std::string nc_str = nc_oss.str();

    std::string cnonce = GenerateUuidV4().substr(0, 8);
    std::string ha1 = Md5Hex(username + ":" + session.realm + ":" + password);
    std::string ha2 = Md5Hex(method + ":" + uri);
    std::string response;
    if (session.use_qop_auth) {
        response = Md5Hex(ha1 + ":" + session.nonce + ":" + nc_str + ":" + cnonce + ":auth:" + ha2);
    } else {
        response = Md5Hex(ha1 + ":" + session.nonce + ":" + ha2);
    }

    std::ostringstream auth;
    auth << "Digest username=\"" << username << "\", "
         << "realm=\"" << session.realm << "\", "
         << "nonce=\"" << session.nonce << "\", "
         << "uri=\"" << uri << "\", "
         << "response=\"" << response << "\"";
    if (session.use_qop_auth) {
        auth << ", qop=auth, nc=" << nc_str << ", cnonce=\"" << cnonce << "\"";
    }
    if (!session.opaque.empty()) {
        auth << ", opaque=\"" << session.opaque << "\"";
    }
    return auth.str();
}

void UpdateDigestSession(const std::string& key, const std::string& www_authenticate) {
    std::string lower = ToLowerAscii(www_authenticate);
    if (lower.find("digest") == std::string::npos) return;

    DigestSession ds;
    ds.realm = ExtractDigestParam(www_authenticate, "realm");
    ds.nonce = ExtractDigestParam(www_authenticate, "nonce");
    ds.opaque = ExtractDigestParam(www_authenticate, "opaque");
    std::string qop_list = ExtractDigestParam(www_authenticate, "qop");
    if (!qop_list.empty()) {
        std::istringstream qss(qop_list);
        std::string tok;
        while (std::getline(qss, tok, ',')) {
            if (TrimWhitespace(tok) == "auth") {
                ds.use_qop_auth = true;
                break;
            }
        }
    }
    ds.nc = 0;
    ds.last_active_ms = time_utils::WallTimeMs();

    std::lock_guard<std::mutex> lock(s_onvif_session_mutex);
    s_digest_sessions[key] = std::move(ds);
}

std::string BuildWsSecurityHeader(const std::string& username,
                                  const std::string& password,
                                  int64_t clock_offset_ms = 0) {
    if (username.empty()) return "";

    uint8_t nonce_raw[16];
    std::random_device rd;
    std::mt19937 gen(rd() ^ static_cast<uint32_t>(time_utils::MonotonicUs()));
    for (int i = 0; i < 16; ++i) {
        nonce_raw[i] = static_cast<uint8_t>(gen() & 0xFF);
    }

    int64_t now_ms = time_utils::WallTimeMs() + clock_offset_ms;
    if (now_ms < 0) now_ms = 0;
    std::time_t sec = static_cast<std::time_t>(now_ms / 1000);
    int ms = static_cast<int>(now_ms % 1000);
    std::tm utc_tm{};
    gmtime_r(&sec, &utc_tm);
    char utc_buf[64];
    std::snprintf(utc_buf, sizeof(utc_buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday,
                  utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec, ms);
    std::string created(utc_buf);
    std::vector<uint8_t> concat;
    concat.insert(concat.end(), nonce_raw, nonce_raw + 16);
    concat.insert(concat.end(), created.begin(), created.end());
    concat.insert(concat.end(), password.begin(), password.end());

    uint8_t sha1_out[20];
    Sha1Digest(concat.data(), concat.size(), sha1_out);

    std::string digest_b64 = Base64EncodeBytes(sha1_out, 20);
    std::string nonce_b64 = Base64EncodeBytes(nonce_raw, 16);

    std::ostringstream oss;
    oss << "<Security s:mustUnderstand=\"1\" "
        << "xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
        << "<UsernameToken>"
        << "<Username>" << XmlEscape(username) << "</Username>"
        << "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">"
        << digest_b64 << "</Password>"
        << "<Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">"
        << nonce_b64 << "</Nonce>"
        << "<Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
        << created << "</Created>"
        << "</UsernameToken>"
        << "</Security>";
    return oss.str();
}

// Splits a whitespace-delimited WS-Discovery <XAddrs> list and selects the best reachable HTTP XAddr
std::string SelectBestXAddr(const std::string& xaddrs_raw, const std::string& preferred_ip) {
    std::istringstream iss(XmlUnescape(xaddrs_raw));
    std::string token;
    std::string first_valid;
    std::string first_ipv4;

    while (iss >> token) {
        token = TrimWhitespace(token);
        if (token.empty()) continue;
        if (first_valid.empty()) {
            first_valid = token;
        }
        if (!preferred_ip.empty() && token.find(preferred_ip) != std::string::npos) {
            return token;
        }
        if (first_ipv4.empty() && token.find('[') == std::string::npos) {
            first_ipv4 = token;
        }
    }
    if (!first_ipv4.empty()) return first_ipv4;
    return first_valid;
}

bool ParseHttpUrl(const std::string& raw_url, std::string& host, int& port, std::string& path) {
    std::string url = SelectBestXAddr(raw_url, "");
    if (url.empty()) return false;

    size_t scheme_end = url.find("://");
    std::string rem = (scheme_end != std::string::npos) ? url.substr(scheme_end + 3) : url;
    int default_port = (url.rfind("https://", 0) == 0) ? 443 : 80;

    size_t slash_pos = rem.find('/');
    size_t at_pos = rem.find('@');
    if (at_pos != std::string::npos && (slash_pos == std::string::npos || at_pos < slash_pos)) {
        rem = rem.substr(at_pos + 1);
        slash_pos = rem.find('/');
    }

    std::string host_port = (slash_pos != std::string::npos) ? rem.substr(0, slash_pos) : rem;
    path = (slash_pos != std::string::npos) ? rem.substr(slash_pos) : "/onvif/device_service";
    if (path.empty() || path[0] != '/') {
        path = "/" + path;
    }

    if (!host_port.empty() && host_port.front() == '[') {
        size_t close_bracket = host_port.find(']');
        if (close_bracket == std::string::npos) return false;
        host = host_port.substr(1, close_bracket - 1);
        if (close_bracket + 1 < host_port.size() && host_port[close_bracket + 1] == ':') {
            try {
                port = std::stoi(host_port.substr(close_bracket + 2));
            } catch (...) {
                port = default_port;
            }
        } else {
            port = default_port;
        }
    } else {
        size_t colon_pos = host_port.rfind(':');
        if (colon_pos != std::string::npos) {
            host = host_port.substr(0, colon_pos);
            try {
                port = std::stoi(host_port.substr(colon_pos + 1));
            } catch (...) {
                port = default_port;
            }
        } else {
            host = host_port;
            port = default_port;
        }
    }
    return !host.empty();
}

std::string NormalizeXAddrHost(const std::string& xaddr, const std::string& reachable_ip, int fallback_port) {
    if (xaddr.empty()) return "";
    std::string host, path;
    int port = fallback_port > 0 ? fallback_port : 80;
    if (!ParseHttpUrl(xaddr, host, port, path)) {
        return xaddr;
    }
    std::string scheme = (xaddr.rfind("https://", 0) == 0) ? "https://" : "http://";
    std::string use_host = (!reachable_ip.empty()) ? reachable_ip : host;
    return scheme + use_host + ":" + std::to_string(port) + path;
}

// Zero-backtracking linear XML tag value extractor
std::string ExtractXmlTagValue(const std::string& xml, const std::string& tag_name) {
    size_t search_pos = 0;
    while (search_pos < xml.size()) {
        size_t lt = xml.find('<', search_pos);
        if (lt == std::string::npos || lt + 1 >= xml.size()) break;
        if (xml[lt + 1] == '/' || xml[lt + 1] == '?' || xml[lt + 1] == '!') {
            search_pos = lt + 1;
            continue;
        }

        size_t gt = xml.find('>', lt + 1);
        if (gt == std::string::npos) break;
        if (xml[gt - 1] == '/') {
            search_pos = gt + 1;
            continue;
        }

        size_t name_end = lt + 1;
        while (name_end < gt && !std::isspace(static_cast<unsigned char>(xml[name_end])) && xml[name_end] != '/') {
            ++name_end;
        }
        std::string full_tag = xml.substr(lt + 1, name_end - (lt + 1));
        size_t colon = full_tag.find(':');
        std::string local_tag = (colon != std::string::npos) ? full_tag.substr(colon + 1) : full_tag;

        if (local_tag == tag_name) {
            std::string close_full = "</" + full_tag + ">";
            size_t close_pos = xml.find(close_full, gt + 1);
            if (close_pos == std::string::npos && colon != std::string::npos) {
                std::string close_local = ":" + tag_name + ">";
                size_t cand = xml.find(close_local, gt + 1);
                if (cand != std::string::npos) {
                    size_t rev = xml.rfind("</", cand);
                    if (rev != std::string::npos && rev >= gt + 1) {
                        close_pos = rev;
                    }
                }
            }
            if (close_pos != std::string::npos) {
                return XmlUnescape(xml.substr(gt + 1, close_pos - (gt + 1)));
            }
        }
        search_pos = gt + 1;
    }
    return "";
}

struct XmlElementBlock {
    std::string open_tag_attrs;
    std::string inner_xml;
};

// Zero-backtracking linear XML element block extractor (immune to libstdc++ std::regex stack overflow on large XML)
std::vector<XmlElementBlock> ExtractXmlBlocks(const std::string& xml, const std::string& tag_name) {
    std::vector<XmlElementBlock> blocks;
    size_t search_pos = 0;
    while (search_pos < xml.size()) {
        size_t lt = xml.find('<', search_pos);
        if (lt == std::string::npos || lt + 1 >= xml.size()) break;
        if (xml[lt + 1] == '/' || xml[lt + 1] == '?' || xml[lt + 1] == '!') {
            search_pos = lt + 1;
            continue;
        }

        size_t gt = xml.find('>', lt + 1);
        if (gt == std::string::npos) break;

        size_t name_end = lt + 1;
        while (name_end < gt && !std::isspace(static_cast<unsigned char>(xml[name_end])) && xml[name_end] != '/') {
            ++name_end;
        }
        std::string full_tag = xml.substr(lt + 1, name_end - (lt + 1));
        size_t colon = full_tag.find(':');
        std::string local_tag = (colon != std::string::npos) ? full_tag.substr(colon + 1) : full_tag;

        if (local_tag == tag_name) {
            std::string attrs = (name_end < gt) ? xml.substr(name_end, gt - name_end) : "";
            if (xml[gt - 1] == '/') {
                XmlElementBlock blk;
                blk.open_tag_attrs = std::move(attrs);
                blk.inner_xml = "";
                blocks.push_back(std::move(blk));
                search_pos = gt + 1;
                continue;
            }
            std::string close_full = "</" + full_tag + ">";
            size_t close_pos = xml.find(close_full, gt + 1);
            size_t next_search = gt + 1;
            if (close_pos != std::string::npos) {
                XmlElementBlock blk;
                blk.open_tag_attrs = std::move(attrs);
                blk.inner_xml = xml.substr(gt + 1, close_pos - (gt + 1));
                blocks.push_back(std::move(blk));
                next_search = close_pos + close_full.size();
            }
            search_pos = next_search;
        } else {
            search_pos = gt + 1;
        }
    }
    return blocks;
}

std::string ExtractXmlAttribute(const std::string& attrs, const std::string& attr_name) {
    size_t pos = 0;
    while ((pos = attrs.find(attr_name, pos)) != std::string::npos) {
        if (pos > 0 && (std::isalnum(static_cast<unsigned char>(attrs[pos - 1])) || attrs[pos - 1] == '_' || attrs[pos - 1] == '-')) {
            pos += attr_name.size();
            continue;
        }
        size_t eq = pos + attr_name.size();
        while (eq < attrs.size() && std::isspace(static_cast<unsigned char>(attrs[eq]))) ++eq;
        if (eq < attrs.size() && attrs[eq] == '=') {
            size_t q = eq + 1;
            while (q < attrs.size() && std::isspace(static_cast<unsigned char>(attrs[q]))) ++q;
            if (q < attrs.size() && (attrs[q] == '"' || attrs[q] == '\'')) {
                char quote = attrs[q];
                size_t end_q = attrs.find(quote, q + 1);
                if (end_q != std::string::npos) {
                    return XmlUnescape(attrs.substr(q + 1, end_q - (q + 1)));
                }
            }
        }
        pos += attr_name.size();
    }
    return "";
}

std::string DecodeChunkedHttpBody(const std::string& raw_body) {
    std::string decoded;
    decoded.reserve(raw_body.size());
    size_t pos = 0;
    while (pos < raw_body.size()) {
        size_t line_end = raw_body.find("\r\n", pos);
        if (line_end == std::string::npos) {
            decoded.append(raw_body.substr(pos));
            break;
        }
        std::string size_str = TrimWhitespace(raw_body.substr(pos, line_end - pos));
        size_t semi = size_str.find(';');
        if (semi != std::string::npos) {
            size_str = size_str.substr(0, semi);
        }
        if (size_str.empty()) {
            pos = line_end + 2;
            continue;
        }
        char* end_ptr = nullptr;
        unsigned long chunk_len = std::strtoul(size_str.c_str(), &end_ptr, 16);
        if (end_ptr == size_str.c_str()) {
            return raw_body;
        }
        pos = line_end + 2;
        if (chunk_len == 0) {
            break;
        }
        if (pos + chunk_len <= raw_body.size()) {
            decoded.append(raw_body, pos, chunk_len);
            pos += chunk_len;
            if (pos + 2 <= raw_body.size() && raw_body[pos] == '\r' && raw_body[pos + 1] == '\n') {
                pos += 2;
            }
        } else {
            decoded.append(raw_body.substr(pos));
            break;
        }
    }
    return decoded;
}

struct RawHttpResult {
    int status_code{0};
    std::string www_authenticate;
    std::string body;
    bool network_ok{false};
};

// Establishes a non-blocking TCP socket to host:port with TCP_NODELAY and socket timeouts
static int ConnectTcpSocket(const std::string& host, int port, int timeout_ms) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int c_ret = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (c_ret < 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }

    struct pollfd pfd{};
    pfd.fd = sock;
    pfd.events = POLLOUT;
    int poll_ret = 0;
    do {
        poll_ret = poll(&pfd, 1, timeout_ms);
    } while (poll_ret < 0 && errno == EINTR);
    if (poll_ret <= 0) {
        close(sock);
        return -1;
    }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
        close(sock);
        return -1;
    }

    fcntl(sock, F_SETFL, flags);
    struct timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    return sock;
}

RawHttpResult ExecuteSingleHttpSoapPost(const std::string& host,
                                        int port,
                                        const std::string& path,
                                        const std::string& soap_action,
                                        const std::string& soap_body_xml,
                                        const std::string& username,
                                        const std::string& password,
                                        int64_t clock_offset_ms,
                                        const std::string& http_auth_header,
                                        bool use_soap11,
                                        int timeout_ms) {
    RawHttpResult result;
    std::string host_key = host + ":" + std::to_string(port);

    const char* soap_ns = use_soap11
        ? "http://schemas.xmlsoap.org/soap/envelope/"
        : "http://www.w3.org/2003/05/soap-envelope";

    std::ostringstream envelope;
    envelope << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
             << "<s:Envelope xmlns:s=\"" << soap_ns << "\" "
             << "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
             << "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" "
             << "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
             << "xmlns:tr2=\"http://www.onvif.org/ver20/media/wsdl\" "
             << "xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" "
             << "xmlns:tt=\"http://www.onvif.org/ver10/schema\">";
    std::string wsse = BuildWsSecurityHeader(username, password, clock_offset_ms);
    if (!wsse.empty()) {
        envelope << "<s:Header>" << wsse << "</s:Header>";
    }
    envelope << "<s:Body>" << soap_body_xml << "</s:Body>"
             << "</s:Envelope>";
    std::string payload = envelope.str();

    std::ostringstream http_req;
    http_req << "POST " << path << " HTTP/1.1\r\n";
    if (port == 80) {
        http_req << "Host: " << host << "\r\n";
    } else {
        http_req << "Host: " << host << ":" << port << "\r\n";
    }
    http_req << "User-Agent: NVR_Core/2.1\r\n";
    if (use_soap11) {
        http_req << "Content-Type: text/xml; charset=utf-8\r\n"
                 << "SOAPAction: \"" << soap_action << "\"\r\n";
    } else {
        http_req << "Content-Type: application/soap+xml; charset=utf-8; action=\"" << soap_action << "\"\r\n";
    }
    if (!http_auth_header.empty()) {
        http_req << "Authorization: " << http_auth_header << "\r\n";
    }
    http_req << "Content-Length: " << payload.size() << "\r\n"
             << "Connection: keep-alive\r\n\r\n"
             << payload;
    std::string req_str = http_req.str();

    for (int attempt = 0; attempt < 2; ++attempt) {
        int sock = -1;
        bool is_reused = false;

        {
            std::lock_guard<std::mutex> lock(s_onvif_session_mutex);
            auto it = s_persistent_conns.find(host_key);
            if (it != s_persistent_conns.end() && it->second.sock >= 0) {
                int64_t now_ms = time_utils::WallTimeMs();
                if (now_ms - it->second.last_active_ms < 45000LL) {
                    struct pollfd pfd{};
                    pfd.fd = it->second.sock;
                    pfd.events = POLLIN;
                    int pr = poll(&pfd, 1, 0);
                    if (pr == 0) {
                        sock = it->second.sock;
                        is_reused = true;
                    } else if (pr > 0) {
                        char peek_buf[1];
                        ssize_t pn = recv(it->second.sock, peek_buf, 1, MSG_PEEK | MSG_DONTWAIT);
                        close(it->second.sock);
                        (void)pn;
                    } else {
                        close(it->second.sock);
                    }
                } else {
                    close(it->second.sock);
                }
                s_persistent_conns.erase(it);
            }
        }

        if (sock < 0) {
            sock = ConnectTcpSocket(host, port, timeout_ms);
            is_reused = false;
        }

        if (sock < 0) {
            return result;
        }

        size_t total_sent = 0;
        bool send_failed = false;
        while (total_sent < req_str.size()) {
            ssize_t n = send(sock, req_str.data() + total_sent, req_str.size() - total_sent, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) continue;
                send_failed = true;
                break;
            }
            if (n == 0) break;
            total_sent += static_cast<size_t>(n);
        }

        if (send_failed) {
            close(sock);
            if (is_reused && attempt == 0) {
                continue;
            }
            return result;
        }

        std::string raw_resp;
        raw_resp.reserve(8192);
        char buf[4096];
        size_t header_end = std::string::npos;
        long content_length = -1;
        bool is_chunked = false;
        bool server_connection_close = false;
        bool recv_failed = false;

        while (true) {
            ssize_t n = recv(sock, buf, sizeof(buf), 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                recv_failed = true;
                break;
            }
            if (n == 0) break;
            raw_resp.append(buf, static_cast<size_t>(n));

            if (header_end == std::string::npos) {
                header_end = raw_resp.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    std::string hdrs = raw_resp.substr(0, header_end);
                    std::istringstream hss(hdrs);
                    std::string line;
                    if (std::getline(hss, line)) {
                        std::istringstream lss(line);
                        std::string proto;
                        lss >> proto >> result.status_code;
                    }
                    while (std::getline(hss, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        size_t col = line.find(':');
                        if (col == std::string::npos) continue;
                        std::string k = ToLowerAscii(TrimWhitespace(line.substr(0, col)));
                        std::string v = TrimWhitespace(line.substr(col + 1));
                        if (k == "content-length") {
                            content_length = std::atol(v.c_str());
                        } else if (k == "transfer-encoding" && ToLowerAscii(v).find("chunked") != std::string::npos) {
                            is_chunked = true;
                        } else if (k == "connection" && ToLowerAscii(v).find("close") != std::string::npos) {
                            server_connection_close = true;
                        } else if (k == "www-authenticate") {
                            result.www_authenticate = v;
                        }
                    }
                }
            }

            if (header_end != std::string::npos) {
                size_t body_bytes = raw_resp.size() - (header_end + 4);
                if (content_length >= 0 && body_bytes >= static_cast<size_t>(content_length)) {
                    break;
                }
                if (is_chunked && raw_resp.find("\r\n0\r\n\r\n", header_end) != std::string::npos) {
                    break;
                }
            }
        }

        if (raw_resp.empty()) {
            close(sock);
            if (is_reused && attempt == 0) {
                continue;
            }
            return result;
        }

        result.network_ok = true;
        if (header_end != std::string::npos) {
            std::string raw_body = raw_resp.substr(header_end + 4);
            if (content_length >= 0 && raw_body.size() > static_cast<size_t>(content_length)) {
                raw_body.resize(static_cast<size_t>(content_length));
            }
            result.body = is_chunked ? DecodeChunkedHttpBody(raw_body) : raw_body;
        } else {
            result.body = raw_resp;
        }

        bool can_keep_alive = !server_connection_close && !recv_failed && (content_length >= 0 || is_chunked);
        if (can_keep_alive) {
            std::lock_guard<std::mutex> lock(s_onvif_session_mutex);
            auto it = s_persistent_conns.find(host_key);
            if (it != s_persistent_conns.end() && it->second.sock >= 0) {
                close(it->second.sock);
            }
            PersistentConnection pc;
            pc.sock = sock;
            pc.last_active_ms = time_utils::WallTimeMs();
            s_persistent_conns[host_key] = pc;
        } else {
            close(sock);
        }

        return result;
    }

    return result;
}

bool IsSoapResponseSuccess(const RawHttpResult& r) {
    if (!r.network_ok || r.status_code != 200) return false;
    if (r.body.find(":Fault>") != std::string::npos || r.body.find("<Fault>") != std::string::npos ||
        r.body.find("<Fault ") != std::string::npos) {
        return false;
    }
    return true;
}

bool SendSoapHttpRequest(const std::string& xaddr,
                         const std::string& soap_action,
                         const std::string& soap_body_xml,
                         const std::string& username,
                         const std::string& password,
                         std::string& out_response,
                         int timeout_ms,
                         int64_t clock_offset_ms = 0,
                         bool* out_network_reachable = nullptr) {
    std::string host, path;
    int port = 80;
    if (!ParseHttpUrl(xaddr, host, port, path)) {
        if (out_network_reachable) *out_network_reachable = false;
        return false;
    }

    std::string host_key = host + ":" + std::to_string(port);
    std::string digest_key = host_key + ":" + username;

    bool use_soap11 = false;
    std::string preemptive_auth;
    {
        std::lock_guard<std::mutex> lock(s_onvif_session_mutex);
        auto it_soap = s_soap_sessions.find(host_key);
        if (it_soap != s_soap_sessions.end() && it_soap->second.soap_negotiated) {
            use_soap11 = it_soap->second.use_soap11;
        }

        auto it_digest = s_digest_sessions.find(digest_key);
        if (it_digest != s_digest_sessions.end() && !username.empty()) {
            int64_t now_ms = time_utils::WallTimeMs();
            if (now_ms - it_digest->second.last_active_ms < 1800000LL) {
                preemptive_auth = BuildDigestHeaderFromSession(it_digest->second, "POST", path, username, password);
                it_digest->second.last_active_ms = now_ms;
            } else {
                s_digest_sessions.erase(it_digest);
            }
        }
    }

    RawHttpResult r = ExecuteSingleHttpSoapPost(host, port, path, soap_action, soap_body_xml,
                                                username, password, clock_offset_ms, preemptive_auth, use_soap11, timeout_ms);
    if (out_network_reachable) {
        *out_network_reachable = r.network_ok;
    }
    if (!r.network_ok) {
        return false;
    }

    // 1. Handle HTTP 401 Digest / Basic Auth Challenge
    if (r.status_code == 401 && !r.www_authenticate.empty() && !username.empty()) {
        UpdateDigestSession(digest_key, r.www_authenticate);
        std::string auth_hdr;
        {
            std::lock_guard<std::mutex> lock(s_onvif_session_mutex);
            auto it = s_digest_sessions.find(digest_key);
            if (it != s_digest_sessions.end()) {
                auth_hdr = BuildDigestHeaderFromSession(it->second, "POST", path, username, password);
            }
        }
        if (auth_hdr.empty()) {
            auth_hdr = BuildHttpAuthHeader(r.www_authenticate, "POST", path, username, password);
        }

        if (!auth_hdr.empty()) {
            r = ExecuteSingleHttpSoapPost(host, port, path, soap_action, soap_body_xml,
                                          username, password, clock_offset_ms, auth_hdr, use_soap11, timeout_ms);
        }
    }

    // 2. Fallback to SOAP 1.1 if camera rejected SOAP 1.2 with 415/VersionMismatch
    if (!IsSoapResponseSuccess(r) &&
        (r.status_code == 415 || r.body.find("VersionMismatch") != std::string::npos)) {
        use_soap11 = true;
        std::string auth_hdr;
        {
            std::lock_guard<std::mutex> lock(s_onvif_session_mutex);
            auto it = s_digest_sessions.find(digest_key);
            if (it != s_digest_sessions.end()) {
                auth_hdr = BuildDigestHeaderFromSession(it->second, "POST", path, username, password);
            }
        }
        if (auth_hdr.empty() && !r.www_authenticate.empty() && !username.empty()) {
            auth_hdr = BuildHttpAuthHeader(r.www_authenticate, "POST", path, username, password);
        }

        r = ExecuteSingleHttpSoapPost(host, port, path, soap_action, soap_body_xml,
                                      username, password, clock_offset_ms, auth_hdr, true, timeout_ms);
    }

    if (IsSoapResponseSuccess(r)) {
        std::lock_guard<std::mutex> lock(s_onvif_session_mutex);
        s_soap_sessions[host_key] = {use_soap11, true};
    }

    out_response = r.body;
    return IsSoapResponseSuccess(r);
}

std::string NormalizeRtspUri(const std::string& raw_uri,
                             const std::string& reachable_ip,
                             const std::string& username,
                             const std::string& password) {
    std::string uri = XmlUnescape(raw_uri);
    if (uri.empty()) return "";

    size_t proto_pos = uri.find("://");
    if (proto_pos == std::string::npos) return uri;

    std::string scheme = uri.substr(0, proto_pos + 3);
    std::string rem = uri.substr(proto_pos + 3);
    size_t slash_pos = rem.find('/');
    std::string authority = (slash_pos != std::string::npos) ? rem.substr(0, slash_pos) : rem;
    std::string path_and_query = (slash_pos != std::string::npos) ? rem.substr(slash_pos) : "/";

    std::string userinfo;
    std::string host_port = authority;
    size_t at_pos = authority.rfind('@');
    if (at_pos != std::string::npos) {
        userinfo = authority.substr(0, at_pos);
        host_port = authority.substr(at_pos + 1);
    } else if (!username.empty()) {
        userinfo = UrlEncodeUserInfoComponent(username) + ":" + UrlEncodeUserInfoComponent(password);
    }

    if (!reachable_ip.empty() && !host_port.empty() && host_port.front() != '[') {
        size_t colon_pos = host_port.rfind(':');
        std::string rtsp_host = (colon_pos != std::string::npos) ? host_port.substr(0, colon_pos) : host_port;
        std::string rtsp_port = (colon_pos != std::string::npos) ? host_port.substr(colon_pos) : "";
        if (!rtsp_host.empty() && rtsp_host != reachable_ip) {
            host_port = reachable_ip + rtsp_port;
        }
    }

    if (!userinfo.empty()) {
        return scheme + userinfo + "@" + host_port + path_and_query;
    }
    return scheme + host_port + path_and_query;
}

} // namespace

void OnvifClient::ClearAuthCache() {
    ClearAuthSessionsInternal();
}

std::vector<DiscoveredCamera> OnvifClient::DiscoverCameras(int timeout_ms, uint16_t multicast_port) {
    std::vector<DiscoveredCamera> cameras;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return cameras;

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    unsigned char ttl = 4;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    unsigned char loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    struct sockaddr_in dest_addr{};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(multicast_port);
    inet_pton(AF_INET, kMulticastIp, &dest_addr.sin_addr);

    auto build_probe_xml = [](bool typed_nvt) -> std::string {
        std::ostringstream probe_oss;
        probe_oss << "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                  << "<Envelope xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\" "
                  << "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
                  << "xmlns=\"http://www.w3.org/2003/05/soap-envelope\">"
                  << "<Header>"
                  << "<wsa:MessageID xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\">"
                  << "uuid:" << GenerateUuidV4() << "</wsa:MessageID>"
                  << "<wsa:To xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\">"
                  << "urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>"
                  << "<wsa:Action xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe\">"
                  << "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</wsa:Action>"
                  << "</Header>"
                  << "<Body>"
                  << "<Probe xmlns=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">";
        if (typed_nvt) {
            probe_oss << "<Types>dn:NetworkVideoTransmitter</Types>";
        }
        probe_oss << "</Probe>"
                  << "</Body>"
                  << "</Envelope>";
        return probe_oss.str();
    };

    std::string probe_nvt = build_probe_xml(true);
    std::string probe_any = build_probe_xml(false);

    bool any_sent = false;

    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0 && ifaddr) {
        for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_UP)) continue;

            struct sockaddr_in* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
            struct in_addr iface_addr = sa->sin_addr;
            if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iface_addr, sizeof(iface_addr)) == 0) {
                if (sendto(sock, probe_nvt.c_str(), probe_nvt.size(), 0,
                           reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr)) > 0) {
                    any_sent = true;
                }
                sendto(sock, probe_any.c_str(), probe_any.size(), 0,
                       reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
            }
        }
        freeifaddrs(ifaddr);
    }

    struct in_addr any_iface{};
    any_iface.s_addr = htonl(INADDR_ANY);
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &any_iface, sizeof(any_iface));
    if (sendto(sock, probe_nvt.c_str(), probe_nvt.size(), 0,
               reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr)) > 0) {
        any_sent = true;
    }

    if (multicast_port != 3702) {
        struct sockaddr_in lo_addr{};
        lo_addr.sin_family = AF_INET;
        lo_addr.sin_port = htons(multicast_port);
        inet_pton(AF_INET, "127.0.0.1", &lo_addr.sin_addr);
        if (sendto(sock, probe_nvt.c_str(), probe_nvt.size(), 0,
                   reinterpret_cast<struct sockaddr*>(&lo_addr), sizeof(lo_addr)) > 0) {
            any_sent = true;
        }
    }

    if (!any_sent) {
        close(sock);
        return cameras;
    }

    auto start_time = time_utils::MonotonicUs();
    std::vector<char> buffer(16384);

    while (true) {
        int64_t elapsed_ms = (time_utils::MonotonicUs() - start_time) / 1000;
        int remaining_ms = timeout_ms - static_cast<int>(elapsed_ms);
        if (remaining_ms <= 0) break;

        struct pollfd pfd{};
        pfd.fd = sock;
        pfd.events = POLLIN;

        int poll_ret = poll(&pfd, 1, remaining_ms);
        if (poll_ret < 0 && errno == EINTR) continue;
        if (poll_ret <= 0) break;

        struct sockaddr_in sender_addr{};
        socklen_t addr_len = sizeof(sender_addr);
        ssize_t len = recvfrom(sock, buffer.data(), buffer.size() - 1, 0,
                               reinterpret_cast<struct sockaddr*>(&sender_addr), &addr_len);
        if (len > 0) {
            buffer[len] = '\0';
            std::string xml(buffer.data(), static_cast<size_t>(len));
            if (xml.find("ProbeMatches") == std::string::npos && xml.find("ProbeMatch") == std::string::npos) {
                continue;
            }

            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sender_addr.sin_addr, ip_str, sizeof(ip_str));

            DiscoveredCamera cam;
            cam.ip_address = ip_str;

            std::string xaddrs_raw = ExtractXmlTagValue(xml, "XAddrs");
            if (!xaddrs_raw.empty()) {
                cam.xaddr = SelectBestXAddr(xaddrs_raw, cam.ip_address);
                std::string host, path;
                int parsed_port = 80;
                if (ParseHttpUrl(cam.xaddr, host, parsed_port, path)) {
                    cam.port = parsed_port;
                    if ((cam.ip_address.empty() || cam.ip_address == "0.0.0.0") && !host.empty()) {
                        cam.ip_address = host;
                    }
                }
            }

            cam.endpoint_reference = ExtractXmlTagValue(xml, "Address");
            cam.scopes = ExtractXmlTagValue(xml, "Scopes");
            if (!cam.scopes.empty()) {
                std::istringstream sss(cam.scopes);
                std::string scope_tok;
                while (sss >> scope_tok) {
                    const std::string kNamePrefix = "onvif://www.onvif.org/name/";
                    const std::string kHwPrefix = "onvif://www.onvif.org/hardware/";
                    if (scope_tok.rfind(kNamePrefix, 0) == 0 && cam.name.empty()) {
                        cam.name = UrlDecodeString(scope_tok.substr(kNamePrefix.size()));
                    } else if (scope_tok.rfind(kHwPrefix, 0) == 0 && cam.hardware.empty()) {
                        cam.hardware = UrlDecodeString(scope_tok.substr(kHwPrefix.size()));
                    }
                }
            }

            bool exists = false;
            for (const auto& c : cameras) {
                if (c.ip_address == cam.ip_address && c.port == cam.port &&
                    (c.xaddr == cam.xaddr || cam.xaddr.empty() || c.xaddr.empty())) {
                    exists = true;
                    break;
                }
            }
            if (!exists && !cam.ip_address.empty()) {
                cameras.push_back(cam);
                LOG_INFO << "[OnvifClient] Discovered ONVIF Camera: IP=" << cam.ip_address
                         << " Port=" << cam.port << " XAddr=" << cam.xaddr
                         << " Name=" << cam.name;
            }
        }
    }

    close(sock);
    return cameras;
}

int64_t OnvifClient::QueryCameraClockOffsetMs(const std::string& device_xaddr, int timeout_ms) {
    std::string resp;
    if (!SendSoapHttpRequest(device_xaddr,
                             "http://www.onvif.org/ver10/device/wsdl/GetSystemDateAndTime",
                             "<tds:GetSystemDateAndTime xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/>",
                             "", "", resp, timeout_ms, 0)) {
        return 0;
    }

    auto utc_blocks = ExtractXmlBlocks(resp, "UTCDateTime");
    const std::string& scope = utc_blocks.empty() ? resp : utc_blocks.front().inner_xml;

    std::string year_s  = ExtractXmlTagValue(scope, "Year");
    std::string month_s = ExtractXmlTagValue(scope, "Month");
    std::string day_s   = ExtractXmlTagValue(scope, "Day");
    std::string hour_s  = ExtractXmlTagValue(scope, "Hour");
    std::string min_s   = ExtractXmlTagValue(scope, "Minute");
    std::string sec_s   = ExtractXmlTagValue(scope, "Second");

    if (year_s.empty() || month_s.empty() || day_s.empty()) {
        return 0;
    }

    std::tm cam_tm{};
    cam_tm.tm_year = std::atoi(year_s.c_str()) - 1900;
    cam_tm.tm_mon  = std::atoi(month_s.c_str()) - 1;
    cam_tm.tm_mday = std::atoi(day_s.c_str());
    cam_tm.tm_hour = std::atoi(hour_s.c_str());
    cam_tm.tm_min  = std::atoi(min_s.c_str());
    cam_tm.tm_sec  = std::atoi(sec_s.c_str());

    if (cam_tm.tm_year < 70 || cam_tm.tm_mon < 0 || cam_tm.tm_mon > 11 || cam_tm.tm_mday < 1 || cam_tm.tm_mday > 31) {
        return 0;
    }

    std::time_t cam_epoch = timegm(&cam_tm);
    if (cam_epoch == static_cast<std::time_t>(-1)) {
        return 0;
    }

    int64_t cam_ms = static_cast<int64_t>(cam_epoch) * 1000LL;
    int64_t nvr_ms = time_utils::WallTimeMs();
    return cam_ms - nvr_ms;
}



std::string OnvifClient::GetStreamUri(const std::string& media_xaddr, const std::string& profile_token,
                                      const std::string& username, const std::string& password,
                                      int timeout_ms, int64_t clock_offset_ms, bool use_media2) {
    if (media_xaddr.empty() || profile_token.empty()) return "";

    std::ostringstream body;
    std::string action;
    if (use_media2) {
        action = "http://www.onvif.org/ver20/media/wsdl/GetStreamUri";
        body << "<tr2:GetStreamUri>"
             << "<tr2:Protocol>RTSP</tr2:Protocol>"
             << "<tr2:ProfileToken>" << XmlEscape(profile_token) << "</tr2:ProfileToken>"
             << "</tr2:GetStreamUri>";
    } else {
        action = "http://www.onvif.org/ver10/media/wsdl/GetStreamUri";
        body << "<trt:GetStreamUri>"
             << "<trt:StreamSetup>"
             << "<tt:Stream>RTP-Unicast</tt:Stream>"
             << "<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport>"
             << "</trt:StreamSetup>"
             << "<trt:ProfileToken>" << XmlEscape(profile_token) << "</trt:ProfileToken>"
             << "</trt:GetStreamUri>";
    }

    std::string resp;
    if (!SendSoapHttpRequest(media_xaddr, action, body.str(), username, password, resp, timeout_ms, clock_offset_ms)) {
        return "";
    }

    return ExtractXmlTagValue(resp, "Uri");
}

std::string OnvifClient::GetSnapshotUri(const std::string& media_xaddr, const std::string& profile_token,
                                        const std::string& username, const std::string& password,
                                        int timeout_ms, int64_t clock_offset_ms, bool use_media2) {
    if (media_xaddr.empty() || profile_token.empty()) return "";

    std::ostringstream body;
    std::string action;
    if (use_media2) {
        action = "http://www.onvif.org/ver20/media/wsdl/GetSnapshotUri";
        body << "<tr2:GetSnapshotUri>"
             << "<tr2:ProfileToken>" << XmlEscape(profile_token) << "</tr2:ProfileToken>"
             << "</tr2:GetSnapshotUri>";
    } else {
        action = "http://www.onvif.org/ver10/media/wsdl/GetSnapshotUri";
        body << "<trt:GetSnapshotUri>"
             << "<trt:ProfileToken>" << XmlEscape(profile_token) << "</trt:ProfileToken>"
             << "</trt:GetSnapshotUri>";
    }

    std::string resp;
    if (!SendSoapHttpRequest(media_xaddr, action, body.str(), username, password, resp, timeout_ms, clock_offset_ms)) {
        return "";
    }

    return ExtractXmlTagValue(resp, "Uri");
}

bool OnvifClient::GetDeviceInformation(const std::string& device_xaddr,
                                       const std::string& username, const std::string& password,
                                       OnvifDeviceInfo& out_info, int timeout_ms,
                                       int64_t clock_offset_ms) {
    std::string resp;
    bool ok = SendSoapHttpRequest(device_xaddr,
                                  "http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation",
                                  "<tds:GetDeviceInformation xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/>",
                                  username, password, resp, timeout_ms, clock_offset_ms);
    if (!ok) return false;

    out_info.manufacturer     = ExtractXmlTagValue(resp, "Manufacturer");
    out_info.model            = ExtractXmlTagValue(resp, "Model");
    out_info.firmware_version = ExtractXmlTagValue(resp, "FirmwareVersion");
    out_info.serial_number    = ExtractXmlTagValue(resp, "SerialNumber");
    out_info.hardware_id      = ExtractXmlTagValue(resp, "HardwareId");
    return true;
}

bool OnvifClient::SetSystemDateAndTime(const std::string& device_xaddr,
                                     const std::string& username, const std::string& password,
                                     int64_t target_utc_ms, const std::string& timezone_str,
                                     int timeout_ms, int64_t clock_offset_ms) {
    if (device_xaddr.empty()) return false;

    std::time_t sec = static_cast<std::time_t>(target_utc_ms / 1000);
    std::tm utc_tm{};
    gmtime_r(&sec, &utc_tm);

    std::ostringstream body;
    body << "<tds:SetSystemDateAndTime>"
         << "<tds:DateTimeType>Manual</tds:DateTimeType>"
         << "<tds:DaylightSavings>false</tds:DaylightSavings>"
         << "<tds:TimeZone><tt:TZ>" << XmlEscape(timezone_str.empty() ? "UTC" : timezone_str) << "</tt:TZ></tds:TimeZone>"
         << "<tds:UTCDateTime>"
         << "<tt:Time>"
         << "<tt:Hour>" << utc_tm.tm_hour << "</tt:Hour>"
         << "<tt:Minute>" << utc_tm.tm_min << "</tt:Minute>"
         << "<tt:Second>" << utc_tm.tm_sec << "</tt:Second>"
         << "</tt:Time>"
         << "<tt:Date>"
         << "<tt:Year>" << (utc_tm.tm_year + 1900) << "</tt:Year>"
         << "<tt:Month>" << (utc_tm.tm_mon + 1) << "</tt:Month>"
         << "<tt:Day>" << utc_tm.tm_mday << "</tt:Day>"
         << "</tt:Date>"
         << "</tds:UTCDateTime>"
         << "</tds:SetSystemDateAndTime>";

    std::string resp;
    return SendSoapHttpRequest(device_xaddr, "http://www.onvif.org/ver10/device/wsdl/SetSystemDateAndTime",
                               body.str(), username, password, resp, timeout_ms, clock_offset_ms);
}

bool OnvifClient::RebootCamera(const std::string& device_xaddr,
                              const std::string& username, const std::string& password,
                              int timeout_ms, int64_t clock_offset_ms) {
    if (device_xaddr.empty()) return false;

    std::string resp;
    return SendSoapHttpRequest(device_xaddr, "http://www.onvif.org/ver10/device/wsdl/SystemReboot",
                               "<tds:SystemReboot xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/>",
                               username, password, resp, timeout_ms, clock_offset_ms);
}

bool OnvifClient::GetVideoEncoderConfig(const std::string& media_xaddr, const std::string& config_or_profile_token,
                                       const std::string& username, const std::string& password,
                                       OnvifVideoEncoderConfig& out_config, int timeout_ms,
                                       int64_t clock_offset_ms, bool use_media2) {
    if (media_xaddr.empty() || config_or_profile_token.empty()) return false;

    std::ostringstream body;
    std::string action;
    if (use_media2) {
        action = "http://www.onvif.org/ver20/media/wsdl/GetVideoEncoderConfiguration";
        body << "<tr2:GetVideoEncoderConfiguration>"
             << "<tr2:ConfigurationToken>" << XmlEscape(config_or_profile_token) << "</tr2:ConfigurationToken>"
             << "</tr2:GetVideoEncoderConfiguration>";
    } else {
        action = "http://www.onvif.org/ver10/media/wsdl/GetVideoEncoderConfiguration";
        body << "<trt:GetVideoEncoderConfiguration>"
             << "<trt:ConfigurationToken>" << XmlEscape(config_or_profile_token) << "</trt:ConfigurationToken>"
             << "</trt:GetVideoEncoderConfiguration>";
    }

    std::string resp;
    if (!SendSoapHttpRequest(media_xaddr, action, body.str(), username, password, resp, timeout_ms, clock_offset_ms)) {
        return false;
    }

    out_config.token = config_or_profile_token;
    out_config.is_media2 = use_media2;
    out_config.name = ExtractXmlTagValue(resp, "Name");
    std::string enc = ExtractXmlTagValue(resp, "Encoding");
    if (!enc.empty()) {
        std::string up = ToLowerAscii(enc);
        if (up.find("265") != std::string::npos || up.find("hevc") != std::string::npos) out_config.encoding = "H265";
        else out_config.encoding = "H264";
    }
    std::string w_str = ExtractXmlTagValue(resp, "Width");
    std::string h_str = ExtractXmlTagValue(resp, "Height");
    std::string fps_str = ExtractXmlTagValue(resp, "FrameRateLimit");
    std::string br_str = ExtractXmlTagValue(resp, "BitrateLimit");
    std::string gov_str = ExtractXmlTagValue(resp, "GovLength");
    std::string prof_str = ExtractXmlTagValue(resp, "H264Profile");

    if (!w_str.empty()) out_config.width = std::atoi(w_str.c_str());
    if (!h_str.empty()) out_config.height = std::atoi(h_str.c_str());
    if (!fps_str.empty()) out_config.fps = static_cast<int>(std::atof(fps_str.c_str()));
    if (!br_str.empty()) out_config.bitrate_kbps = static_cast<int>(std::atof(br_str.c_str()));
    if (!gov_str.empty()) out_config.gov_length = std::atoi(gov_str.c_str());
    if (!prof_str.empty()) out_config.h264_profile = prof_str;

    return (out_config.width > 0 && out_config.height > 0);
}

bool OnvifClient::GetVideoEncoderOptions(const std::string& media_xaddr, const std::string& config_or_profile_token,
                                        const std::string& username, const std::string& password,
                                        OnvifVideoEncoderOptions& out_options, int timeout_ms,
                                        int64_t clock_offset_ms, bool use_media2) {
    if (media_xaddr.empty() || config_or_profile_token.empty()) return false;

    std::ostringstream body;
    std::string action;
    if (use_media2) {
        action = "http://www.onvif.org/ver20/media/wsdl/GetVideoEncoder2ConfigurationOptions";
        body << "<tr2:GetVideoEncoder2ConfigurationOptions>"
             << "<tr2:ConfigurationToken>" << XmlEscape(config_or_profile_token) << "</tr2:ConfigurationToken>"
             << "</tr2:GetVideoEncoder2ConfigurationOptions>";
    } else {
        action = "http://www.onvif.org/ver10/media/wsdl/GetVideoEncoderConfigurationOptions";
        body << "<trt:GetVideoEncoderConfigurationOptions>"
             << "<trt:ConfigurationToken>" << XmlEscape(config_or_profile_token) << "</trt:ConfigurationToken>"
             << "</trt:GetVideoEncoderConfigurationOptions>";
    }

    std::string resp;
    if (!SendSoapHttpRequest(media_xaddr, action, body.str(), username, password, resp, timeout_ms, clock_offset_ms)) {
        return false;
    }

    out_options.is_media2 = use_media2;
    out_options.supported_encodings = {"H264"};
    if (resp.find("H265") != std::string::npos || resp.find("HEVC") != std::string::npos) {
        out_options.supported_encodings.push_back("H265");
    }

    auto res_blocks = ExtractXmlBlocks(resp, "ResolutionsAvailable");
    for (const auto& rb : res_blocks) {
        std::string w_str = ExtractXmlTagValue(rb.inner_xml, "Width");
        std::string h_str = ExtractXmlTagValue(rb.inner_xml, "Height");
        if (!w_str.empty() && !h_str.empty()) {
            OnvifVideoResolution res;
            res.width = std::atoi(w_str.c_str());
            res.height = std::atoi(h_str.c_str());
            if (res.width > 0 && res.height > 0) {
                out_options.available_resolutions.push_back(res);
            }
        }
    }

    auto fps_blocks = ExtractXmlBlocks(resp, "FrameRateRange");
    if (!fps_blocks.empty()) {
        std::string min_s = ExtractXmlTagValue(fps_blocks.front().inner_xml, "Min");
        std::string max_s = ExtractXmlTagValue(fps_blocks.front().inner_xml, "Max");
        if (!min_s.empty()) out_options.min_fps = static_cast<int>(std::atof(min_s.c_str()));
        if (!max_s.empty()) out_options.max_fps = static_cast<int>(std::atof(max_s.c_str()));
    }

    auto br_blocks = ExtractXmlBlocks(resp, "BitrateRange");
    if (!br_blocks.empty()) {
        std::string min_s = ExtractXmlTagValue(br_blocks.front().inner_xml, "Min");
        std::string max_s = ExtractXmlTagValue(br_blocks.front().inner_xml, "Max");
        if (!min_s.empty()) out_options.min_bitrate_kbps = static_cast<int>(std::atof(min_s.c_str()));
        if (!max_s.empty()) out_options.max_bitrate_kbps = static_cast<int>(std::atof(max_s.c_str()));
    }

    auto gov_blocks = ExtractXmlBlocks(resp, "GovLengthRange");
    if (!gov_blocks.empty()) {
        std::string min_s = ExtractXmlTagValue(gov_blocks.front().inner_xml, "Min");
        std::string max_s = ExtractXmlTagValue(gov_blocks.front().inner_xml, "Max");
        if (!min_s.empty()) out_options.min_gov_length = std::atoi(min_s.c_str());
        if (!max_s.empty()) out_options.max_gov_length = std::atoi(max_s.c_str());
    }

    return true;
}

bool OnvifClient::SetVideoEncoderConfig(const std::string& media_xaddr, const OnvifVideoEncoderConfig& config,
                                       const std::string& username, const std::string& password,
                                       int timeout_ms, int64_t clock_offset_ms, bool use_media2) {
    if (media_xaddr.empty() || config.token.empty()) return false;

    std::ostringstream body;
    std::string action;
    std::string enc_tag = (config.encoding == "H265") ? "H265" : "H264";

    if (use_media2) {
        action = "http://www.onvif.org/ver20/media/wsdl/SetVideoEncoder2Configuration";
        body << "<tr2:SetVideoEncoder2Configuration>"
             << "<tr2:Configuration token=\"" << XmlEscape(config.token) << "\">"
             << "<tt:Name>" << XmlEscape(config.name.empty() ? "VideoEncoderConfig" : config.name) << "</tt:Name>"
             << "<tr2:Encoding>" << enc_tag << "</tr2:Encoding>"
             << "<tr2:Resolution>"
             << "<tt:Width>" << config.width << "</tt:Width>"
             << "<tt:Height>" << config.height << "</tt:Height>"
             << "</tr2:Resolution>"
             << "<tr2:RateControl>"
             << "<tr2:FrameRateLimit>" << config.fps << "</tr2:FrameRateLimit>"
             << "<tr2:BitrateLimit>" << config.bitrate_kbps << "</tr2:BitrateLimit>"
             << "</tr2:RateControl>"
             << "<tr2:GovLength>" << config.gov_length << "</tr2:GovLength>"
             << "</tr2:Configuration>"
             << "</tr2:SetVideoEncoder2Configuration>";
    } else {
        action = "http://www.onvif.org/ver10/media/wsdl/SetVideoEncoderConfiguration";
        body << "<trt:SetVideoEncoderConfiguration>"
             << "<trt:Configuration token=\"" << XmlEscape(config.token) << "\">"
             << "<tt:Name>" << XmlEscape(config.name.empty() ? "VideoEncoderConfig" : config.name) << "</tt:Name>"
             << "<tt:UseCount>1</tt:UseCount>"
             << "<tt:Encoding>" << enc_tag << "</tt:Encoding>"
             << "<tt:Resolution>"
             << "<tt:Width>" << config.width << "</tt:Width>"
             << "<tt:Height>" << config.height << "</tt:Height>"
             << "</tt:Resolution>"
             << "<tt:RateControl>"
             << "<tt:FrameRateLimit>" << config.fps << "</tt:FrameRateLimit>"
             << "<tt:EncodingInterval>1</tt:EncodingInterval>"
             << "<tt:BitrateLimit>" << config.bitrate_kbps << "</tt:BitrateLimit>"
             << "</tt:RateControl>";
        if (enc_tag == "H264") {
            body << "<tt:H264>"
                 << "<tt:GovLength>" << config.gov_length << "</tt:GovLength>"
                 << "<tt:H264Profile>" << XmlEscape(config.h264_profile.empty() ? "Main" : config.h264_profile) << "</tt:H264Profile>"
                 << "</tt:H264>";
        } else {
            body << "<tt:H265>"
                 << "<tt:GovLength>" << config.gov_length << "</tt:GovLength>"
                 << "<tt:H265Profile>Main</tt:H265Profile>"
                 << "</tt:H265>";
        }
        body << "</trt:Configuration>"
             << "<trt:ForcePersistence>true</trt:ForcePersistence>"
             << "</trt:SetVideoEncoderConfiguration>";
    }

    std::string resp;
    bool ok = SendSoapHttpRequest(media_xaddr, action, body.str(), username, password, resp, timeout_ms, clock_offset_ms);
    if (!ok) {
        LOG_WARN << "[OnvifClient] SetVideoEncoderConfiguration failed for " << media_xaddr;
    }
    return ok;
}



bool OnvifClient::GetProfiles(const std::string& media_xaddr,
                              const std::string& username, const std::string& password,
                              std::vector<OnvifProfileInfo>& out_profiles, int timeout_ms,
                              int64_t clock_offset_ms, bool use_media2) {
    std::string resp;
    std::string action = use_media2
        ? "http://www.onvif.org/ver20/media/wsdl/GetProfiles"
        : "http://www.onvif.org/ver10/media/wsdl/GetProfiles";
    std::string req_body = use_media2
        ? "<tr2:GetProfiles xmlns:tr2=\"http://www.onvif.org/ver20/media/wsdl\"><tr2:Type>All</tr2:Type></tr2:GetProfiles>"
        : "<trt:GetProfiles xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"/>";

    if (!SendSoapHttpRequest(media_xaddr, action, req_body, username, password, resp, timeout_ms, clock_offset_ms)) {
        return false;
    }

    auto prof_blocks = ExtractXmlBlocks(resp, "Profiles");
    for (const auto& blk : prof_blocks) {
        OnvifProfileInfo p;
        p.is_media2 = use_media2;
        p.token = ExtractXmlAttribute(blk.open_tag_attrs, "token");
        if (p.token.empty()) continue;

        p.name = ExtractXmlTagValue(blk.inner_xml, "Name");

        std::string venc_scope = blk.inner_xml;
        auto venc_blocks = ExtractXmlBlocks(blk.inner_xml, "VideoEncoderConfiguration");
        if (venc_blocks.empty()) {
            venc_blocks = ExtractXmlBlocks(blk.inner_xml, "VideoEncoder");
        }
        if (!venc_blocks.empty()) {
            venc_scope = venc_blocks.front().inner_xml;
            p.video_encoder_token = ExtractXmlAttribute(venc_blocks.front().open_tag_attrs, "token");
        } else {
            auto vsrc_blocks = ExtractXmlBlocks(blk.inner_xml, "VideoSourceConfiguration");
            if (vsrc_blocks.empty() && ExtractXmlBlocks(blk.inner_xml, "VideoSource").empty() &&
                ExtractXmlTagValue(blk.inner_xml, "Width").empty()) {
                continue;
            }
        }

        auto vsrc_blocks = ExtractXmlBlocks(blk.inner_xml, "VideoSourceConfiguration");
        if (vsrc_blocks.empty()) {
            vsrc_blocks = ExtractXmlBlocks(blk.inner_xml, "VideoSource");
        }
        if (!vsrc_blocks.empty()) {
            p.video_source_token = ExtractXmlTagValue(vsrc_blocks.front().inner_xml, "SourceToken");
            if (p.video_source_token.empty()) {
                p.video_source_token = ExtractXmlAttribute(vsrc_blocks.front().open_tag_attrs, "token");
            }
        }
        if (p.video_source_token.empty()) {
            p.video_source_token = ExtractXmlTagValue(blk.inner_xml, "VideoSourceToken");
        }

        std::string enc = ExtractXmlTagValue(venc_scope, "Encoding");
        if (!enc.empty()) {
            std::string up_enc = enc;
            for (char& c : up_enc) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (up_enc.find("265") != std::string::npos || up_enc.find("HEVC") != std::string::npos) {
                p.encoding = "H265";
            } else if (up_enc.find("264") != std::string::npos || up_enc.find("AVC") != std::string::npos) {
                p.encoding = "H264";
            } else {
                p.encoding = enc;
            }
        }

        std::string w_str   = ExtractXmlTagValue(venc_scope, "Width");
        std::string h_str   = ExtractXmlTagValue(venc_scope, "Height");
        std::string fps_str = ExtractXmlTagValue(venc_scope, "FrameRateLimit");
        std::string br_str  = ExtractXmlTagValue(venc_scope, "BitrateLimit");

        if (!w_str.empty())   p.width = std::atoi(w_str.c_str());
        if (!h_str.empty())   p.height = std::atoi(h_str.c_str());
        if (!fps_str.empty()) p.fps = static_cast<int>(std::atof(fps_str.c_str()));
        if (!br_str.empty())  p.bitrate_kbps = static_cast<int>(std::atof(br_str.c_str()));

        bool dup = false;
        for (auto& existing : out_profiles) {
            if (existing.token == p.token) {
                if (use_media2 && existing.encoding != "H265" && p.encoding == "H265") {
                    existing = p;
                }
                dup = true;
                break;
            }
        }
        if (!dup) {
            out_profiles.push_back(p);
        }
    }
    return !out_profiles.empty();
}

bool OnvifClient::ProbeCamera(const std::string& ip_or_xaddr, int port,
                              const std::string& username, const std::string& password,
                              OnvifCameraDetails& out_details, int timeout_ms) {
    bool explicit_url = (ip_or_xaddr.find("://") != std::string::npos);
    std::string device_xaddr = SelectBestXAddr(ip_or_xaddr, "");
    int use_port = (port > 0) ? port : 80;
    if (!explicit_url) {
        device_xaddr = "http://" + device_xaddr + ":" + std::to_string(use_port) + "/onvif/device_service";
    }

    std::string host, path;
    int parsed_port = use_port;
    if (!ParseHttpUrl(device_xaddr, host, parsed_port, path)) {
        return false;
    }

    std::vector<int> candidate_ports = {parsed_port};
    if (!explicit_url) {
        for (int fp : {80, 8090, 8000, 8080, 8899}) {
            if (std::find(candidate_ports.begin(), candidate_ports.end(), fp) == candidate_ports.end()) {
                candidate_ports.push_back(fp);
            }
        }
    }

    bool connected = false;
    for (size_t idx = 0; idx < candidate_ports.size(); ++idx) {
        int try_port = candidate_ports[idx];
        std::string try_xaddr = explicit_url
            ? device_xaddr
            : ("http://" + host + ":" + std::to_string(try_port) + path);

        int try_timeout = (idx == 0) ? timeout_ms : std::min(timeout_ms, 800);

        int64_t clock_offset_ms = QueryCameraClockOffsetMs(try_xaddr, try_timeout);

        OnvifDeviceInfo dev_info;
        bool reachable = false;
        std::string capabilities_resp;
        if (GetDeviceInformation(try_xaddr, username, password, dev_info, try_timeout, clock_offset_ms)) {
            device_xaddr = try_xaddr;
            parsed_port = try_port;
            out_details.clock_offset_ms = clock_offset_ms;
            out_details.device_info = dev_info;
            connected = true;
            break;
        }
        if (SendSoapHttpRequest(try_xaddr,
                                "http://www.onvif.org/ver10/device/wsdl/GetCapabilities",
                                "<tds:GetCapabilities xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"><tds:Category>All</tds:Category></tds:GetCapabilities>",
                                username, password, capabilities_resp, try_timeout, clock_offset_ms, &reachable)) {
            device_xaddr = try_xaddr;
            parsed_port = try_port;
            out_details.clock_offset_ms = clock_offset_ms;
            connected = true;
            break;
        }
        if (reachable && idx == 0) {
            device_xaddr = try_xaddr;
            parsed_port = try_port;
            out_details.clock_offset_ms = clock_offset_ms;
            break;
        }
    }

    out_details.xaddr = device_xaddr;
    out_details.ip_address = host;
    out_details.port = parsed_port;
    int64_t clock_offset_ms = out_details.clock_offset_ms;

    std::string svc_resp;
    std::string media_xaddr;
    std::string media2_xaddr;

    if (SendSoapHttpRequest(device_xaddr,
                            "http://www.onvif.org/ver10/device/wsdl/GetServices",
                            "<tds:GetServices xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"><tds:IncludeCapability>false</tds:IncludeCapability></tds:GetServices>",
                            username, password, svc_resp, timeout_ms, clock_offset_ms)) {
        auto svc_blocks = ExtractXmlBlocks(svc_resp, "Service");
        for (const auto& sb : svc_blocks) {
            std::string ns = ExtractXmlTagValue(sb.inner_xml, "Namespace");
            std::string xa = SelectBestXAddr(ExtractXmlTagValue(sb.inner_xml, "XAddr"), host);
            if (ns == "http://www.onvif.org/ver10/media/wsdl" && !xa.empty()) {
                media_xaddr = NormalizeXAddrHost(xa, host, parsed_port);
            } else if (ns == "http://www.onvif.org/ver20/media/wsdl" && !xa.empty()) {
                media2_xaddr = NormalizeXAddrHost(xa, host, parsed_port);
            }
        }
    }

    if (media_xaddr.empty()) {
        std::string cap_resp;
        if (SendSoapHttpRequest(device_xaddr,
                                "http://www.onvif.org/ver10/device/wsdl/GetCapabilities",
                                "<tds:GetCapabilities xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"><tds:Category>Media</tds:Category></tds:GetCapabilities>",
                                username, password, cap_resp, timeout_ms, clock_offset_ms) ||
            SendSoapHttpRequest(device_xaddr,
                                "http://www.onvif.org/ver10/device/wsdl/GetCapabilities",
                                "<tds:GetCapabilities xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"><tds:Category>All</tds:Category></tds:GetCapabilities>",
                                username, password, cap_resp, timeout_ms, clock_offset_ms)) {
            auto media_blocks = ExtractXmlBlocks(cap_resp, "Media");
            if (!media_blocks.empty()) {
                std::string xa = SelectBestXAddr(ExtractXmlTagValue(media_blocks.front().inner_xml, "XAddr"), host);
                if (!xa.empty()) {
                    media_xaddr = NormalizeXAddrHost(xa, host, parsed_port);
                }
            }
        }
    }

    if (media_xaddr.empty()) {
        media_xaddr = "http://" + out_details.ip_address + ":" + std::to_string(out_details.port) + "/onvif/media_service";
    }
    out_details.media_xaddr = media_xaddr;
    out_details.media2_xaddr = media2_xaddr;

    if (!media2_xaddr.empty()) {
        GetProfiles(media2_xaddr, username, password, out_details.profiles, timeout_ms, clock_offset_ms, true);
    }
    GetProfiles(media_xaddr, username, password, out_details.profiles, timeout_ms, clock_offset_ms, false);

    if (out_details.profiles.empty() && media2_xaddr.empty()) {
        std::string fallback_m2 = "http://" + out_details.ip_address + ":" + std::to_string(out_details.port) + "/onvif/media2_service";
        if (GetProfiles(fallback_m2, username, password, out_details.profiles, timeout_ms, clock_offset_ms, true)) {
            out_details.media2_xaddr = fallback_m2;
        }
    }

    if (!out_details.profiles.empty()) {
        for (auto& prof : out_details.profiles) {
            std::string use_xaddr = (prof.is_media2 && !out_details.media2_xaddr.empty())
                ? out_details.media2_xaddr
                : out_details.media_xaddr;
            std::string raw_uri = GetStreamUri(use_xaddr, prof.token, username, password,
                                               timeout_ms, clock_offset_ms, prof.is_media2);
            if (raw_uri.empty() && prof.is_media2) {
                raw_uri = GetStreamUri(out_details.media_xaddr, prof.token, username, password,
                                       timeout_ms, clock_offset_ms, false);
            }
            prof.rtsp_uri = NormalizeRtspUri(raw_uri, out_details.ip_address, username, password);

            std::string snap_raw = GetSnapshotUri(use_xaddr, prof.token, username, password,
                                                  timeout_ms, clock_offset_ms, prof.is_media2);
            if (snap_raw.empty() && prof.is_media2) {
                snap_raw = GetSnapshotUri(out_details.media_xaddr, prof.token, username, password,
                                          timeout_ms, clock_offset_ms, false);
            }
            prof.snapshot_uri = NormalizeRtspUri(snap_raw, out_details.ip_address, username, password);
        }

        int best_main_idx = -1;
        int64_t best_main_score = -1;
        int best_sub_idx = -1;
        int64_t best_sub_score = INT64_MAX;

        for (size_t i = 0; i < out_details.profiles.size(); ++i) {
            const auto& p = out_details.profiles[i];
            if (p.rtsp_uri.empty()) continue;
            int64_t pixels = static_cast<int64_t>(p.width) * static_cast<int64_t>(p.height);
            int64_t score = (pixels > 0) ? (pixels * 100000LL + p.bitrate_kbps) : p.bitrate_kbps;
            if (best_main_idx == -1 || score > best_main_score) {
                best_main_score = score;
                best_main_idx = static_cast<int>(i);
            }
        }

        for (size_t i = 0; i < out_details.profiles.size(); ++i) {
            const auto& p = out_details.profiles[i];
            if (p.rtsp_uri.empty()) continue;
            if (static_cast<int>(i) == best_main_idx && out_details.profiles.size() > 1) continue;
            int64_t pixels = static_cast<int64_t>(p.width) * static_cast<int64_t>(p.height);
            int64_t score = (pixels > 0) ? (pixels * 100000LL + p.bitrate_kbps) : p.bitrate_kbps;
            if (best_sub_idx == -1 || score < best_sub_score) {
                best_sub_score = score;
                best_sub_idx = static_cast<int>(i);
            }
        }

        if (best_main_idx >= 0) {
            out_details.main_rtsp_url = out_details.profiles[best_main_idx].rtsp_uri;
            out_details.snapshot_url = out_details.profiles[best_main_idx].snapshot_uri;
        }
        if (best_sub_idx >= 0) {
            out_details.sub_rtsp_url = out_details.profiles[best_sub_idx].rtsp_uri;
        } else {
            out_details.sub_rtsp_url = out_details.main_rtsp_url;
        }
        if (out_details.snapshot_url.empty() && !out_details.profiles.empty()) {
            out_details.snapshot_url = out_details.profiles.front().snapshot_uri;
        }

        // Query Camera Hardware Network Interfaces & MAC Address
        std::vector<OnvifNetworkInterface> ifaces;
        if (GetNetworkInterfaces(device_xaddr, username, password, ifaces, timeout_ms, clock_offset_ms)) {
            out_details.network_interfaces = ifaces;
            for (const auto& ifc : ifaces) {
                if (!ifc.hw_address.empty()) {
                    out_details.mac_address = ifc.hw_address;
                    break;
                }
            }
        }

        return true;
    }

    return connected && !out_details.device_info.model.empty();
}

bool OnvifClient::GetNetworkInterfaces(const std::string& device_xaddr,
                                       const std::string& username, const std::string& password,
                                       std::vector<OnvifNetworkInterface>& out_interfaces,
                                       int timeout_ms, int64_t clock_offset_ms) {
    if (device_xaddr.empty()) return false;

    std::string resp;
    if (!SendSoapHttpRequest(device_xaddr,
                             "http://www.onvif.org/ver10/device/wsdl/GetNetworkInterfaces",
                             "<tds:GetNetworkInterfaces xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/>",
                             username, password, resp, timeout_ms, clock_offset_ms)) {
        return false;
    }

    auto iface_blocks = ExtractXmlBlocks(resp, "NetworkInterfaces");
    for (const auto& ib : iface_blocks) {
        OnvifNetworkInterface ifc;
        ifc.token = ExtractXmlAttribute(ib.open_tag_attrs, "token");
        ifc.name = ExtractXmlTagValue(ib.inner_xml, "Name");
        ifc.hw_address = ExtractXmlTagValue(ib.inner_xml, "HwAddress");

        auto ipv4_blocks = ExtractXmlBlocks(ib.inner_xml, "IPv4");
        const std::string& ipv4_xml = ipv4_blocks.empty() ? ib.inner_xml : ipv4_blocks.front().inner_xml;
        ifc.ipv4_address = ExtractXmlTagValue(ipv4_xml, "Address");
        std::string prefix_s = ExtractXmlTagValue(ipv4_xml, "PrefixLength");
        if (!prefix_s.empty()) {
            int prefix = std::atoi(prefix_s.c_str());
            if (prefix >= 0 && prefix <= 32) {
                uint32_t mask = (prefix == 0) ? 0 : (~0U << (32 - prefix));
                char mask_buf[32];
                std::snprintf(mask_buf, sizeof(mask_buf), "%u.%u.%u.%u",
                              (mask >> 24) & 0xFF, (mask >> 16) & 0xFF,
                              (mask >> 8) & 0xFF, mask & 0xFF);
                ifc.subnet_mask = mask_buf;
            }
        }
        std::string dhcp_s = ExtractXmlTagValue(ipv4_xml, "DHCP");
        if (dhcp_s.empty()) dhcp_s = ExtractXmlTagValue(ipv4_xml, "FromDHCP");
        ifc.dhcp_enabled = (ToLowerAscii(dhcp_s) == "true");

        if (!ifc.hw_address.empty() || !ifc.ipv4_address.empty()) {
            out_interfaces.push_back(std::move(ifc));
        }
    }
    return true;
}

} // namespace nvr
