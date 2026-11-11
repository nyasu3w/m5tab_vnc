#include "ConnectionProfile.h"
#include "ProfileStore.h"
#include "Preferences.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
unsigned assertions = 0;
#define CHECK(condition) do { ++assertions; if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + " check failed: " #condition); } while (false)

template <size_t N>
void text(char (&dst)[N], const std::string& value) {
    if (value.size() >= N) throw std::runtime_error("Test fixture overflow");
    std::memset(dst, 0, N);
    std::memcpy(dst, value.data(), value.size());
}
ConnectionProfile valid(const std::string& name = "Office") {
    ConnectionProfile profile;
    text(profile.name, name);
    text(profile.ssid, "test-network");
    text(profile.wifiPassword, "test-only-password");
    text(profile.vncHost, "desktop.example.local");
    text(profile.vncPassword, "testpass");
    return profile;
}
bool passes(const ConnectionProfile& profile) {
    char error[128];
    const bool result = validateConnectionProfile(profile, error, sizeof(error));
    CHECK(result ? error[0] == '\0' : error[0] != '\0');
    return result;
}
void closed() {
    CHECK(FakeNVS::state().activeHandles == 0);
    CHECK(FakeNVS::state().opens == FakeNVS::state().closes);
}
uint32_t crcPart(uint32_t crc, const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
    }
    return crc;
}
void fixCrc(std::vector<uint8_t>& blob) {
    uint32_t crc = crcPart(0xffffffffUL, blob.data(), 8);
    crc = crcPart(crc, blob.data() + 12, blob.size() - 12) ^ 0xffffffffUL;
    for (unsigned i = 0; i < 4; ++i) blob[8 + i] = static_cast<uint8_t>(crc >> (8 * i));
}
std::vector<uint8_t> wire(const ConnectionProfile& profile = valid()) {
    FakeNVS::reset();
    ProfileStore store;
    CHECK(store.load());
    CHECK(store.save(0, profile));
    closed();
    return FakeNVS::value("p0").bytes;
}

void validationLengths() {
    CHECK(MAX_CONNECTION_PROFILES == 8);
    ConnectionProfile p = valid();
    CHECK(p.vncPort == 5900);
    CHECK(passes(p));
    text(p.name, ""); CHECK(!passes(p));
    p = valid(); text(p.ssid, ""); CHECK(!passes(p));
    p = valid(); text(p.vncHost, ""); CHECK(!passes(p));
    p = valid(); text(p.name, std::string(63, 'n')); CHECK(passes(p));
    p = valid(); text(p.ssid, std::string(32, 's')); CHECK(passes(p));
    p = valid(); text(p.ssid, std::string(10, 'a') + "日本語日本語" + "1234");
    CHECK(std::strlen(p.ssid) == 32); CHECK(passes(p));
    p = valid(); std::memset(p.name, 'n', sizeof(p.name)); CHECK(!passes(p));
    p = valid(); std::memset(p.ssid, 's', sizeof(p.ssid)); CHECK(!passes(p));
    p = valid(); std::memset(p.wifiPassword, 'w', sizeof(p.wifiPassword)); CHECK(!passes(p));
    p = valid(); std::memset(p.vncHost, 'h', sizeof(p.vncHost)); CHECK(!passes(p));
    p = valid(); std::memset(p.vncPassword, 'v', sizeof(p.vncPassword)); CHECK(!passes(p));
    p = valid(); p.vncPort = 0; CHECK(!passes(p));
    p.vncPort = 1; CHECK(passes(p));
    p.vncPort = 65535; CHECK(passes(p));
    p = valid(); p.name[0] = '\0';
    char tiny[] = {'?', '!'};
    CHECK(!validateConnectionProfile(p, tiny, 1)); CHECK(tiny[0] == '\0'); CHECK(tiny[1] == '!');
    tiny[0] = '?'; CHECK(!validateConnectionProfile(p, tiny, 0)); CHECK(tiny[0] == '?');
    CHECK(!validateConnectionProfile(p, nullptr, 99));
    CHECK(validateConnectionProfile(valid(), nullptr, 0));
    char error[128];
    p = valid(); text(p.wifiPassword, "secret");
    CHECK(!validateConnectionProfile(p, error, sizeof(error)));
    CHECK(std::strstr(error, p.wifiPassword) == nullptr);
}

void validationPasswords() {
    ConnectionProfile p = valid();
    text(p.wifiPassword, ""); CHECK(passes(p));
    text(p.wifiPassword, std::string(7, 'x')); CHECK(!passes(p));
    text(p.wifiPassword, std::string(8, 'x')); CHECK(passes(p));
    text(p.wifiPassword, std::string(63, 'x')); CHECK(passes(p));
    text(p.wifiPassword, std::string(64, 'a')); CHECK(passes(p));
    text(p.wifiPassword, std::string(64, 'F')); CHECK(passes(p));
    text(p.wifiPassword, std::string(64, '0')); CHECK(passes(p));
    p.wifiPassword[42] = 'g'; CHECK(!passes(p));
    text(p.wifiPassword, std::string(63, 'x')); p.wifiPassword[0] = static_cast<char>(0xc3); CHECK(passes(p));
    text(p.vncPassword, ""); CHECK(passes(p));
    text(p.vncPassword, " !~abc12"); CHECK(passes(p));
    p.vncPassword[0] = '\x1f'; CHECK(!passes(p));
    p.vncPassword[0] = '\x7f'; CHECK(!passes(p));
    p.vncPassword[0] = static_cast<char>(0x80); CHECK(!passes(p));
    text(p.vncPassword, "12345678"); CHECK(passes(p));
}

void validationHosts() {
    ConnectionProfile p = valid();
    const char* accepted[] = {"localhost", "tab5.local", "NODE-2.example", "example.com.",
        "0.0.0.0", "255.255.255.255", "192.168.1.2", "::", "::1", "2001:db8::1",
        "2001:db8:0:0:0:0:0:1", "::ffff:192.168.1.2", "1:2:3:4:5:6:192.168.1.2", "123"};
    for (const auto* host : accepted) { text(p.vncHost, host); CHECK(passes(p)); }
    const char* rejected[] = {"host name", " host", "host\t", "host\n", "https://host", "host:5900",
        "[::1]", "-host", "host-", ".host", "host..local", ".", "host_1", "日本.local",
        "256.1.1.1", "1.2.3", "1.2.3.4.5", "1.2.3.999", "1.2.3.-1", ":", ":1", "1:",
        "1:::2", "1::2::3", "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7:8::",
        "::gg", "::12345", "::ffff:300.1.1.1", "fe80::1%eth0"};
    for (const auto* host : rejected) { text(p.vncHost, host); CHECK(!passes(p)); }
    const std::string label(63, 'a');
    text(p.vncHost, label + "." + label + "." + label + "." + std::string(61, 'z'));
    CHECK(std::strlen(p.vncHost) == 253); CHECK(passes(p));
    text(p.vncHost, std::string(64, 'a')); CHECK(!passes(p));
}

void crudAndReload() {
    FakeNVS::reset();
    ProfileStore store;
    CHECK(!store.available()); CHECK(!store.initialized()); CHECK(store.count() == 0);
    CHECK(store.lastUsed() == -1); CHECK(store.findFreeSlot() == 0); CHECK(!store.save(0, valid()));
    CHECK(store.load()); CHECK(store.available()); CHECK(!store.initialized()); CHECK(store.lastError()[0] == '\0');
    for (uint8_t slot = 0; slot < 8; ++slot) {
        CHECK(store.findFreeSlot() == slot);
        auto p = valid("Profile " + std::to_string(slot)); p.vncPort = static_cast<uint16_t>(5900 + slot);
        CHECK(store.save(slot, p)); CHECK(store.isUsed(slot)); CHECK(store.count() == slot + 1);
        CHECK(std::strcmp(store.get(slot)->name, p.name) == 0);
        closed();
    }
    CHECK(store.initialized()); CHECK(store.findFreeSlot() == -1); CHECK(store.lastUsed() == -1);
    const auto& ns = FakeNVS::state().namespaces.at("tab5vnc");
    CHECK(ns.size() == 9); CHECK(ns.at("init").type == PT_U8);
    CHECK(ns.at("p7").type == PT_BLOB); CHECK(!ns.count("last"));
    ProfileStore reloaded; CHECK(reloaded.load()); CHECK(reloaded.count() == 8); CHECK(reloaded.initialized());
    for (uint8_t slot = 0; slot < 8; ++slot) {
        CHECK(reloaded.get(slot)->vncPort == 5900 + slot);
        CHECK(std::strcmp(reloaded.get(slot)->wifiPassword, "test-only-password") == 0);
    }
    auto edited = valid("Edited"); text(edited.wifiPassword, "");
    CHECK(reloaded.save(3, edited)); CHECK(reloaded.count() == 8);
    CHECK(reloaded.remove(3)); CHECK(!reloaded.isUsed(3)); CHECK(!reloaded.get(3));
    CHECK(reloaded.findFreeSlot() == 3); CHECK(reloaded.count() == 7); CHECK(reloaded.remove(3));
    CHECK(reloaded.save(3, edited)); CHECK(reloaded.get(3)->wifiPassword[0] == '\0');
    CHECK(store.load()); CHECK(std::strcmp(store.get(3)->name, "Edited") == 0);
    CHECK(store.corruptCount() == 0); closed();
}

void bounds() {
    FakeNVS::reset(); ProfileStore store; CHECK(store.load());
    for (uint8_t slot : {uint8_t(8), uint8_t(255)}) {
        CHECK(!store.save(slot, valid())); CHECK(!store.remove(slot)); CHECK(!store.markLastUsed(slot));
        CHECK(!store.isUsed(slot)); CHECK(store.get(slot) == nullptr);
        CHECK(!store.isCorrupt(slot));
    }
    CHECK(!store.markLastUsed(0)); CHECK(store.count() == 0); CHECK(!store.initialized());
    CHECK(FakeNVS::state().writes.empty()); closed();
}

void lastUsed() {
    FakeNVS::reset(); ProfileStore store; CHECK(store.load());
    CHECK(store.save(0, valid())); CHECK(store.save(7, valid("Remote")));
    CHECK(store.markLastUsed(7)); CHECK(store.lastUsed() == 7);
    const unsigned writes = FakeNVS::state().writes["last"];
    CHECK(store.markLastUsed(7)); CHECK(FakeNVS::state().writes["last"] == writes);
    FakeNVS::state().capacity = FakeNVS::usedEntries();
    CHECK(store.markLastUsed(7)); // No write/capacity needed when unchanged.
    CHECK(!store.markLastUsed(0)); CHECK(store.lastUsed() == 7);
    FakeNVS::state().capacity = 1024;
    ProfileStore reload; CHECK(reload.load()); CHECK(reload.lastUsed() == 7);
    CHECK(reload.remove(0)); CHECK(reload.lastUsed() == 7);
    CHECK(reload.remove(7)); CHECK(reload.lastUsed() == -1); CHECK(reload.count() == 0);
    CHECK(reload.initialized()); CHECK(store.load()); CHECK(store.lastUsed() == -1); CHECK(store.initialized());
    closed();
}

void migrationMarker() {
    FakeNVS::reset(); ProfileStore store; CHECK(store.load()); CHECK(!store.initialized());
    CHECK(store.markInitialized()); CHECK(store.initialized());
    const unsigned writes = FakeNVS::state().writes["init"];
    CHECK(store.markInitialized()); CHECK(FakeNVS::state().writes["init"] == writes);
    CHECK(store.load()); CHECK(store.initialized()); CHECK(store.count() == 0);
    CHECK(store.save(0, valid())); CHECK(store.remove(0)); CHECK(store.load()); CHECK(store.initialized());
    CHECK(store.count() == 0);
    CHECK(store.save(1, valid())); FakeNVS::state().namespaces.at("tab5vnc").erase("init");
    CHECK(store.load()); CHECK(store.initialized()); CHECK(store.get(1));
    CHECK(store.remove(1)); CHECK(FakeNVS::value("init").bytes[0] == 1);
    CHECK(store.load()); CHECK(store.initialized()); CHECK(store.count() == 0);
    FakeNVS::reset(); FakeNVS::scalar("init", PT_U8, 0);
    CHECK(store.load()); CHECK(store.initialized()); CHECK(store.lastError()[0] != '\0');
    CHECK(store.markInitialized()); CHECK(FakeNVS::value("init").bytes[0] == 1);
    FakeNVS::reset(); FakeNVS::scalar("init", PT_I8, 1);
    CHECK(store.load()); CHECK(store.initialized()); CHECK(store.lastError()[0] != '\0');
    closed();
}

void corruptedPayloads() {
    const auto original = wire();
    std::vector<std::vector<uint8_t> > damaged;
    auto b = original; b.back() ^= 1; damaged.push_back(b); // Payload CRC.
    b = original; b[8] ^= 1; damaged.push_back(b); // CRC field.
    b = original; b[0] ^= 1; fixCrc(b); damaged.push_back(b); // Magic.
    b = original; b[4] = 2; fixCrc(b); damaged.push_back(b); // Unsupported version.
    b = original; b[5] = 1; fixCrc(b); damaged.push_back(b); // Reserved.
    b = original; ++b[6]; fixCrc(b); damaged.push_back(b); // Length.
    b = original; b[12] = 64; fixCrc(b); damaged.push_back(b); // Overlong name.
    b = original; b[17] = 0; b[18] = 0; fixCrc(b); damaged.push_back(b); // Invalid port.
    b = original; b[19] = 0; fixCrc(b); damaged.push_back(b); // Embedded NUL.
    b = original; b[12] = 0; fixCrc(b); damaged.push_back(b); // Missing name/extra bytes.
    b = original; b.push_back(42); b[6] = static_cast<uint8_t>(b.size() - 12); fixCrc(b); damaged.push_back(b);
    b = original; b.resize(b.size() - 1); damaged.push_back(b); // Truncation.
    damaged.push_back({}); damaged.push_back({1, 2}); damaged.push_back(std::vector<uint8_t>(440, 0));
    for (const auto& bad : damaged) {
        FakeNVS::reset(); FakeNVS::blob("p0", original); FakeNVS::blob("p1", bad);
        ProfileStore store; CHECK(store.load()); CHECK(store.available()); CHECK(store.initialized());
        CHECK(store.count() == 1); CHECK(store.corruptCount() == 1); CHECK(!store.isUsed(1)); CHECK(!store.get(1));
        CHECK(store.findFreeSlot() == 2); CHECK(store.lastError()[0] != '\0');
        CHECK(store.isCorrupt(1)); CHECK(!store.isCorrupt(0)); CHECK(!store.isCorrupt(2));
        CHECK(FakeNVS::value("p1").bytes == bad); CHECK(FakeNVS::state().writes.empty());
        CHECK(store.remove(1)); CHECK(store.corruptCount() == 0); CHECK(store.findFreeSlot() == 1);
        CHECK(!store.isCorrupt(1));
        closed();
    }
    FakeNVS::reset(); FakeNVS::scalar("p0", PT_U8, 1); ProfileStore store;
    CHECK(store.load()); CHECK(store.count() == 0); CHECK(store.corruptCount() == 1); CHECK(store.initialized());
    CHECK(store.save(0, valid())); CHECK(store.corruptCount() == 0); CHECK(store.get(0));
    FakeNVS::state().shortRead = true;
    CHECK(store.load()); CHECK(store.corruptCount() == 1); CHECK(store.count() == 0);
    FakeNVS::state().shortRead = false;
    CHECK(store.load()); CHECK(store.count() == 1); CHECK(store.corruptCount() == 0);
    FakeNVS::reset();
    for (uint8_t i = 0; i < 8; ++i) FakeNVS::blob("p" + std::to_string(i), {0});
    CHECK(store.load()); CHECK(store.corruptCount() == 8); CHECK(store.findFreeSlot() == -1); CHECK(store.count() == 0);
    for (uint8_t i = 0; i < 8; ++i) CHECK(store.isCorrupt(i));
    CHECK(store.remove(4)); CHECK(!store.isCorrupt(4)); CHECK(store.corruptCount() == 7);
    CHECK(store.findFreeSlot() == 4); CHECK(store.save(4, valid("Recovered"))); CHECK(store.count() == 1);
    CHECK(store.load()); CHECK(store.isUsed(4)); CHECK(!store.isCorrupt(4)); CHECK(store.corruptCount() == 7);
    closed();
}

void metadataCorruption() {
    const auto original = wire();
    for (uint8_t value : {uint8_t(8), uint8_t(254), uint8_t(1)}) {
        FakeNVS::reset(); FakeNVS::blob("p0", original); FakeNVS::scalar("last", PT_I8, value);
        ProfileStore store; CHECK(store.load()); CHECK(store.lastUsed() == -1); CHECK(store.corruptCount() == 0);
        CHECK(store.lastError()[0] != '\0'); CHECK(store.count() == 1); CHECK(store.initialized());
        CHECK(FakeNVS::state().writes.empty()); closed();
    }
    FakeNVS::reset(); FakeNVS::scalar("last", PT_U8, 0);
    ProfileStore store; CHECK(store.load()); CHECK(store.lastUsed() == -1); CHECK(store.initialized());
    CHECK(store.lastError()[0] != '\0'); closed();
}

void writeFailures() {
    FakeNVS::reset(); ProfileStore store; CHECK(store.load());
    FakeNVS::state().failPutKey = "init";
    CHECK(!store.save(0, valid())); CHECK(!store.initialized()); CHECK(!store.isUsed(0));
    CHECK(!store.markInitialized()); CHECK(FakeNVS::state().writes["p0"] == 0);
    FakeNVS::state().failPutKey = "p0";
    CHECK(!store.save(0, valid())); CHECK(store.initialized()); CHECK(!store.isUsed(0)); // Marker committed first.
    CHECK(FakeNVS::value("init").bytes[0] == 1); closed();
    FakeNVS::state().failPutKey.clear(); CHECK(store.save(0, valid("Old"))); CHECK(store.markLastUsed(0));
    const auto oldBlob = FakeNVS::value("p0").bytes;
    const ConnectionProfile* oldPtr = store.get(0);
    FakeNVS::state().failPutKey = "p0";
    CHECK(!store.save(0, valid("New"))); CHECK(store.get(0) == oldPtr);
    CHECK(std::strcmp(store.get(0)->name, "Old") == 0); CHECK(FakeNVS::value("p0").bytes == oldBlob);
    CHECK(store.lastUsed() == 0); CHECK(store.count() == 1); CHECK(store.lastError()[0] != '\0');
    FakeNVS::state().failPutKey.clear(); FakeNVS::state().shortPut = true;
    CHECK(!store.save(0, valid("New"))); CHECK(std::strcmp(store.get(0)->name, "Old") == 0);
    FakeNVS::state().shortPut = false;
    auto invalid = valid(); invalid.vncPort = 0;
    CHECK(!store.save(0, invalid)); CHECK(FakeNVS::value("p0").bytes == oldBlob);
    CHECK(store.save(1, valid("Second")));
    FakeNVS::state().failPutKey = "last";
    CHECK(!store.markLastUsed(1)); CHECK(store.lastUsed() == 0);
    CHECK(!store.remove(0)); CHECK(store.get(0)); CHECK(store.lastUsed() == 0);
    FakeNVS::state().failPutKey.clear(); FakeNVS::state().failRemoveKey = "p0";
    CHECK(!store.remove(0)); CHECK(store.get(0)); CHECK(store.count() == 2); CHECK(store.lastUsed() == 0);
    CHECK(FakeNVS::value("last").bytes[0] == 255); // Metadata write is separate, not a transaction.
    ProfileStore reload; CHECK(reload.load()); CHECK(reload.get(0)); CHECK(reload.lastUsed() == -1);
    FakeNVS::state().failRemoveKey.clear(); CHECK(store.remove(0)); CHECK(store.lastUsed() == -1);
    closed();
}

void unavailableAndReadOnly() {
    const auto original = wire();
    FakeNVS::reset(); FakeNVS::blob("p0", original);
    FakeNVS::state().failWriteOpen = true;
    ProfileStore store; CHECK(store.load()); CHECK(store.available()); CHECK(store.count() == 1);
    CHECK(store.lastError()[0] != '\0'); CHECK(store.initialized());
    CHECK(!store.save(0, valid("New"))); CHECK(!store.remove(0)); CHECK(!store.markLastUsed(0)); CHECK(!store.markInitialized());
    CHECK(FakeNVS::value("p0").bytes == original); CHECK(FakeNVS::state().writes.empty());
    FakeNVS::state().failReadOpen = true;
    CHECK(!store.load()); CHECK(!store.available()); CHECK(store.get(0)); CHECK(!store.save(0, valid()));
    FakeNVS::state().failReadOpen = false; FakeNVS::state().failWriteOpen = false;
    CHECK(store.load()); CHECK(store.save(0, valid("New")));
    FakeNVS::state().failWriteOpen = true; // Failure can occur after successful load.
    CHECK(!store.save(0, valid("Later"))); CHECK(!store.remove(0)); CHECK(!store.markLastUsed(0));
    CHECK(std::strcmp(store.get(0)->name, "New") == 0);
    closed();
}

void capacityAndCanonicalFormat() {
    auto p = valid();
    text(p.name, std::string(63, 'n')); text(p.ssid, std::string(32, 's'));
    text(p.wifiPassword, std::string(64, 'a')); text(p.vncPassword, "12345678");
    const std::string label(63, 'h');
    text(p.vncHost, label + "." + label + "." + label + "." + std::string(61, 'z'));
    p.vncPort = 65535;
    FakeNVS::reset();
    // 0x5000 NVS partition has five 4KB pages; approximate free-entry budget
    // reserves one full page for GC. Real fragmentation is NOT simulated.
    FakeNVS::state().capacity = 126 * 4;
    ProfileStore store; CHECK(store.load());
    for (uint8_t i = 0; i < 8; ++i) CHECK(store.save(i, p));
    CHECK(FakeNVS::value("p0").bytes.size() == 439);
    for (unsigned round = 0; round < 30; ++round) {
        for (uint8_t i = 0; i < 8; ++i) { p.vncPort = static_cast<uint16_t>(5900 + round); CHECK(store.save(i, p)); }
    }
    CHECK(store.load()); CHECK(store.count() == 8); CHECK(store.get(7)->vncPort == 5929);
    FakeNVS::state().capacity = FakeNVS::usedEntries() + 15; // Largest blob needs 16 fresh entries.
    CHECK(!store.save(0, p)); CHECK(store.count() == 8); CHECK(store.lastError()[0] != '\0');
    FakeNVS::state().capacity = FakeNVS::usedEntries() + 16;
    CHECK(store.save(0, p));
    closed();
    FakeNVS::reset(); ProfileStore empty; CHECK(empty.load());
    FakeNVS::state().capacity = FakeNVS::usedEntries();
    CHECK(!empty.save(0, valid())); CHECK(!empty.markInitialized()); CHECK(!empty.initialized());
    CHECK(FakeNVS::state().writes.empty());
    const auto clean = wire(valid());
    auto dirtyProfile = valid();
    dirtyProfile.name[sizeof(dirtyProfile.name) - 1] = 'x';
    dirtyProfile.ssid[sizeof(dirtyProfile.ssid) - 1] = 'x';
    dirtyProfile.wifiPassword[sizeof(dirtyProfile.wifiPassword) - 1] = 'x';
    dirtyProfile.vncHost[sizeof(dirtyProfile.vncHost) - 1] = 'x';
    CHECK(wire(dirtyProfile) == clean); // Garbage following terminators is never serialized.
    CHECK(clean[0] == 'T' && clean[1] == '5' && clean[2] == 'V' && clean[3] == 'P');
    CHECK(clean[4] == 1 && clean[5] == 0); CHECK(clean[17] == 0x0c && clean[18] == 0x17); // 5900 LE.
    CHECK(clean.size() != sizeof(ConnectionProfile));
    closed();
}
} // namespace

int main() {
    const struct { const char* name; void (*run)(); } cases[] = {
        {"validation lengths and error buffers", validationLengths},
        {"WiFi/VNC password edges", validationPasswords},
        {"IPv4/IPv6/hostname validation", validationHosts},
        {"8 slots, CRUD and reload", crudAndReload},
        {"bounds", bounds}, {"last-used persistence and write suppression", lastUsed},
        {"migration marker and final deletion", migrationMarker},
        {"corruption, unsupported versions and quarantine", corruptedPayloads},
        {"metadata corruption", metadataCorruption}, {"write failures preserve old profiles", writeFailures},
        {"unavailable/read-only storage", unavailableAndReadOnly},
        {"capacity and canonical serialization", capacityAndCanonicalFormat}
    };
    try {
        for (const auto& test : cases) {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        }
        std::cout << "All " << (sizeof(cases) / sizeof(cases[0])) << " test groups passed ("
                  << assertions << " assertions).\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
