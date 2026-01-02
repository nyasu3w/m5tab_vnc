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
//#include "VNC_user_config.h"  // Must be included before VNC.h
#include <VNC.h>
#include "M5GFX_VNCDriver.h"

// ============================================================================
// Configuration - Modify these settings for your environment
// ============================================================================

//#define CREDENTIAL_SEPARATED  // to use the following defs, comment this line out
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
const uint8_t DISPLAY_ROTATION = 1;             // Display rotation (0-3)

// ESP32-P4 Tab5 SDIO2 pins for WiFi (ESP32-C6)
#define SDIO2_CLK GPIO_NUM_12
#define SDIO2_CMD GPIO_NUM_13
#define SDIO2_D0  GPIO_NUM_11
#define SDIO2_D1  GPIO_NUM_10
#define SDIO2_D2  GPIO_NUM_9
#define SDIO2_D3  GPIO_NUM_8
#define SDIO2_RST GPIO_NUM_15

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
void displayStatus(const String& title, const String& message, uint16_t color);
String getVNCAddress();

// ============================================================================
// Setup
// ============================================================================

void setup() {
    // Set SDIO pins for ESP32-C6 WiFi module on Tab5
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
    // Update M5 button states
    M5.update();
    
    // Handle button press for reconnection
    if (M5.BtnA.wasPressed() || M5.BtnPWR.wasPressed()) {
        Serial.println("Button pressed - attempting reconnection...");
        if (vnc != nullptr && !vnc->connected()) {
            displayStatus("Reconnecting", getVNCAddress(), TFT_YELLOW);
            vnc->reconnect();
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
        
        // Run VNC loop
        if (vnc != nullptr) {
            vnc->loop();
            
            if (!vnc->connected()) {
                vncConnected = false;
                displayStatus("Connecting VNC", getVNCAddress(), TFT_GREEN);
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vncConnected = true;
                // Handle touch input when connected
                handleTouch();
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
