#pragma once

#include "ConnectionProfile.h"

class Preferences;

// Single-owner, non-thread-safe store. Serialize ALL calls in the application;
// in particular do not use it concurrently from UI and VNC tasks. Preferences
// handles are operation-scoped and always ended (also on failure).
// load() returns true if NVS can be read, including read-only/corrupt data.
// Check lastError()/corruptCount() for warnings; available() is not writability.
class ProfileStore {
public:
    bool load();
    bool isUsed(uint8_t slot) const;
    bool isCorrupt(uint8_t slot) const;
    const ConnectionProfile* get(uint8_t slot) const;
    bool save(uint8_t slot, const ConnectionProfile& profile);
    bool remove(uint8_t slot);
    int findFreeSlot() const;
    uint8_t count() const;
    int lastUsed() const;
    bool markLastUsed(uint8_t slot);
    bool initialized() const;
    bool markInitialized();
    bool available() const;
    const char* lastError() const;
    uint8_t corruptCount() const;

private:
    ConnectionProfile profiles_[MAX_CONNECTION_PROFILES] = {};
    bool used_[MAX_CONNECTION_PROFILES] = {};
    bool corrupt_[MAX_CONNECTION_PROFILES] = {};
    bool available_ = false;
    bool readOnly_ = false;
    bool initialized_ = false;
    bool markerPersisted_ = false;
    int lastUsed_ = -1;
    uint8_t corruptCount_ = 0;
    char error_[128] = {};

    void setError(const char* message);
    bool writable();
    bool ensureInitialized(Preferences& preferences);
    void repairSlot(uint8_t slot);
};
