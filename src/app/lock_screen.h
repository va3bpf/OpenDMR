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

#ifndef DM32_LOCK_SCREEN_H
#define DM32_LOCK_SCREEN_H

#include <stdbool.h>
#include "ui.h"

// The keypad and PTT lock (uiLockScreen.c). The main menu locks: * the keypad, # toggles the PTT lock.
void lock_screen_lock(bool keypad, bool ptt); // goes home and shows "Locked" a moment; false, false: unlocks
void lock_screen_unlock(void);                // shows "Unlocked" (the PIN screen, after the lock PIN)
// At start: locked when the lock PIN is on (not the power-on PIN); the lock screen to show over the home screen,
// or NULL
const ui_screen_t *lock_screen_start(void);
bool lock_screen_is_keypad_locked(void);
bool lock_screen_is_ptt_locked(void); // for the transmit code: no transmit while true

// First thing in the handle() of the home screens (channel, VFO): true when the lock takes the event (return false
// then). A key while locked shows "Locked / SK2 + * to unlock"; SK2 + * unlocks.
bool lock_screen_filter(const ui_event_t *ev);

#endif
