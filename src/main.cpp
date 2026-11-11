/**
 * @file main.cpp
 * @brief VNC Client for M5Stack Tab5
 * 
 * This application implements a VNC (Virtual Network Computing) client
 * for the M5Stack Tab5 device, allowing remote desktop viewing and
 * touch-based interaction with VNC servers.
 * 
 * Hardware: M5Stack Tab5
 *   - Main MCU: ESP32-P4 (RISC-V Dual-core 400MHz)
 *   - Wireless: ESP32-C6 (Wi-Fi 6)
 *   - Display: 5" IPS TFT 1280x720 (MIPI-DSI)
 *   - Touch: GT911 multi-touch controller
 *   - Memory: 16MB Flash, 32MB PSRAM
 * 
 * Required Libraries:
 *   - M5Unified (https://github.com/M5Stack/M5Unified)
 *   - M5GFX (https://github.com/M5Stack/M5GFX)
 *   - arduinoVNC (https://github.com/Links2004/arduinoVNC)
 * 
 * @author Your Name
 * @date 2024
 */

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <VNC.h>
#include "M5GFX_VNCDriver.h"
#include "ConnectionProfile.h"
#include "ProfileStore.h"
#include "ProfileUI.h"
#include <atomic>

// ============================================================================
// Optional legacy credentials - imported once, never required for the build
// ============================================================================

#if __has_include("user_setting.h")
#include "user_setting.h"
#define HAS_LEGACY_CONNECTION_CONFIG 1
#endif

// Display settings
const uint8_t DISPLAY_BRIGHTNESS = 128;         // Display brightness (0-255)
const uint8_t DISPLAY_ROTATION = 3;             // Display rotation (0-3)

// ESP32-P4 Tab5 SDIO2 pins for WiFi (ESP32-C6)
#define SDIO2_CLK GPIO_NUM_12
#define SDIO2_CMD GPIO_NUM_13
#define SDIO2_D0  GPIO_NUM_11
#define SDIO2_D1  GPIO_NUM_10
#define SDIO2_D2  GPIO_NUM_9
#define SDIO2_D3  GPIO_NUM_8
#define SDIO2_RST GPIO_NUM_15

// CardKB
constexpr uint8_t cardkb_addr = 0x5f;
bool cardkb_available=false;

// ============================================================================
// Global variables
// ============================================================================

// VNC display driver and client - using pointers to control initialization timing
M5GFX_VNCDriver* vncDisplay = nullptr;
arduinoVNC* vnc = nullptr;

// Selected once before the network task starts; never edited during a session.
ProfileStore profileStore;
ConnectionProfile activeProfile;
uint8_t activeProfileSlot = 0;
std::atomic<bool> profileConnectedPending{false};

// Touch state tracking
int32_t lastTouchX = 0;
int32_t lastTouchY = 0;
bool wasTouched = false;

// Two-finger scroll tracking
bool twoFingerScrollActive = false;
int32_t scrollStartY = 0;
int32_t lastScrollY = 0;
uint32_t lastScrollTime = 0;
const int32_t SCROLL_THRESHOLD = 50;  // Pixels to move before sending scroll event
const uint32_t SCROLL_MIN_INTERVAL = 100;  // Minimum ms between scroll events

// Connection state
bool wifiConnected = false;
bool vncConnected = false;

// Screen state
bool vncScreenPaused = false;
bool showingInfoScreen = false;
bool screenJustSwitched = false;  // Flag to prevent handleTouch after screen switch

// Multi-touch detection
uint32_t lastThreeTouchTime = 0;
const uint32_t THREE_TOUCH_DEBOUNCE = 500;  // 500ms debounce

// Swipe gesture detection
bool swipeInProgress = false;
bool swipePotential = false;  // Touch started at top, waiting for movement
int32_t swipeStartY = 0;
int32_t swipeStartX = 0;
uint32_t swipeStartTime = 0;
const int32_t SWIPE_TOP_THRESHOLD = 50;  // Top edge detection threshold
const int32_t SWIPE_START_DISTANCE = 20; // Minimum movement to start swipe mode
const int32_t SWIPE_MIN_DISTANCE = 100;  // Minimum swipe distance to complete
const uint32_t SWIPE_MAX_TIME = 1000;    // Maximum swipe time (ms)/ Task handles
TaskHandle_t vncTaskHandle = nullptr;

// ============================================================================
// Function prototypes
// ============================================================================

void setupDisplay();
bool setupWiFi();
void setupVNC();
void loadConnectionProfiles();
void vncTask(void* pvParameters);
void handleTouch();
void checkMultiTouch();
void checkSwipeGesture();
void displayStatus(const String& title, const String& message, uint16_t color);
void displayConnectionInfo();
void showVNCScreen();
void showInfoScreen();
String getVNCAddress();
void pauseVNCScreen();
void resumeVNCScreen();

void setupCardKB();
uint8_t cardkb_getch();
uint32_t cardKBToKeysym(uint8_t cardKBCode);

// ============================================================================
// Setup
// ============================================================================

void setup() {
    // Set SDIO pins for ESP32-C6 WiFi module on Tab5
    // MUST be called before M5.begin()
    WiFi.setPins(SDIO2_CLK, SDIO2_CMD, SDIO2_D0, SDIO2_D1, SDIO2_D2, SDIO2_D3, SDIO2_RST);

    // Initialize M5Stack Tab5
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    cfg.clear_display = true;
    cfg.output_power = true;
    cfg.internal_imu = true;
    cfg.internal_rtc = true;
    M5.begin(cfg);
    
    Serial.begin(115200);
    Serial.println("\n=================================");
    Serial.println("M5Stack Tab5 VNC Client");
    Serial.println("=================================\n");
    
    // Setup display
    setupDisplay();
    
    // CardKB is a local configuration keyboard until the VNC task starts.
    setupCardKB();
    loadConnectionProfiles();
    do {
        while (!selectConnectionProfile(profileStore, activeProfile, activeProfileSlot,
                                        cardkb_available ? cardkb_getch : nullptr)) {
            delay(10);
        }
        Serial.printf("[Profiles] Connecting profile slot %u\n", activeProfileSlot + 1);
    } while (!setupWiFi());
    
    // Setup VNC client
    setupVNC();

    // Never let the Connect button touch become a remote mouse press.
    M5.update();
    while (M5.Touch.getCount() != 0) {
        delay(10);
        M5.update();
    }
    if (cardkb_available) {
        for (int i = 0; i < 32 && cardkb_getch() != 0; ++i) {
            delay(1);
        }
    }
    screenJustSwitched = true;

    // Create VNC task on core 0 (core 1 is used for Arduino loop)
    xTaskCreatePinnedToCore(
        vncTask,           // Task function
        "vnc_task",        // Task name
        32768,             // Stack size (bytes) - increased for VNC
        NULL,              // Task parameters
        1,                 // Priority
        &vncTaskHandle,    // Task handle
        0                  // Core ID (0 or 1)
    );
    
    Serial.println("Setup complete!");
}

// ============================================================================
// Main loop
// ============================================================================

void loop() {
    // Update M5 button states and touch
    M5.update();

    // Only this core writes preferences after startup. The network task merely
    // notifies us after a successful VNC handshake, using an atomic handoff.
    if (profileConnectedPending.exchange(false)) {
        if (!profileStore.markLastUsed(activeProfileSlot)) {
            Serial.println("[Profiles] Could not save last successful profile");
        }
    }
    
    // Check for 3-finger touch to toggle screens
    checkMultiTouch();
    
    // Check for swipe down gesture from top edge
    checkSwipeGesture();
    
    // Handle button press for reconnection
    if (M5.BtnA.wasPressed() || M5.BtnPWR.wasPressed()) {
        Serial.println("Button pressed - attempting reconnection...");
        if (vnc != nullptr && !vnc->connected()) {
            displayStatus("Reconnecting", getVNCAddress(), TFT_YELLOW);
            vnc->reconnect();
        }
    }
    
    uint8_t c;
    if(cardkb_available && (c = cardkb_getch())) {
        // 
        Serial.printf("CardKB[0x%x]:%c\n",c,c);
        uint32_t keysym = cardKBToKeysym(c);
        if (vnc != nullptr) {
            vnc->keyEvent(keysym, 1);  // press
            delay(50);  // 遅延
            vnc->keyEvent(keysym, 0);  // release
        }
    }
    // Small delay to prevent watchdog issues
    delay(10);
}

// ============================================================================
// VNC Task (runs on core 0)
// ============================================================================

void vncTask(void* pvParameters) {
    Serial.println("VNC task started on core " + String(xPortGetCoreID()));
    bool profileSuccessNotified = false;
    
    while (true) {
        // Check Wi-Fi connection
        if (WiFi.status() != WL_CONNECTED) {
            wifiConnected = false;
            displayStatus("WiFi Disconnected", "Reconnecting...", TFT_RED);
            
            // Attempt to reconnect
            WiFi.reconnect();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        wifiConnected = true;
        
        // Run VNC loop (always, even when screen is paused)
        // This maintains the VNC connection
        if (vnc != nullptr) {
            vnc->loop();
            
            if (!vnc->connected()) {
                vncConnected = false;
                if (!showingInfoScreen) {
                    displayStatus("Connecting VNC", getVNCAddress(), TFT_GREEN);
                }
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vncConnected = true;
                if (!profileSuccessNotified) {
                    profileSuccessNotified = true;
                    profileConnectedPending.store(true);
                    Serial.println("[Profiles] VNC connected successfully");
                }
                // Handle touch input when connected and screen is not paused
                if (!vncScreenPaused && !showingInfoScreen) {
                    handleTouch();
                }
            }
        }
        
        // Small delay to prevent watchdog timeout
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ============================================================================
// Display setup
// ============================================================================

void setupDisplay() {
    Serial.println("Initializing display...");
    
    // Set display brightness
    M5.Display.setBrightness(DISPLAY_BRIGHTNESS);
    
    // Set display rotation
    M5.Display.setRotation(DISPLAY_ROTATION);
    
    // Clear display
    M5.Display.fillScreen(TFT_BLACK);
    
    // Create VNC display driver
    vncDisplay = new M5GFX_VNCDriver(&M5.Display);
    
    // Display startup message
    displayStatus("M5Stack Tab5", "VNC Client Starting...", TFT_CYAN);
    
    Serial.println("Display initialized: " + 
                   String(M5.Display.width()) + "x" + 
                   String(M5.Display.height()));
}

// ============================================================================
// Wi-Fi setup
// ============================================================================

void loadConnectionProfiles() {
    if (!profileStore.load()) {
        Serial.println("[Profiles] Some settings could not be loaded; check the selection screen");
    }
    if (!profileStore.available() || profileStore.initialized() ||
        profileStore.corruptCount() != 0) {
        return;
    }

#ifdef HAS_LEGACY_CONNECTION_CONFIG
    ConnectionProfile legacy;
    auto copyField = [](char* destination, size_t capacity, const char* source) {
        if (source == nullptr || strlen(source) >= capacity) return false;
        memcpy(destination, source, strlen(source) + 1);
        return true;
    };
    strcpy(legacy.name, "Imported settings");
    legacy.vncPort = VNC_PORT;
    bool fits = copyField(legacy.ssid, sizeof(legacy.ssid), WIFI_SSID) &&
                copyField(legacy.wifiPassword, sizeof(legacy.wifiPassword), WIFI_PASSWORD) &&
                copyField(legacy.vncHost, sizeof(legacy.vncHost), VNC_HOST) &&
                copyField(legacy.vncPassword, sizeof(legacy.vncPassword), VNC_PASSWORD) &&
                VNC_PORT > 0 && static_cast<uint32_t>(VNC_PORT) <= UINT16_MAX;
    char error[128];
    if (fits && validateConnectionProfile(legacy, error, sizeof(error))) {
        int slot = profileStore.findFreeSlot();
        if (slot >= 0 && !profileStore.save(static_cast<uint8_t>(slot), legacy)) {
            Serial.println("[Profiles] Legacy import could not be saved; create a profile on screen");
            return;
        }
        Serial.println("[Profiles] Imported legacy settings (credentials not logged)");
    } else {
        Serial.println("[Profiles] Legacy settings are invalid or too long; create a profile on screen");
    }
#endif
    if (!profileStore.markInitialized()) {
        Serial.println("[Profiles] Could not save initialization state");
    }
}

bool setupWiFi() {
    // This entire UI runs before vncTask exists, so no display/network owner
    // can race with a Cancel, Retry or profile edit operation.
    const int buttonY = M5.Display.height() - 100;
    const int buttonWidth = 280;
    const int buttonHeight = 60;
    const int chooseX = M5.Display.width() / 2 + 20;
    const int retryX = M5.Display.width() / 2 - buttonWidth - 20;
    auto drawButton = [&](int x, const char* text) {
        M5.Display.fillRoundRect(x, buttonY, buttonWidth, buttonHeight, 10, TFT_NAVY);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.setFont(&fonts::FreeSans12pt7b);
        M5.Display.drawString(text, x + buttonWidth / 2, buttonY + buttonHeight / 2);
    };
    auto drawScreen = [&](bool failed) {
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextSize(1);
        M5.Display.setTextDatum(MC_DATUM);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(failed ? TFT_RED : TFT_YELLOW);
        M5.Display.drawString(failed ? "WiFi connection failed" : "Connecting WiFi",
                              M5.Display.width() / 2, M5.Display.height() / 2 - 70);
        M5.Display.setFont(&fonts::FreeSans12pt7b);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString(activeProfile.ssid, M5.Display.width() / 2,
                              M5.Display.height() / 2);
        if (failed) drawButton(retryX, "Retry");
        drawButton(chooseX, "Choose profile");
    };

    // A click needs a single-finger press and release in the same button.
    // Multitouch or dragging outside the pressed button cancels the click.
    int pressedButton = 0;
    bool touchWasDown = false;
    auto pollAction = [&]() {
        M5.update();
        if (cardkb_available) cardkb_getch();  // Discard local startup input.
        // getCount() can include a released contact for one update frame.
        // Counting only active contacts avoids losing that button release.
        int count = 0;
        auto touch = M5.Touch.getDetail();
        for (uint8_t i = 0; i < M5.Touch.getCount(); ++i) {
            auto detail = M5.Touch.getDetail(i);
            if (detail.isPressed()) {
                ++count;
                touch = detail;
            }
        }
        auto hit = [&](int x, int y) {
            if (y < buttonY || y >= buttonY + buttonHeight) return 0;
            if (x >= chooseX && x < chooseX + buttonWidth) return 2;
            if (x >= retryX && x < retryX + buttonWidth) return 1;
            return 0;
        };
        int result = 0;
        if (count == 1 && touch.isPressed()) {
            int current = hit(touch.x, touch.y);
            if (!touchWasDown) pressedButton = current;
            else if (current != pressedButton) pressedButton = 0;
            touchWasDown = true;
        } else if (count == 0) {
            if (touchWasDown) result = pressedButton;
            touchWasDown = false;
            pressedButton = 0;
        } else {
            pressedButton = 0;
            touchWasDown = true;
        }
        return result;
    };

    WiFi.mode(WIFI_STA);
    // Credentials live in our own NVS records, not in another implicit WiFi
    // persistence store. Do not erase the framework's NVS partition.
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    while (true) {
        wifiConnected = false;
        pressedButton = 0;
        touchWasDown = false;
        drawScreen(false);
        Serial.println("[WiFi] Connecting selected profile");
        WiFi.begin(activeProfile.ssid, activeProfile.wifiPassword);
        uint32_t started = millis();
        bool cancelled = false;
        while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
            if (pollAction() == 2) {
                cancelled = true;
                break;
            }
            delay(10);
        }
        if (!cancelled && WiFi.status() == WL_CONNECTED) {
            wifiConnected = true;
            WiFi.setAutoReconnect(true);
            Serial.println("[WiFi] Connected, local IP: " + WiFi.localIP().toString());
            return true;
        }
        WiFi.disconnect(false, false);
        if (cancelled) {
            Serial.println("[WiFi] Connection cancelled; returning to profile selection");
            return false;
        }
        Serial.println("[WiFi] Connection timed out");
        // Consume a touch still held during the timeout before showing buttons.
        M5.update();
        while (M5.Touch.getCount() != 0) {
            delay(10);
            M5.update();
        }
        pressedButton = 0;
        touchWasDown = false;
        drawScreen(true);
        int action = 0;
        while (action == 0) {
            action = pollAction();
            delay(10);
        }
        if (action == 2) return false;
    }
}

// ============================================================================
// VNC setup
// ============================================================================

void setupVNC() {
    Serial.println("Initializing VNC client...");
    Serial.println("Server: " + getVNCAddress());
    displayStatus("Connecting VNC", getVNCAddress(), TFT_GREEN);
    
    // Create VNC client with display driver
    // Using new to allocate on heap instead of stack
    vnc = new arduinoVNC(vncDisplay);
    
    // Configure VNC connection
    vnc->begin(activeProfile.vncHost, activeProfile.vncPort);
    vnc->setPassword(activeProfile.vncPassword);
    Serial.println("VNC client initialized");
}

// ============================================================================
// Multi-touch detection for screen switching
// ============================================================================

void checkMultiTouch() {
    uint8_t touchCount = M5.Touch.getCount();
    
    // Detect 3-finger touch
    if (touchCount >= 3) {
        uint32_t now = millis();
        
        // Debounce: ignore rapid repeated touches
        if (now - lastThreeTouchTime > THREE_TOUCH_DEBOUNCE) {
            lastThreeTouchTime = now;
            
            if (showingInfoScreen) {
                // If on info screen, return to VNC screen
                screenJustSwitched = true;
                showVNCScreen();
            } else {
                // If on VNC screen, force full screen refresh
                if (vnc != nullptr && vnc->connected()) {
                    Serial.println("[checkMultiTouch] Forcing full screen refresh");
                    vnc->forceFullUpdate();
                }
            }
        }
    }
}

// ============================================================================
// Swipe gesture detection for screen switching
// ============================================================================

void checkSwipeGesture() {
    auto touch = M5.Touch.getDetail();
    uint8_t touchCount = M5.Touch.getCount();
    
    // Only process single touch for swipe
    if (touchCount != 1) {
        if (swipeInProgress || swipePotential) {
            Serial.printf("[checkSwipeGesture] Swipe cancelled - touch count: %d\n", touchCount);
        }
        swipeInProgress = false;
        swipePotential = false;
        return;
    }
    
    if (touch.isPressed()) {
        if (!swipePotential && !swipeInProgress) {
            // Check if touch started at top edge
            if (touch.y <= SWIPE_TOP_THRESHOLD) {
                // Mark as potential swipe, but don't block normal touch yet
                swipePotential = true;
                swipeStartX = touch.x;
                swipeStartY = touch.y;
                swipeStartTime = millis();
                Serial.println("[checkSwipeGesture] Potential swipe detected at top edge");
            }
        } else if (swipePotential && !swipeInProgress) {
            // Check if finger moved down enough to confirm swipe intent
            int32_t deltaY = touch.y - swipeStartY;
            int32_t deltaX = abs(touch.x - swipeStartX);
            
            if (deltaY >= SWIPE_START_DISTANCE && deltaX < SWIPE_START_DISTANCE) {
                // Confirmed as swipe - enter swipe mode
                swipeInProgress = true;
                swipePotential = false;
                Serial.println("[checkSwipeGesture] Swipe mode activated");
                
                // Release mouse button to prevent drag during swipe
                if (vnc != nullptr && wasTouched) {
                    vnc->mouseEvent(lastTouchX, lastTouchY, 0b000);
                    wasTouched = false;
                }
            } else if (millis() - swipeStartTime > SWIPE_MAX_TIME) {
                // Timeout - not a swipe, allow normal touch
                swipePotential = false;
                Serial.println("[checkSwipeGesture] Potential swipe timeout - allowing normal touch");
            }
        } else if (swipeInProgress) {
            // Track swipe progress
            int32_t deltaY = touch.y - swipeStartY;
            int32_t deltaX = abs(touch.x - swipeStartX);
            uint32_t swipeTime = millis() - swipeStartTime;
            
            // Check if swipe is valid (downward, not too horizontal, within time limit)
            if (deltaY >= SWIPE_MIN_DISTANCE && 
                deltaX < SWIPE_MIN_DISTANCE && 
                swipeTime < SWIPE_MAX_TIME) {
                Serial.println("[checkSwipeGesture] Swipe completed");
                
                // Mouse button already released at swipe start
                // Set flag to prevent handleTouch from re-triggering
                screenJustSwitched = true;
                
                showInfoScreen();
                swipeInProgress = false;
            } else if (swipeTime >= SWIPE_MAX_TIME) {
                // Timeout - cancel swipe
                swipeInProgress = false;
            }
        }
    } else {
        // Touch released
        if (swipeInProgress) {
            Serial.println("[checkSwipeGesture] Swipe cancelled - touch released");
        }
        if (swipePotential) {
            Serial.println("[checkSwipeGesture] Potential swipe cancelled - touch released");
        }
        swipeInProgress = false;
        swipePotential = false;
    }
}

// ============================================================================
// Screen switching functions
// ============================================================================

void showInfoScreen() {
    Serial.println("[showInfoScreen] Entering info screen");
    
    // Release any active mouse button to prevent unwanted selection
    if (vnc != nullptr && wasTouched) {
        vnc->mouseEvent(lastTouchX, lastTouchY, 0b000);  // Release all buttons
        wasTouched = false;
    }
    
    // Also reset two-finger scroll state
    if (twoFingerScrollActive) {
        twoFingerScrollActive = false;
    }
    
    // First pause VNC drawing to prevent interference
    pauseVNCScreen();
    
    // Small delay to ensure VNC drawing has stopped
    delay(50);
    
    // Set flag after pausing to ensure VNC task sees it
    showingInfoScreen = true;
    
    // Display connection information
    displayConnectionInfo();
}

void showVNCScreen() {
    Serial.println("[showVNCScreen] Returning to VNC screen");
    
    // First, set flag to stop info screen display
    showingInfoScreen = false;
    
    // Clear the screen to remove info screen content
    M5.Display.fillScreen(TFT_BLACK);
    
    // Small delay to ensure screen clear is complete
    delay(50);
    
    // Resume VNC drawing
    resumeVNCScreen();
}

// ============================================================================
// Connection information display
// ============================================================================

void displayConnectionInfo() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextSize(1);
    
    // Title bar - adjusted height and position
    M5.Display.fillRect(0, 0, M5.Display.width(), 70, TFT_NAVY);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);  // Smaller font for title
    M5.Display.drawString("Connection Info", M5.Display.width() / 2, 35);

    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.setTextColor(TFT_LIGHTGREY);
    String profileName(activeProfile.name);
    while (profileName.length() > 0 &&
           M5.Display.textWidth(profileName + "...") > M5.Display.width() - 80) {
        profileName.remove(profileName.length() - 1);
    }
    if (profileName != activeProfile.name) profileName += "...";
    M5.Display.drawString(profileName, M5.Display.width() / 2, 96);
    
    // Content area - adjusted starting position and spacing
    int y = 145;  // Leave room for the active profile name below the title
    int lineHeight = 90;  // Increased line height for better spacing
    
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);  // Smaller font for labels
    M5.Display.setTextDatum(ML_DATUM);
    
    // WiFi SSID
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.drawString("WiFi Network", 40, y);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setFont(&fonts::FreeSans12pt7b);  // Smaller font for values
    M5.Display.drawString(String(activeProfile.ssid), 40, y + 40);
    
    y += lineHeight;
    
    // VNC Server IP
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_GREEN);
    M5.Display.drawString("VNC Server", 40, y);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    String host(activeProfile.vncHost);
    while (host.length() > 0 && M5.Display.textWidth(host + "...") > M5.Display.width() - 80) {
        host.remove(host.length() - 1);
    }
    if (host != activeProfile.vncHost) host += "...";
    M5.Display.drawString(host, 40, y + 40);
    
    y += lineHeight;
    
    // VNC Port
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_YELLOW);
    M5.Display.drawString("Port", 40, y);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString(String(activeProfile.vncPort), 40, y + 40);
    
    // Status indicators - adjusted position
    y += lineHeight + 20;
    M5.Display.setFont(&fonts::FreeSans9pt7b);  // Smaller font for status
    M5.Display.setTextDatum(MC_DATUM);
    
    // WiFi status
    if (wifiConnected) {
        M5.Display.fillCircle(M5.Display.width() / 4, y, 12, TFT_GREEN);
        M5.Display.setTextColor(TFT_GREEN);
        M5.Display.drawString("WiFi OK", M5.Display.width() / 4, y + 28);
    } else {
        M5.Display.fillCircle(M5.Display.width() / 4, y, 12, TFT_RED);
        M5.Display.setTextColor(TFT_RED);
        M5.Display.drawString("WiFi Disconnected", M5.Display.width() / 4, y + 28);
    }
    
    // VNC status
    if (vncConnected) {
        M5.Display.fillCircle(M5.Display.width() * 3 / 4, y, 12, TFT_GREEN);
        M5.Display.setTextColor(TFT_GREEN);
        M5.Display.drawString("VNC Connected", M5.Display.width() * 3 / 4, y + 28);
    } else {
        M5.Display.fillCircle(M5.Display.width() * 3 / 4, y, 12, TFT_RED);
        M5.Display.setTextColor(TFT_RED);
        M5.Display.drawString("VNC Disconnected", M5.Display.width() * 3 / 4, y + 28);
    }
    
    // Footer instruction
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.setTextColor(TFT_LIGHTGREY);
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.drawString("Touch with 3 fingers to return to VNC", 
                          M5.Display.width() / 2, M5.Display.height() - 30);
}

// ============================================================================
// Screen control methods (for switching between VNC and other screens)
// ============================================================================

void pauseVNCScreen() {
    if (vncDisplay != nullptr) {
        vncDisplay->setPaused(true);
        vncScreenPaused = true;
        Serial.println("VNC screen paused - drawing disabled");
    }
}

void resumeVNCScreen() {
    if (vncDisplay != nullptr) {
        // First resume drawing capability
        vncDisplay->setPaused(false);
        vncScreenPaused = false;
        Serial.println("VNC screen resumed - drawing enabled");
        
        // Request full screen update from VNC server
        if (vnc != nullptr && vnc->connected()) {
            // Force immediate full update
            vnc->forceFullUpdate();
            Serial.println("Requested full screen update from VNC server");
            
            // Give VNC a moment to start processing the update
            delay(100);
        }
    }
}

// ===============================
// CardKB
// ================================

uint32_t cardKBToKeysym(uint8_t cardKBCode) {
    switch (cardKBCode) {
        // 標準ASCII文字はそのまま返す（例: 'a' = 0x61）
        case 0x20 ... 0x7E:  // スペースから~までの printable ASCII
            return cardKBCode;

        // 特殊キー: CardKBの値 → X11 keysym
        case 0x0A:  // Line Feed (一部のEnter)
        case 0x0D:  // Carriage Return (Enter)
            return 0xff0d;  //XK_Return;  // 0xff0d

        case 0x08:  // Backspace
            return 0xff08; //XK_BackSpace;  // 0xff08

        case 0x1B:  // Escape
            return 0xff1b; //XK_Escape;  // 0xff1b

        case 0x09:  // Tab
            return 0xff09; //XK_Tab;  // 0xff09

        // 矢印キー: CardKBの実際のコードに合わせて調整（ドキュメント確認: 例として仮定）
        case 0xb4:  // Left arrow (仮: CardKBの値)
            return 0xff51; // XK_Left;  // 0xff51
        case 0xb7:  // Right arrow
            return 0xff53; // XK_Right;  // 0xff53
        case 0xb5:  // Up arrow
            return 0xff52; // XK_Up;  // 0xff52
        case 0xb6:  // Down arrow
            return 0xff54; // XK_Down;  // 0xff54

        // Delete
        case 0xff:  // DEL
            return 0xffff; // XK_Delete;  // 0xffff

        // Function keys (F1-F12): CardKBにない場合無視、またはカスタム
        // 例: Fn + 1 = F1
        // case 0xXX: return XK_F1;  // 0xffbe

        // 未知のコード: 0を返すか、無視
        default:
            return 0;  // 無効
    }
}

void setupCardKB() {
    Wire.begin();
    Wire.beginTransmission(cardkb_addr);
    byte r = Wire.endTransmission();
    if (r == 0) {
        Serial.println("CardKB available");
        cardkb_available=true;

    }
}

uint8_t cardkb_getch(){
    uint8_t r = 0;
    Wire.requestFrom(cardkb_addr,1);
    if(Wire.available()) {
        r = Wire.read();
    }
    return r;
}


// ============================================================================
// Touch handling
// ============================================================================

void handleTouch() {
    if (vnc == nullptr) return;
    
    // Get touch count and state
    uint8_t touchCount = M5.Touch.getCount();
    auto touch = M5.Touch.getDetail();
    
    //    // Skip touch handling during swipe gesture
    if (swipeInProgress) {
        Serial.println("[handleTouch] SKIPPED - swipe in progress");
        return;
    }
    
    // Check screenJustSwitched flag first
    // This prevents re-triggering mouse events after screen transitions
    if (screenJustSwitched) {        // Wait for touch to be released before resuming normal touch handling
        if (touchCount == 0) {
            screenJustSwitched = false;
            Serial.println("[handleTouch] Screen switch flag cleared - resuming normal touch");
        } else {
            Serial.printf("[handleTouch] SKIPPED - screen just switched (touchCount=%d)\n", touchCount);
        }
        return;
    }
    
    // (Touch count logging removed for cleaner output)
    
    // Handle two-finger scroll
    if (touchCount == 2) {
        if (!twoFingerScrollActive) {
            // Start two-finger scroll
            twoFingerScrollActive = true;
            scrollStartY = touch.y;
            lastScrollY = touch.y;
            
            // Release any active single-touch drag
            if (wasTouched) {
                vnc->mouseEvent(lastTouchX, lastTouchY, 0b000);
                wasTouched = false;
            }
            
            Serial.println("Two-finger scroll started");
        } else {
            // Continue two-finger scroll
            int32_t deltaY = lastScrollY - touch.y;  // Inverted for natural scroll
            uint32_t now = millis();
            
            // Rate limiting: only send scroll event if enough time has passed
            if (abs(deltaY) >= SCROLL_THRESHOLD && (now - lastScrollTime) >= SCROLL_MIN_INTERVAL) {
                // Send only ONE scroll event per threshold crossing
                if (deltaY > 0) {
                    // Scroll up (wheel up)
                    vnc->mouseEvent(touch.x, touch.y, 0b01000);
                    delay(50);  // Longer delay for stability
                    vnc->mouseEvent(touch.x, touch.y, 0b00000);
                    Serial.println("Scroll: UP");
                } else {
                    // Scroll down (wheel down)
                    vnc->mouseEvent(touch.x, touch.y, 0b10000);
                    delay(50);  // Longer delay for stability
                    vnc->mouseEvent(touch.x, touch.y, 0b00000);
                    Serial.println("Scroll: DOWN");
                }
                
                lastScrollY = touch.y;
                lastScrollTime = now;
            }
        }
        return;
    }
    
    // Reset two-finger scroll when not exactly 2 touches
    if (twoFingerScrollActive) {
        twoFingerScrollActive = false;
        Serial.println("Two-finger scroll ended");
    }
    
    // Handle single-touch (normal mouse operation)
    if (touchCount == 1 && touch.isPressed()) {
        // Touch is active
        int32_t x = touch.x;
        int32_t y = touch.y;
        
        // Scale touch coordinates if display is rotated or scaled
        // Tab5 display is 1280x720, touch coordinates should match
        
        if (!wasTouched || x != lastTouchX || y != lastTouchY) {
            // Send mouse move with button pressed
            vnc->mouseEvent(x, y, 0b001);  // Left button pressed
            
            lastTouchX = x;
            lastTouchY = y;
            wasTouched = true;
        }
    } else if (wasTouched) {
        // Touch was released
        vnc->mouseEvent(lastTouchX, lastTouchY, 0b000);  // No buttons pressed
        wasTouched = false;
    }
}

// ============================================================================
// Helper functions
// ============================================================================

void displayStatus(const String& title, const String& message, uint16_t color) {
    if (vncDisplay != nullptr) {
        vncDisplay->printScreen(title, message, color);
    }
}

String getVNCAddress() {
    return String(activeProfile.vncHost) + ":" + String(activeProfile.vncPort);
}
