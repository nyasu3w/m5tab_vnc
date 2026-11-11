#include "Preferences.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {
size_t entries(const FakeNVS::Value& value) {
    return value.type == PT_BLOB ? 2 + (value.bytes.size() + 31) / 32 : 1;
}
} // namespace

namespace FakeNVS {
State& state() {
    static State instance;
    return instance;
}
void reset() {
    if (state().activeHandles) throw std::runtime_error("Leaked Preferences handle");
    state() = State{};
}
void blob(const std::string& key, const std::vector<uint8_t>& bytes) {
    state().namespaces["tab5vnc"][key] = Value{PT_BLOB, bytes};
}
void scalar(const std::string& key, PreferenceType type, uint8_t value) {
    state().namespaces["tab5vnc"][key] = Value{type, {value}};
}
Value& value(const std::string& key) {
    return state().namespaces.at("tab5vnc").at(key);
}
size_t usedEntries() {
    size_t total = 0;
    for (const auto& ns : state().namespaces) {
        ++total; // Namespace entry.
        for (const auto& pair : ns.second) total += entries(pair.second);
    }
    return total;
}
} // namespace FakeNVS

Preferences::~Preferences() { end(); }
bool Preferences::begin(const char* name, bool readOnly, const char*) {
    auto& state = FakeNVS::state();
    if (started_ || !name || !*name || std::strlen(name) > 15 ||
        (readOnly ? state.failReadOpen : state.failWriteOpen)) return false;
    if (readOnly && !state.namespaces.count(name)) return false;
    state.namespaces[name];
    namespace_ = name;
    readOnly_ = readOnly;
    started_ = true;
    ++state.activeHandles;
    ++state.opens;
    return true;
}
void Preferences::end() {
    if (!started_) return;
    started_ = false;
    --FakeNVS::state().activeHandles;
    ++FakeNVS::state().closes;
}
const FakeNVS::Value* Preferences::find(const char* key) const {
    if (!started_ || !key || std::strlen(key) > 15) return nullptr;
    const auto& ns = FakeNVS::state().namespaces.at(namespace_);
    const auto found = ns.find(key);
    return found == ns.end() ? nullptr : &found->second;
}
PreferenceType Preferences::getType(const char* key) {
    const auto* value = find(key);
    return value ? value->type : PT_INVALID;
}
bool Preferences::isKey(const char* key) { return find(key) != nullptr; }
size_t Preferences::freeEntries() {
    if (!started_) return 0;
    const size_t used = FakeNVS::usedEntries();
    return used < FakeNVS::state().capacity ? FakeNVS::state().capacity - used : 0;
}
size_t Preferences::put(const char* key, PreferenceType type, const uint8_t* data, size_t length) {
    auto& state = FakeNVS::state();
    if (!started_ || readOnly_ || !key || !*key || std::strlen(key) > 15 || !data || !length) return 0;
    ++state.writes[key];
    if (state.failPutKey == key) return 0;
    const FakeNVS::Value candidate{type, std::vector<uint8_t>(data, data + length)};
    if (freeEntries() < entries(candidate)) return 0;
    // A short return simulates an unsuccessful write; old storage is unchanged.
    if (state.shortPut) return length > 1 ? length - 1 : 0;
    state.namespaces[namespace_][key] = candidate;
    return length;
}
size_t Preferences::putBytes(const char* key, const void* data, size_t length) {
    return put(key, PT_BLOB, static_cast<const uint8_t*>(data), length);
}
size_t Preferences::putChar(const char* key, int8_t value) {
    const uint8_t byte = static_cast<uint8_t>(value);
    return put(key, PT_I8, &byte, 1);
}
size_t Preferences::putUChar(const char* key, uint8_t value) { return put(key, PT_U8, &value, 1); }
int8_t Preferences::getChar(const char* key, int8_t defaultValue) {
    const auto* value = find(key);
    if (!value || value->type != PT_I8 || value->bytes.size() != 1) return defaultValue;
    const unsigned byte = value->bytes[0];
    return static_cast<int8_t>(byte <= 127 ? static_cast<int>(byte) : static_cast<int>(byte) - 256);
}
uint8_t Preferences::getUChar(const char* key, uint8_t defaultValue) {
    const auto* value = find(key);
    return value && value->type == PT_U8 && value->bytes.size() == 1 ? value->bytes[0] : defaultValue;
}
size_t Preferences::getBytesLength(const char* key) {
    const auto* value = find(key);
    return value && value->type == PT_BLOB ? value->bytes.size() : 0;
}
size_t Preferences::getBytes(const char* key, void* data, size_t capacity) {
    const auto* value = find(key);
    if (!value || value->type != PT_BLOB || !data || capacity < value->bytes.size()) return 0;
    const size_t length = value->bytes.size();
    const size_t read = FakeNVS::state().shortRead && length ? length - 1 : length;
    std::memcpy(data, value->bytes.data(), read);
    return read;
}
bool Preferences::remove(const char* key) {
    if (!started_ || readOnly_ || !find(key)) return false;
    auto& state = FakeNVS::state();
    ++state.removes[key];
    if (state.failRemoveKey == key) return false;
    state.namespaces[namespace_].erase(key);
    return true;
}
