#include "ConnectionProfile.h"

#include <string.h>

namespace {
bool fail(char* error, size_t size, const char* message) {
    if (error && size) {
        const size_t len = strlen(message);
        const size_t copy = len < size - 1 ? len : size - 1;
        memcpy(error, message, copy);
        error[copy] = '\0';
    }
    return false;
}

template <size_t N>
bool lengthOf(const char (&value)[N], size_t& length) {
    const char* end = static_cast<const char*>(memchr(value, '\0', N));
    if (!end) return false;
    length = static_cast<size_t>(end - value);
    return true;
}

bool hexDigit(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

bool ipv4(const char* host, size_t length) {
    size_t pos = 0;
    for (unsigned part = 0; part < 4; ++part) {
        unsigned value = 0, digits = 0;
        while (pos < length && host[pos] >= '0' && host[pos] <= '9') {
            value = value * 10 + static_cast<unsigned>(host[pos++] - '0');
            if (++digits > 3 || value > 255) return false;
        }
        if (!digits) return false;
        if (part == 3) return pos == length;
        if (pos >= length || host[pos++] != '.') return false;
    }
    return false;
}

// Bare IPv6, including an IPv4 tail. No URL brackets, port, or zone suffix.
bool ipv6(const char* host, size_t length) {
    size_t pos = 0;
    unsigned groups = 0;
    bool compressed = false;
    if (host[0] == ':') {
        if (length < 2 || host[1] != ':') return false;
        compressed = true;
        pos = 2;
    }
    while (pos < length) {
        const size_t start = pos;
        while (pos < length && host[pos] != ':') ++pos;
        const size_t digits = pos - start;
        if (memchr(host + start, '.', digits)) {
            if (!ipv4(host + start, digits) || pos != length) return false;
            groups += 2;
            break;
        }
        if (!digits || digits > 4) return false;
        for (size_t i = start; i < pos; ++i) {
            if (!hexDigit(static_cast<unsigned char>(host[i]))) return false;
        }
        if (++groups > 8) return false;
        if (pos == length) break;
        ++pos;
        if (pos < length && host[pos] == ':') {
            if (compressed) return false;
            compressed = true;
            ++pos;
        } else if (pos == length) {
            return false;
        }
    }
    return compressed ? groups < 8 : groups == 8;
}

bool validHost(const char* host, size_t length) {
    if (memchr(host, ':', length)) return ipv6(host, length);
    bool numeric = true;
    for (size_t i = 0; i < length; ++i) {
        if ((host[i] < '0' || host[i] > '9') && host[i] != '.') numeric = false;
    }
    // Numeric dotted addresses must be valid IPv4, not malformed addresses
    // accidentally accepted as DNS names. Single numeric DNS labels are allowed.
    if (numeric && memchr(host, '.', length)) return ipv4(host, length);
    if (host[length - 1] == '.') --length; // Fully-qualified DNS name.
    if (!length) return false;
    size_t labelStart = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (i == length || host[i] == '.') {
            const size_t n = i - labelStart;
            if (!n || n > 63 || host[labelStart] == '-' || host[i - 1] == '-') {
                return false;
            }
            labelStart = i + 1;
        } else {
            const unsigned char c = static_cast<unsigned char>(host[i]);
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-')) return false;
        }
    }
    return true;
}
} // namespace

bool validateConnectionProfile(const ConnectionProfile& profile,
                               char* error, size_t errorSize) {
    if (error && errorSize) error[0] = '\0';
    size_t name, ssid, wifi, host, vnc;
    if (!lengthOf(profile.name, name)) return fail(error, errorSize, "Name is not NUL terminated");
    if (!lengthOf(profile.ssid, ssid)) return fail(error, errorSize, "SSID is not NUL terminated");
    if (!lengthOf(profile.wifiPassword, wifi)) return fail(error, errorSize, "WiFi password is not NUL terminated");
    if (!lengthOf(profile.vncHost, host)) return fail(error, errorSize, "VNC host is not NUL terminated");
    if (!lengthOf(profile.vncPassword, vnc)) return fail(error, errorSize, "VNC password is not NUL terminated");
    if (!name) return fail(error, errorSize, "Name is required");
    if (!ssid) return fail(error, errorSize, "SSID is required");
    if (!host) return fail(error, errorSize, "VNC host is required");
    if (wifi && (wifi < 8 || wifi > 64)) return fail(error, errorSize, "WiFi password must be 8-63 bytes or 64 hex digits");
    if (wifi == 64) {
        for (size_t i = 0; i < wifi; ++i) {
            if (!hexDigit(static_cast<unsigned char>(profile.wifiPassword[i]))) {
                return fail(error, errorSize, "64-byte WiFi password must be hexadecimal");
            }
        }
    }
    for (size_t i = 0; i < vnc; ++i) {
        const unsigned char c = static_cast<unsigned char>(profile.vncPassword[i]);
        if (c < 0x20 || c > 0x7e) return fail(error, errorSize, "VNC password must be printable ASCII");
    }
    if (!validHost(profile.vncHost, host)) return fail(error, errorSize, "VNC host must be an IP address or hostname without whitespace");
    if (!profile.vncPort) return fail(error, errorSize, "VNC port must be 1-65535");
    return true;
}
