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
 * OpenDMR, driver: the 25Q128 data flash on SPI0, chip select GPIO 18. After OpenDM32 user/SOURCE/
 * hardware/SPI_Flash.c.
 *
 * Nothing is erased when the data are already in the flash: the main firmware saves the settings at each power off, and
 * an erase cut by the power (switched off just after switching on) once left a page blank (VFOs lost, 2026-11-01).
 * All the functions take a mutex: the UI and the cable link task both use the flash.
 */

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "hw.h"
#include "hal.h"
#include "spi0.h"
#include "flash.h"

#define PIN_FLASH_CS PIN_SPI0_CS

#define CMD_WRITE_ENABLE 0x06
#define CMD_READ_STATUS1 0x05
#define CMD_PAGE_PROGRAM 0x02
#define CMD_SECTOR_ERASE 0x20
#define CMD_JEDEC_ID     0x9F
#define CMD_READ_DATA    0x03
#define STATUS1_BUSY     0x01

#define PAGE_PROGRAM_MS 3   // worst case
#define SECTOR_ERASE_MS 500 // worst case

static uint32_t part_id;
static uint8_t sector_buffer[FLASH_SECTOR_SIZE];
static bool batch = false;
static int32_t batch_sector = -1;
static bool batch_dirty = false;

static StaticSemaphore_t mutex_buffer;
static SemaphoreHandle_t mutex = NULL;

// Recursive: flash_write() reads the sector with flash_read()
static void lock(void)
{
    if (mutex && (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)) {
        xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (mutex && (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)) {
        xSemaphoreGiveRecursive(mutex);
    }
}

static void select(void)
{
    gpio_reset(PIN_FLASH_CS);
}

static void deselect(void)
{
    gpio_set(PIN_FLASH_CS);
}

static void command(const uint8_t *cmd, uint32_t cmd_length, uint8_t *reply, uint32_t reply_length)
{
    select();
    spi0_write(cmd, cmd_length);
    if (reply_length > 0) {
        spi0_read(reply, reply_length);
    }
    deselect();
}

static void address_command(uint8_t *cmd, uint8_t code, uint32_t address)
{
    cmd[0] = code;
    cmd[1] = (address >> 16) & 0xFF;
    cmd[2] = (address >> 8) & 0xFF;
    cmd[3] = address & 0xFF;
}

static bool wait_ready(uint32_t timeout_ms)
{
    const uint8_t cmd = CMD_READ_STATUS1;

    for (uint32_t i = 0; i <= (timeout_ms * 10); i++) {
        uint8_t status;

        command(&cmd, 1, &status, 1);
        if ((status & STATUS1_BUSY) == 0) {
            return true;
        }
        delay_us(100);
    }
    return false;
}

static void write_enable(void)
{
    const uint8_t cmd = CMD_WRITE_ENABLE;

    command(&cmd, 1, NULL, 0);
}

static bool erase_sector_raw(uint32_t address)
{
    uint8_t cmd[4];

    address_command(cmd, CMD_SECTOR_ERASE, address);
    write_enable();
    command(cmd, sizeof(cmd), NULL, 0);
    return wait_ready(SECTOR_ERASE_MS);
}

static bool program_page_raw(uint32_t address, const uint8_t *data)
{
    uint8_t cmd[4];

    address_command(cmd, CMD_PAGE_PROGRAM, address);
    write_enable();
    select();
    spi0_write(cmd, sizeof(cmd));
    spi0_write(data, FLASH_PAGE_SIZE);
    deselect();
    return wait_ready(PAGE_PROGRAM_MS);
}

bool flash_init(void)
{
    const uint8_t cmd = CMD_JEDEC_ID;
    uint8_t id[3];

    mutex = xSemaphoreCreateRecursiveMutexStatic(&mutex_buffer);
    spi0_init();
    gpio_output(PIN_FLASH_CS, true);
    delay_us(160);

    command(&cmd, 1, id, sizeof(id));
    part_id = (id[1] << 8) | id[2];

    return ((part_id == 0x4018) || (part_id == 0x2018));
}

uint32_t flash_part_id(void)
{
    return part_id;
}

bool flash_read_raw(uint32_t address, uint8_t *data, uint32_t length)
{
    uint8_t cmd[4];

    if ((length == 0) || (address >= FLASH_SIZE) || (length > (FLASH_SIZE - address))) {
        return false;
    }

    address_command(cmd, CMD_READ_DATA, address);
    lock();
    command(cmd, sizeof(cmd), data, length);
    unlock();
    return true;
}

static bool write_sector_raw_locked(uint32_t address, const uint8_t *data)
{
    if (!erase_sector_raw(address)) {
        return false;
    }
    for (uint32_t page = 0; page < FLASH_SECTOR_SIZE; page += FLASH_PAGE_SIZE) {
        if (!program_page_raw(address + page, data + page)) {
            return false;
        }
    }
    return true;
}

bool flash_write_sector_raw(uint32_t address, const uint8_t *data)
{
    bool ok;

    if (((address % FLASH_SECTOR_SIZE) != 0) || (address >= FLASH_SIZE)) {
        return false;
    }
    lock();
    ok = write_sector_raw_locked(address, data);
    unlock();
    return ok;
}

static bool in_window(uint32_t address, uint32_t length)
{
    return (address < FLASH_WINDOW_SIZE) && (length <= (FLASH_WINDOW_SIZE - address));
}

bool flash_read(uint32_t address, uint8_t *data, uint32_t length)
{
    return in_window(address, length) && flash_read_raw(FLASH_WINDOW_BASE + address, data, length);
}

static bool program_buffered_sector(int32_t sector)
{
    return write_sector_raw_locked(FLASH_WINDOW_BASE + (sector * FLASH_SECTOR_SIZE), sector_buffer);
}

static bool batch_flush(void)
{
    bool ok = true;

    if (batch_dirty) {
        ok = program_buffered_sector(batch_sector);
        batch_dirty = false;
    }
    batch_sector = -1;
    return ok;
}

// The part of one sector at address: read, patched, erased and programmed, unless it is already there
static bool write_in_sector(uint32_t address, const uint8_t *data, uint32_t length)
{
    int32_t sector = address / FLASH_SECTOR_SIZE;
    uint8_t *position = &sector_buffer[address % FLASH_SECTOR_SIZE];

    if (batch && (sector == batch_sector)) {
        if (memcmp(position, data, length) != 0) {
            memcpy(position, data, length);
            batch_dirty = true;
        }
        return true;
    }

    if (batch && !batch_flush()) {
        return false;
    }

    if (!flash_read(sector * FLASH_SECTOR_SIZE, sector_buffer, FLASH_SECTOR_SIZE)) {
        return false;
    }
    if (batch) {
        batch_sector = sector;
    }
    if (memcmp(position, data, length) == 0) {
        return true;
    }
    memcpy(position, data, length);

    if (batch) {
        batch_dirty = true;
        return true;
    }
    return program_buffered_sector(sector);
}

bool flash_write(uint32_t address, const uint8_t *data, uint32_t length)
{
    bool ok = true;

    if (!in_window(address, length)) {
        return false;
    }

    lock();
    while (ok && (length > 0)) {
        uint32_t in_sector = FLASH_SECTOR_SIZE - (address % FLASH_SECTOR_SIZE);

        if (in_sector > length) {
            in_sector = length;
        }
        ok = write_in_sector(address, data, in_sector);
        address += in_sector;
        data += in_sector;
        length -= in_sector;
    }
    unlock();
    return ok;
}

void flash_batch_start(void)
{
    lock();
    batch = true;
    batch_sector = -1;
    batch_dirty = false;
    unlock();
}

bool flash_batch_end(void)
{
    bool ok;

    lock();
    ok = batch_flush();
    batch = false;
    unlock();
    return ok;
}
