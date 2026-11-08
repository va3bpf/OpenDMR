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

#ifndef DM32_FLASH_H
#define DM32_FLASH_H

#include <stdbool.h>
#include <stdint.h>

// The 25Q128 data flash (16 MB). Below 7 MB: the stock firmware's data (factory calibration, Chinese font...), read
// with the raw functions only. From 7 MB: the window of this firmware (and of OpenDM32: codeplug, settings...),
// addressed from 0 by flash_read() / flash_write().
#define FLASH_SIZE        0x1000000
#define FLASH_WINDOW_BASE 0x700000
#define FLASH_WINDOW_SIZE (FLASH_SIZE - FLASH_WINDOW_BASE)
#define FLASH_SECTOR_SIZE 4096
#define FLASH_PAGE_SIZE   256

bool flash_init(void);        // true for a 25Q128 (Winbond 0x4018, Puya PY25Q128HA 0x2018)
uint32_t flash_part_id(void); // JEDEC memory type and capacity, e.g. 0x4018

bool flash_read_raw(uint32_t address, uint8_t *data, uint32_t length);
bool flash_write_sector_raw(uint32_t address, const uint8_t *data); // a whole 4 KB sector

// In the window. A write only erases a sector whose data change; between flash_batch_start() and flash_batch_end(),
// the writes to one sector are programmed once.
bool flash_read(uint32_t address, uint8_t *data, uint32_t length);
bool flash_write(uint32_t address, const uint8_t *data, uint32_t length);
void flash_batch_start(void);
bool flash_batch_end(void);

#endif
