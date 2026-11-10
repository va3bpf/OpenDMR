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
 * OpenDMR, service (debug): the received AMBE frames to the data flash (rx_capture.h). The decoder fills one of two
 * 4 KB RAM sectors; a low priority task writes a full one, or the one being filled 1 s after the last frame (the end
 * of a call: the rest erased). The HR_C7000 task never waits for the flash: when both sectors wait, frames are dropped
 * (counted in rx_capture_dropped).
 */

#include "rx_capture.h"

#ifdef RX_CAPTURE // make RX_CAPTURE=1

#include <stdbool.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "flash.h"

#define RECORD_SIZE      16
#define RECORDS          (FLASH_SECTOR_SIZE / RECORD_SIZE)
#define IDLE_FLUSH_MS    1000
#define TASK_PRIORITY    1
#define NO_SEQ           0xFFFFFFFFUL
#define SEQ_MASK         0xFFFFFFUL // 3 bytes

static uint8_t buffers[2][FLASH_SECTOR_SIZE];
static volatile int filling;         // the buffer the decoder fills
static volatile int count;           // records in it
static volatile int full = -1;       // a buffer to write, -1: none
static volatile TickType_t last_frame;
static uint32_t seq;
static uint8_t call;
static int next_sector;
uint32_t rx_capture_dropped;         // read over the cable
uint32_t rx_capture_written;
uint8_t rx_capture_read_buffer[1024]; // tools/debug/rx_capture.py: flash_read_raw() into it, then read over the cable

static TaskHandle_t task;
static StaticTask_t task_tcb;
static StackType_t task_stack[256];

void rx_capture_new_call(void)
{
    call++;
}

void rx_capture_frame(const uint8_t ambe[9], int kind)
{
    if (task == NULL) {
        return;
    }
    taskENTER_CRITICAL();
    if (count >= RECORDS) {
        if (full >= 0) {
            rx_capture_dropped++; // both buffers wait for the flash
            taskEXIT_CRITICAL();
            return;
        }
        full = filling;
        filling ^= 1;
        count = 0;
    }
    uint8_t *r = &buffers[filling][count * RECORD_SIZE];
    uint32_t ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    r[0] = seq & 0xFF;
    r[1] = (seq >> 8) & 0xFF;
    r[2] = (seq >> 16) & 0xFF;
    seq = (seq + 1) & SEQ_MASK;
    r[3] = ms & 0xFF;
    r[4] = (ms >> 8) & 0xFF;
    memcpy(r + 5, ambe, 9);
    r[14] = (uint8_t)kind;
    r[15] = call;
    count++;
    last_frame = xTaskGetTickCount();
    bool wake = (count >= RECORDS) && (full < 0);
    if (wake) {
        full = filling;
        filling ^= 1;
        count = 0;
    }
    taskEXIT_CRITICAL();
    if (wake) {
        xTaskNotifyGive(task);
    }
}

// The sector after the one whose first record has the highest sequence number; the numbers go on past that sector
static void find_place(void)
{
    uint32_t best = NO_SEQ;

    next_sector = 0;
    seq = 0;
    for (int s = 0; s < RX_CAPTURE_SECTORS; s++) {
        uint8_t first[3];

        if (flash_read_raw(RX_CAPTURE_BASE + (s * FLASH_SECTOR_SIZE), first, sizeof(first))) {
            uint32_t v = first[0] | (first[1] << 8) | ((uint32_t)first[2] << 16);

            if ((v != SEQ_MASK) && ((best == NO_SEQ) || (v > best))) {
                best = v;
                next_sector = (s + 1) % RX_CAPTURE_SECTORS;
            }
        }
    }
    if (best != NO_SEQ) {
        seq = (best + RECORDS) & SEQ_MASK;
    }
}

static void write_buffer(int b)
{
    flash_write_sector_raw(RX_CAPTURE_BASE + (next_sector * FLASH_SECTOR_SIZE), buffers[b]);
    next_sector = (next_sector + 1) % RX_CAPTURE_SECTORS;
    rx_capture_written++;
}

static void capture_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IDLE_FLUSH_MS / 2));
        int b = -1;

        taskENTER_CRITICAL();
        if (full >= 0) {
            b = full;
        } else if ((count > 0) && ((xTaskGetTickCount() - last_frame) >= pdMS_TO_TICKS(IDLE_FLUSH_MS))) {
            memset(&buffers[filling][count * RECORD_SIZE], 0xFF, (RECORDS - count) * RECORD_SIZE);
            b = filling;
            full = b;
            filling ^= 1;
            count = 0;
        }
        taskEXIT_CRITICAL();
        if (b >= 0) {
            write_buffer(b);
            memset(buffers[b], 0xFF, FLASH_SECTOR_SIZE);
            taskENTER_CRITICAL();
            full = -1;
            taskEXIT_CRITICAL();
        }
    }
}

void rx_capture_start(void)
{
    memset(buffers, 0xFF, sizeof(buffers));
    memset(rx_capture_read_buffer, 0, sizeof(rx_capture_read_buffer)); // referenced: kept by the linker
    find_place();
    task = xTaskCreateStatic(capture_task, "capture", sizeof(task_stack) / sizeof(task_stack[0]), NULL,
                             TASK_PRIORITY, task_stack, &task_tcb);
}

#endif // RX_CAPTURE
