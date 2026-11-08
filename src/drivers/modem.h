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

#ifndef DM32_MODEM_H
#define DM32_MODEM_H

#include <stdbool.h>
#include <stdint.h>

// The baseband of the HR_C7000 SoC, its audio codec and DACs, for FM (OpenDM32 hardware/HR-C7000.c:
// HRC7000Init(), HRC7000SetFMRx() / SetFMTx(), SetFmAudio(), SetFrequencyCalibration(); interfaces/dac.c). The FD6818
// demodulates; its audio goes through the codec ADC, the baseband and the codec DAC to the speaker amplifier, and the
// microphone the other way.

void modem_init(void);
// SIG_CENTER / RF_MOD_BIAS_CTRL from the factory oscillator tune and the bias of the frequency
void modem_set_frequency_calibration(bool valid, uint8_t osc_tune, uint16_t mod_bias);
void modem_fm_receive(bool wide, uint8_t dac_gain); // dac_gain: GCR_DACL (factory_cal_rx_dac_gain())
void modem_fm_transmit(bool wide, uint8_t mic_gain); // mic_gain: GCR_ADCL (settings.mic_gain_fm)
void modem_fm_audio(bool on);                        // the received audio through to the DAC

#define MODEM_BEEP_BLOCK 32 // bytes: 16 samples
void modem_beep(bool on);                            // the prompt path to the DAC (the FM audio path off meanwhile)
bool modem_beep_fifo_full(void);
void modem_beep_write(const uint8_t data[MODEM_BEEP_BLOCK]);
void modem_pa_drive(uint16_t value);                 // the PA bias DAC, 0 - 4095 (0: off)

#endif
