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

#ifndef DM32_SECURITY_H
#define DM32_SECURITY_H

#include <stdbool.h>
#include <stdint.h>

// The power-on PIN and the lock PIN (both off by default), 4 - 6 digits. The power-on PIN is the OpenGD77 one of the
// codeplug (the OpenGD77 CPS sets it in its boot settings); the lock PIN is the firmware's own, the CPS can't change
// it. Setting the PIN on the radio sets both. A firmware update or another firmware turns both off: the way back
// when the PIN is forgotten.
#define SECURITY_PIN_MIN 4
#define SECURITY_PIN_MAX 6

void security_load(void); // at start, after codeplug_init()

bool security_has_pin(void); // one of the two is on (else a new PIN is asked first)
bool security_check_boot_pin(const char *digits); // a NUL terminated string of digits
bool security_check_lock_pin(const char *digits);
bool security_set_pin(const char *digits);   // 4 - 6 digits; false: not valid, or not saved

bool security_boot_pin_on(void);
bool security_lock_pin_on(void);
void security_set_boot_pin(bool on); // saved; on needs a PIN
void security_set_lock_pin(bool on);

void security_clear(void);     // both off (a firmware update starts)
void security_keep_block(uint32_t sector_address, uint8_t *sector); // a CPS sector write keeps the lock PIN

// The radio waits for the power-on PIN (set at start, cleared by the PIN screen): nothing works until then
bool security_boot_locked(void);
void security_boot_unlocked(void);

// The keypad lock with the lock PIN (lock_screen.c): the VOX is off then (radio.c)
void security_set_locked(bool locked);
bool security_vox_blocked(void);

#endif
