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

#ifndef DM32_VFO_SCREEN_H
#define DM32_VFO_SCREEN_H

#include <stdbool.h>
#include "ui.h"
#include "codeplug.h"

// The VFO screen (docs/ui-design.md 5.2): the home screen in VFO mode
extern const ui_screen_t vfo_screen;

// settings.initial_menu_number: the home screen at power on (the main firmware's UI_VFO_MODE and UI_CHANNEL_MODE)
#define VFO_SCREEN_MENU_VFO     35
#define VFO_SCREEN_MENU_CHANNEL 36

// The step of a VFO or channel: index (vfo_flag5 >> 4) into the steps, in 10 Hz (VFO_FREQ_STEP_TABLE)
#define VFO_SCREEN_STEPS 8
extern const uint16_t vfo_screen_steps[VFO_SCREEN_STEPS];

bool vfo_screen_is_home(void); // VFO mode (else channel mode)
void vfo_screen_show(void);    // makes the VFO screen the home screen and shows it
void vfo_screen_leave(void);   // back to the channel screen as the home screen

// The VFOs in RAM (0: A, 1: B), read from the codeplug at the first use; settings.current_vfo_number is the one in use
int vfo_screen_current(void);
codeplug_channel_t *vfo_screen_vfo(int vfo);
void vfo_screen_save(int vfo); // after a change from another screen: written to the codeplug at once
void vfo_screen_tone_scan(int type); // the quick menu: the receive tone scanned (0 all, 1 CTCSS, 2 DCS, 3 iDCS)

#endif
