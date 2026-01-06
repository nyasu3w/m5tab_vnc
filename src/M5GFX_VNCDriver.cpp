/**
 * @file M5GFX_VNCDriver.cpp
 * @brief VNC Display Driver implementation for M5Stack Tab5 using M5GFX
 */

#include "VNC_user_config.h"  // Must be included before VNC.h
#include "M5GFX_VNCDriver.h"

#ifdef ESP32

M5GFX_VNCDriver::M5GFX_VNCDriver(M5GFX* gfx) 
    : _gfx(gfx)
    , _isPaused(false)
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
    return true;
}

uint32_t M5GFX_VNCDriver::getHeight(void) {
    return _gfx->height();
}

uint32_t M5GFX_VNCDriver::getWidth(void) {
    return _gfx->width();
}

void M5GFX_VNCDriver::draw_area(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* data) {
    if (_isPaused) return;

    _gfx->startWrite();
    _gfx->setAddrWindow(x, y, w, h);
    
    uint16_t* pixels = (uint16_t*)data;
    uint32_t pixelCount = w * h;
    
    for (uint32_t i = 0; i < pixelCount; i++) {
        uint16_t color = pixels[i];
        color = (color >> 8) | (color << 8);
        _gfx->writePixel(x + (i % w), y + (i / w), color);
    }
    
    _gfx->endWrite();
}

void M5GFX_VNCDriver::draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t color) {
    if (_isPaused) return;
    uint16_t swappedColor = (color >> 8) | (color << 8);
    _gfx->fillRect(x, y, w, h, swappedColor);
}

void M5GFX_VNCDriver::copy_rect(uint32_t src_x, uint32_t src_y, uint32_t dest_x, uint32_t dest_y, uint32_t w, uint32_t h) {
    if (_isPaused) return;
    
    uint16_t* buffer = (uint16_t*)heap_caps_malloc(w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (buffer != nullptr) {
        _gfx->readRect(src_x, src_y, w, h, buffer);
        _gfx->pushImage(dest_x, dest_y, w, h, buffer);
        heap_caps_free(buffer);
    } else {
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
    
    if (!_isPaused) {
        _gfx->startWrite();
        _gfx->setAddrWindow(x, y, w, h);
    }
}

void M5GFX_VNCDriver::area_update_data(char* data, uint32_t pixel) {
    if (_isPaused) {
        _pixelCount += pixel;
        return;
    }
    
    uint16_t* pixels = (uint16_t*)data;
    
    for (uint32_t i = 0; i < pixel; i++) {
        uint32_t pos = _pixelCount + i;
        uint32_t px = _updateX + (pos % _updateW);
        uint32_t py = _updateY + (pos / _updateW);
        
        uint16_t color = pixels[i];
        color = (color >> 8) | (color << 8);
        
        _gfx->writePixel(px, py, color);
    }
    
    _pixelCount += pixel;
}

void M5GFX_VNCDriver::area_update_end(void) {
    if (!_isPaused) {
        _gfx->endWrite();
    }
    _pixelCount = 0;
}

void M5GFX_VNCDriver::vnc_options_override(dfb_vnc_options* opt) {
    // Override VNC options for optimal performance on Tab5
}

void M5GFX_VNCDriver::printScreen(const String& title, const String& msg, uint16_t color) {
    if (_isPaused) return;
    
    _gfx->fillScreen(TFT_BLACK);
    _gfx->setTextColor(color);
    _gfx->setTextSize(2);
    
    int16_t titleX = (_gfx->width() - title.length() * 12) / 2;
    int16_t msgX = (_gfx->width() - msg.length() * 12) / 2;
    int16_t centerY = _gfx->height() / 2;
    
    _gfx->setCursor(titleX > 0 ? titleX : 10, centerY - 30);
    _gfx->println(title);
    
    _gfx->setCursor(msgX > 0 ? msgX : 10, centerY + 10);
    _gfx->println(msg);
}

void M5GFX_VNCDriver::print(const String& text) {
    if (_isPaused) return;
    _gfx->print(text);
}

void M5GFX_VNCDriver::clear(uint16_t color) {
    if (_isPaused) return;
    _gfx->fillScreen(color);
}

#endif // ESP32
