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

#ifndef DM32_TX_SCREEN_H
#define DM32_TX_SCREEN_H

#include "codeplug.h"

// The transmit screen (docs/ui-design.md 5.3, uiTxScreen.c): pushed by the home screens on the PTT press, it transmits
// until the PTT is released or the channel's timeout; or it says why it can't (Rx only, out of band, DMR).
// label: the zone line; title: the channel or VFO name; the channel is the screen's (ui_screen_t.channel), tuned
// first (ui_tune()).
void tx_screen_open(const char *label, const char *title, const codeplug_channel_t *channel);

// Sends a DTMF sequence on the radio's channel (uiUtilities.c dtmfSequencePrepare() / dtmfProcess()): transmits,
// keys the digits with the codeplug's DTMF timing, ends by itself (RED stops it). code: 0 - 15 ("0123456789ABCD*#"),
// 0xFF after the last one, up to CODEPLUG_DTMF_CODE_LENGTH. FM channels only: false on DMR or without a channel.
bool tx_screen_send_dtmf(const uint8_t *code);

#endif
