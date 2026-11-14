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
 * OpenDMR, service: the power-on PIN and the lock PIN (security.h).
 *
 * The power-on PIN is the OpenGD77 one, in the codeplug (codeplugGetPasswordPin(), the CPS's boot settings): 0x7519
 * bit 0 on, the PIN at 0x751C, 3 bytes BCD, up to 6 digits, 0xF after the last one. So the OpenGD77 CPS sets and
 * clears it, and a codeplug keeps it between OpenGD77 / OpenDM32 and this firmware. The lock PIN is ours: its
 * switch and a copy of the PIN (set together with the power-on PIN on the radio), in a block after the settings
 * (their flash sector, behind the main firmware's settings_t: 0x604B - 0x60B6), with an id of the firmware build.
 * The CPS can't change it: its sector writes keep the block (security_keep_block()). Another build (an update by
 * any way, also the bootloader) or an update over the cable turns both off: the way back from a forgotten PIN.
 * This file is built every time (Makefile), for the build id.
 */

#include <string.h>
#include "flash.h"
#include "security.h"

#define STRINGIFY(x)  #x
#define XSTRINGIFY(x) STRINGIFY(x)
#ifdef GITVERSION
#define BUILD_TEXT XSTRINGIFY(GITVERSION) " " __DATE__ " " __TIME__
#else
#define BUILD_TEXT __DATE__ " " __TIME__
#endif

#define ADDR_PIN_ENABLE 0x7519 // CODEPLUG_ADDR_BOOT_PASSWORD_PIN + 1: bit 0
#define ADDR_PIN        0x751C // 3 bytes BCD, high nibble first, 0xF: the end
#define PIN_BYTES       3

#define ADDRESS 0x6100     // our block, in the data flash window
#define MAGIC   0x324E4950 // "PIN2"

#define FLAG_LOCK 0x02

typedef struct {
    uint32_t magic;
    uint8_t flags;
    uint8_t length; // of the lock PIN; 0: none
    uint8_t reserved[2];
    uint32_t build; // build_id() of the firmware that wrote it
    char pin[SECURITY_PIN_MAX];
} block_t;

static block_t block;
static bool boot_on = false;
static volatile bool boot_locked = false;
static volatile bool locked = false;

// FNV-1a of the build text
static uint32_t build_id(void)
{
    const char *s = BUILD_TEXT;
    uint32_t hash = 2166136261UL;

    while (*s) {
        hash = (hash ^ (uint8_t)*s++) * 16777619UL;
    }
    return hash;
}

static bool save(void)
{
    block.magic = MAGIC;
    block.build = build_id();
    return flash_write(ADDRESS, (const uint8_t *)&block, sizeof(block));
}

// The codeplug's PIN as digits (codeplugGetPasswordPin()); its length, 0: none
static size_t read_pin(char *digits)
{
    uint8_t bytes[PIN_BYTES];
    size_t length = 0;

    if (flash_read(ADDR_PIN, bytes, sizeof(bytes))) {
        for (size_t i = 0; i < (2 * PIN_BYTES); i++) {
            uint8_t nibble = (bytes[i / 2] >> (((i % 2) == 0) ? 4 : 0)) & 0x0F;

            if (nibble > 9) {
                break;
            }
            digits[length++] = (char)('0' + nibble);
        }
    }
    digits[length] = '\0';
    return length;
}

static bool read_boot_on(void)
{
    uint8_t enable = 0;

    return flash_read(ADDR_PIN_ENABLE, &enable, 1) && (enable != 0xFF) && (enable & 0x01);
}

static bool write_boot_on(bool on)
{
    uint8_t enable = 0;

    if (!flash_read(ADDR_PIN_ENABLE, &enable, 1)) {
        return false;
    }
    if (enable == 0xFF) {
        enable = 0;
    }
    enable = on ? (uint8_t)(enable | 0x01) : (uint8_t)(enable & ~0x01);
    if (!flash_write(ADDR_PIN_ENABLE, &enable, 1)) {
        return false;
    }
    boot_on = on;
    return true;
}

static bool pin_present(void)
{
    char digits[SECURITY_PIN_MAX + 1];

    return read_pin(digits) > 0;
}

void security_load(void)
{
    bool ours = flash_read(ADDRESS, (uint8_t *)&block, sizeof(block)) && (block.magic == MAGIC);

    if (!ours || (block.length > SECURITY_PIN_MAX)) {
        memset(&block, 0, sizeof(block));
        ours = false;
    }
    boot_on = read_boot_on() && pin_present();
    if (ours && (block.build != build_id()) && (boot_on || (block.flags & FLAG_LOCK))) {
        security_clear(); // another firmware: off (the PIN is kept)
    } else if (!ours || (block.build != build_id())) {
        (void)save(); // the build from now on (a codeplug's power-on PIN from before is kept)
    }
    boot_locked = boot_on;
}

bool security_has_pin(void)
{
    return boot_on || security_lock_pin_on();
}

bool security_check_boot_pin(const char *digits)
{
    char pin[SECURITY_PIN_MAX + 1];
    size_t length = read_pin(pin);

    return (length > 0) && (strcmp(digits, pin) == 0);
}

bool security_check_lock_pin(const char *digits)
{
    size_t length = strlen(digits);

    return (block.length > 0) && (length == block.length) && (memcmp(digits, block.pin, length) == 0);
}

bool security_set_pin(const char *digits)
{
    size_t length = strlen(digits);
    uint8_t bytes[PIN_BYTES];

    if ((length < SECURITY_PIN_MIN) || (length > SECURITY_PIN_MAX)) {
        return false;
    }
    memset(bytes, 0xFF, sizeof(bytes));
    for (size_t i = 0; i < length; i++) {
        uint8_t shift = ((i % 2) == 0) ? 4 : 0;

        if ((digits[i] < '0') || (digits[i] > '9')) {
            return false;
        }
        bytes[i / 2] = (uint8_t)((bytes[i / 2] & ~(0x0F << shift)) | ((digits[i] - '0') << shift));
    }
    if (!flash_write(ADDR_PIN, bytes, sizeof(bytes))) {
        return false;
    }
    memset(block.pin, 0, sizeof(block.pin)); // the lock PIN's copy
    memcpy(block.pin, digits, length);
    block.length = (uint8_t)length;
    return save();
}

static void set_flag(uint8_t flag, bool on)
{
    uint8_t flags = on ? (uint8_t)(block.flags | flag) : (uint8_t)(block.flags & ~flag);

    if (flags != block.flags) {
        uint8_t before = block.flags;

        block.flags = flags;
        if (!save()) {
            block.flags = before;
        }
    }
}

bool security_boot_pin_on(void)
{
    return boot_on;
}

bool security_lock_pin_on(void)
{
    return (block.flags & FLAG_LOCK) != 0;
}

void security_set_boot_pin(bool on)
{
    if (!on || pin_present()) {
        (void)write_boot_on(on);
    }
}

void security_set_lock_pin(bool on)
{
    if (!on || (block.length > 0)) {
        set_flag(FLAG_LOCK, on);
    }
}

void security_clear(void)
{
    if (boot_on) {
        (void)write_boot_on(false);
    }
    block.flags &= (uint8_t)~FLAG_LOCK;
    (void)save(); // also the build id
}

// The CPS writes a 4 KB sector of the window: ours stays as it is (lock state and PIN)
void security_keep_block(uint32_t sector_address, uint8_t *sector)
{
    if ((ADDRESS >= sector_address) && ((ADDRESS + sizeof(block)) <= (sector_address + FLASH_SECTOR_SIZE))) {
        block_t stored;

        if (flash_read(ADDRESS, (uint8_t *)&stored, sizeof(stored))) {
            memcpy(&sector[ADDRESS - sector_address], &stored, sizeof(stored));
        }
    }
}

bool security_boot_locked(void)
{
    return boot_locked;
}

void security_boot_unlocked(void)
{
    boot_locked = false;
}

void security_set_locked(bool on)
{
    locked = on;
}

bool security_vox_blocked(void)
{
    return boot_locked || (locked && security_lock_pin_on());
}
