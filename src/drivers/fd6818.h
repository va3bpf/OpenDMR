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

#ifndef DM32_FD6818_H
#define DM32_FD6818_H

#include <stdbool.h>
#include <stdint.h>

// The FD6818B RF transceiver (an AT1846S relative; OpenDM32 hardware/AT1846S.c and radioHardwareInterface.c):
// 16-bit registers on a soft SPI (GPIO 0x07 clock, 0x08 data, 0x18 chip select). Only the radio service uses it.
// Frequencies in 10 Hz units (the chip takes them as they are). Tones as the codeplug holds them (services/tones.h).

void fd6818_init(void);      // pins and the power on settings
void fd6818_post_init(void); // after the modem init
void fd6818_set_fm(uint8_t rx_volume); // the FM settings; rx_volume: the factory analog receive level (0x48)
void fd6818_set_dmr(void);             // the DMR settings (bandwidth: DMR whatever "wide" says, until fd6818_set_fm())

void fd6818_write(uint8_t reg, uint16_t value);
uint16_t fd6818_read(uint8_t reg);

// radioSetFrequency(): AF muted, the band, the frequency, the bandwidth, receive on
void fd6818_set_frequency(uint32_t frequency, bool wide);
void fd6818_receive(bool wide);  // receive on (radioSetRx(), radioSetIF())
// Transmit (radioSetTx()), the frequency set before: the transmit settings, then (after the baseband) transmit on
#define FD6818_MIC_GAIN_MAX 31
void fd6818_set_mic_gain(uint8_t gain); // FM: the mic sensitivity, 0 .. 31 (settings.mic_gain_fm, 26 the stock default)
void fd6818_transmit_prepare(void);
void fd6818_transmit(bool wide);
void fd6818_dmr_burst_tx(uint32_t frequency); // DMR, each burst (the stock RF task)
void fd6818_dmr_burst_rx(uint32_t frequency);
// While transmitting: Tone1 (and Tone2, for DTMF) on the air, the mic off; 0 Hz: that one silent
void fd6818_tx_tone(uint16_t hz1, uint16_t hz2, uint8_t gain);
void fd6818_tx_tone_end(void);                  // the mic back
void fd6818_tx_tone_hz(uint16_t hz);             // Tone1 to another frequency (the APRS AFSK), its phase goes on
void fd6818_tx_tail(bool dcs, bool phase_shift); // the tail: DCS 134.4 Hz; CTCSS shifted 240 degrees, or 55 Hz
void fd6818_tx_tail_end(void);                  // after the transmission
void fd6818_tx_fsk(const uint8_t *data, uint32_t length, bool wide); // FFSK 1200 / 1800 Hz (MDC1200), waits till sent
void fd6818_af_out(bool on);     // the receive audio out (the squelch)

// Signal and noise, raw (RSSI 0x67 / 2, noise 0x65 low byte); from RAM, in a critical section a register
void fd6818_read_signal(uint8_t *signal, uint8_t *noise);

void fd6818_set_rx_tone(uint16_t tone); // CODEPLUG_CSS_NONE: off
void fd6818_set_tx_tone(uint16_t tone);
bool fd6818_rx_tone_detected(uint16_t tone);

#endif
