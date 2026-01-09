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

// ============================================================================
// Configuration - Modify these settings for your environment
// ============================================================================

#define CREDENTIAL_SEPARATED  // to use the following defs, comment this line out
#ifdef CREDENTIAL_SEPARATED
#  include "user_setting.h"
#else
  // Wi-Fi credentials
  const char* WIFI_SSID = "your-ssid";           // Your Wi-Fi network name
  const char* WIFI_PASSWORD = "your-password";    // Your Wi-Fi password

  // VNC server settings
  const char* VNC_HOST = "192.168.1.100";        // VNC server IP address
  const uint16_t VNC_PORT = 5900;                 // VNC server port (default: 5900)
  const char* VNC_PASSWORD = "the_password"; // VNC server password
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

// Swipe down detection
bool swipeInProgress = false;
int32_t swipeStartY = 0;
int32_t swipeStartX = 0;
uint32_t swipeStartTime = 0;
const int32_t SWIPE_TOP_THRESHOLD = 50;  // Top edge detection threshold
const int32_t SWIPE_MIN_DISTANCE = 100;  // Minimum swipe distance
const uint32_t SWIPE_MAX_TIME = 1000;    // Maximum swipe time (ms)

// Task handles
TaskHandle_t vncTaskHandle = nullptr;

// ============================================================================
// Function prototypes
// ============================================================================

void setupDisplay();
void setupWiFi();
void setupVNC();
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
    
    // Setup Wi-Fi connection
    setupWiFi();
    
    // Setup VNC client
    setupVNC();
    
    // setup CardKB if available
    setupCardKB();

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

void setupWiFi() {
    Serial.println("Connecting to Wi-Fi...");
    Serial.println("SSID: " + String(WIFI_SSID));
    
    displayStatus("Connecting WiFi", String(WIFI_SSID), TFT_YELLOW);
    
    // Set Wi-Fi mode to station
    WiFi.mode(WIFI_STA);
    
   
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    // Wait for connection with timeout
    int attempts = 0;
    const int maxAttempts = 30;
    
    while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
        delay(500);
        Serial.print(".");
        if (vncDisplay != nullptr) {
            vncDisplay->print(".");
        }
        attempts++;
    }
    
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.println("WiFi connected!");
        Serial.println("IP address: " + WiFi.localIP().toString());
        Serial.println("Signal strength: " + String(WiFi.RSSI()) + " dBm");
        
        displayStatus("WiFi Connected", WiFi.localIP().toString(), TFT_GREEN);
        delay(1500);
    } else {
        Serial.println("WiFi connection failed!");
        displayStatus("WiFi Failed", "Check credentials", TFT_RED);
        delay(3000);
    }
}

// ============================================================================
// VNC setup
// ============================================================================

void setupVNC() {
    Serial.println("Initializing VNC client...");
    Serial.println("Server: " + String(VNC_HOST) + ":" + String(VNC_PORT));
    
    // Create VNC client with display driver
    // Using new to allocate on heap instead of stack
    vnc = new arduinoVNC(vncDisplay);
    
    // Configure VNC connection
    vnc->begin(VNC_HOST, VNC_PORT);
    vnc->setPassword(VNC_PASSWORD);
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
        if (swipeInProgress) {
            Serial.printf("[checkSwipeGesture] Swipe cancelled - touch count: %d\n", touchCount);
        }
        swipeInProgress = false;
        return;
    }
    
    if (touch.isPressed()) {
        if (!swipeInProgress) {
            // Check if touch started at top edge
            if (touch.y <= SWIPE_TOP_THRESHOLD) {
                swipeInProgress = true;
                swipeStartX = touch.x;
                swipeStartY = touch.y;
                swipeStartTime = millis();
                Serial.println("[checkSwipeGesture] Swipe started");
                
                // Immediately release mouse button to prevent drag during swipe
                if (vnc != nullptr && wasTouched) {
                    vnc->mouseEvent(lastTouchX, lastTouchY, 0b000);
                    wasTouched = false;
                }
            }
        } else {
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
        swipeInProgress = false;
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
    
    // Title bar - adjusted height and position
    M5.Display.fillRect(0, 0, M5.Display.width(), 70, TFT_NAVY);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);  // Smaller font for title
    M5.Display.drawString("Connection Info", M5.Display.width() / 2, 35);
    
    // Content area - adjusted starting position and spacing
    int y = 110;  // Start lower to avoid overlap with title
    int lineHeight = 90;  // Increased line height for better spacing
    
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);  // Smaller font for labels
    M5.Display.setTextDatum(ML_DATUM);
    
    // WiFi SSID
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.drawString("WiFi Network", 40, y);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setFont(&fonts::FreeSans12pt7b);  // Smaller font for values
    M5.Display.drawString(String(WIFI_SSID), 40, y + 40);
    
    y += lineHeight;
    
    // VNC Server IP
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_GREEN);
    M5.Display.drawString("VNC Server", 40, y);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString(String(VNC_HOST), 40, y + 40);
    
    y += lineHeight;
    
    // VNC Port
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_YELLOW);
    M5.Display.drawString("Port", 40, y);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString(String(VNC_PORT), 40, y + 40);
    
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
    return String(VNC_HOST) + ":" + String(VNC_PORT);
}
