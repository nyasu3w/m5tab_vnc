#include "ProfileStore.h"

#include <Preferences.h>
#include <string.h>

namespace {
constexpr char kNamespace[] = "tab5vnc";
constexpr char kInitializedKey[] = "init";
constexpr char kLastUsedKey[] = "last";
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 12;
constexpr size_t kFieldsSize = 7;
constexpr size_t kMaxBlobSize = kHeaderSize + kFieldsSize + 63 + 32 + 64 + 253 + 8;
constexpr uint8_t kMagic[] = {'T', '5', 'V', 'P'};

// Version 1, little-endian; no native struct layout is used:
// magic[4], version:u8, reserved:u8(0), payloadLength:u16, crc32:u32.
// Payload: name/ssid/wifi/host/vnc byte lengths (five u8), port:u16,
// followed by those five strings without terminators. CRC32/IEEE covers the
// first eight header bytes and the payload. CRC detects damage, NOT tampering;
// passwords are plaintext unless the deployment enables NVS encryption.
class EndPreferences {
public:
    explicit EndPreferences(Preferences& preferences) : preferences_(preferences) {}
    ~EndPreferences() { preferences_.end(); }
private:
    Preferences& preferences_;
    EndPreferences(const EndPreferences&) = delete;
    EndPreferences& operator=(const EndPreferences&) = delete;
};

void slotKey(uint8_t slot, char (&key)[3]) {
    key[0] = 'p';
    key[1] = static_cast<char>('0' + slot);
    key[2] = '\0';
}

void put16(uint8_t* dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value);
    dst[1] = static_cast<uint8_t>(value >> 8);
}
uint16_t get16(const uint8_t* src) {
    return static_cast<uint16_t>(src[0] | (static_cast<uint16_t>(src[1]) << 8));
}
void put32(uint8_t* dst, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) dst[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint32_t get32(const uint8_t* src) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(src[i]) << (8 * i);
    return value;
}
uint32_t crcBytes(uint32_t crc, const uint8_t* src, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        crc ^= src[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
        }
    }
    return crc;
}
uint32_t blobCrc(const uint8_t* bytes, size_t length) {
    uint32_t crc = crcBytes(0xffffffffUL, bytes, 8);
    return crcBytes(crc, bytes + kHeaderSize, length - kHeaderSize) ^ 0xffffffffUL;
}

size_t encode(const ConnectionProfile& profile, uint8_t* bytes) {
    const char* fields[] = {profile.name, profile.ssid, profile.wifiPassword,
                            profile.vncHost, profile.vncPassword};
    memcpy(bytes, kMagic, sizeof(kMagic));
    bytes[4] = kVersion;
    bytes[5] = 0;
    size_t offset = kHeaderSize + kFieldsSize;
    for (size_t i = 0; i < 5; ++i) {
        const size_t length = strlen(fields[i]); // Already validated and bounded.
        bytes[kHeaderSize + i] = static_cast<uint8_t>(length);
        memcpy(bytes + offset, fields[i], length);
        offset += length;
    }
    put16(bytes + kHeaderSize + 5, profile.vncPort);
    put16(bytes + 6, static_cast<uint16_t>(offset - kHeaderSize));
    put32(bytes + 8, blobCrc(bytes, offset));
    return offset;
}

bool decode(const uint8_t* bytes, size_t length, ConnectionProfile& result) {
    if (length < kHeaderSize + kFieldsSize || length > kMaxBlobSize ||
        memcmp(bytes, kMagic, sizeof(kMagic)) != 0 || bytes[4] != kVersion ||
        bytes[5] != 0 || get16(bytes + 6) != length - kHeaderSize ||
        get32(bytes + 8) != blobCrc(bytes, length)) return false;
    ConnectionProfile candidate;
    char* fields[] = {candidate.name, candidate.ssid, candidate.wifiPassword,
                      candidate.vncHost, candidate.vncPassword};
    const size_t capacities[] = {sizeof(candidate.name), sizeof(candidate.ssid),
        sizeof(candidate.wifiPassword), sizeof(candidate.vncHost), sizeof(candidate.vncPassword)};
    size_t offset = kHeaderSize + kFieldsSize;
    for (size_t i = 0; i < 5; ++i) {
        const size_t fieldLength = bytes[kHeaderSize + i];
        if (fieldLength >= capacities[i] || fieldLength > length - offset ||
            memchr(bytes + offset, '\0', fieldLength)) return false;
        memcpy(fields[i], bytes + offset, fieldLength);
        fields[i][fieldLength] = '\0';
        offset += fieldLength;
    }
    candidate.vncPort = get16(bytes + kHeaderSize + 5);
    if (offset != length || !validateConnectionProfile(candidate, nullptr, 0)) return false;
    result = candidate;
    return true;
}

// NVS blob index + data overhead + 32-byte entries, conservatively requiring
// room for a new copy even on overwrite. freeEntries() is only a preflight:
// fragmentation and concurrent users can still cause putBytes() to fail.
size_t blobEntries(size_t length) {
    return 2 + (length + 31) / 32;
}
} // namespace

void ProfileStore::setError(const char* message) {
    const size_t length = strlen(message);
    const size_t copy = length < sizeof(error_) - 1 ? length : sizeof(error_) - 1;
    memcpy(error_, message, copy);
    error_[copy] = '\0';
}

bool ProfileStore::load() {
    setError("");
    Preferences preferences;
    EndPreferences end(preferences);
    readOnly_ = false;
    if (!preferences.begin(kNamespace, false)) {
        if (!preferences.begin(kNamespace, true)) {
            available_ = false;
            setError("Unable to open profile storage");
            return false; // Preserve the previous RAM snapshot on open failure.
        }
        readOnly_ = true;
    }
    available_ = true;
    initialized_ = false;
    markerPersisted_ = false;
    lastUsed_ = -1;
    corruptCount_ = 0;
    for (uint8_t slot = 0; slot < MAX_CONNECTION_PROFILES; ++slot) {
        profiles_[slot] = ConnectionProfile{};
        used_[slot] = false;
        corrupt_[slot] = false;
    }
    bool badMetadata = false;
    if (preferences.isKey(kInitializedKey)) {
        // Any existing marker, including an unknown/corrupt value, suppresses
        // legacy seeding. Never mistake corrupted storage for a fresh install.
        initialized_ = true;
        markerPersisted_ = preferences.getType(kInitializedKey) == PT_U8 &&
                           preferences.getUChar(kInitializedKey, 0) == 1;
        badMetadata = !markerPersisted_;
    }
    uint8_t bytes[kMaxBlobSize];
    for (uint8_t slot = 0; slot < MAX_CONNECTION_PROFILES; ++slot) {
        char key[3];
        slotKey(slot, key);
        if (!preferences.isKey(key)) continue;
        initialized_ = true; // Also recover safely from a missing init marker.
        const size_t length = preferences.getType(key) == PT_BLOB
                            ? preferences.getBytesLength(key) : 0;
        if (length < kHeaderSize + kFieldsSize || length > sizeof(bytes) ||
            preferences.getBytes(key, bytes, sizeof(bytes)) != length ||
            !decode(bytes, length, profiles_[slot])) {
            corrupt_[slot] = true;
            ++corruptCount_;
            continue; // Leave the NVS key untouched; only quarantine it in RAM.
        }
        used_[slot] = true;
    }
    if (preferences.isKey(kLastUsedKey)) {
        initialized_ = true;
        if (preferences.getType(kLastUsedKey) == PT_I8) {
            const int value = preferences.getChar(kLastUsedKey, -2);
            if (value == -1 || (value >= 0 && value < MAX_CONNECTION_PROFILES && used_[value])) {
                lastUsed_ = value;
            } else {
                badMetadata = true; // Stale or damaged last-used is never used.
            }
        } else {
            badMetadata = true;
        }
    }
    if (corruptCount_) setError("Ignored corrupt or unsupported profile slots");
    else if (badMetadata) setError("Ignored invalid profile metadata");
    else if (readOnly_) setError("Profile storage is read-only");
    return true;
}

bool ProfileStore::isUsed(uint8_t slot) const {
    return slot < MAX_CONNECTION_PROFILES && used_[slot];
}
bool ProfileStore::isCorrupt(uint8_t slot) const {
    return slot < MAX_CONNECTION_PROFILES && corrupt_[slot];
}
const ConnectionProfile* ProfileStore::get(uint8_t slot) const {
    return isUsed(slot) ? &profiles_[slot] : nullptr;
}

bool ProfileStore::writable() {
    if (!available_) {
        setError("Profile storage is not loaded or unavailable");
        return false;
    }
    if (readOnly_) {
        setError("Profile storage is read-only");
        return false;
    }
    return true;
}

bool ProfileStore::ensureInitialized(Preferences& preferences) {
    if (markerPersisted_) return true;
    if (preferences.freeEntries() < 1) {
        setError("Insufficient NVS capacity for initialization marker");
        return false;
    }
    if (preferences.putUChar(kInitializedKey, 1) != 1) {
        setError("Unable to save initialization marker");
        return false;
    }
    markerPersisted_ = true;
    initialized_ = true;
    return true;
}

void ProfileStore::repairSlot(uint8_t slot) {
    if (corrupt_[slot]) {
        corrupt_[slot] = false;
        --corruptCount_;
    }
}

bool ProfileStore::save(uint8_t slot, const ConnectionProfile& profile) {
    setError("");
    if (slot >= MAX_CONNECTION_PROFILES) {
        setError("Profile slot is out of range");
        return false;
    }
    if (!validateConnectionProfile(profile, error_, sizeof(error_)) || !writable()) return false;
    uint8_t bytes[kMaxBlobSize];
    const size_t length = encode(profile, bytes);
    Preferences preferences;
    EndPreferences end(preferences);
    if (!preferences.begin(kNamespace, false)) {
        setError("Unable to open profile storage for writing");
        return false;
    }
    const size_t needed = blobEntries(length) + (markerPersisted_ ? 0 : 1);
    if (preferences.freeEntries() < needed) {
        setError("Insufficient NVS capacity for profile");
        return false;
    }
    // Marker first: a power failure before/after the blob must not trigger
    // unexpected legacy seeding. This is NOT a multi-key transaction; a failed
    // blob write may leave a successful initialization marker in NVS and RAM.
    if (!ensureInitialized(preferences)) return false;
    char key[3];
    slotKey(slot, key);
    if (preferences.putBytes(key, bytes, length) != length) {
        setError("Unable to save profile blob");
        return false; // Existing RAM profile is unchanged.
    }
    // Decode our canonical wire image: trailing bytes after caller terminators
    // are not retained in RAM or persisted (and struct padding is irrelevant).
    ConnectionProfile canonical;
    decode(bytes, length, canonical);
    profiles_[slot] = canonical;
    used_[slot] = true;
    repairSlot(slot);
    return true;
}

bool ProfileStore::remove(uint8_t slot) {
    setError("");
    if (slot >= MAX_CONNECTION_PROFILES) {
        setError("Profile slot is out of range");
        return false;
    }
    if (!writable()) return false;
    if (!used_[slot] && !corrupt_[slot]) return true; // Idempotent empty remove.
    Preferences preferences;
    EndPreferences end(preferences);
    if (!preferences.begin(kNamespace, false)) {
        setError("Unable to open profile storage for writing");
        return false;
    }
    // Ensure the marker BEFORE deleting, including when recovering older data.
    // Removing the final profile must not re-enable legacy migration.
    if (!ensureInitialized(preferences)) return false;
    if (lastUsed_ == slot) {
        if (preferences.freeEntries() < 1 || preferences.putChar(kLastUsedKey, -1) != 1) {
            setError("Unable to clear last-used profile");
            return false;
        }
        // Separate key, separate commit: on a subsequent remove failure the
        // durable metadata can already be -1. Preserve the entire previous RAM
        // profile/last-used snapshot on failure; a reload may observe that -1.
    }
    char key[3];
    slotKey(slot, key);
    if (!preferences.remove(key)) {
        setError("Unable to remove profile blob");
        return false;
    }
    profiles_[slot] = ConnectionProfile{};
    if (lastUsed_ == slot) lastUsed_ = -1;
    used_[slot] = false;
    repairSlot(slot);
    return true;
}

int ProfileStore::findFreeSlot() const {
    for (uint8_t slot = 0; slot < MAX_CONNECTION_PROFILES; ++slot) {
        // Quarantined/unsupported blobs are not silently reused. A caller can
        // explicitly save/remove that slot to replace/delete it if desired.
        if (!used_[slot] && !corrupt_[slot]) return slot;
    }
    return -1;
}
uint8_t ProfileStore::count() const {
    uint8_t total = 0;
    for (uint8_t slot = 0; slot < MAX_CONNECTION_PROFILES; ++slot) {
        if (used_[slot]) ++total;
    }
    return total;
}
int ProfileStore::lastUsed() const { return lastUsed_; }

bool ProfileStore::markLastUsed(uint8_t slot) {
    setError("");
    if (!isUsed(slot)) {
        setError("Last-used profile slot is empty or out of range");
        return false;
    }
    if (!writable()) return false;
    if (lastUsed_ == slot) return true; // Do not wear flash for unchanged values.
    Preferences preferences;
    EndPreferences end(preferences);
    if (!preferences.begin(kNamespace, false)) {
        setError("Unable to open profile storage for writing");
        return false;
    }
    if (preferences.freeEntries() < 1) {
        setError("Insufficient NVS capacity for last-used profile");
        return false;
    }
    if (preferences.putChar(kLastUsedKey, static_cast<int8_t>(slot)) != 1) {
        setError("Unable to save last-used profile");
        return false;
    }
    lastUsed_ = slot;
    return true;
}

bool ProfileStore::initialized() const { return initialized_; }
bool ProfileStore::markInitialized() {
    setError("");
    if (!writable()) return false;
    if (markerPersisted_) return true;
    Preferences preferences;
    EndPreferences end(preferences);
    if (!preferences.begin(kNamespace, false)) {
        setError("Unable to open profile storage for writing");
        return false;
    }
    return ensureInitialized(preferences);
}
bool ProfileStore::available() const { return available_; }
const char* ProfileStore::lastError() const { return error_; }
uint8_t ProfileStore::corruptCount() const { return corruptCount_; }
