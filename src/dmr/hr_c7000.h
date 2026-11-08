/*
 * Copyright (C) 2019      Kai Ludwig, DG4KLU
 * Copyright (C) 2020-2024 Roger Clark, VK3KYY / G4KYF
 *                         Daniel Caujolle-Bert, F1RMB
 *
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden. This includes, without limitation,
 *    incorporation in a commercial product or incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef _OPENGD77_HR_C7000_H_
#define _OPENGD77_HR_C7000_H_

#include "dmr_compat.h"
//#include "interfaces/hr-c6000_spi.h"
//#include "dmr_codec/codec.h"

#define DMR_FRAME_BUFFER_SIZE     64

#define AMBE_AUDIO_LENGTH         27
#define LC_DATA_LENGTH            12
#define MODEM_VOICE      (volatile uint8_t *)(0x16000960)
#define RECV_INFO_COUNT_FSK_REG (*(volatile uint32_t*)(USB_BASE + 0x164))

#define TG_CALL_FLAG            0x00
#define PC_CALL_FLAG            0x03

extern volatile int slotState;
extern volatile uint8_t DMR_frame_buffer[DMR_FRAME_BUFFER_SIZE];
extern volatile bool updateLastHeard;
extern volatile int dmrMonitorCapturedTS;
extern volatile ticksTimer_t readDMRRSSITimer;

enum DMR_SLOT_STATE { DMR_STATE_IDLE, DMR_STATE_RX_1, DMR_STATE_RX_2, DMR_STATE_RX_END,
					  DMR_STATE_TX_START_1, DMR_STATE_TX_START_2, DMR_STATE_TX_START_3, DMR_STATE_TX_START_4, DMR_STATE_TX_START_5,
					  DMR_STATE_TX_1, DMR_STATE_TX_2, DMR_STATE_TX_END_1, DMR_STATE_TX_END_2, DMR_STATE_TX_END_3_RMO, DMR_STATE_TX_END_3_DMO,
					  DMR_STATE_REPEATER_WAKE_1, DMR_STATE_REPEATER_WAKE_2, DMR_STATE_REPEATER_WAKE_3,
					  DMR_STATE_REPEATER_WAKE_FAIL_1, DMR_STATE_REPEATER_WAKE_FAIL_2 };

enum WakingMode { WAKING_MODE_NONE, WAKING_MODE_WAITING, WAKING_MODE_AWAKEN, WAKING_MODE_FAILED };

enum DMR_Embedded_Data
{
	DMR_EMBEDDED_DATA_GROUP               = 0U,
	DMR_EMBEDDED_DATA_USER_USER           = 3U,
	DMR_EMBEDDED_DATA_TALKER_ALIAS_HEADER = 4U,
	DMR_EMBEDDED_DATA_TALKER_ALIAS_BLOCK1 = 5U,
	DMR_EMBEDDED_DATA_TALKER_ALIAS_BLOCK2 = 6U,
	DMR_EMBEDDED_DATA_TALKER_ALIAS_BLOCK3 = 7U,
	DMR_EMBEDDED_DATA_GPS_INFO            = 8U
};

typedef struct
{
	uint8_t Mode;
	uint8_t Dev;
	uint8_t D1;
} HRC7000_Tone1Config_t;

void PORTC_IRQHandler(void);
void hrc7000Tick(void);

void HRC7000Init(void);
void HRC7000InitInterrupts(void);
void HRC7000InitDigitalDmrRx(void);
void HRC7000SetRepeaterMode(bool rmo);
void HRC7000UpdateRepeaterMode(void); // after a change of channel: repeater mode (Rx != Tx) set up as the stock firmware
void HRC7000ResetTimeSlotDetection(void);
void HRC7000InitDigital(void);
void HRC7000TerminateDigital(void);
void HRC7000InitTask(void);
void HRC7000ResyncTimeSlot(void);
uint32_t HRC7000GetReceivedTgOrPcId(void);
uint32_t HRC7000GetReceivedSrcId(void);
void HRC7000ClearTimecodeSynchronisation(void);
void HRC7000ClearColorCodeSynchronisation(void);
void HRC7000SetTalkerAlias(const char *text);
void HRC7000SetTalkerAliasLocation(uint32_t Lat, uint32_t Lon);
bool HRC7000IRQHandlerIsRunning(void);
bool HRC7000HasGotSync(void);
bool HRC7000CCIsHeld(void);
void HRC7000SetDmrRxGain(int8_t gain);
void HRC7000SetDmrAGCGain(int8_t gain);
void HRC7000ClearIsWakingState(void);
int HRC7000GetIsWakingState(void);
void HRC7000ClearActiveDMRID(void);
bool HRC7000CheckTalkGroupFilter(void);
void HRC7000SetMicGainDMR(uint8_t gain);
void HRC7000FlushMusic(void);

void HRC7000SetMicGainFM(uint8_t gain);
void HRC7000SetFMTx(void);
void HRC7000SetFMRx(void);
void HRC7000SetDMR(void);;
void HRC7000SetTxCTCSS(uint8_t index);
void HRC7000SetTxDCS(uint16_t code,bool inverted);
void HRC7000SetRxCTCSS(uint8_t index);
void HRC7000SetRxDCS(uint16_t code, bool inverted);
bool HRC7000CheckCSS(void);
void HRC7000SetMicGainFM(uint8_t gain);
void HRC7000SetLineOut(bool isOn);
void HRC7000SetFmAudio(bool isOn);
void HRC7000SetFrequencyCalibration(uint16_t modBias);
void HRC7000SetDMRModemCalibration(void);
void hrc7000TxInterruptHandler(void);
void hrc7000RfRxInterruptHandler(void);
void hrc7000TraceRf(uint32_t kind); // the RF task's PA switching in the Tx trace
#define DM32_TRACE(id)
void HRC7000GetTone1Config(HRC7000_Tone1Config_t *cfg);
void HRC7000SetTone1Config(HRC7000_Tone1Config_t *cfg);
void HRC7000SetDTMF(uint8_t code);
void HRC7000InitDTMF(void);
void HRC7000DTMFoff(bool enableMic);
void hrc7000SysInterruptHandler(void);
void hrc7000TimeslotInterruptHandler(void);
void hrc7000TxInterruptHandler(void);
void hrc7000SetInIRQHandler(bool in);
void HRC7000SendTone(int tonefreq);
void HRC7000MuteFmAudio(bool ismute);
void HRC7000SetMic(bool isOn);

#endif /* _OPENGD77_HR_C7000_H_ */
