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

#ifndef DM32_CONTACTS_H
#define DM32_CONTACTS_H

#include <stdbool.h>
#include <stdint.h>
#include "list_screen.h"

// The contacts menu and its screens (menuContactList.c, menuContactDetails.c, the contacts menu of menuSystem.c)
extern const ui_list_t contacts_menu;            // main menu "Contacts" (UI_ITEM_LIST)
extern const ui_screen_t contact_list_screen;    // arg: from contacts_menu only
extern const ui_screen_t contact_details_screen; // arg: from the list and contacts_menu only

// The contact quick list (SK2 + # on the channel screen): the DMR contacts on a DMR channel, the DTMF contacts on an
// FM one. The left key takes the contact for transmitting at once (DTMF: sends its code)
void contacts_open_quick_list(bool dtmf);

// Transmit to this talk group or private call from now on (the override of the channel's contact), on this timeslot
// (1, 2; 0: the channel's). For the contact lists, the last heard list, the TG / PC entry
void contacts_set_tx(uint32_t id, bool private_call, int timeslot);

#endif
