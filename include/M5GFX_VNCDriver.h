/**
 * @file M5GFX_VNCDriver.h
 * @brief VNC Display Driver for M5Stack Tab5 using M5GFX
 * 
 * This driver implements the VNCdisplay interface for M5Stack Tab5,
 * allowing the device to function as a VNC client with touch support.
 * 
 * Hardware: M5Stack Tab5
 *   - Display: 5" IPS TFT 1280x720 (MIPI-DSI)
 *   - Touch: GT911 multi-touch controller
 * 
 * @author Based on LovyanGFX_VNCDriver by Eric Nam
 * @date 2024
 */

#pragma once

#ifndef M5GFX_VNCDRIVER_H
#define M5GFX_VNCDRIVER_H

#ifdef ESP32

#include "VNC_config.h"
#include "VNC.h"
#include <M5Unified.h>
#include <M5GFX.h>

/**
 * @class M5GFX_VNCDriver
 * @brief VNC display driver implementation for M5Stack Tab5
 * 
 * This class implements the VNCdisplay interface using M5GFX library,
 * providing display rendering and touch input capabilities for VNC sessions.
 * Supports pausing/resuming screen drawing while maintaining VNC connection.
 */
class M5GFX_VNCDriver : public VNCdisplay {
public:
    /**
     * @brief Constructor
     * @param gfx Pointer to M5GFX display object (typically &M5.Display)
     */
    M5GFX_VNCDriver(M5GFX* gfx);
    
    /**
     * @brief Destructor
     */
    ~M5GFX_VNCDriver();

    // VNCdisplay interface implementation
    
    /**
     * @brief Check if the display supports copy rectangle operation
     * @return true if COPYRECT encoding is supported
     */
    bool hasCopyRect(void) override;
    
    /**
     * @brief Get display height
     * @return Display height in pixels
     */
    uint32_t getHeight(void) override;
    
    /**
     * @brief Get display width
     * @return Display width in pixels
     */
    uint32_t getWidth(void) override;
    
    /**
     * @brief Draw raw pixel data to display
     * @param x X coordinate
     * @param y Y coordinate
     * @param w Width of the area
     * @param h Height of the area
     * @param data Pointer to RGB565 pixel data
     */
    void draw_area(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* data) override;
    
    /**
     * @brief Draw a filled rectangle
     * @param x X coordinate
     * @param y Y coordinate
     * @param w Width of the rectangle
     * @param h Height of the rectangle
     * @param color RGB565 color value
     */
    void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t color) override;
    
    /**
     * @brief Copy a rectangular area to another location
     * @param src_x Source X coordinate
     * @param src_y Source Y coordinate
     * @param dest_x Destination X coordinate
     * @param dest_y Destination Y coordinate
     * @param w Width of the area
     * @param h Height of the area
     */
    void copy_rect(uint32_t src_x, uint32_t src_y, uint32_t dest_x, uint32_t dest_y, uint32_t w, uint32_t h) override;
    
    /**
     * @brief Start an area update operation
     * @param x X coordinate
     * @param y Y coordinate
     * @param w Width of the area
     * @param h Height of the area
     */
    void area_update_start(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override;
    
    /**
     * @brief Send pixel data during area update
     * @param data Pointer to pixel data
     * @param pixel Number of pixels
     */
    void area_update_data(char* data, uint32_t pixel) override;
    
    /**
     * @brief End an area update operation
     */
    void area_update_end(void) override;
    
    /**
     * @brief Override VNC options (optional)
     * @param opt Pointer to VNC options structure
     */
    void vnc_options_override(dfb_vnc_options* opt) override;

    // Screen control methods
    
    /**
     * @brief Pause screen drawing while maintaining VNC connection
     * @param paused true to pause drawing, false to resume
     * 
     * When paused, VNC communication continues but no drawing updates are applied.
     * This allows displaying alternative screens while keeping the VNC session alive.
     */
    void setPaused(bool paused) { _isPaused = paused; }
    
    /**
     * @brief Check if screen drawing is paused
     * @return true if drawing is paused
     */
    bool isPaused() const { return _isPaused; }

    // Additional helper methods
    
    /**
     * @brief Display a status message on screen
     * @param title Title text
     * @param msg Message text
     * @param color Text color (RGB565)
     */
    void printScreen(const String& title, const String& msg, uint16_t color);
    
    /**
     * @brief Print text at current cursor position
     * @param text Text to print
     */
    void print(const String& text);
    
    /**
     * @brief Clear the display
     * @param color Fill color (default: black)
     */
    void clear(uint16_t color = 0x0000);

private:
    M5GFX* _gfx;           ///< Pointer to M5GFX display object
    bool _isPaused;         ///< Flag to pause/resume drawing
    uint32_t _updateX;      ///< Current update area X coordinate
    uint32_t _updateY;      ///< Current update area Y coordinate
    uint32_t _updateW;      ///< Current update area width
    uint32_t _updateH;      ///< Current update area height
    uint32_t _pixelCount;   ///< Pixel counter for area updates
};

#endif // ESP32
#endif // M5GFX_VNCDRIVER_H
