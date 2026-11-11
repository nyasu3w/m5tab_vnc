#include "ProfileUI.h"
#include <Preferences.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace {
void idle(unsigned count = 3) {
    while (count--) M5.frames.push_back({{}, 0});
}
void click(int x, int y) {
    M5.frames.push_back({{{x, y, 0, true, false}}, 0});
    // A release detail is still counted for one M5Unified update frame.
    M5.frames.push_back({{{x, y, 0, false, true}}, 0});
    idle();
}
void key(uint8_t value) { M5.frames.push_back({{}, value}); }
uint8_t readKey() { uint8_t value = M5.key; M5.key = 0; return value; }
ConnectionProfile profile() {
    ConnectionProfile p;
    std::strcpy(p.name, "Home PC");
    std::strcpy(p.ssid, "Home WiFi");
    std::strcpy(p.wifiPassword, "wifi-pass");
    std::strcpy(p.vncHost, "192.168.1.100");
    std::strcpy(p.vncPassword, "secret12");
    return p;
}
void start() { M5.reset(); idle(); }
void noSecrets() {
    for (const auto& label : M5.Display.labels) {
        assert(label.find("wifi-pass") == std::string::npos);
        assert(label.find("secret12") == std::string::npos);
    }
}
ProfileStore seed() {
    FakeNVS::reset();
    ProfileStore store;
    assert(store.load());
    assert(store.save(0, profile()));
    return store;
}
void explicitConnectAndHistory() {
    auto store = seed();
    assert(store.save(1, profile()));
    assert(store.markLastUsed(1));
    start();
    key(13); idle(); // Enter on the selection page must not connect.
    click(100, 205); // Select slot 0, different from last successful slot 1.
    click(100, 650);
    ConnectionProfile out;
    uint8_t slot = 99;
    assert(selectConnectionProfile(store, out, slot, readKey));
    assert(slot == 0 && std::strcmp(out.ssid, "Home WiFi") == 0);
    // Selecting or connecting MUST NOT record a successful VNC connection.
    assert(store.lastUsed() == 1);
    noSecrets();
}
void cancelLeavesOutputUntouched() {
    auto store = seed();
    start(); click(1120, 650);
    ConnectionProfile out; std::strcpy(out.name, "sentinel");
    uint8_t slot = 77;
    assert(!selectConnectionProfile(store, out, slot));
    assert(slot == 77 && std::strcmp(out.name, "sentinel") == 0);
}
void multitouchAndDragAreNotClicks() {
    auto store = seed();
    start();
    M5.frames.push_back({{{100, 650, 0, true, false}}, 0});
    M5.frames.push_back({{{100, 650, 0, true, false}, {200, 650, 1, true, false}}, 0});
    M5.frames.push_back({{{100, 650, 0, true, false}}, 0});
    idle();
    M5.frames.push_back({{{100, 650, 0, true, false}}, 0});
    M5.frames.push_back({{{180, 650, 0, true, false}}, 0});
    M5.frames.push_back({{{180, 650, 0, false, true}}, 0});
    idle();
    click(1120, 650); // If either earlier gesture connected, this is not reached.
    ConnectionProfile out; uint8_t slot = 99;
    assert(!selectConnectionProfile(store, out, slot));
}
void editCancelDoesNotSave() {
    auto store = seed();
    start();
    click(600, 650); // Edit.
    click(100, 135); // Name keyboard.
    key('X'); key(13); idle(); // Applied only to the draft.
    click(1000, 650); // Cancel entire draft.
    click(100, 650); // Connect original saved settings.
    ConnectionProfile out; uint8_t slot = 99;
    assert(selectConnectionProfile(store, out, slot, readKey));
    assert(std::strcmp(out.name, "Home PC") == 0);
    assert(std::strcmp(store.get(0)->name, "Home PC") == 0);
    noSecrets();
}
void existingUtf8IsNotReplacedWhenOpeningField() {
    auto store = seed();
    auto p = profile(); std::strcpy(p.ssid, u8"家のWiFi");
    assert(store.save(0, p));
    start();
    click(600, 650); click(100, 218); // Edit SSID.
    key(13); idle(); // Done, without any character edits.
    click(100, 650); // Save draft.
    click(100, 650); // Explicit Connect.
    ConnectionProfile out; uint8_t slot = 99;
    assert(selectConnectionProfile(store, out, slot, readKey));
    assert(std::strcmp(out.ssid, p.ssid) == 0);
}
void keyboardCancelDoesNotApply() {
    auto store = seed();
    start(); click(600, 650); click(100, 135);
    key('X'); key(27); idle(); // Esc cancels just this field.
    click(100, 650); click(100, 650);
    ConnectionProfile out; uint8_t slot = 99;
    assert(selectConnectionProfile(store, out, slot, readKey));
    assert(std::strcmp(out.name, "Home PC") == 0);
}
void invalidPortStaysInKeyboard() {
    auto store = seed();
    start(); click(600, 650); click(100, 467); // Port.
    for (int i = 0; i < 4; ++i) key(8);
    for (char c : std::string("70000")) key(static_cast<uint8_t>(c));
    key(13); idle(); // Out of range must not wrap or apply.
    key(27); idle(); // Cancel keyboard.
    click(100, 650); click(100, 650);
    ConnectionProfile out; uint8_t slot = 99;
    assert(selectConnectionProfile(store, out, slot, readKey));
    assert(out.vncPort == 5900);
    bool error = false;
    for (const auto& label : M5.Display.labels)
        if (label.find("Port must be digits") != std::string::npos) error = true;
    assert(error);
}
void explicitlyDeleteDamagedRecord() {
    FakeNVS::reset(); FakeNVS::blob("p0", {1, 2, 3});
    ProfileStore store; assert(store.load()); assert(store.isCorrupt(0));
    start(); click(100, 205); click(880, 650); // Damaged slot -> Delete.
    click(100, 650); // Confirm deletion.
    click(1120, 650); // Cancel selection without any connection.
    ConnectionProfile out; uint8_t slot = 99;
    assert(!selectConnectionProfile(store, out, slot, readKey));
    assert(!store.isCorrupt(0) && store.corruptCount() == 0);
    assert(store.findFreeSlot() == 0);
}
}
int main() {
    explicitConnectAndHistory();
    cancelLeavesOutputUntouched();
    multitouchAndDragAreNotClicks();
    editCancelDoesNotSave();
    existingUtf8IsNotReplacedWhenOpeningField();
    keyboardCancelDoesNotApply();
    invalidPortStaysInKeyboard();
    explicitlyDeleteDamagedRecord();
    std::cout << "PASS: 8 profile UI scenarios (mock touch, keyboard and display)\n";
}
