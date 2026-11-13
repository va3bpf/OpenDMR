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
 * OpenDMR, service: the HR_C7000 physical layer, receive and transmit (HR_C7000 user guide v2.7, "Physical layer
 * reception" / "transmission", Tables 46 - 49, Figures 70, 71; docs/p25.md). Receive: WORK_MODE 0x02 (continuous),
 * LAYER2_CONTROL 0x40, LAYER2_TXRX_CTRL 0x41; every 30 ms the system interrupt 0x800 with RX_TYPE_INFO and 288 bits in
 * the receive RAM at 0x30 .. 0x53. Transmit: below.
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "dmr_compat.h"
#include "systime.h"
#include "radio.h"
#include "imbe.h"
#include "p25_frame.h"
#include "p25_phy.h"

#define WORK_MODE_PHY_CONTINUOUS 0x02
#define LAYER2_PHY_RECEIVE       0x40
#define TXRX_PHY_RECEIVE         0x41
#define TXRX_RESET               0x800000 // the receive timing reset (as hr_c7000.c's direct mode receive): the modem
                                          // waits for a sync again
#define SYS_PHY_RX               0x800
#define RX_RAM_INFO              0x30
#define P25_SYNC_H               0x805575F5 // the frame sync 0x5575F5FF77FF, 24 bits a register (Open25-DM32)
#define P25_SYNC_L               0x00FF77FF

p25_phy_block_t p25_phy_buf[P25_PHY_BLOCKS];
volatile uint32_t p25_phy_count;

static volatile bool active;
static uint32_t limit;
static p25_phy_block_cb stream_cb; // receiving into it (p25_phy_rx_start()), else capturing

// the modem's playback while the physical layer receives (dmr_sound.c send()); tunable over the cable
uint32_t p25_phy_work_mode = WORK_MODE_PHY_CONTINUOUS;

// the registers changed, to put back
static uint32_t saved_work_mode, saved_layer2, saved_txrx, saved_mask;
static uint32_t saved_sync[4];
static bool saved_p25_sync;

static volatile bool tx_active;

bool p25_phy_active(void)
{
    return active || tx_active;
}

bool p25_phy_tx_active(void)
{
    return tx_active;
}

static void restore(void)
{
    *LAYER2_TXRX_CTRL = 0;
    *LAYER2_CONTROL = saved_layer2;
    *WORK_MODE = saved_work_mode;
    if (saved_p25_sync) {
        *RECV_MS_SYNC_H = saved_sync[0];
        *RECV_MS_SYNC_L = saved_sync[1];
        *RECV_BS_SYNC_H = saved_sync[2];
        *RECV_BS_SYNC_L = saved_sync[3];
    }
    *SYS_INTERP_MASK = saved_mask;
    *LAYER2_TXRX_CTRL = saved_txrx;
    stream_cb = NULL;
    active = false;
}

// the registers saved, the physical layer receive on
static void setup(bool p25_sync)
{
    saved_work_mode = *WORK_MODE;
    saved_layer2 = *LAYER2_CONTROL;
    saved_txrx = *LAYER2_TXRX_CTRL;
    saved_mask = *SYS_INTERP_MASK;
    saved_p25_sync = p25_sync;
    saved_sync[0] = *RECV_MS_SYNC_H;
    saved_sync[1] = *RECV_MS_SYNC_L;
    saved_sync[2] = *RECV_BS_SYNC_H;
    saved_sync[3] = *RECV_BS_SYNC_L;

    active = true; // the DMR interrupt handlers stand aside from here (dmr.c)
    *LAYER2_TXRX_CTRL = 0;
    *WORK_MODE = WORK_MODE_PHY_CONTINUOUS;
    *LAYER2_CONTROL = LAYER2_PHY_RECEIVE;
    if (saved_p25_sync) {
        *RECV_MS_SYNC_H = P25_SYNC_H;
        *RECV_MS_SYNC_L = P25_SYNC_L;
        *RECV_BS_SYNC_H = P25_SYNC_H;
        *RECV_BS_SYNC_L = P25_SYNC_L;
    }
    *SYS_INTERP_MASK = saved_mask | SYS_PHY_RX; // a set bit enables the interrupt
    *LAYER2_TXRX_CTRL = TXRX_RESET;
    *LAYER2_TXRX_CTRL = TXRX_PHY_RECEIVE;
}

// Once it found a sync the modem gives a block every 30 ms for good, its timeslot timing running on; after the signal
// that is noise, and a new signal is not aligned to it (a quarter of the blocks then not written). The reset puts it
// back to waiting for a sync (measured: the blocks stop at once).
void p25_phy_rx_rearm(void)
{
    if (active && stream_cb) {
        *LAYER2_TXRX_CTRL = TXRX_RESET;
        *LAYER2_TXRX_CTRL = TXRX_PHY_RECEIVE;
    }
}

int p25_phy_capture(int p25_sync, int blocks)
{
    if (active || tx_active) {
        return -1;
    }
    limit = (blocks > 0 && blocks <= P25_PHY_BLOCKS) ? (uint32_t)blocks : P25_PHY_BLOCKS;
    p25_phy_count = 0;
    memset(p25_phy_buf, 0, sizeof(p25_phy_buf));
    setup(p25_sync != 0);
    return 0;
}

void p25_phy_stop(void)
{
    if (active && !stream_cb) {
        restore();
    }
}

void p25_phy_rx_start(p25_phy_block_cb cb)
{
    if (active || tx_active) {
        return;
    }
    stream_cb = cb;
    setup(true);
}

void p25_phy_rx_stop(void)
{
    if (active && stream_cb) {
        restore();
    }
}

void RAM_CODE p25_phy_isr(void)
{
    uint32_t list = *SYS_INTERP_LIST;
    if ((list & SYS_PHY_RX) && stream_cb) {
        stream_cb(*RX_TYPE_INFO, MODEM_RX_RAM + RX_RAM_INFO);
        return;
    }
    if ((list & SYS_PHY_RX) && (p25_phy_count < limit)) {
        p25_phy_block_t *b = &p25_phy_buf[p25_phy_count];
        b->us = systime_get_us();
        b->type = *RX_TYPE_INFO;
        b->sys = list;
        for (int i = 0; i < P25_PHY_BLOCK_BYTES; i++) {
            b->data[i] = *(MODEM_RX_RAM + RX_RAM_INFO + i);
        }
        p25_phy_count++;
    }
}

// The receive RAM read again at the receive timeslot interrupt, into the last block; the capture ends there once full
// (the last block got its second read too)
void RAM_CODE p25_phy_rx_slot_isr(void)
{
    if (!active || stream_cb || (p25_phy_count == 0)) {
        return;
    }
    p25_phy_block_t *b = &p25_phy_buf[p25_phy_count - 1];
    if (b->us2 == 0) {
        b->us2 = systime_get_us();
        for (int i = 0; i < P25_PHY_BLOCK_BYTES; i++) {
            b->data2[i] = *(MODEM_RX_RAM + RX_RAM_INFO + i);
        }
    }
    if (p25_phy_count >= limit) {
        restore();
    }
}

/* ---- physical layer transmit ----

   The modem sends its send RAM's two 36-byte halves (SEND_DATA0 at 0x00, SEND_DATA1 at 0x24, Table 46) in turn, 30 ms
   each, reading a half as it goes out. Measured (2026-11-12, docs/p25.md, tools/debug/p25_txcheck.py; the steps in the
   OpenDM32 notes of the ai-project-context repository):
   - the reading runs on a timer of the modem's own: started by the first send enable after power on and never stopped,
     not by later enables, not by LAYER2_TXRX_CTRL's resets, not by WORK_MODE / LAYER2_CONTROL. The transmit timeslot
     interrupt (and phy_rdy, the PCM interrupts) start at the enable: where the reading was at the interrupt, and which
     half, came from how long ago the first transmission was (the timer +15 ppm against systime);
   - the physical layer soft reset (SYS_SOFT_RSTN bit 1 low, then high; only status registers change) starts that timer
     again: every transmission is then as the first, the transmit interrupt 10 dibits into half 0 at the even interrupts;
   - so at interrupt n the half being read is n & 1, and the next block goes into the other one, (n + 1) & 1, which
     starts some 28 ms later. Written into the half being read, a block went out with its first dibits from two blocks
     before (the interrupt comes after the modem has read them), or at another reading place with its tail from two
     blocks later: the frame syncs (dibits 108 .. 131 of a block here) gone, nothing received at all.
   This interrupt calls no FreeRTOS function: a task notification from it broke the RF task (no transmission until a
   restart). */

#define LAYER2_PHY_SEND   0x82 // txen, the transmit timeslot established actively (Table 48)
#define TXRX_PHY_SEND     0x81 // send, autotest: the modem keeps sending its halves in turn
#define TX_NAC            0x293
#define TX_MAX_MS         20000
#define TX_FIFO           4096 // dibits: the sender keeps an LDU or more ahead of the interrupt
#define TX_BLOCK_DIBITS   (P25_PHY_BLOCK_BYTES * 4)
#define TX_UNDERRUN_STOP  10
#define TX_RF_SETUP_MS    30   // radio_phy_tx(): the set up and the PA ramp, then the modem
#define SYS_SOFT_RSTN     (volatile uint32_t *)(MODEM_REG_BASE + 0x0000)
#define PHY_SOFT_RSTN     0x02
#define LAYER2_STATUS_REG (volatile uint32_t *)(MODEM_REG_BASE + 0x041C)

volatile uint32_t p25_phy_tx_state;
volatile uint32_t p25_phy_tx_blocks;
volatile uint32_t p25_phy_tx_underrun;
volatile uint32_t p25_phy_tx_xor; // each dibit sent XOR this: 2 turns the symbols over (+3 <-> -3, +1 <-> -1), a test

// the transmit log (tools/debug/p25_txlog.py): the begin, the interrupts, the end
p25_phy_txlog_t p25_phy_txlog[P25_PHY_TXLOG];
volatile uint32_t p25_phy_txlog_n;

static void RAM_CODE txlog(uint8_t event, uint32_t arg)
{
    uint32_t i = p25_phy_txlog_n;
    if (i < P25_PHY_TXLOG) {
        p25_phy_txlog_t *e = &p25_phy_txlog[i];
        e->us = systime_get_us();
        e->status = *LAYER2_STATUS_REG;
        e->sloton = *LAYER2_SLOTON;
        e->event = event;
        e->arg = (uint16_t)arg;
        p25_phy_txlog_n = i + 1;
    }
}

static uint8_t fifo[TX_FIFO];
static volatile uint32_t fifo_in, fifo_out; // dibit counts, wrapping
static uint32_t saved_tx_work_mode, saved_tx_layer2, saved_tx_txrx;

uint32_t p25_phy_tx_level(void)
{
    return fifo_in - fifo_out;
}

uint32_t p25_phy_tx_room(void)
{
    return TX_FIFO - (fifo_in - fifo_out);
}

void p25_phy_tx_put(const uint8_t *d, int n)
{
    for (int i = 0; i < n; i++) {
        fifo[(fifo_in + (uint32_t)i) % TX_FIFO] = d[i];
    }
    fifo_in += (uint32_t)n;
}

// zeros up to a whole block: the interrupt takes whole blocks only, the end of the last data unit (the TDU) would
// stay in the FIFO, zeros sent in its place
void p25_phy_tx_pad(void)
{
    uint32_t rest = (fifo_in - fifo_out) % TX_BLOCK_DIBITS;
    if (rest) {
        for (uint32_t i = rest; i < TX_BLOCK_DIBITS; i++) {
            fifo[fifo_in % TX_FIFO] = 0;
            fifo_in++;
        }
    }
}

// the next 144 dibits as 36 bytes; zeros (and an underrun) if the stream isn't there
static void RAM_CODE next_block(uint8_t *out)
{
    bool have = p25_phy_tx_level() >= TX_BLOCK_DIBITS;
    for (int i = 0; i < P25_PHY_BLOCK_BYTES; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 4; k++) {
            b = (uint8_t)((b << 2) | ((have ? fifo[(fifo_out + (uint32_t)(i * 4 + k)) % TX_FIFO] : 0) ^ (p25_phy_tx_xor & 3)));
        }
        out[i] = b;
    }
    if (have) {
        fifo_out += TX_BLOCK_DIBITS;
    } else {
        p25_phy_tx_underrun++;
    }
}

static void RAM_CODE write_half(int half, const uint8_t *b)
{
    for (int i = 0; i < P25_PHY_BLOCK_BYTES; i++) {
        *(MODEM_TX_RAM + half * P25_PHY_BLOCK_BYTES + i) = b[i];
    }
}

// the transmit timeslot interrupt (dmr.c timeslot_tx_isr()): half n & 1 is being sent, the next block into the other
void RAM_CODE p25_phy_tx_isr(void)
{
    uint8_t b[P25_PHY_BLOCK_BYTES];
    uint32_t n = p25_phy_tx_blocks++;

    txlog(1, n);
    next_block(b);
    write_half((int)((n + 1) & 1), b);
}

void p25_phy_tx_reset(void)
{
    fifo_in = fifo_out = 0;
    p25_phy_tx_blocks = p25_phy_tx_underrun = 0;
}

int p25_phy_tx_begin(bool low_power)
{
    if (active || tx_active) {
        return -1;
    }
    p25_phy_txlog_n = 0;
    txlog(0, 0);
    tx_active = true; // the DMR engine stands aside (dmr.c)
    radio_phy_tx(true, low_power);
    vTaskDelay(pdMS_TO_TICKS(TX_RF_SETUP_MS));
    saved_tx_work_mode = *WORK_MODE;
    saved_tx_layer2 = *LAYER2_CONTROL;
    saved_tx_txrx = *LAYER2_TXRX_CTRL;
    *LAYER2_TXRX_CTRL = 0;
    uint32_t rst = *SYS_SOFT_RSTN; // the modem's send timing from the start (above)
    *SYS_SOFT_RSTN = rst & ~PHY_SOFT_RSTN;
    *SYS_SOFT_RSTN = rst | PHY_SOFT_RSTN;
    *WORK_MODE = WORK_MODE_PHY_CONTINUOUS;
    *LAYER2_CONTROL = LAYER2_PHY_SEND;
    uint8_t b[P25_PHY_BLOCK_BYTES];
    next_block(b); // the first block in both halves: half 0 goes first, half 1 is written at the first interrupt
    write_half(0, b);
    write_half(1, b);
    p25_phy_tx_state = 2;
    *LAYER2_TXRX_CTRL = TXRX_PHY_SEND;
    txlog(0, 1);
    return 0;
}

void p25_phy_tx_end(void)
{
    if (!tx_active) {
        return;
    }
    txlog(0, 2);
    *LAYER2_TXRX_CTRL = 0;
    // The physical layer reset again: else the modulator keeps its last output after the send, and the next signal
    // received (one call, whatever its length) came with every level a step up, +1 taken as +3 (~10 % of the symbols
    // wrong; measured 2026-11-12: 6 calls in 8 without it, none in 10 with it)
    uint32_t rst = *SYS_SOFT_RSTN;
    *SYS_SOFT_RSTN = rst & ~PHY_SOFT_RSTN;
    *SYS_SOFT_RSTN = rst | PHY_SOFT_RSTN;
    *LAYER2_CONTROL = saved_tx_layer2;
    *WORK_MODE = saved_tx_work_mode;
    *LAYER2_TXRX_CTRL = saved_tx_txrx;
    radio_phy_tx(false, false);
    vTaskDelay(pdMS_TO_TICKS(TX_RF_SETUP_MS));
    tx_active = false;
    p25_phy_tx_state = (p25_phy_tx_underrun >= TX_UNDERRUN_STOP) ? 4 : 3;
}

/* ---- the transmit test (tools/debug/p25_phy.py txtest) ---- */

static StaticTask_t tx_tcb;
static StackType_t tx_stack[512];
static TaskHandle_t tx_task;
static uint32_t tx_delay_ms, tx_ms;
static uint8_t unit[P25_FRAME_MAX];

void p25_phy_tx_frame(uint32_t n, uint8_t u88[88])
{
    uint32_t x = 0x25u + n * 0x9E3779B9u;
    for (int i = 0; i < 88; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        u88[i] = (uint8_t)(x & 1);
    }
}

// the next data unit of the test call into the FIFO: k counts the LDUs
static void add_ldu(uint32_t k)
{
    uint8_t imbe[9][18];
    p25_lc lc;
    p25_es es;

    for (int f = 0; f < 9; f++) {
        uint8_t u[88];
        p25_phy_tx_frame(k * 9 + (uint32_t)f, u);
        imbe_encode_frame(u, imbe[f]);
    }
    memset(&lc, 0, sizeof(lc));
    lc.lco = 0x00;
    lc.tgid = 1;
    lc.src = 1234567;
    p25_lc_pack(&lc);
    memset(&es, 0, sizeof(es));
    es.algid = P25_ALGID_CLEAR;
    uint8_t lsd[2] = {(uint8_t)k, (uint8_t)(k >> 8)};
    int n = p25_tx_ldu(unit, TX_NAC, (k & 1) ? P25_DUID_LDU2 : P25_DUID_LDU1, (const uint8_t(*)[18])imbe, &lc, &es,
                       lsd, 2);
    p25_phy_tx_put(unit, n);
}

static void tx_task_main(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        p25_phy_tx_state = 1;
        vTaskDelay(pdMS_TO_TICKS(tx_delay_ms));

        p25_es es;
        memset(&es, 0, sizeof(es));
        es.algid = P25_ALGID_CLEAR;
        p25_phy_tx_reset();
        p25_phy_tx_put(unit, p25_tx_hdu(unit, TX_NAC, &es, 0, 1, 2));
        uint32_t k = 0;
        add_ldu(k++);
        add_ldu(k++);
        if (p25_phy_tx_begin(true) != 0) {
            p25_phy_tx_state = 5;
            continue;
        }
        TickType_t start = xTaskGetTickCount();
        bool ended = false;
        while (p25_phy_tx_underrun < TX_UNDERRUN_STOP) {
            if (!ended && (xTaskGetTickCount() - start) >= pdMS_TO_TICKS(tx_ms)) {
                if (k & 1) {
                    add_ldu(k++); // end on an LDU2
                }
                p25_phy_tx_put(unit, p25_tx_tdu(unit, TX_NAC, 2));
                p25_phy_tx_pad();
                ended = true;
            }
            if (!ended && p25_phy_tx_room() > P25_FRAME_MAX) {
                add_ldu(k++);
                continue;
            }
            if (ended && p25_phy_tx_level() < TX_BLOCK_DIBITS) {
                vTaskDelay(pdMS_TO_TICKS(60)); // the last blocks out of the modem
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        p25_phy_tx_end();
    }
}

int p25_phy_tx_test(int delay_ms, int ms, int write_mode)
{
    if (active || tx_active || (p25_phy_tx_state == 1) || (p25_phy_tx_state == 2)) {
        return -1;
    }
    tx_delay_ms = (delay_ms > 0) ? (uint32_t)delay_ms : 0;
    tx_ms = (ms > 0 && ms <= TX_MAX_MS) ? (uint32_t)ms : TX_MAX_MS;
    (void)write_mode; // the halves are written one way (p25_phy_tx_isr())
    if (!tx_task) {
        tx_task = xTaskCreateStatic(tx_task_main, "p25tx", sizeof(tx_stack) / sizeof(tx_stack[0]), NULL,
                                    tskIDLE_PRIORITY + 2, tx_stack, &tx_tcb);
    }
    p25_phy_tx_state = 1;
    xTaskNotifyGive(tx_task);
    return 0;
}
