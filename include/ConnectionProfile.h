#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr uint8_t MAX_CONNECTION_PROFILES = 8;

struct ConnectionProfile {
    char name[64] = {};
    char ssid[33] = {};
    char wifiPassword[65] = {};
    char vncHost[254] = {};
    uint16_t vncPort = 5900;
    char vncPassword[9] = {};
};

// Length limits are bytes, not characters. Error messages never contain values.
// A null error buffer or errorSize == 0 is permitted.
bool validateConnectionProfile(const ConnectionProfile& profile,
                               char* error, size_t errorSize);
