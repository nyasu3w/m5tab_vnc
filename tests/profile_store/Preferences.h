#pragma once

#include <stddef.h>
#include <stdint.h>
#include <map>
#include <string>
#include <vector>

enum PreferenceType {
    PT_I8, PT_U8, PT_I16, PT_U16, PT_I32, PT_U32, PT_I64, PT_U64,
    PT_STR, PT_BLOB, PT_INVALID
};

namespace FakeNVS {
struct Value {
    PreferenceType type;
    std::vector<uint8_t> bytes;
};
struct State {
    std::map<std::string, std::map<std::string, Value> > namespaces;
    bool failReadOpen = false;
    bool failWriteOpen = false;
    bool shortRead = false;
    bool shortPut = false;
    std::string failPutKey;
    std::string failRemoveKey;
    size_t capacity = 1024;
    unsigned activeHandles = 0;
    unsigned opens = 0;
    unsigned closes = 0;
    std::map<std::string, unsigned> writes;
    std::map<std::string, unsigned> removes;
};
State& state();
void reset();
void blob(const std::string& key, const std::vector<uint8_t>& bytes);
void scalar(const std::string& key, PreferenceType type, uint8_t value);
Value& value(const std::string& key);
size_t usedEntries();
} // namespace FakeNVS

// Only the real API subset used by ProfileStore is implemented.
class Preferences {
public:
    Preferences() = default;
    ~Preferences();
    bool begin(const char* name, bool readOnly = false, const char* partition = nullptr);
    void end();
    bool remove(const char* key);
    size_t putChar(const char* key, int8_t value);
    size_t putUChar(const char* key, uint8_t value);
    size_t putBytes(const char* key, const void* data, size_t length);
    bool isKey(const char* key);
    PreferenceType getType(const char* key);
    int8_t getChar(const char* key, int8_t defaultValue = 0);
    uint8_t getUChar(const char* key, uint8_t defaultValue = 0);
    size_t getBytesLength(const char* key);
    size_t getBytes(const char* key, void* data, size_t capacity);
    size_t freeEntries();
private:
    bool started_ = false;
    bool readOnly_ = false;
    std::string namespace_;
    const FakeNVS::Value* find(const char* key) const;
    size_t put(const char* key, PreferenceType type, const uint8_t* data, size_t length);
};
