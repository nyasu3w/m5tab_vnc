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

// Connection state
bool wifiConnected = false;
bool vncConnected = false;

// Screen state
bool vncScreenPaused = false;
bool showingInfoScreen = false;

// Multi-touch detection
uint32_t lastThreeTouchTime = 0;
const uint32_t THREE_TOUCH_DEBOUNCE = 500;  // 500ms debounce

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
            
            // Toggle between VNC screen and info screen
            if (showingInfoScreen) {
                showVNCScreen();
            } else {
                showInfoScreen();
            }
        }
    }
}

// ============================================================================
// Screen switching functions
// ============================================================================

void showInfoScreen() {
    Serial.println("Switching to connection info screen");
    
    // Pause VNC drawing
    pauseVNCScreen();
    showingInfoScreen = true;
    
    // Display connection information
    displayConnectionInfo();
}

void showVNCScreen() {
    Serial.println("Switching to VNC screen");
    
    showingInfoScreen = false;
    
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
    int lineHeight = 80;  // Reduced line height for better fit
    
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);  // Smaller font for labels
    M5.Display.setTextDatum(ML_DATUM);
    
    // WiFi SSID
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.drawString("WiFi Network", 40, y);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setFont(&fonts::FreeSans12pt7b);  // Smaller font for values
    M5.Display.drawString(String(WIFI_SSID), 40, y + 35);
    
    y += lineHeight;
    
    // VNC Server IP
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_GREEN);
    M5.Display.drawString("VNC Server", 40, y);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString(String(VNC_HOST), 40, y + 35);
    
    y += lineHeight;
    
    // VNC Port
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_YELLOW);
    M5.Display.drawString("Port", 40, y);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString(String(VNC_PORT), 40, y + 35);
    
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
        vncDisplay->setPaused(false);
        vncScreenPaused = false;
        Serial.println("VNC screen resumed - drawing enabled");
        
        // Request full screen update from VNC server
        if (vnc != nullptr && vnc->connected()) {
            vnc->forceFullUpdate();
            Serial.println("Requested full screen update from VNC server");
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
    
    // Get touch state from M5Unified
    auto touch = M5.Touch.getDetail();
    
    if (touch.isPressed()) {
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
