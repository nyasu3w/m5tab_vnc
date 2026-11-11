#include "ProfileUI.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {
constexpr uint32_t BG = 0x101820;
constexpr uint32_t PANEL = 0x21313f;
constexpr uint32_t TEXT = 0xf0f4f8;
constexpr uint32_t MUTED = 0xb3c2cf;
constexpr uint32_t ACCENT = 0x167ba5;
constexpr uint32_t DANGER = 0x983a3a;
constexpr uint32_t WARN = 0xffd275;
constexpr int GAP = 10;
constexpr int MARGIN = 20;
constexpr size_t MAX_BUTTONS = 60;

struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};
struct Button { Rect rect; int action; bool enabled; };
enum class Screen { Select, Edit, Keyboard, Delete, Message };
enum Action {
    A_NONE = -1, A_CONNECT = 1, A_NEW, A_EDIT, A_DELETE, A_CANCEL,
    A_SAVE, A_DELETE_YES, A_OK, A_CONNECT_ANYWAY,
    A_LOWER, A_UPPER, A_SYMBOL, A_SPACE, A_BACKSPACE, A_DONE,
    A_KEY_CANCEL, A_SHOW,
    A_SLOT = 100, A_FIELD = 200, A_CHAR = 1000
};

// Treat data as bytes, not UTF-8: the selected FreeSans fonts are ASCII fonts.
// Never pass user-provided control bytes to the display's text engine.
void asciiCopy(char* dst, size_t capacity, const char* src, size_t limit = 511) {
    size_t n = 0;
    if (src) {
        while (n + 1 < capacity && n < limit && src[n]) {
            const unsigned char c = static_cast<unsigned char>(src[n]);
            dst[n++] = c >= 32 && c <= 126 ? static_cast<char>(c) : '?';
        }
    }
    dst[n] = '\0';
}

bool parsePort(const char* text, uint16_t& result) {
    if (!text || !*text) return false;
    uint32_t value = 0;
    size_t digits = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || ++digits > 5) return false;
        const uint32_t digit = static_cast<uint32_t>(*p - '0');
        // Check BEFORE multiplication/conversion; no integer wrap is possible.
        if (value > (65535U - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    if (value == 0 || value > 65535U) return false;
    result = static_cast<uint16_t>(value);
    return true;
}

class ProfileUI {
public:
    ProfileUI(ProfileStore& store, uint8_t (*readKey)())
        : store_(store), readKey_(readKey), width_(M5.Display.width()),
          height_(M5.Display.height()) {}

    bool run(ConnectionProfile& out, uint8_t& slot) {
        // These helpers always update M5 and yield, including at entry/exit.
        releaseBarrier(true);
        selected_ = store_.lastUsed();
        if (!validSlot(selected_)) {
            selected_ = -1;
            for (uint8_t i = 0; i < MAX_CONNECTION_PROFILES; ++i) {
                if (validSlot(i)) { selected_ = i; break; }
            }
        }
        bool finished = false;
        while (!finished) {
            if (dirty_) { render(); dirty_ = false; }
            M5.update();
            const int action = touchAction();
            // Read at most one key per loop. Outside a local keyboard this is
            // deliberately discarded (including Enter, Escape and arrows).
            const uint8_t key = readKey_ ? readKey_() : 0;
            if (action != A_NONE) finished = handleAction(action);
            else if (screen_ == Screen::Keyboard && key) handleKey(key);
            delay(10); // Cooperative/Watchdog-safe, never a busy waiting loop.
        }
        releaseBarrier(true);
        if (connected_) {
            out = pending_;
            slot = static_cast<uint8_t>(selected_);
        }
        // Do not leave a password rendered when the caller resumes networking.
        M5.Display.fillScreen(BG);
        M5.Display.setTextColor(TEXT, BG);
        M5.Display.setTextDatum(TL_DATUM);
        M5.Display.setFont(&fonts::FreeSans9pt7b);
        return connected_;
    }

private:
    ProfileStore& store_;
    uint8_t (*readKey_)();
    const int width_, height_;
    Screen screen_ = Screen::Select;
    Screen messageReturn_ = Screen::Select;
    bool dirty_ = true, connected_ = false, connectWarning_ = false;
    int selected_ = -1, editSlot_ = -1, field_ = 0;
    ConnectionProfile draft_{};
    ConnectionProfile pending_{};
    char keyText_[254]{};
    char keyStatus_[96]{};
    char message_[512]{};
    char messageTitle_[64]{};
    size_t keyLimit_ = 0;
    int keyMode_ = 0; // 0 lower-case, 1 upper-case, 2 all punctuation.
    bool showPassword_ = false;
    Button buttons_[MAX_BUTTONS]{};
    size_t buttonCount_ = 0;
    bool touchBlocked_ = false, tracking_ = false, moved_ = false;
    int pressedButton_ = -1, startX_ = 0, startY_ = 0;
    int lastX_ = 0, lastY_ = 0;
    uint8_t touchID_ = 0;

    bool validSlot(int slot) const {
        return slot >= 0 && slot < MAX_CONNECTION_PROFILES &&
               store_.isUsed(static_cast<uint8_t>(slot)) &&
               store_.get(static_cast<uint8_t>(slot));
    }

    bool deletableSlot(int slot) const {
        return slot >= 0 && slot < MAX_CONNECTION_PROFILES &&
               (validSlot(slot) || store_.isCorrupt(static_cast<uint8_t>(slot)));
    }

    // Count ACTIVE contacts: some M5Unified versions include the release
    // detail in getCount() for one frame. Never mistake that for a held finger.
    int activeContacts() const {
        int count = 0;
        for (uint8_t i = 0; i < M5.Touch.getCount(); ++i)
            if (M5.Touch.getDetail(i).isPressed()) ++count;
        return count;
    }

    void releaseBarrier(bool drainKeyboard) {
        tracking_ = false;
        touchBlocked_ = true;
        unsigned drained = 0;
        bool keyIdle = !drainKeyboard || !readKey_;
        do {
            M5.update();
            if (drainKeyboard && readKey_ && drained < 8) {
                // Bound stale drain; never wait indefinitely for a held key.
                if (readKey_()) ++drained;
                else { keyIdle = true; drained = 8; }
            }
            delay(10);
        } while (activeContacts() != 0 || (!keyIdle && drained < 8));
        touchBlocked_ = false;
    }

    void changeScreen(Screen next) {
        screen_ = next;
        dirty_ = true;
        // Discard only a small bounded set of stale keys on transitions.
        releaseBarrier(true);
    }

    int hit(int x, int y) const {
        for (size_t i = 0; i < buttonCount_; ++i)
            if (buttons_[i].enabled && buttons_[i].rect.contains(x, y))
                return static_cast<int>(i);
        return -1;
    }

    int touchAction() {
        const int count = activeContacts();
        if (count > 1) {
            // A multi-finger gesture invalidates the WHOLE contact sequence.
            tracking_ = false; touchBlocked_ = true;
            return A_NONE;
        }
        if (touchBlocked_) {
            if (!count) touchBlocked_ = false;
            return A_NONE;
        }
        if (count == 1) {
            for (uint8_t i = 0; i < M5.Touch.getCount(); ++i) {
                const auto& t = M5.Touch.getDetail(i);
                if (!t.isPressed()) continue;
                if (!tracking_) {
                    tracking_ = true; moved_ = false;
                    touchID_ = t.id;
                    startX_ = lastX_ = t.x; startY_ = lastY_ = t.y;
                    pressedButton_ = hit(t.x, t.y);
                } else if (touchID_ != t.id) {
                    tracking_ = false; touchBlocked_ = true;
                    return A_NONE;
                }
                lastX_ = t.x; lastY_ = t.y;
                if (abs(t.x - startX_) > 12 || abs(t.y - startY_) > 12 ||
                    hit(t.x, t.y) != pressedButton_ || t.isFlicking() ||
                    t.isDragging()) moved_ = true;
                return A_NONE;
            }
        }
        if (!tracking_) return A_NONE;
        // Prefer final release coordinates if present; otherwise the last
        // active coordinates are the last sample available from the driver.
        for (uint8_t i = 0; i < M5.Touch.getCount(); ++i) {
            const auto& t = M5.Touch.getDetail(i);
            if (t.id == touchID_ && t.wasReleased()) {
                lastX_ = t.x; lastY_ = t.y;
            }
        }
        tracking_ = false;
        if (moved_ || abs(lastX_ - startX_) > 12 ||
            abs(lastY_ - startY_) > 12 || pressedButton_ < 0 ||
            hit(lastX_, lastY_) != pressedButton_) return A_NONE;
        return buttons_[pressedButton_].action;
    }

    void font(bool large = false, bool bold = false) {
        M5.Display.setTextSize(1);
        if (large) M5.Display.setFont(bold ? &fonts::FreeSansBold12pt7b : &fonts::FreeSans12pt7b);
        else M5.Display.setFont(bold ? &fonts::FreeSansBold9pt7b : &fonts::FreeSans9pt7b);
        M5.Display.setTextDatum(TL_DATUM);
    }

    // All untrusted strings are pixel-truncated before drawString, so they
    // cannot spill into adjacent cards/buttons. No sprite/framebuffer needed.
    void text(const char* value, int x, int y, int maxWidth,
              uint32_t color = TEXT, bool large = false, bool bold = false,
              size_t limit = 511) {
        font(large, bold);
        char line[512]; asciiCopy(line, sizeof(line), value, limit);
        size_t length = strlen(line);
        if (M5.Display.textWidth(line) > maxWidth) {
            while (length && M5.Display.textWidth(line) > maxWidth) line[--length] = '\0';
            if (length >= 3) {
                line[length - 3] = '.'; line[length - 2] = '.'; line[length - 1] = '.';
                while (length && M5.Display.textWidth(line) > maxWidth) {
                    line[--length] = '\0';
                }
            }
        }
        M5.Display.setTextColor(color);
        if (maxWidth > 0) M5.Display.drawString(line, x, y);
    }

    void paragraph(const char* value, int x, int y, int maxWidth, int rows,
                   uint32_t color = MUTED) {
        char safe[512]; asciiCopy(safe, sizeof(safe), value);
        size_t offset = 0;
        font();
        for (int row = 0; row < rows && safe[offset]; ++row) {
            char line[512]; size_t n = 0, wordBreak = 0;
            while (safe[offset + n] && n + 1 < sizeof(line)) {
                line[n] = safe[offset + n]; line[n + 1] = '\0';
                if (M5.Display.textWidth(line) > maxWidth) break;
                if (line[n] == ' ') wordBreak = n;
                ++n;
            }
            if (!n) n = 1;
            if (safe[offset + n] && wordBreak) n = wordBreak;
            memcpy(line, safe + offset, n); line[n] = '\0';
            text(line, x, y + row * 30, maxWidth, color);
            offset += n;
            while (safe[offset] == ' ') ++offset;
            if (row == rows - 1 && safe[offset])
                text("...", x + maxWidth - 30, y + row * 30, 30, color);
        }
    }

    void button(Rect r, const char* label, int action, bool enabled = true,
                uint32_t color = PANEL) {
        M5.Display.fillRoundRect(r.x, r.y, r.w, r.h, 8, enabled ? color : BG);
        M5.Display.drawRoundRect(r.x, r.y, r.w, r.h, 8, enabled ? MUTED : PANEL);
        // Font height, not baseline, is used so 720/1280 rotations remain safe.
        font(false, true);
        const int labelWidth = M5.Display.textWidth(label);
        const int x = r.x + (labelWidth < r.w - 16 ? (r.w - labelWidth) / 2 : 8);
        text(label, x, r.y + (r.h - M5.Display.fontHeight()) / 2,
             r.w - (x - r.x) - 8, enabled ? TEXT : MUTED, false, true);
        if (buttonCount_ < MAX_BUTTONS)
            buttons_[buttonCount_++] = {r, action, enabled};
    }

    void title(const char* heading, const char* subtitle) {
        text(heading, MARGIN, 20, width_ - MARGIN * 2, TEXT, true, true);
        text(subtitle, MARGIN, 59, width_ - MARGIN * 2, MUTED);
    }

    void endpoint(const ConnectionProfile& profile, int x, int y, int maxWidth) {
        char host[254]; asciiCopy(host, sizeof(host), profile.vncHost, sizeof(profile.vncHost));
        char suffix[12];
        const bool ipv6 = strchr(host, ':') != nullptr;
        snprintf(suffix, sizeof(suffix), "%s:%u", ipv6 ? "]" : "", static_cast<unsigned>(profile.vncPort));
        font();
        const int hostWidth = maxWidth - M5.Display.textWidth(suffix) -
                              (ipv6 ? M5.Display.textWidth("[") : 0);
        size_t n = strlen(host);
        if (M5.Display.textWidth(host) > hostWidth) {
            while (n && M5.Display.textWidth(host) > hostWidth) host[--n] = '\0';
            if (n >= 3) {
                host[n - 3] = '.'; host[n - 2] = '.'; host[n - 1] = '.';
                while (n && M5.Display.textWidth(host) > hostWidth) host[--n] = '\0';
            }
        }
        char value[270];
        snprintf(value, sizeof(value), "%s%s%s", ipv6 ? "[" : "", host, suffix);
        text(value, x, y, maxWidth, TEXT);
    }

    void drawSelect() {
        title("Connection profiles", "Select a profile, then tap Connect. CardKB does not connect here.");
        char notice[512];
        if (!store_.available()) {
            snprintf(notice, sizeof(notice), "Storage unavailable: %s", store_.lastError() ? store_.lastError() : "unknown error");
            text(notice, MARGIN, 96, width_ - 2 * MARGIN, WARN);
        } else if (store_.lastError() && *store_.lastError()) {
            snprintf(notice, sizeof(notice), "Storage notice: %s", store_.lastError());
            text(notice, MARGIN, 96, width_ - 2 * MARGIN, WARN);
        } else text("New / Edit / Delete changes saved profiles only. No automatic connection.", MARGIN, 96, width_ - 2 * MARGIN, MUTED);
        if (store_.corruptCount()) {
            snprintf(notice, sizeof(notice), "%u damaged record(s). Select a damaged slot and Delete to free it.", static_cast<unsigned>(store_.corruptCount()));
            text(notice, MARGIN, 127, width_ - 2 * MARGIN, WARN);
        } else if (!store_.count()) {
            text("No profiles yet: tap New, enter WiFi and VNC details, then Save.", MARGIN, 127, width_ - 2 * MARGIN, WARN);
        }
        const int top = 170;
        const int bottom = height_ - 126;
        const int cardW = (width_ - MARGIN * 2 - GAP) / 2;
        const int cardH = (bottom - top - 3 * GAP) / 4;
        for (uint8_t i = 0; i < MAX_CONNECTION_PROFILES; ++i) {
            const Rect r{MARGIN + (i % 2) * (cardW + GAP),
                         top + (i / 2) * (cardH + GAP), cardW, cardH};
            const auto* profile = store_.get(i);
            const bool used = store_.isUsed(i) && profile;
            M5.Display.fillRoundRect(r.x, r.y, r.w, r.h, 8, selected_ == i ? ACCENT : PANEL);
            M5.Display.drawRoundRect(r.x, r.y, r.w, r.h, 8, selected_ == i ? TEXT : MUTED);
            if (used) {
                char label[100];
                char name[64]; asciiCopy(name, sizeof(name), profile->name, sizeof(profile->name));
                snprintf(label, sizeof(label), "%u. %s%s", static_cast<unsigned>(i + 1), name, store_.lastUsed() == i ? " [last]" : "");
                text(label, r.x + 12, r.y + 9, r.w - 24, TEXT, false, true);
                char ssid[48]; snprintf(ssid, sizeof(ssid), "SSID: %.32s", profile->ssid);
                text(ssid, r.x + 12, r.y + 37, r.w - 24, TEXT);
                endpoint(*profile, r.x + 12, r.y + 65, r.w - 24);
                if (buttonCount_ < MAX_BUTTONS) buttons_[buttonCount_++] = {r, A_SLOT + i, true};
            } else if (store_.isCorrupt(i)) {
                char label[48];
                snprintf(label, sizeof(label), "%u. Damaged slot", static_cast<unsigned>(i + 1));
                text(label, r.x + 12, r.y + 9, r.w - 24, WARN, false, true);
                text("Cannot connect or edit", r.x + 12, r.y + 37, r.w - 24, WARN);
                text("Select, then Delete to free", r.x + 12, r.y + 65, r.w - 24, MUTED);
                if (buttonCount_ < MAX_BUTTONS) buttons_[buttonCount_++] = {r, A_SLOT + i, true};
            } else text("Empty slot", r.x + 12, r.y + 18, r.w - 24, MUTED);
        }
        const int y = height_ - 100;
        const int w = (width_ - MARGIN * 2 - 4 * GAP) / 5;
        const bool selected = validSlot(selected_);
        button({MARGIN, y, w, 64}, "Connect", A_CONNECT, selected, ACCENT);
        button({MARGIN + (w + GAP), y, w, 64}, "New", A_NEW, store_.findFreeSlot() >= 0);
        button({MARGIN + 2 * (w + GAP), y, w, 64}, "Edit", A_EDIT, selected);
        button({MARGIN + 3 * (w + GAP), y, w, 64}, "Delete", A_DELETE, deletableSlot(selected_), DANGER);
        button({MARGIN + 4 * (w + GAP), y, w, 64}, "Cancel", A_CANCEL);
    }

    static const char* fieldLabel(int field) {
        static const char* const labels[] = {"Profile name", "WiFi SSID", "WiFi password", "VNC host / IP", "VNC port", "VNC password"};
        return labels[field];
    }

    const char* fieldValue(int field, char* port, size_t size) const {
        switch (field) {
            case 0: return draft_.name;
            case 1: return draft_.ssid;
            case 2: return draft_.wifiPassword;
            case 3: return draft_.vncHost;
            case 4: snprintf(port, size, "%u", static_cast<unsigned>(draft_.vncPort)); return port;
            default: return draft_.vncPassword;
        }
    }

    void drawEdit() {
        title(store_.isUsed(static_cast<uint8_t>(editSlot_)) ? "Edit profile" : "New profile",
              "Tap a field to edit. Save writes all six fields; Cancel discards this draft.");
        const int rowH = (height_ - 220) / 6;
        for (int i = 0; i < 6; ++i) {
            const Rect r{MARGIN, 100 + i * rowH, width_ - MARGIN * 2, rowH - GAP};
            M5.Display.fillRoundRect(r.x, r.y, r.w, r.h, 8, PANEL);
            text(fieldLabel(i), r.x + 14, r.y + 8, r.w - 30, MUTED, false, true);
            char port[6]; const char* value = fieldValue(i, port, sizeof(port));
            char mask[65];
            if (i == 2 || i == 5) {
                size_t n = strlen(value); if (n > sizeof(mask) - 1) n = sizeof(mask) - 1;
                memset(mask, '*', n); mask[n] = '\0'; value = mask;
            }
            text(*value ? value : "(empty)", r.x + 14, r.y + 36, r.w - 30, TEXT);
            if (buttonCount_ < MAX_BUTTONS) buttons_[buttonCount_++] = {r, A_FIELD + i, true};
        }
        const int w = (width_ - MARGIN * 2 - GAP) / 2;
        button({MARGIN, height_ - 96, w, 64}, "Save", A_SAVE, true, ACCENT);
        button({MARGIN + w + GAP, height_ - 96, w, 64}, "Cancel", A_CANCEL);
    }

    void drawKeyboard() {
        title(fieldLabel(field_), "Done applies this field to the draft. Cancel leaves this field unchanged.");
        const char* help[] = {"1-63 bytes; ASCII letters, numbers and symbols.",
                              "1-32 bytes. SSID is case-sensitive.",
                              "Empty (open), 8-63 bytes, or exactly 64 hex digits.",
                              "Hostname / IPv4 / IPv6, up to 253 bytes. No URL scheme.",
                              "Digits only. Valid port range: 1-65535 (no wrap).",
                              "0-8 bytes. Classic VNC uses at most 8 password bytes."};
        text(help[field_], MARGIN, 97, width_ - MARGIN * 2, MUTED);
        const bool password = field_ == 2 || field_ == 5;
        const int toggleW = password ? 116 : 0;
        const Rect input{MARGIN, 135, width_ - MARGIN * 2 - (password ? toggleW + GAP : 0), 62};
        M5.Display.fillRoundRect(input.x, input.y, input.w, input.h, 8, PANEL);
        char visible[254];
        const size_t n = strlen(keyText_);
        if (password && !showPassword_) { memset(visible, '*', n); visible[n] = '\0'; }
        else asciiCopy(visible, sizeof(visible), keyText_);
        // Display the TAIL while typing, preserving the newest character.
        font(true);
        size_t start = 0;
        while (visible[start] && M5.Display.textWidth(visible + start) > input.w - 28) ++start;
        text(*visible ? visible + start : "(empty)", input.x + 14, input.y + 16, input.w - 28, TEXT, true);
        if (password) button({width_ - MARGIN - toggleW, 135, toggleW, 62}, showPassword_ ? "Hide" : "Show", A_SHOW);
        char status[128];
        if (*keyStatus_) asciiCopy(status, sizeof(status), keyStatus_);
        else snprintf(status, sizeof(status), "%u / %u bytes   |   CardKB: Enter=Done, Esc=Cancel, Backspace=erase", static_cast<unsigned>(n), static_cast<unsigned>(keyLimit_));
        text(status, MARGIN, 211, width_ - MARGIN * 2, *keyStatus_ ? WARN : MUTED);

        const int top = 252;
        const int rowH = (height_ - top - MARGIN - 5 * GAP) / 6;
        if (field_ == 4) {
            static const char* const digits[] = {"123", "456", "789", "0"};
            const int w = (width_ - 2 * MARGIN - 2 * GAP) / 3;
            for (int row = 0; row < 4; ++row) {
                const int count = static_cast<int>(strlen(digits[row]));
                const int x = (width_ - (count * w + (count - 1) * GAP)) / 2;
                for (int col = 0; col < count; ++col) {
                    char label[2]{digits[row][col], '\0'};
                    button({x + col * (w + GAP), top + row * (rowH + GAP), w, rowH}, label, A_CHAR + digits[row][col]);
                }
            }
            button({MARGIN, top + 4 * (rowH + GAP), width_ - 2 * MARGIN, rowH}, "Backspace", A_BACKSPACE);
        } else {
            static const char* const lower[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
            static const char* const upper[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
            // All 32 non-alphanumeric, non-space printable ASCII characters.
            static const char symbols[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
            for (int row = 0; row < 4; ++row) {
                const char* chars = keyMode_ == 1 ? upper[row] : lower[row];
                const int count = keyMode_ == 2 ? 8 : static_cast<int>(strlen(chars));
                const int columns = keyMode_ == 2 ? 8 : 10;
                const int w = (width_ - 2 * MARGIN - (columns - 1) * GAP) / columns;
                const int x = (width_ - (count * w + (count - 1) * GAP)) / 2;
                for (int col = 0; col < count; ++col) {
                    const char c = keyMode_ == 2 ? symbols[row * 8 + col] : chars[col];
                    char label[2]{c, '\0'};
                    button({x + col * (w + GAP), top + row * (rowH + GAP), w, rowH}, label, A_CHAR + static_cast<unsigned char>(c));
                }
            }
            const int w = (width_ - 2 * MARGIN - 3 * GAP) / 4;
            const int y = top + 4 * (rowH + GAP);
            button({MARGIN, y, w, rowH}, "abc", A_LOWER, true, keyMode_ == 0 ? ACCENT : PANEL);
            button({MARGIN + w + GAP, y, w, rowH}, "ABC", A_UPPER, true, keyMode_ == 1 ? ACCENT : PANEL);
            button({MARGIN + 2 * (w + GAP), y, w, rowH}, "Symbols", A_SYMBOL, true, keyMode_ == 2 ? ACCENT : PANEL);
            button({MARGIN + 3 * (w + GAP), y, w, rowH}, "Backspace", A_BACKSPACE);
        }
        const int y = top + 5 * (rowH + GAP);
        const int columns = field_ == 4 ? 2 : 3;
        const int w = (width_ - 2 * MARGIN - (columns - 1) * GAP) / columns;
        int x = MARGIN;
        if (field_ != 4) { button({x, y, w, rowH}, "Space", A_SPACE); x += w + GAP; }
        button({x, y, w, rowH}, "Done", A_DONE, true, ACCENT);
        button({x + w + GAP, y, w, rowH}, "Cancel", A_KEY_CANCEL);
    }

    void drawDelete() {
        title("Delete profile?", "This permanently removes the saved profile. Cancel keeps it unchanged.");
        if (validSlot(selected_)) {
            const auto& p = *store_.get(static_cast<uint8_t>(selected_));
            text(p.name, MARGIN, 137, width_ - 2 * MARGIN, TEXT, true, true, sizeof(p.name));
            text("WiFi SSID", MARGIN, 193, width_ - 2 * MARGIN, MUTED);
            text(p.ssid, MARGIN, 229, width_ - 2 * MARGIN, TEXT, false, false, sizeof(p.ssid));
            text("VNC server", MARGIN, 284, width_ - 2 * MARGIN, MUTED);
            endpoint(p, MARGIN, 320, width_ - 2 * MARGIN);
        } else if (deletableSlot(selected_)) {
            char label[48];
            snprintf(label, sizeof(label), "Slot %u: damaged record", static_cast<unsigned>(selected_ + 1));
            text(label, MARGIN, 137, width_ - 2 * MARGIN, WARN, true, true);
            paragraph("Damaged / unsupported profile record. Explicit delete will free this slot. The stored bytes cannot be recovered after deletion.",
                      MARGIN, 198, width_ - 2 * MARGIN, 6, WARN);
        }
        const int w = (width_ - 2 * MARGIN - GAP) / 2;
        button({MARGIN, height_ - 105, w, 70}, "Delete", A_DELETE_YES, true, DANGER);
        button({MARGIN + w + GAP, height_ - 105, w, 70}, "Cancel", A_CANCEL);
    }

    void drawMessage() {
        title(messageTitle_, connectWarning_ ? "Connection is possible, but storage was not updated." : "No changes were confirmed. Review the message below.");
        paragraph(message_, MARGIN, 138, width_ - 2 * MARGIN,
                  (height_ - 290) / 30, WARN);
        if (connectWarning_) {
            const int w = (width_ - 2 * MARGIN - GAP) / 2;
            button({MARGIN, height_ - 105, w, 70}, "Connect anyway", A_CONNECT_ANYWAY, true, ACCENT);
            button({MARGIN + w + GAP, height_ - 105, w, 70}, "Back", A_OK);
        } else button({MARGIN, height_ - 105, width_ - 2 * MARGIN, 70}, "OK", A_OK);
    }

    void render() {
        // Entire pages are drawn only on state/data/input changes, not polling.
        M5.Display.fillScreen(BG);
        buttonCount_ = 0;
        switch (screen_) {
            case Screen::Select: drawSelect(); break;
            case Screen::Edit: drawEdit(); break;
            case Screen::Keyboard: drawKeyboard(); break;
            case Screen::Delete: drawDelete(); break;
            case Screen::Message: drawMessage(); break;
        }
    }

    void message(const char* heading, const char* reason, Screen returnTo,
                 bool connectionWarning = false) {
        asciiCopy(messageTitle_, sizeof(messageTitle_), heading);
        asciiCopy(message_, sizeof(message_), reason ? reason : "Unknown storage error.");
        messageReturn_ = returnTo;
        connectWarning_ = connectionWarning;
        changeScreen(Screen::Message);
    }

    void storeError(const char* heading, Screen returnTo) {
        const char* error = store_.lastError();
        message(heading, error && *error ? error : "Storage operation failed. The changes were not saved.", returnTo);
    }

    void startEdit(bool create) {
        editSlot_ = create ? store_.findFreeSlot() : selected_;
        if (editSlot_ < 0 || editSlot_ >= MAX_CONNECTION_PROFILES) {
            message("No free slots", "All 8 slots are used. Edit a profile or delete one first.", Screen::Select);
            return;
        }
        if (create) draft_ = ConnectionProfile{};
        else if (validSlot(editSlot_)) draft_ = *store_.get(static_cast<uint8_t>(editSlot_));
        else return;
        changeScreen(Screen::Edit);
    }

    void openKeyboard(int field) {
        field_ = field;
        static const size_t limits[] = {63, 32, 64, 253, 5, 8};
        keyLimit_ = limits[field];
        char port[6];
        const char* original = fieldValue(field, port, sizeof(port));
        size_t length = 0;
        while (length < keyLimit_ && original[length]) ++length;
        // Keep saved SSID/name/password bytes exactly; ASCII substitution is
        // display-only. Opening a field then Done must not corrupt UTF-8 data.
        memcpy(keyText_, original, length);
        keyText_[length] = '\0';
        keyStatus_[0] = '\0'; keyMode_ = 0; showPassword_ = false;
        changeScreen(Screen::Keyboard);
    }

    void append(char c) {
        if (c < 32 || c > 126) return;
        if (field_ == 4 && (c < '0' || c > '9')) return;
        const size_t n = strlen(keyText_);
        if (n >= keyLimit_) {
            snprintf(keyStatus_, sizeof(keyStatus_), "Maximum length is %u bytes. Backspace to replace text.", static_cast<unsigned>(keyLimit_));
        } else {
            keyText_[n] = c; keyText_[n + 1] = '\0'; keyStatus_[0] = '\0';
        }
        dirty_ = true;
    }

    void backspace() {
        const size_t n = strlen(keyText_);
        if (n) {
            size_t start = n - 1;
            // Remove one UTF-8 codepoint if the existing text ends in one.
            // New ASCII input is one byte; byte limits remain unchanged.
            while (start && (static_cast<unsigned char>(keyText_[start]) & 0xc0) == 0x80) --start;
            keyText_[start] = '\0'; dirty_ = true;
        }
        if (*keyStatus_) { keyStatus_[0] = '\0'; dirty_ = true; }
    }

    void keyboardDone() {
        char* target = nullptr;
        switch (field_) {
            case 0: target = draft_.name; break;
            case 1: target = draft_.ssid; break;
            case 2: target = draft_.wifiPassword; break;
            case 3: target = draft_.vncHost; break;
            case 4: {
                uint16_t port;
                if (!parsePort(keyText_, port)) {
                    asciiCopy(keyStatus_, sizeof(keyStatus_), "Port must be digits in the range 1-65535.");
                    dirty_ = true; return;
                }
                draft_.vncPort = port; break;
            }
            case 5: target = draft_.vncPassword; break;
        }
        if (target) memcpy(target, keyText_, strlen(keyText_) + 1);
        memset(keyText_, 0, sizeof(keyText_)); showPassword_ = false;
        changeScreen(Screen::Edit);
    }

    void handleKey(uint8_t key) {
        switch (key) {
            case 8: case 127: backspace(); break;
            case 10: case 13: keyboardDone(); break;
            case 27:
                memset(keyText_, 0, sizeof(keyText_)); showPassword_ = false;
                changeScreen(Screen::Edit); break;
            default:
                // CardKB arrow/function/control codes are deliberately ignored.
                if (key >= 32 && key <= 126) append(static_cast<char>(key));
                break;
        }
    }

    bool handleAction(int action) {
        if (screen_ == Screen::Keyboard) {
            if (action >= A_CHAR + 32 && action <= A_CHAR + 126) append(static_cast<char>(action - A_CHAR));
            else switch (action) {
                case A_LOWER: keyMode_ = 0; dirty_ = true; break;
                case A_UPPER: keyMode_ = 1; dirty_ = true; break;
                case A_SYMBOL: keyMode_ = 2; dirty_ = true; break;
                case A_SPACE: append(' '); break;
                case A_BACKSPACE: backspace(); break;
                case A_DONE: keyboardDone(); break;
                case A_KEY_CANCEL:
                    memset(keyText_, 0, sizeof(keyText_)); showPassword_ = false;
                    changeScreen(Screen::Edit); break;
                case A_SHOW: showPassword_ = !showPassword_; dirty_ = true; break;
            }
            return false;
        }
        if (screen_ == Screen::Message) {
            if (action == A_CONNECT_ANYWAY && connectWarning_) { connected_ = true; return true; }
            if (action == A_OK) changeScreen(messageReturn_);
            return false;
        }
        if (screen_ == Screen::Edit) {
            if (action >= A_FIELD && action < A_FIELD + 6) openKeyboard(action - A_FIELD);
            else if (action == A_CANCEL) {
                draft_ = ConnectionProfile{};
                changeScreen(Screen::Select);
            } else if (action == A_SAVE) {
                char error[256]{};
                if (!validateConnectionProfile(draft_, error, sizeof(error))) {
                    message("Invalid profile", *error ? error : "Please check all six fields.", Screen::Edit);
                } else if (!store_.save(static_cast<uint8_t>(editSlot_), draft_)) {
                    storeError("Save failed", Screen::Edit);
                } else {
                    selected_ = editSlot_;
                    draft_ = ConnectionProfile{};
                    changeScreen(Screen::Select); // Save NEVER means Connect.
                }
            }
            return false;
        }
        if (screen_ == Screen::Delete) {
            if (action == A_CANCEL) changeScreen(Screen::Select);
            else if (action == A_DELETE_YES && deletableSlot(selected_)) {
                if (!store_.remove(static_cast<uint8_t>(selected_))) storeError("Delete failed", Screen::Select);
                else {
                    selected_ = -1;
                    for (uint8_t i = 0; i < MAX_CONNECTION_PROFILES; ++i)
                        if (validSlot(i)) { selected_ = i; break; }
                    changeScreen(Screen::Select);
                }
            }
            return false;
        }
        if (action >= A_SLOT && action < A_SLOT + MAX_CONNECTION_PROFILES) {
            selected_ = action - A_SLOT; dirty_ = true;
        } else switch (action) {
            case A_CANCEL: return true;
            case A_NEW: startEdit(true); break;
            case A_EDIT: if (validSlot(selected_)) startEdit(false); break;
            case A_DELETE: if (deletableSlot(selected_)) changeScreen(Screen::Delete); break;
            case A_CONNECT: {
                if (!validSlot(selected_)) break;
                char error[256]{};
                pending_ = *store_.get(static_cast<uint8_t>(selected_));
                if (!validateConnectionProfile(pending_, error, sizeof(error))) {
                    message("Invalid saved profile", *error ? error : "Edit this profile before connecting.", Screen::Select);
                    break;
                }
                if (!store_.available()) {
                    message("Storage unavailable", "This validated saved profile can still connect. Storage changes and the last-used selection cannot be persisted. Tap Connect anyway to continue.", Screen::Select, true);
                } else { connected_ = true; return true; }
                // The caller persists last-used only after connection succeeds.
                break;
            }
        }
        return false;
    }
};
} // namespace

bool selectConnectionProfile(ProfileStore& store, ConnectionProfile& out,
                             uint8_t& slot, uint8_t (*readKey)()) {
    ProfileUI ui(store, readKey);
    return ui.run(out, slot);
}
