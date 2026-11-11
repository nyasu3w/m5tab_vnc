#pragma once

// Host-only event/display model. This does not replace testing the real panel.
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

constexpr int TL_DATUM = 0;
namespace fonts {
struct Font {};
inline const Font FreeSans9pt7b{}, FreeSansBold9pt7b{}, FreeSans12pt7b{}, FreeSansBold12pt7b{};
}
struct MockContact {
    int x = 0, y = 0;
    uint8_t id = 0;
    bool pressed = false, released = false;
    bool flicking = false, dragging = false;
    bool isPressed() const { return pressed; }
    bool wasReleased() const { return released; }
    bool isFlicking() const { return flicking; }
    bool isDragging() const { return dragging; }
};
struct MockFrame {
    std::vector<MockContact> contacts;
    uint8_t key = 0;
};
struct MockDisplay {
    std::vector<std::string> labels;
    int width() const { return 1280; }
    int height() const { return 720; }
    void fillScreen(uint32_t) {}
    void setTextColor(uint32_t, uint32_t = 0) {}
    void setTextDatum(int) {}
    void setTextSize(int) {}
    void setFont(const fonts::Font*) {}
    int textWidth(const char* text) const { return static_cast<int>(std::strlen(text)) * 12; }
    int fontHeight() const { return 22; }
    void drawString(const char* text, int x, int y) {
        if (x < 0 || y < 0 || x + textWidth(text) > width() || y + fontHeight() > height())
            throw std::runtime_error("Text outside mock screen");
        labels.emplace_back(text);
    }
    void fillRoundRect(int x, int y, int w, int h, int, uint32_t) { check(x, y, w, h); }
    void drawRoundRect(int x, int y, int w, int h, int, uint32_t) { check(x, y, w, h); }
    void check(int x, int y, int w, int h) {
        if (x < 0 || y < 0 || w < 1 || h < 1 || x + w > width() || y + h > height())
            throw std::runtime_error("Rectangle outside mock screen");
    }
};
struct MockTouch {
    std::vector<MockContact> contacts;
    uint8_t getCount() const { return static_cast<uint8_t>(contacts.size()); }
    const MockContact& getDetail(uint8_t index = 0) const {
        static const MockContact empty;
        return index < contacts.size() ? contacts[index] : empty;
    }
};
struct MockM5 {
    MockDisplay Display;
    MockTouch Touch;
    std::vector<MockFrame> frames;
    size_t cursor = 0;
    unsigned updates = 0;
    uint8_t key = 0;
    void update() {
        if (++updates > 2000) throw std::runtime_error("UI did not complete its scripted events");
        if (cursor < frames.size()) {
            Touch.contacts = frames[cursor].contacts;
            key = frames[cursor++].key;
        } else {
            Touch.contacts.clear();
            key = 0;
        }
    }
    void reset() { *this = MockM5{}; }
};
inline MockM5 M5;
inline void delay(unsigned long) {}
