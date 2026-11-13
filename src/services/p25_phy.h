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

#ifndef P25_PHY_H
#define P25_PHY_H

#include <stdbool.h>
#include <stdint.h>

// The HR_C7000 physical layer receive (docs/p25.md): the demodulated bits as they are, 36 bytes every 30 ms, for P25.
// For now an experiment over the cable (tools/debug/p25_phy.py): a DMR channel set, p25_phy_capture() switches the
// modem to the physical layer, records the blocks into p25_phy_buf and switches back when it is full.

#define P25_PHY_BLOCK_BYTES 36
#define P25_PHY_BLOCKS      64 // 1.9 s

typedef struct {
    uint32_t us;    // systime_get_us() at the system interrupt
    uint32_t type;  // RX_TYPE_INFO
    uint32_t sys;   // SYS_INTERP_LIST
    uint32_t us2;   // at the receive timeslot interrupt after it (0: none)
    uint8_t data[P25_PHY_BLOCK_BYTES];  // receive RAM 0x30 .. 0x53, as the modem gives them (Figure 70's order)
    uint8_t data2[P25_PHY_BLOCK_BYTES]; // the same, read again at the receive timeslot interrupt
} p25_phy_block_t;

extern p25_phy_block_t p25_phy_buf[P25_PHY_BLOCKS];
extern volatile uint32_t p25_phy_count; // blocks recorded

// Starts recording `blocks` blocks (at most P25_PHY_BLOCKS); p25_sync: the sync detection on the P25 frame sync,
// else the DMR syncs set up as they are. Returns 0, -1 if already running.
int p25_phy_capture(int p25_sync, int blocks);
void p25_phy_stop(void);
bool p25_phy_active(void);

// Receive into a function (the P25 receiver, p25_radio.c): the P25 frame sync, every 30 ms block from the interrupt
// (RX_TYPE_INFO, the 36 bytes of the receive RAM in Figure 70's order). Stopped by p25_phy_rx_stop().
typedef void (*p25_phy_block_cb)(uint32_t type, const volatile uint8_t *data);
void p25_phy_rx_start(p25_phy_block_cb cb);
void p25_phy_rx_stop(void);
// back to waiting for a sync (the modem's receive timing reset)
void p25_phy_rx_rearm(void);
// WORK_MODE for the modem's playback meanwhile (dmr_sound.c), tunable over the cable
extern uint32_t p25_phy_work_mode;

// The modem's system interrupt while active (dmr.c system_isr()); its receive timeslot interrupt (timeslot_rx_isr())
void p25_phy_isr(void);
void p25_phy_rx_slot_isr(void);

// Physical layer transmit test (tools/debug/p25_phy.py txtest): after delay_ms, a P25 call on the channel's Tx frequency
// at 50 mW for ms (at most 20 s): HDU, LDU1 / LDU2 pairs, TDU; NAC 0x293, group call TG 1 from 1234567, the IMBE frames'
// 88 bits from xorshift32 seeded 0x25 (p25_phy_tx_frame()). write_mode: not used any more (the halves are written one
// way, p25_phy.c). Returns 0, -1 if the physical layer is busy. Not keyed through radio.c: the radio task may switch
// the RF meanwhile; p25_radio_transmit() through radio_transmit() (the PTT) is the transmission to test.
int p25_phy_tx_test(int delay_ms, int ms, int write_mode);
bool p25_phy_tx_active(void);
// The modem's 30 ms transmit timeslot interrupt while transmitting (dmr.c timeslot_tx_isr())
void p25_phy_tx_isr(void);
// The 88 parameter bits (one a byte) of the test call's IMBE frame n
void p25_phy_tx_frame(uint32_t n, uint8_t u88[88]);

extern volatile uint32_t p25_phy_tx_state;    // 0 idle, 1 waiting, 2 transmitting, 3 done, 4 stopped on underruns
extern volatile uint32_t p25_phy_tx_blocks;   // blocks handed to the modem
extern volatile uint32_t p25_phy_tx_underrun; // blocks sent as zeros, the stream not ready

// The physical layer transmit: data units (dibits, as p25_tx_*() make them) into a FIFO, sent 144 a block from the
// modem's transmit timeslot interrupt (zeros when it runs dry: p25_phy_tx_underrun). p25_phy_tx_begin() (a task: it
// waits for the RF): the RF on the channel's Tx frequency (low_power: 50 mW, else the channel's power), the first
// blocks from the FIFO into the send RAM, the modem sending; -1 if the physical layer is busy. p25_phy_tx_end(): the
// modem as it was, back to receive.
void p25_phy_tx_reset(void); // the FIFO emptied, the counters zeroed
void p25_phy_tx_put(const uint8_t *dibits, int n);
uint32_t p25_phy_tx_level(void); // dibits in the FIFO
void p25_phy_tx_pad(void);        // zeros up to a whole block (after the last data unit)
uint32_t p25_phy_tx_room(void);
int p25_phy_tx_begin(bool low_power);
void p25_phy_tx_end(void);

// The transmit log: event 0 begin / end (arg 0 before the set up, 1 the send enabled, 2 the end), 1 the transmit
// interrupt (arg its number)
#define P25_PHY_TXLOG 64
typedef struct {
    uint32_t us;     // systime_get_us()
    uint32_t status; // LAYER2_STATUS: [31] tx_slot_choose, [8:0] tx_bit_cnt
    uint32_t sloton; // LAYER2_SLOTON
    uint16_t event;
    uint16_t arg;
} p25_phy_txlog_t;
extern p25_phy_txlog_t p25_phy_txlog[P25_PHY_TXLOG];
extern volatile uint32_t p25_phy_txlog_n;

#endif
