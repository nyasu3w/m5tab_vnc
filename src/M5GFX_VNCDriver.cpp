/**
 * @file M5GFX_VNCDriver.cpp
 * @brief VNC Display Driver implementation for M5Stack Tab5 using M5GFX
 */

//#include "VNC_user_config.h"  // Must be included before VNC.h
#include "M5GFX_VNCDriver.h"

#ifdef ESP32

M5GFX_VNCDriver::M5GFX_VNCDriver(M5GFX* gfx) 
    : _gfx(gfx)
    , _updateX(0)
    , _updateY(0)
    , _updateW(0)
    , _updateH(0)
    , _pixelCount(0)
{
}

M5GFX_VNCDriver::~M5GFX_VNCDriver() {
}

bool M5GFX_VNCDriver::hasCopyRect(void) {
    // M5GFX supports copy rectangle operation
    return true;
}

uint32_t M5GFX_VNCDriver::getHeight(void) {
    return _gfx->height();
}

uint32_t M5GFX_VNCDriver::getWidth(void) {
    return _gfx->width();
}

void M5GFX_VNCDriver::draw_area(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* data) {
    // Draw raw RGB565 pixel data to the display
    // The data is in big-endian RGB565 format from VNC
    _gfx->startWrite();
    _gfx->setAddrWindow(x, y, w, h);
    
    // Push pixels - data is RGB565 big-endian, need to swap bytes
    uint16_t* pixels = (uint16_t*)data;
    uint32_t pixelCount = w * h;
    
    for (uint32_t i = 0; i < pixelCount; i++) {
        // Swap bytes for correct color display (VNC sends big-endian)
        uint16_t color = pixels[i];
        color = (color >> 8) | (color << 8);
        _gfx->writePixel(x + (i % w), y + (i / w), color);
    }
    
    _gfx->endWrite();
}

void M5GFX_VNCDriver::draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t color) {
    // Swap bytes for correct color (VNC sends big-endian RGB565)
    uint16_t swappedColor = (color >> 8) | (color << 8);
    _gfx->fillRect(x, y, w, h, swappedColor);
}

void M5GFX_VNCDriver::copy_rect(uint32_t src_x, uint32_t src_y, uint32_t dest_x, uint32_t dest_y, uint32_t w, uint32_t h) {
    // Use M5GFX's copy functionality
    // Note: This requires reading from the display, which may not be supported on all displays
    
    // Create a temporary buffer for the copy operation
    uint16_t* buffer = (uint16_t*)heap_caps_malloc(w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (buffer != nullptr) {
        // Read source area
        _gfx->readRect(src_x, src_y, w, h, buffer);
        // Write to destination
        _gfx->pushImage(dest_x, dest_y, w, h, buffer);
        // Free buffer
        heap_caps_free(buffer);
    } else {
        // Fallback: If no PSRAM available, use smaller chunks
        // This is slower but works with limited memory
        for (uint32_t row = 0; row < h; row++) {
            uint16_t lineBuffer[w];
            _gfx->readRect(src_x, src_y + row, w, 1, lineBuffer);
            _gfx->pushImage(dest_x, dest_y + row, w, 1, lineBuffer);
        }
    }
}

void M5GFX_VNCDriver::area_update_start(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    _updateX = x;
    _updateY = y;
    _updateW = w;
    _updateH = h;
    _pixelCount = 0;
    
    _gfx->startWrite();
    _gfx->setAddrWindow(x, y, w, h);
}

void M5GFX_VNCDriver::area_update_data(char* data, uint32_t pixel) {
    // Write pixel data during streaming update
    uint16_t* pixels = (uint16_t*)data;
    
    for (uint32_t i = 0; i < pixel; i++) {
        // Calculate position
        uint32_t pos = _pixelCount + i;
        uint32_t px = _updateX + (pos % _updateW);
        uint32_t py = _updateY + (pos / _updateW);
        
        // Swap bytes for correct color
        uint16_t color = pixels[i];
        color = (color >> 8) | (color << 8);
        
        _gfx->writePixel(px, py, color);
    }
    
    _pixelCount += pixel;
}

void M5GFX_VNCDriver::area_update_end(void) {
    _gfx->endWrite();
    _pixelCount = 0;
}

void M5GFX_VNCDriver::vnc_options_override(dfb_vnc_options* opt) {
    // Override VNC options for optimal performance on Tab5
    // Tab5 has a 1280x720 display, but we may want to request a smaller
    // resolution from the VNC server for better performance
    
    // Uncomment to force a specific resolution from the server
    // opt->client.width = 1280;
    // opt->client.height = 720;
}

void M5GFX_VNCDriver::printScreen(const String& title, const String& msg, uint16_t color) {
    _gfx->fillScreen(TFT_BLACK);
    _gfx->setTextColor(color);
    _gfx->setTextSize(2);
    
    // Calculate center position
    int16_t titleX = (_gfx->width() - title.length() * 12) / 2;
    int16_t msgX = (_gfx->width() - msg.length() * 12) / 2;
    int16_t centerY = _gfx->height() / 2;
    
    _gfx->setCursor(titleX > 0 ? titleX : 10, centerY - 30);
    _gfx->println(title);
    
    _gfx->setCursor(msgX > 0 ? msgX : 10, centerY + 10);
    _gfx->println(msg);
}

void M5GFX_VNCDriver::print(const String& text) {
    _gfx->print(text);
}

void M5GFX_VNCDriver::clear(uint16_t color) {
    _gfx->fillScreen(color);
}

#endif // ESP32
