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
 * OpenDMR, service: the programming cable link. After OpenDM32 user/SOURCE/functions/cps.c
 * (request framing, tick_com_request()), event driven: the UART interrupt wakes the cable link task with a notification.
 *
 *   'R' area(1) address(4) length(2)    read -> 'R' length(2) data, or '-'; area 1: the data flash window,
 *                                       area 9: radio info (dm32dbg.py info)
 *   'D' ...                             debug commands, see debug.c
 */

#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "uart.h"
#include "systime.h"
#include "cable_link.h"
#include "debug.h"
#include "flash.h"
#include "hal.h"
#include "settings.h"
#include "datetime.h"
#include "hotspot.h"
#include "dmr_ids.h"
#include "panel.h"
#include "radio.h"
#include "serial_update.h"
#include "dmr_compat.h"
#include "cps_screen.h"

#define CPS_REQUEST_IDLE_MS   5   // a request without a known length is complete after this silence
#define CPS_REQUEST_LOST_MS   200 // a debug request with a known length that stopped short is handled anyway
#define CPS_LINK_BUSY_MS      1000
#define CPS_ACCESS_FLASH      1 // the window of the data flash (flash.h), as the CPS sees it
#define CPS_ACCESS_EEPROM     2 // the same window on the DM-32 (the emulated EEPROM is at its start)
#define CPS_ACCESS_MCU_ROM    5 // memory, at the address + 0x8000000 (the STM32 flash base of the MD-UV380 CPS)
#define MCU_ROM_OFFSET        0x8000000
#define CPS_ACCESS_RADIO_INFO 9
#define CPS_RADIO_TYPE_DM32   11 // what the OpenDM32 releases report
#define CPS_WRITE_PREPARE     1  // 'X': a sector into the buffer
#define CPS_WRITE_DATA        2  // 'X': bytes into it
#define CPS_WRITE_SECTOR      3  // 'X': the buffer to the flash
#define CPS_WRITE_EEPROM      4  // 'X': the EEPROM, in the flash sectors on the DM-32
#define CPS_CALIBRATION       0x10000    // the OpenGD77 calibration: not this radio's (the factory one is read only)
#define DMR_IDS_AREA_1        0x50000    // the DMR ID database (dmr_ids.c): its header and first records (0x20000 +
#define DMR_IDS_AREA_1_END    0x90000    // 0x30000: the CPS adds 0x20000 to its flash addresses on the DM-32)
#define DMR_IDS_AREA_2        0xAF000    // the rest (from 0xAF400 or 0xD8000)
#define DMR_IDS_AREA_2_END    FLASH_WINDOW_SIZE // as far as the CPS writes: init again after its last sector
#define CPS_RESTART_DELAY_MS  500        // the reply goes out first
#define UPDATE_RADIO_OFF_MS   300        // the radio task's time to turn the receiver off before an update

#define CABLE_LINK_TASK_PRIORITY 6 // above the radio and DMR tasks (below the RF task): the debug requests are answered while another task never yields

static volatile uint8_t request[CABLE_LINK_BUFFER_SIZE];
static volatile uint8_t reply[CABLE_LINK_BUFFER_SIZE];
static volatile uint32_t request_length = 0;
static volatile uint32_t last_byte_ms = 0;   // 1 ms timer (systime: still counts when the tick stalls)
static volatile uint32_t last_byte_tick = 0; // FreeRTOS tick (still counts when the 1 ms timer stops)
static volatile bool handling = false;
static volatile bool used = false;           // a request handled since the start
static volatile uint32_t used_ms = 0;        // when the last one was
static volatile uint32_t quiet_until = 0;
static volatile bool restart_asked = false;
static volatile bool update_asked = false;

// The CPS write (OpenDM32 functions/cps.c cpsHandleWriteCommand()): the sector being written, SECTOR_NONE,
// or SECTOR_IGNORED: the OpenGD77 calibration (accepted, not written)
#define SECTOR_NONE    (-1)
#define SECTOR_IGNORED (-2)

static int32_t write_sector = SECTOR_NONE;
static uint8_t sector_buffer[FLASH_SECTOR_SIZE];

static StaticTask_t task_tcb;
static StackType_t task_stack[512];
static TaskHandle_t task_handle = NULL;

static void RAM_CODE rx_byte(uint8_t c)
{
    BaseType_t woken = pdFALSE;

    // The hotspot's MMDVM frames (hotspot.c) start with a byte no CPS request starts with
    if (hotspot_rx_byte(c, (request_length == 0) && !handling)) {
        return;
    }
    // Bytes arriving while a request is handled are dropped: the CPS and the tool wait for each reply
    if ((handling == false) && (request_length < CABLE_LINK_BUFFER_SIZE)) {
        request[request_length++] = c;
    }
    last_byte_ms = systime_get_ms();
    last_byte_tick = xTaskGetTickCountFromISR();

    if (task_handle) {
        vTaskNotifyGiveFromISR(task_handle, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

// No byte for ms milliseconds, by either clock: one of them may have stopped
static bool silent_for(uint32_t ms)
{
    return ((systime_get_ms() - last_byte_ms) >= ms) ||
           ((xTaskGetTickCountFromISR() - last_byte_tick) >= pdMS_TO_TICKS(ms));
}

// Length of the request, 0 while not known yet
static uint32_t expected_length(uint32_t received)
{
    if (request[0] == 'R') {
        return 8;
    }
    if (((request[0] == 'X') || (request[0] == 'W')) && (received >= 8) &&
        ((request[1] == CPS_WRITE_DATA) || (request[1] == CPS_WRITE_EEPROM))) {
        return 8 + ((request[6] << 8) | request[7]);
    }
    if ((request[0] == 'D') && (received >= 2) && (request[1] == 'U')) {
        return 6; // 'D' 'U' "UPD!": the firmware update
    }
    if ((request[0] == 'D') && (received >= 2) && (request[1] == 'F')) {
        return 9 + 1024; // font upload: a gap in the UART must not end it early
    }
    return 0;
}

#define XSTRINGIFY(x) STRINGIFY(x)
#define STRINGIFY(x)  #x

// The build time as the CPS parses it: "yyyymmddhhmmss" (from __DATE__ "Nov  5 2026" and __TIME__ "08:19:12")
static void build_date_time(char *out, size_t size)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *date = __DATE__;
    const char *time = __TIME__;
    int month = 0;

    for (int i = 0; i < 12; i++) {
        if (strncmp(date, &months[i * 3], 3) == 0) {
            month = i + 1;
        }
    }
    snprintf(out, size, "%.4s%02d%02d%.2s%.2s%.2s", &date[7], month,
             ((date[4] == ' ') ? 0 : ((date[4] - '0') * 10)) + (date[5] - '0'), &time[0], &time[3], &time[6]);
}

static uint32_t handle_read(void)
{
    uint32_t address = ((uint32_t)request[2] << 24) | ((uint32_t)request[3] << 16) | (request[4] << 8) | request[5];
    uint32_t length = (request[6] << 8) | request[7];

    if (length > (CABLE_LINK_BUFFER_SIZE - 3)) {
        length = CABLE_LINK_BUFFER_SIZE - 3;
    }
    cps_screen_transfer(request[1], address, length, false);

    if (request[1] == CPS_ACCESS_MCU_ROM) {
        memcpy((uint8_t *)&reply[3], (const uint8_t *)(address + MCU_ROM_OFFSET), length);
        reply[0] = 'R';
        reply[1] = (length >> 8) & 0xFF;
        reply[2] = length & 0xFF;
        return length + 3;
    }

    if (((request[1] == CPS_ACCESS_FLASH) || (request[1] == CPS_ACCESS_EEPROM)) &&
        flash_read(address, (uint8_t *)&reply[3], length)) {
        reply[0] = 'R';
        reply[1] = (length >> 8) & 0xFF;
        reply[2] = length & 0xFF;
        return length + 3;
    }

    if (request[1] == CPS_ACCESS_RADIO_INFO) {
        struct __attribute__((__packed__)) {
            uint32_t struct_version;
            uint32_t radio_type;
            char git_revision[16];
            char build_date_time[16];
            uint32_t flash_id;
            uint16_t features;
        } radio_info;

        memset(&radio_info, 0, sizeof(radio_info));
        radio_info.struct_version = 0x03;
        radio_info.radio_type = CPS_RADIO_TYPE_DM32;
#if defined(GITVERSION)
        snprintf(radio_info.git_revision, sizeof(radio_info.git_revision), "%s", XSTRINGIFY(GITVERSION));
#else
        snprintf(radio_info.git_revision, sizeof(radio_info.git_revision), "OpenDMR");
#endif
        build_date_time(radio_info.build_date_time, sizeof(radio_info.build_date_time));
        radio_info.flash_id = flash_part_id();
        length = sizeof(radio_info);
        memcpy((uint8_t *)&reply[3], &radio_info, length);

        reply[0] = 'R';
        reply[1] = (length >> 8) & 0xFF;
        reply[2] = length & 0xFF;
        return length + 3;
    }

    reply[0] = '-';
    return 1;
}

static uint32_t request_u32(int at)
{
    return ((uint32_t)request[at] << 24) | ((uint32_t)request[at + 1] << 16) | ((uint32_t)request[at + 2] << 8) |
           request[at + 3];
}

// 'X': the CPS writes the codeplug a 4 KB sector at a time, in the window of the flash
static uint32_t handle_write(void)
{
    bool ok = false;

    switch (request[1]) {
        case CPS_WRITE_PREPARE:
            if (write_sector == SECTOR_NONE) {
                uint32_t sector = ((uint32_t)request[2] << 16) | ((uint32_t)request[3] << 8) | request[4];
                uint32_t address = sector * FLASH_SECTOR_SIZE;

                if (address == CPS_CALIBRATION) {
                    write_sector = SECTOR_IGNORED;
                    ok = true;
                } else if (flash_read(address, sector_buffer, FLASH_SECTOR_SIZE)) {
                    write_sector = (int32_t)sector;
                    ok = true;
                }
            }
            break;
        case CPS_WRITE_DATA: {
            uint32_t address = request_u32(2);
            uint32_t length = (request[6] << 8) | request[7];

            if (length > (CABLE_LINK_BUFFER_SIZE - 8)) {
                length = CABLE_LINK_BUFFER_SIZE - 8;
            }
            cps_screen_transfer(CPS_ACCESS_FLASH, address, length, true);
            if (write_sector == SECTOR_IGNORED) {
                ok = true;
            } else if (write_sector >= 0) {
                for (uint32_t i = 0; i < length; i++) {
                    if (((address + i) / FLASH_SECTOR_SIZE) == (uint32_t)write_sector) {
                        sector_buffer[(address + i) % FLASH_SECTOR_SIZE] = request[8 + i];
                    }
                }
                ok = true;
            }
            break;
        }
        case CPS_WRITE_SECTOR:
            if (write_sector == SECTOR_IGNORED) {
                ok = true;
            } else if ((write_sector >= 0) && (((uint32_t)write_sector * FLASH_SECTOR_SIZE) < FLASH_WINDOW_SIZE)) {
                uint32_t address = (uint32_t)write_sector * FLASH_SECTOR_SIZE;

                ok = flash_write_sector_raw(FLASH_WINDOW_BASE + address, sector_buffer);
                if (ok && (((address >= DMR_IDS_AREA_1) && (address < DMR_IDS_AREA_1_END)) ||
                           ((address >= DMR_IDS_AREA_2) && (address < DMR_IDS_AREA_2_END)))) {
                    dmr_ids_init(); // the DMR ID database (cpsHandleCommand() dmrIDCacheInit())
                }
            }
            write_sector = SECTOR_NONE;
            break;
        case CPS_WRITE_EEPROM:
            ok = true;
            break;
        default:
            break;
    }

    if (!ok) {
        write_sector = SECTOR_NONE;
        reply[0] = '-';
        return 1;
    }
    reply[0] = request[0];
    reply[1] = request[1];
    return 2;
}

// 'C' (cpsHandleCommand()): 0 - 5 the CPS's screen: only 5 (close) is taken, ours (cps_screen.c) opens with the first
// codeplug data read or written and says what it is. 6: 0 / 1 the settings saved and a restart (the codeplug is read
// again at start), 2 the settings saved, 7 the date and time
static uint32_t handle_command(void)
{
    if (request[1] == 5) {
        cps_screen_close();
    }
    if (request[1] == 6) {
        switch (request[2]) {
            case 0:
            case 1:
                settings_save_if_changed();
                restart_asked = true;
                break;
            case 2:
                settings_save_if_changed();
                break;
            case 7:
                datetime_set((uint32_t)request[3] | ((uint32_t)request[4] << 8) | ((uint32_t)request[5] << 16) |
                             ((uint32_t)request[6] << 24));
                break;
            default:
                break;
        }
    }
    reply[0] = '-';
    return 1;
}

// A whole chip reset by the watchdog: the main firmware's soft reset (SYS_SOFT_RSTN) leaves the clock and flash
// settings as they were and hangs, and a jump to the bootloader left the modem as it was
static void restart(void)
{
    watchdog_reset();
}

// 'D' 'U' "UPD!": the firmware update over the cable (services/serial_update.c, tools/debug/fw_update.py); not while
// transmitting. Reply 'D' 'U' ok(1) area(4) size(4): the RAM for the PC's update program (big endian)
static uint32_t handle_update(void)
{
    bool ok = (request[2] == 'U') && (request[3] == 'P') && (request[4] == 'D') && (request[5] == '!') &&
              !radio_is_transmitting() && !trxTransmissionEnabled;
    uint32_t area = serial_update_area(), size = serial_update_area_size();

    reply[0] = 'D';
    reply[1] = 'U';
    reply[2] = ok ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        reply[3 + i] = area >> (24 - (8 * i));
        reply[7 + i] = size >> (24 - (8 * i));
    }
    update_asked = ok;
    return 11;
}

// Everything stops for good: no receiving (the channel off, the speaker muted), the LCD bus kept from the keypad,
// then the update with the interrupts off (no keys, knob, PTT or power switch any more) until it restarts the radio
static void update(void)
{
    radio_set_channel(NULL);
    vTaskDelay(pdMS_TO_TICKS(UPDATE_RADIO_OFF_MS));
    panel_bus_take();
    serial_update_run();
}

void cable_link_handle_pending(bool from_monitor)
{
    uint32_t received = request_length;
    uint32_t now = systime_get_ms();

    if ((received == 0) || handling) {
        return;
    }

    uint32_t expected = expected_length(received);

    if (!(((expected != 0) && (received >= expected)) ||
          ((expected == 0) && silent_for(CPS_REQUEST_IDLE_MS)) ||
          ((request[0] == 'D') && silent_for(CPS_REQUEST_LOST_MS)))) {
        return;
    }

    handling = true;
    if ((request[0] == 'R') || (request[0] == 'X') || (request[0] == 'W') || (request[0] == 'C') ||
        (request[0] == 'D')) {
        quiet_until = now + CPS_LINK_BUSY_MS; // the bytes on the cable can read as the PTT of the jack
    }

    uint32_t reply_length;

    switch (request[0]) {
        case 'R':
            reply_length = handle_read();
            break;
        case 'X':
            reply_length = handle_write();
            break;
        case 'W': // the old fake write
            reply[0] = request[0];
            reply[1] = request[1];
            reply_length = 2;
            break;
        case 'C':
            reply_length = handle_command();
            break;
        case 'D':
            if (request[1] == 'U') {
                reply_length = handle_update();
            } else {
                reply_length = debug_handle_command(request, reply, CABLE_LINK_BUFFER_SIZE);
            }
            break;
        default:
            reply[0] = '-';
            reply_length = 1;
            break;
    }

    uart0_write((const uint8_t *)reply, reply_length);
    (void)from_monitor;
    used_ms = systime_get_ms();
    used = true;

    request_length = 0;
    handling = false;
}

// In the 1 ms interrupt (services/systime.c, the watchdog): from RAM
bool RAM_CODE cable_link_used_within(uint32_t ms)
{
    return handling || (used && ((systime_get_ms() - used_ms) < ms));
}

bool cable_link_is_busy(void)
{
    return ((int32_t)(quiet_until - systime_get_ms()) > 0);
}

static void cable_link_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (request_length == 0) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // until a byte comes
        } else {
            cable_link_handle_pending(false);
            if (update_asked) {
                update();
            }
            if (restart_asked) {
                vTaskDelay(pdMS_TO_TICKS(CPS_RESTART_DELAY_MS));
                restart();
            }
            if (request_length != 0) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CPS_REQUEST_IDLE_MS)); // the rest, or the silence that ends it
            }
        }
    }
}

void cable_link_init(void)
{
    task_handle = xTaskCreateStatic(cable_link_task, "cable", sizeof(task_stack) / sizeof(task_stack[0]), NULL,
                                    CABLE_LINK_TASK_PRIORITY, task_stack, &task_tcb);
    uart0_init(115200, rx_byte);
}
