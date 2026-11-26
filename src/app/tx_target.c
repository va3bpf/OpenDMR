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
 * OpenDMR: the DMR call target of the channel or VFO shown (tx_target.h).
 */

#include <stdio.h>
#include <string.h>
#include "settings.h"
#include "codeplug.h"
#include "codeplug_text.h"
#include "text.h"
#include "vfo_screen.h"
#include "dmr.h"
#include "tx_target.h"
#include "p25_radio.h"
#include "nxdn_radio.h"
#include "ysf/ysf_radio.h"
#include "m17_radio.h"
#include "nxdn_id.h"

#define DMR_DEFAULT_TG 9 // a DMR channel or VFO without a contact transmits to it

#define TS_MASK           0x03
#define TS_CONTACT_SHIFT  6    // tsSetContactHasBeenOverriden(): a bit for each source after the 3 x 2 bits
#define FLAG2_TIMESLOT_2  0x40

tx_target_source_t tx_target_source(void)
{
    return vfo_screen_is_home() ? (tx_target_source_t)vfo_screen_current() : TX_TARGET_CHANNEL;
}

// The TG / PC entered or picked: a codeplug channel's for now (RAM, tx_target_channel_forget()), the VFOs' in the
// settings (kept over power off, as before: both VFOs share it)
static uint32_t channel_override_tg;

static uint32_t *override_tg(void)
{
    return (tx_target_source() == TX_TARGET_CHANNEL) ? &channel_override_tg : &settings.override_tg;
}

static int manual_timeslot(void)
{
    return (settings.ts_manual_override >> (2 * tx_target_source())) & TS_MASK;
}

static bool contact_overridden(void)
{
    return (settings.ts_manual_override >> (TS_CONTACT_SHIFT + tx_target_source())) & 1;
}

static void set_contact_overridden(bool on)
{
    uint16_t bit = (uint16_t)(1U << (TS_CONTACT_SHIFT + tx_target_source()));

    settings.ts_manual_override = on ? (settings.ts_manual_override | bit) : (settings.ts_manual_override & ~bit);
}

// The RX group's TG at current_index_in_trx_group_list, else the channel's contact; false: none
static bool channel_contact(const codeplug_channel_t *channel, codeplug_contact_t *contact)
{
    codeplug_rx_group_t group;
    int16_t *index = &settings.current_index_in_trx_group_list[(tx_target_source() + 1) % 3];

    if ((channel->rx_group_list != 0) && codeplug_get_rx_group(channel->rx_group_list, &group) &&
        (group.contact_count > 0)) {
        if ((*index < 0) || (*index >= group.contact_count)) {
            *index = 0;
        }
        if (codeplug_get_contact(group.contacts[*index], contact)) {
            return true;
        }
    }
    return (channel->contact != 0) && codeplug_get_contact(channel->contact, contact);
}

void tx_target_get(const codeplug_channel_t *channel, tx_target_t *target)
{
    codeplug_contact_t contact;
    int manual = manual_timeslot();
    int contact_ts = 0;

    memset(target, 0, sizeof(*target));
    uint32_t picked = *override_tg();

    if (picked != 0) {
        bool pc = ((picked >> 24) == TX_TARGET_PC_FLAG);
        uint32_t number = picked & CODEPLUG_CONTACT_ID_MAX;
        int index = codeplug_contact_find(number, (pc ? CODEPLUG_CALL_PRIVATE : CODEPLUG_CALL_GROUP), 0);

        target->id = picked;
        target->overridden = true;
        if ((index != 0) && codeplug_get_contact(index, &contact)) {
            codeplug_text_name(contact.name, CODEPLUG_NAME_LENGTH, target->name, sizeof(target->name));
        } else if (!pc && (number == CODEPLUG_CONTACT_ID_MAX)) {
            snprintf(target->name, sizeof(target->name), "%s", text(TEXT_ALL_CALL)); // the number entry's all call
        } else {
            snprintf(target->name, sizeof(target->name), "%s %lu", text(pc ? TEXT_PC : TEXT_TG),
                     (unsigned long)number);
        }
    } else if (channel_contact(channel, &contact)) {
        target->id = contact.id | ((contact.call_type == CODEPLUG_CALL_PRIVATE) ? ((uint32_t)TX_TARGET_PC_FLAG << 24) : 0);
        codeplug_text_name(contact.name, CODEPLUG_NAME_LENGTH, target->name, sizeof(target->name));
        contact_ts = codeplug_contact_timeslot(&contact);
    }

    if (tx_target_source() == TX_TARGET_CHANNEL) {
        // A codeplug channel: its own timeslot, unless one was set here for now (*, a contact picked, the RX group's
        // TG stepped to: tx_target_channel_forget() drops it). The channel's contact doesn't override it.
        target->timeslot = (manual != 0) ? manual : ((channel->flag2 & FLAG2_TIMESLOT_2) ? 2 : 1);
    } else if (contact_ts != 0) { // a VFO: the contact's own timeslot, unless * changed it
        target->timeslot = (contact_overridden() && (manual != 0)) ? manual : contact_ts;
    } else {
        target->timeslot = (manual != 0) ? manual : ((channel->flag2 & FLAG2_TIMESLOT_2) ? 2 : 1);
    }
}

void tx_target_apply(codeplug_channel_t *channel, tx_target_t *target)
{
    memset(target, 0, sizeof(*target));
    if (channel->mode == CODEPLUG_MODE_P25) {
        // P25: the same target (a TG entered, the RX group's, the contact), group calls of 16 bits; the channel's NAC
        tx_target_get(channel, target);
        bool group = ((target->id >> 24) != TX_TARGET_PC_FLAG) && ((target->id & CODEPLUG_CONTACT_ID_MAX) <= 0xFFFF);
        if (!group || (target->id == 0)) { // none, a private call, over 16 bits: TG 1 (p25_radio_set_target())
            memset(target, 0, sizeof(*target));
            target->id = 1;
            target->timeslot = 1;
            snprintf(target->name, sizeof(target->name), "%s 1", text(TEXT_TG));
        }
        p25_radio_set_target(codeplug_p25_nac(channel), (uint16_t)target->id);
        return;
    }
    if (channel->mode == CODEPLUG_MODE_NXDN) {
        // NXDN: the same target, group or individual calls of 16 bits (none or over 16 bits: TG 1); the channel's
        // RAN, our NXDN ID (nxdn_id.h)
        tx_target_get(channel, target);
        bool pc = ((target->id >> 24) == TX_TARGET_PC_FLAG);
        uint32_t id = target->id & CODEPLUG_CONTACT_ID_MAX;
        if ((id == 0) || (id > 0xFFFF)) {
            memset(target, 0, sizeof(*target));
            target->id = 1;
            target->timeslot = 1;
            snprintf(target->name, sizeof(target->name), "%s 1", text(TEXT_TG));
            pc = false;
            id = 1;
        }
        nxdn_radio_set_target(codeplug_nxdn_ran(channel), nxdn_id_get(), (uint16_t)id, pc);
        return;
    }
    if (channel->mode == CODEPLUG_MODE_YSF) {
        // YSF Fusion: the channel's DG-ID, and the target's name as the callsign to call (ALL when none).
        tx_target_get(channel, target);
        if (target->id == CODEPLUG_CONTACT_ID_MAX) {
            // The # entry's All Call: on the air YSF's group call is "ALL", not the localized UI string.
            snprintf(target->name, sizeof(target->name), "ALL");
        }
        ysf_radio_set_target(codeplug_ysf_dgid(channel), target->name[0] ? target->name : "ALL");
        return;
    }
    if (channel->mode == CODEPLUG_MODE_M17) {
        m17_radio_set_can(codeplug_m17_can(channel)); // to ALL (m17_radio.h)
        return;
    }
    if (channel->mode != CODEPLUG_MODE_DMR) {
        return;
    }
    tx_target_get(channel, target);
    if ((target->id & CODEPLUG_CONTACT_ID_MAX) == 0) {
        // No contact (an FM channel switched to DMR with the keys, a DMR channel without one): TG 9, the local group,
        // shown on the channel. TG 0 isn't a call: receivers don't show it, repeaters and BrandMeister drop it
        target->id = DMR_DEFAULT_TG;
        snprintf(target->name, sizeof(target->name), "%s %d", text(TEXT_TG), DMR_DEFAULT_TG);
    }
    channel->flag2 = (uint8_t)((target->timeslot == 2) ? (channel->flag2 | FLAG2_TIMESLOT_2)
                                                       : (channel->flag2 & ~FLAG2_TIMESLOT_2));
    dmr_set_tx_id(target->id);
}

void tx_target_set(uint32_t id, bool private_call)
{
    *override_tg() = (id & CODEPLUG_CONTACT_ID_MAX) | (private_call ? ((uint32_t)TX_TARGET_PC_FLAG << 24) : 0);
    settings_changed();
}

void tx_target_set_timeslot(int timeslot)
{
    int shift = 2 * tx_target_source();

    settings.ts_manual_override =
        (uint16_t)((settings.ts_manual_override & ~(TS_MASK << shift)) | ((timeslot & TS_MASK) << shift));
    settings_changed();
}

void tx_target_toggle_timeslot(const codeplug_channel_t *channel)
{
    tx_target_t target;
    codeplug_contact_t contact;

    tx_target_get(channel, &target);
    tx_target_set_timeslot(3 - target.timeslot);
    if ((tx_target_source() != TX_TARGET_CHANNEL) && (*override_tg() == 0) && channel_contact(channel, &contact) &&
        (codeplug_contact_timeslot(&contact) != 0)) {
        set_contact_overridden(true);
    }
}

void tx_target_clear_timeslot(void)
{
    tx_target_set_timeslot(0);
    set_contact_overridden(false);
}

// LEFT / RIGHT (uiChannelMode.c): the next TG of the RX group list, and no override any more. On a codeplug channel
// the TG stepped to is a pick for now, as one from the contacts: its own timeslot if it has one, else the channel's.
int tx_target_step_group(const codeplug_channel_t *channel, int step)
{
    codeplug_rx_group_t group;
    codeplug_contact_t contact;
    int16_t *index = &settings.current_index_in_trx_group_list[(tx_target_source() + 1) % 3];
    int stepped = -1;

    if ((channel->rx_group_list != 0) && codeplug_get_rx_group(channel->rx_group_list, &group) &&
        (group.contact_count > 1) && (*override_tg() == 0)) {
        *index = (int16_t)((((*index + step) % group.contact_count) + group.contact_count) % group.contact_count);
        stepped = *index;
        if ((tx_target_source() == TX_TARGET_CHANNEL) && codeplug_get_contact(group.contacts[*index], &contact)) {
            tx_target_set_timeslot(codeplug_contact_timeslot(&contact)); // 0: the channel's
        }
    }
    *override_tg() = 0;
    set_contact_overridden(false); // updateTrxID()
    settings_changed();
    return stepped;
}

// What was picked for a codeplug channel lasts until another channel, the VFO, or power off: the TG / PC picked, the
// RX group's TG stepped to, the timeslot set (channel_screen.c). The last two are in the settings meanwhile, so dropped
// here (the VFOs' own are left alone)
void tx_target_channel_forget(void)
{
    uint16_t channel_bits = (uint16_t)((TS_MASK << (2 * TX_TARGET_CHANNEL)) | (1U << (TS_CONTACT_SHIFT + TX_TARGET_CHANNEL)));
    int16_t *index = &settings.current_index_in_trx_group_list[(TX_TARGET_CHANNEL + 1) % 3];

    channel_override_tg = 0;
    if (((settings.ts_manual_override & channel_bits) != 0) || (*index != 0)) {
        settings.ts_manual_override = (uint16_t)(settings.ts_manual_override & ~channel_bits);
        *index = 0;
        settings_changed();
    }
}
