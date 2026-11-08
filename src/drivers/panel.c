/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 *    disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 *    following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 *    products derived from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden without the written
 *    permission of the copyright holder. This includes, without limitation, incorporation in a commercial product or
 *    incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * OpenDMR bring-up: the LCD (ST7789 command set, 240 x 320, RGB565 on the I8080 8 bit bus), from
 * OpenDM32 user/SOURCE/io/display.c displayInit().
 *
 * The bus is shared with the keypad (its rows are LCD_DB0 - DB4): panel_bus_take() / panel_bus_give() around each use.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "hw.h"
#include "hal.h"
#include "panel.h"

static StaticSemaphore_t bus_mutex_buffer;
static SemaphoreHandle_t bus_mutex = NULL;

// No lock before the scheduler runs: a single thread then
void panel_bus_take(void)
{
    if (bus_mutex && (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)) {
        xSemaphoreTake(bus_mutex, portMAX_DELAY);
    }
}

void panel_bus_give(void)
{
    if (bus_mutex && (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)) {
        xSemaphoreGive(bus_mutex);
    }
}

void RAM_CODE panel_write_cmd(uint8_t cmd)
{
    I8080_CMD = cmd;
}

void RAM_CODE panel_write_data(uint8_t data)
{
    I8080_DATA = data;
}

void RAM_CODE panel_write_cmd_data(uint8_t cmd, const uint8_t *data, int len)
{
    I8080_CMD = cmd;
    for (volatile int x = 0; x < 1; x++) {
        // the LCD needs a short time before the data
    }
    for (int i = 0; i < len; i++) {
        I8080_DATA = data[i];
        for (volatile int x = 0; x < 1; x++) {
        }
    }
}

#define CMD(c, ...)                                                                                                    \
    do {                                                                                                               \
        static const uint8_t d[] = {__VA_ARGS__};                                                                      \
        panel_write_cmd_data((c), d, sizeof(d));                                                                       \
    } while (0)

void panel_init(void)
{
    bus_mutex = xSemaphoreCreateMutexStatic(&bus_mutex_buffer);
    gpio_output(PIN_LCD_NRESET, true);
    gpio_output(PIN_LCD_RD, true);

    // I8080 interface: timing, enable, wait until ready
    I8080_TIMING = 0x303;
    I8080_CTRL |= BITSET(0);
    while ((I8080_CTRL & 0x02) == 0) {
    }

    // LCD pins to the I8080 interface (CS, RS, WR, DB0 - DB7)
    IO_DIPLEX2 &= 0xFFF80107;

    // Reset: 10 us low, 5 ms before a command
    gpio_set(PIN_LCD_NRESET);
    delay_ms(1);
    gpio_reset(PIN_LCD_NRESET);
    delay_ms(1);
    gpio_set(PIN_LCD_NRESET);
    delay_ms(10);

    panel_write_cmd(0x11); // sleep out
    delay_ms(10);

    CMD(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33); // porch control
    CMD(0x35, 0x00);                         // tearing effect line on
    CMD(0x36, 0xC8);                         // memory data access control; bit 3: BGR, the palette's order (0xC0 showed red as blue)
    CMD(0x3A, 0x05);                         // 16 bit pixels
    CMD(0xB7, 0x56);                         // gate control
    CMD(0xBB, 0x17);                         // VCOMS
    CMD(0xC0, 0x2C);                         // LCM control
    CMD(0xC2, 0x01);                         // VDV and VRH command enable
    CMD(0xC3, 0x0B);                         // VRH
    CMD(0xC6, 0xEF);                         // frame rate in normal mode
    CMD(0xD0, 0xA7);                         // power control 1
    CMD(0xD0, 0xA4, 0xA1);
    CMD(0xD6, 0xA1);
    CMD(0xE0, 0x70, 0x03, 0x08, 0x07, 0x08, 0x15, 0x30, 0x54, 0x47, 0x3A, 0x15, 0x14, 0x2B, 0x2E); // gamma +
    CMD(0xE1, 0x70, 0x06, 0x0A, 0x09, 0x06, 0x03, 0x2F, 0x44, 0x47, 0x38, 0x12, 0x15, 0x29, 0x2F); // gamma -
    panel_write_cmd(0x21); // display inversion on (the panel needs it)

    panel_fill(0, 0, PANEL_WIDTH, PANEL_HEIGHT, 0x0000);
    panel_write_cmd(0x29); // display on
}

// Pixels [x0, x1) x [y0, y1), colour RGB565 as the panel takes it
void panel_fill(int x0, int y0, int x1, int y1, uint16_t colour)
{
    uint8_t caset[] = {(x0 >> 8), (x0 & 0xFF), ((x1 - 1) >> 8), ((x1 - 1) & 0xFF)};
    uint8_t raset[] = {(y0 >> 8), (y0 & 0xFF), ((y1 - 1) >> 8), ((y1 - 1) & 0xFF)};

    panel_bus_take();
    panel_write_cmd_data(0x2A, caset, sizeof(caset));
    panel_write_cmd_data(0x2B, raset, sizeof(raset));
    panel_write_cmd(0x2C); // memory write

    for (int n = (x1 - x0) * (y1 - y0); n > 0; n--) {
        I8080_DATA = (colour >> 8);
        I8080_DATA = (colour & 0xFF);
    }
    panel_bus_give();
}

// Palette colour (RGB888) to the 16 bits sent to the panel, low byte first: BGR565, bytes swapped
// (displayConvertRGB888ToNative() of the main firmware)
uint16_t panel_rgb888_to_native(uint32_t rgb888)
{
    uint16_t bgr565 = (uint16_t)(((rgb888 & 0xf80000) >> 19) | ((rgb888 & 0xfc00) >> 5) | ((rgb888 & 0xf8) << 8));

    return (uint16_t)((bgr565 >> 8) | (bgr565 << 8));
}
