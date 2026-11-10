/*
 * Copyright (C) 2019      Kai Ludwig, DG4KLU
 * Copyright (C) 2020-2024 Roger Clark, VK3KYY / G4KYF
 *                         Daniel Caujolle-Bert, F1RMB
 * Copyright (C) 2026      Junqiang Shi, VA3BPF / BG8AM
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

#include <string.h>
#include <stdio.h>
#include "dmr_compat.h"
#include "hr_c7000.h"
#include "systime.h"

#define QSODATA_TIMER_TIMEOUT            2400
#define QSODATA_RX_BEEP_TIMER_TIMEOUT   (QSODATA_TIMER_TIMEOUT - 1000)
// we can't immediately use the LC data out of the chip, as it will return the header from the previous
// reception (except in hotspot mode), and mess up all the lastheard, contact data, display QSO Info
#define QSODATA_THRESHOLD_COUNT             2 // number of frames sequence #1 before sending data to the UI.
#define INTERRUPT_TIMEOUT                 200

#define SYS_INT_SEND_REQUEST_REJECTED    0x80
#define SYS_INT_SEND_START               0x40
#define SYS_INT_SEND_END                 0x20
#define SYS_INT_POST_ACCESS              0x10
#define SYS_INT_RECEIVED_DATA            0x08
#define SYS_INT_RECEIVED_INFORMATION     0x04
#define SYS_INT_ABNORMAL_EXIT            0x02
#define SYS_INT_PHYSICAL_LAYER           0x01

#define WAKEUP_RETRY_PERIOD               600 // The official firmware seems to use a 600mS retry period.

#define NUM_AMBE_BLOCK_PER_DMR_FRAME        3
#define NUM_AMBE_BUFFERS                    4
#define LENGTH_AMBE_BLOCK                   9

#define START_TICK_TIMEOUT                 20
#define END_TICK_TIMEOUT                   13

#define CC_HOLD_TIME                     5000 // 5 second
#define TS_SYNC_STARTUP_TIMEOUT          2500 // 2.5 seconds timeout while synchronizing timeslot
#define TS_SYNC_SCAN_TIMEOUT       (360 + 30) // 1 superframe + 1 TS timeout, for timeslot sync while scanning

#define TS_DISAGREE_THRESHOLD               3
#define TS_STABLE_THRESHOLD                 4
#define TS_IS_LOCKED                        6

#define HS_NUM_OF_SILENCE_SEQ_ON_STARTUP    1 // Hotspot: number of silence sequences (x6 frames) sent to the chip when a transmission is starting (cleaner audio result)

#define SUPERFRAME_NUM_FRAMES               6

#define CC_PROBE_MAX_COUNT                 (4 * 2)
#define CC_PROBE_LOCKED                    (CC_PROBE_MAX_COUNT + 1)

#define HRC7000_TASK_PRIORITY 5 // above the radio task (4): the timeslot work can't wait
static StaticTask_t hrc7000Task;
static StackType_t hrc7000TaskStack[2048]; // as the main firmware (2048 words): the AMBE codec runs on it


static const uint8_t SILENCE_AUDIO[AMBE_AUDIO_LENGTH] = {
		0xB9U, 0xE8U, 0x81U, 0x52U, 0x61U, 0x73U, 0x00U, 0x2AU, 0x6BU, 0xB9U, 0xE8U, 0x81U, 0x52U,
		0x61U, 0x73U, 0x00U, 0x2AU, 0x6BU, 0xB9U, 0xE8U, 0x81U, 0x52U, 0x61U, 0x73U, 0x00U, 0x2AU, 0x6BU
};

//ALL THE FOLLOWING INITIALISATION VALES WERE CAPTURED FROM THE OFFCIAL TYT FIRMWARE FOR THE MD-9600

enum RXSyncClass { SYNC_CLASS_HEADER = 0, SYNC_CLASS_VOICE = 1, SYNC_CLASS_DATA = 2, SYNC_CLASS_RC = 3 };
enum RXSyncType { MS_SYNC = 0, BS_SYNC = 1, TDMA1_SYNC = 2, TDMA2_SYNC = 3 };

volatile uint8_t DMR_frame_buffer[DMR_FRAME_BUFFER_SIZE];

// DMR Tx trace (repeater wake-up, and any transmission), read with tools/debug (dm32dbg.py mr dmrWakeTrace 772): while waking a repeater, each
// timeslot interrupt (kind 2), received frame (kind 1), Tx interrupt (kind 3), RF receive start (kind 4), PA at full drive
// (kind 5) and PA off (kind 6, hrc7000TraceRf()): time in us, kind | slotState << 8 | timeCode << 16 | rxTSToggled << 24,
// RX_TYPE_INFO. 64 entries, dmrWakeTraceIndex counts them all.
volatile uint32_t dmrWakeTraceIndex = 0;
volatile uint32_t dmrWakeTrace[64][3];
// Repeater mode (Rx != Tx frequency), as the stock firmware DM32.01.L01.048 (disassembly; addresses of that firmware):
// passive Tx timing to send the wake-up CSBK, the modem left as it is after the burst, a receive reset after a few
// silent timeslots, a repeater Idle frame as the wake-up answer, and RF_MODE / RF_CONTROL set for repeater mode.
static bool repeaterMode = false;
static int wakeSilentSlots = 0;

// Stock channel set up (0x301d9c0): RF_MODE bit 11 in direct mode only, RF_CONTROL | 0x1512 in repeater mode
void HRC7000SetRepeaterMode(bool rmo)
{
	repeaterMode = rmo;
	*RF_MODE = (rmo ? 0x03269060 : 0x03269860);
	*RF_CONTROL = (rmo ? 0x00041F1A : 0x00041F0A); // direct: the Tx lead of repeater mode (modem.c RF_CONTROL_DIRECT)
}

static void hrc7000RepeaterRxReset(void);

// Channel changed (direct <-> repeater mode, or FM): the modem set up again when the mode changes
void HRC7000UpdateRepeaterMode(void)
{
	bool digital = (currentRadioDevice->currentMode == RADIO_MODE_DIGITAL);
	bool rmo = (digital && (currentRadioDevice->trxDMRModeTx == DMR_MODE_RMO));

	if (rmo != repeaterMode)
	{
		// Registers only: HRC7000InitDigitalDmrRx() here, in the middle of trxSetFrequency(), hung the radio
		HRC7000SetRepeaterMode(rmo);
		if (digital)
		{
			if (rmo)
			{
				hrc7000RepeaterRxReset();
			}
			else
			{
				*LAYER2_CONTROL = 0xC0; // as HRC7000SetDMR() in direct mode
				*LAYER2_SLOTON = 0x00;
				*LAYER2_TXRX_CTRL = 0x800000;
				*LAYER2_TXRX_CTRL = 0x40;
				*LAYER2_SLOTON = 0x05;
			}
		}
	}
}

// Stock 0x30221c0(0): passive Tx timing, before the wake-up
static void hrc7000RepeaterTxTiming(void)
{
	*LAYER2_SLOTON &= ~BITSET(0);
	*LAYER2_SLOTON &= ~BITSET(2);
	*LAYER2_CONTROL = (*LAYER2_CONTROL & 0x10) | 0xCB;
	*LAYER2_SLOT_CNT = 0;
	*LAYER2_SLOT_CNT |= 0x80200;
	*LAYER2_SLOTON |= 0x05;
	*LAYER2_TXRX_CTRL = 0x40;
}

// Stock 0x3022394 + 0x3022258: receive, passive timing (0xC4), when the repeater has not answered, and in repeater mode
static void hrc7000RepeaterRxReset(void)
{
	*LAYER2_SLOTON &= 0x02;
	*LAYER2_TXRX_CTRL |= 0x800000;
	*LAYER2_TXRX_CTRL |= 0x400000;
	*LAYER2_CONTROL = (*LAYER2_CONTROL & 0x10) | 0xC4;
	*LAYER2_SLOTON |= 0x05;
	*LAYER2_TXRX_CTRL = 0x40;
}
static uint8_t deferredUpdateBuffer[AMBE_AUDIO_LENGTH * NUM_AMBE_BUFFERS];// WAS [DMR_FRAME_BUFFER_SIZE * 6]; 384
static const uint8_t *DEFERRED_UPDATE_BUFFER_END = (uint8_t *)deferredUpdateBuffer + (AMBE_AUDIO_LENGTH * NUM_AMBE_BUFFERS) - 1;

static struct
{
	volatile bool hasEncodedAudio;
	volatile bool hasAudioData;
	volatile bool hasAbnormalExit;
	volatile int receivedFramesCount;
	volatile bool insertSilenceFrame;
	volatile bool transmissionEnabled;
	volatile bool rxCRCisValid;
	volatile bool hotspotDMRTxFrameBufferEmpty;
	volatile bool hotspotDMRRxFrameBufferAvailable;
	volatile bool inIRQHandler;
	volatile uint8_t *deferredUpdateBufferOutPtr;
	volatile uint8_t *deferredUpdateBufferInPtr;
	volatile int ambeBufferCount;
	volatile int interruptTimeout;
	volatile uint32_t receivedTgOrPcId;
	volatile uint32_t receivedSrcId;
	volatile int tickCount;
	volatile int skipCount;
	volatile bool skipOneTS;
	volatile int qsoDataSeqCount;
	volatile int qsoDataTimeout;
	volatile int txSequence;
	volatile int timeCode;
	volatile uint8_t rxColorCode;
	volatile int repeaterWakeupResponseTimeout;
	volatile int isWaking;
	volatile int tsAgreed;
	volatile int tsDisagreed;
	volatile int rxTSToggled; // used for Repeater wakeup sequence
	volatile int tsLockedTS;
	volatile int lastTimeCode;
	volatile uint8_t previousLCBuf[LC_DATA_LENGTH];
	volatile int dmrMonitorCapturedTimeout;
	volatile int TAPhase;
	volatile bool keepMonitorCapturedTSAfterTxing;
	volatile uint8_t lastRxColorCode;
	volatile int lastRxColorCodeCount;
	volatile uint32_t lastRxColorCodeTime;
	volatile bool ccHold;
	uint8_t bufferLimitReachedCount;
	volatile int ccHoldTimer;
	volatile uint32_t ccHoldReleaseTickTime;
	int wakeTriesCount;
	int hotspotPostponedFrameHandling;
	char talkAliasText[33];
	uint8_t talkAliasLocation[7];
} hrc = {
		.hasEncodedAudio = false,
		.hasAudioData = false,
		.hasAbnormalExit = false,
		.receivedFramesCount = -1,
		.insertSilenceFrame = false,
		.transmissionEnabled = false,
		.rxCRCisValid = false,
		.hotspotDMRTxFrameBufferEmpty = true,
		.hotspotDMRRxFrameBufferAvailable = false,
		.inIRQHandler = false,
		.deferredUpdateBufferOutPtr = deferredUpdateBuffer,
		.deferredUpdateBufferInPtr = deferredUpdateBuffer,
		.ambeBufferCount = 0,
		.interruptTimeout = 0,
		.receivedTgOrPcId = 0,
		.receivedSrcId = 0,
		.tickCount = 0,
		.skipCount = 0,
		.skipOneTS = false,
		.qsoDataSeqCount = 0,
		.qsoDataTimeout = 0,
		.txSequence = 0,
		.timeCode = -1,
		.rxColorCode = 0,
		.repeaterWakeupResponseTimeout = 0,
		.isWaking = WAKING_MODE_NONE,
		.tsAgreed = 0,
		.tsDisagreed = 0,
		.rxTSToggled = 0,
		.tsLockedTS = -1,
		.lastTimeCode = -2,
		.previousLCBuf = { 0 },
		.dmrMonitorCapturedTimeout = 0,
		.TAPhase = 0,
		.keepMonitorCapturedTSAfterTxing = false,
		.lastRxColorCode = 0xFF,
		.lastRxColorCodeCount = 0,
		.lastRxColorCodeTime = 0,
		.bufferLimitReachedCount = 0,
		.ccHold = true,
		.ccHoldTimer = 0,
		.ccHoldReleaseTickTime = 0,
		.wakeTriesCount = 0,
		.hotspotPostponedFrameHandling = 0,
		.talkAliasText = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
		.talkAliasLocation = { 0, 0, 0, 0, 0, 0, 0}
};

volatile int slotState = DMR_STATE_IDLE;
volatile ticksTimer_t readDMRRSSITimer = { 0, 0 };
volatile bool updateLastHeard = false;
volatile int dmrMonitorCapturedTS = -1;

static bool hrc7000CallAcceptFilter(void);
static void hrc7000SendPcOrTgLCHeader(void);
static inline void hrc7000RxInterruptHandler(void);
static void hrc7000TransitionToTx(void);
static void hrc7000InitDigitalState(void);
static void hrc7000TriggerPrivateCallQSODataDisplay(void);


uint8_t getCurrentTATxFlag(void)
{
	uint8_t flag;
	uint8_t taTxFlagTS1 = currentChannelData->flag1 & 0x03;
	uint8_t taTxFlagTS2 = (currentChannelData->flag1 >> 2) & 0x03;

	if (currentRadioDevice->trxDMRModeTx == DMR_MODE_DMO)
	{
		flag = taTxFlagTS1 | taTxFlagTS2;
	}
	else
	{
		flag = ((trxGetDMRTimeSlot() == 0) ? taTxFlagTS1 : taTxFlagTS2);
	}

	if ((flag == TA_TX_APRS) && (nonVolatileSettings.locationLat == SETTINGS_UNITIALISED_LOCATION_LAT))
	{
		flag = TA_TX_OFF;
	}

	return flag;
}

//Updated by G4EML to reflect the sequence used by the official TYT firmware on the MD-9600

void HRC7000FlushMusic(void)
{
	const int SIZE_OF_FILL_BUFFER = 64;
	uint8_t spi_values[SIZE_OF_FILL_BUFFER];

	HRC7000SetDmrRxGain(-15);							//temporarily set the gain to -15dB. Any less and the buffer flush doesn't seem to work.

	*AUDIO_CONTROL |= BITSET(7);
	memset(spi_values, 0x7F, SIZE_OF_FILL_BUFFER);
	for (int i=0; i < SIZE_OF_FILL_BUFFER; i++){
		*(SOUND + i) = spi_values[i];
	}

	*AUDIO_CONTROL &= BITCLR(7);
	HRC7000SetDmrRxGain(getVolumeControl());			//restore gain to the volume control setting
}

void HRC7000SetMicGainDMR(uint8_t gain)
{
	*GCR_ADCL = gain;
	*GCR_ADCL |= BITSET(7);
}

static inline bool hrc7000CrcIsValid(void)
{
	if ((settingsUsbMode != USB_MODE_HOTSPOT) && settingsIsOptionBitSet(BIT_DMR_CRC_IGNORED))
	{
		return true;
	}

	return hrc.rxCRCisValid;
}

static int hrc7000GetTSTimeoutValue(void)
{
	if (uiDataGlobal.Scan.active)
	{
		if ((uiDataGlobal.Scan.state == SCAN_STATE_SHORT_PAUSED) || (uiDataGlobal.Scan.state == SCAN_STATE_SCANNING))
		{
			return TS_SYNC_SCAN_TIMEOUT;
		}
	}

	if (monitorModeData.isEnabled)
	{
		return (hrc.hasAudioData ? (nonVolatileSettings.dmrCaptureTimeout * 1000) : TS_SYNC_SCAN_TIMEOUT);
	}

	return ((hrc.transmissionEnabled || hrc.hasAudioData) ? (nonVolatileSettings.dmrCaptureTimeout * 1000) : TS_SYNC_STARTUP_TIMEOUT);
}

static inline bool hrc7000CheckTimeSlotFilter(void)
{
	if (hrc.transmissionEnabled)
	{
		return (hrc.timeCode == trxGetDMRTimeSlot());
	}
	else
	{
		if (nonVolatileSettings.dmrCcTsFilter & DMR_TS_FILTER_PATTERN)
		{
			int currentTS = (((hrc.tsAgreed == TS_IS_LOCKED) && (hrc.tsLockedTS != -1)) ? hrc.tsLockedTS : trxGetDMRTimeSlot());

			dmrMonitorCapturedTS = currentTS;
			hrc.dmrMonitorCapturedTimeout = nonVolatileSettings.dmrCaptureTimeout * 1000;

			return (hrc.timeCode == currentTS);
		}
		else
		{
			if ((hrc.timeCode != -1) && (hrc.tsAgreed > TS_STABLE_THRESHOLD) && ((dmrMonitorCapturedTS == -1) || (dmrMonitorCapturedTS == hrc.timeCode)))
			{
				dmrMonitorCapturedTS = hrc.timeCode;
				hrc.dmrMonitorCapturedTimeout = hrc7000GetTSTimeoutValue();
				return true;
			}
		}
	}

	return false;
}

bool HRC7000CheckTalkGroupFilter(void)
{
	if (((hrc.receivedTgOrPcId >> 24) == PC_CALL_FLAG) && (settingsUsbMode != USB_MODE_HOTSPOT))
	{
		// Handle private calls if they are allowed
		if((nonVolatileSettings.privateCalls != 0) || ((trxTalkGroupOrPcId >> 24) == PC_CALL_FLAG))
		{
			// Monitor all private calls if DMR filter = None. Only allow private call to this radio in other cases.
			if((nonVolatileSettings.dmrDestinationFilter == DMR_DESTINATION_FILTER_NONE) || (hrc.receivedTgOrPcId == (trxDMRID | (PC_CALL_FLAG << 24))))
			{
				return true;
			}
		}
		else
		{
			return false;
		}
	}

	// All call bypasses filtering
	if (hrc.receivedTgOrPcId == ALL_CALL_VALUE)
	{
		return true;
	}

	switch(nonVolatileSettings.dmrDestinationFilter)
	{
		case DMR_DESTINATION_FILTER_TG:
			return ((trxTalkGroupOrPcId & 0x00FFFFFF) == hrc.receivedTgOrPcId);
			break;

		case DMR_DESTINATION_FILTER_DC:
			return codeplugContactsContainsPC(hrc.receivedSrcId);
			break;

		case DMR_DESTINATION_FILTER_RXG:
			for(int i = 0; i < currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup; i++)
			{
				if (currentRxGroupData.NOT_IN_CODEPLUG_contactsTG[i] == hrc.receivedTgOrPcId)
				{
					return true;
				}
			}

			// Also include currently selected talkgroup even if it is not in the RXG
			if ((trxTalkGroupOrPcId & 0x00FFFFFF) == hrc.receivedTgOrPcId)
			{
				return true;
			}

			return false;
			break;

		default:
			break;
	}

	return true;
}

static bool hrc7000CheckColourCodeFilter(void)
{
	return (hrc.rxColorCode == trxGetDMRColourCode());
}

static void hrc7000TransmitTalkerAlias(void)
{
	int loopSize;
	uint8_t flag = getCurrentTATxFlag();

	switch(flag)
	{
		case TA_TX_APRS:
			loopSize = 2;
		break;
		case TA_TX_TEXT:
			loopSize = 9;
		break;
		case TA_TX_BOTH:
			loopSize = (nonVolatileSettings.locationLat == SETTINGS_UNITIALISED_LOCATION_LAT) ? 9 : 11;
		break;
		default:
			loopSize = 0;// This should not occur as this function should not be called if the flag is TA_TX_OFF
			break;
	}

	if (hrc.TAPhase % 2 == 0)
	{
		hrc7000SendPcOrTgLCHeader();
	}
	else
	{
		uint8_t TA_LCBuf[LC_DATA_LENGTH] = {0,0,0,0,0,0,0,0,0,0,0,0};
		int taPosition = 2;
		int taOffset, taLength;
		uint32_t phase = hrc.TAPhase / 2;

		if (flag == TA_TX_APRS)
		{
			phase = 4;
		}

		switch(phase)
		{
			case 0:
				taPosition 	= 3;
				TA_LCBuf[2]= (0x01 << 6) | (strlen(hrc.talkAliasText) << 1);
				taOffset	= 0;
				taLength	= 6;
				break;
			case 1:
				taOffset	= 6;
				taLength	= 7;
				break;
			case 2:
				taOffset	= 13;
				taLength	= 7;
				break;
			case 3:
				taOffset	= 20;
				taLength	= 7;
				break;
			case 4:
				taOffset	= 20;// not used for this phase
				taLength	= 7;
				break;
			default:
				taOffset	= 0;
				taLength	= 0;
				break;
		}

		TA_LCBuf[0]= (phase) + 0x04;

		if (phase <= 3)
		{
			memcpy(&TA_LCBuf[taPosition], &hrc.talkAliasText[taOffset], taLength);
		}
		else
		{
			memcpy(&TA_LCBuf[taPosition], &hrc.talkAliasLocation[0], 7);
		}

		for (int i=0; i < taPosition + taLength; i++)
		{
			*(MODEM_TX_RAM + i) = TA_LCBuf[i];// put LC into hardware
		}
	}

	hrc.TAPhase = ((hrc.TAPhase + 1) % loopSize);
}

// lc: an embedded LC (DM32: read by hrc7000SysReceivedEmbeddedLCInt()), NULL to read the LC of the received burst
static void hrc7000HandleLCData(uint8_t syncClass, const uint8_t *lc);

// HR_C7000 interrupt 0x04 (SYS_INTERP_LIST): the embedded LC of a voice superframe is in MODEM_RX_RAM (9 bytes, its
// checksum already checked), as the stock firmware handles it (0x3027be4): no error (RX_TYPE_INFO bit 25), our colour
// code, not a privacy (PI) one (bit 17).
static void hrc7000SysReceivedEmbeddedLCInt(void)
{
	uint32_t info = *RX_TYPE_INFO;
	uint8_t lc[LC_DATA_LENGTH] = { 0 };

	if (hrc.transmissionEnabled || (info & BITSET(25)) || (((info >> 4) & 0x0F) != trxGetDMRColourCode()) || (info & BITSET(17)))
	{
		return;
	}

	for (int i = 0; i < 9; i++)
	{
		lc[i] = *(MODEM_RX_RAM + i);
	}

	hrc7000HandleLCData(SYNC_CLASS_VOICE, lc);
}

static void hrc7000HandleLCData(uint8_t syncClass, const uint8_t *lc)
{
	uint8_t LCBuf[LC_DATA_LENGTH];
	bool lcValid;

	if (lc != NULL)
	{
		memcpy(LCBuf, lc, LC_DATA_LENGTH);
		lcValid = true;
	}
	else
	{
		// The HR_C7000 has no embedded LC in MODEM_RX_RAM with the voice frames (what is there stays the same for the
		// whole call: ff 83 df 17 ...); it comes with its own interrupt, as in the stock firmware.
		if (syncClass != SYNC_CLASS_DATA)
		{
			return;
		}
		for (int i=0; i < LC_DATA_LENGTH; i++)
		{
			LCBuf[i] = *(MODEM_RX_RAM + i); // read the LC from the C7000
		}
		lcValid = hrc7000CrcIsValid();
	}

	bool lcResult = true;

	if (lcResult && lcValid && hrc.ccHold && (hrc.tsAgreed > TS_STABLE_THRESHOLD))
	{
		bool lcSent = false;

		if ((((LCBuf[0] == TG_CALL_FLAG) || (LCBuf[0] == PC_CALL_FLAG))
				|| ((LCBuf[0] >= DMR_EMBEDDED_DATA_TALKER_ALIAS_HEADER) && (LCBuf[0] <= DMR_EMBEDDED_DATA_GPS_INFO))) &&
				(((settingsUsbMode == USB_MODE_HOTSPOT) || (memcmp((uint8_t *)hrc.previousLCBuf, LCBuf, LC_DATA_LENGTH) != 0)) ||
						(monitorModeData.isEnabled && monitorModeData.dmrIsValid && (monitorModeData.qsoInfoUpdated == false))))
		{

			if (((currentRadioDevice->trxDMRModeRx == DMR_MODE_DMO) || hrc7000CheckTimeSlotFilter()) && hrc7000CheckColourCodeFilter()) // only do this for the selected timeslot, or when in Active mode
			{

				if ((LCBuf[0] == TG_CALL_FLAG) || (LCBuf[0] == PC_CALL_FLAG))
				{

					uint32_t prevTgOrPcId = hrc.receivedTgOrPcId;
					uint32_t prevSrcId = hrc.receivedSrcId;

					hrc.receivedTgOrPcId = (LCBuf[0] << 24) + (LCBuf[3] << 16) + (LCBuf[4] << 8) + (LCBuf[5] << 0); // used by the call accept filter
					hrc.receivedSrcId    = (LCBuf[6] << 16) + (LCBuf[7] << 8) + (LCBuf[8] << 0); // used by the call accept filter

					if ((hrc.receivedTgOrPcId != 0) && (hrc.receivedSrcId != 0) && HRC7000CheckTalkGroupFilter() &&
							((settingsUsbMode == USB_MODE_HOTSPOT) || (hrc.qsoDataSeqCount >= QSODATA_THRESHOLD_COUNT))) // only store the data if its actually valid
					{

						if (monitorModeData.isEnabled && monitorModeData.dmrIsValid &&
								(monitorModeData.dmrFrameSkip == 0) && monitorModeData.qsoInfoUpdated && (updateLastHeard == false))
						{
							monitorModeData.qsoInfoUpdated = false;
						}

						if ((monitorModeData.isEnabled && monitorModeData.dmrIsValid &&
								(monitorModeData.dmrFrameSkip == 0) && ((monitorModeData.qsoInfoUpdated == false))) ||
								((monitorModeData.isEnabled == false) && (updateLastHeard == false)))
						{
							memcpy((uint8_t *)DMR_frame_buffer, LCBuf, LC_DATA_LENGTH);

							// Don't update the screen until it's synced, or after the QSO turn has ended
							lcSent = updateLastHeard = ((slotState != DMR_STATE_IDLE) && (hrc.qsoDataTimeout > 0));
							monitorModeData.qsoInfoUpdated = true;

							if (uiDataGlobal.rxBeepState & RX_BEEP_CARRIER_HAS_STARTED)
							{
								// ID or TG/PC has changed too fast (mostly because people are stepping on others toes),
								// hence we were unable to play the CALLER_HAS_ENDED beep.
								if (((prevTgOrPcId > 0) && (prevTgOrPcId != hrc.receivedTgOrPcId)) ||
										((prevSrcId > 0) && (prevSrcId != hrc.receivedSrcId)))
								{
									if ((uiDataGlobal.rxBeepState & RX_BEEP_TALKER_HAS_STARTED) && ((uiDataGlobal.rxBeepState & RX_BEEP_TALKER_HAS_ENDED) == 0))
									{
										uiDataGlobal.rxBeepState |= (RX_BEEP_TALKER_HAS_ENDED | RX_BEEP_TALKER_HAS_ENDED_EXEC);
									}

									// Clearing this bit, as we don't want the qso info timeout trigger that beep again.
									uiDataGlobal.rxBeepState &= ~(RX_BEEP_TALKER_IDENTIFIED | RX_BEEP_TALKER_HAS_STARTED | RX_BEEP_TALKER_HAS_STARTED_EXEC);
								}
								else
								{
									uiDataGlobal.rxBeepState |= RX_BEEP_TALKER_IDENTIFIED;

									if ((uiDataGlobal.rxBeepState & RX_BEEP_TALKER_HAS_STARTED) == 0)
									{
										uiDataGlobal.rxBeepState |= (RX_BEEP_TALKER_HAS_STARTED | RX_BEEP_TALKER_HAS_STARTED_EXEC);
									}
								}
							}
						}
					}
				}
				else
				{
					if ((updateLastHeard == false) && (hrc.receivedTgOrPcId != 0) && HRC7000CheckTalkGroupFilter() &&
							((settingsUsbMode == USB_MODE_HOTSPOT) || (hrc.qsoDataSeqCount >= QSODATA_THRESHOLD_COUNT)))
					{

						if (monitorModeData.isEnabled && monitorModeData.dmrIsValid &&
								(monitorModeData.dmrFrameSkip == 0) && monitorModeData.qsoInfoUpdated)
						{
							monitorModeData.qsoInfoUpdated = false;
						}

						if ((monitorModeData.isEnabled && monitorModeData.dmrIsValid &&
								(monitorModeData.dmrFrameSkip == 0) && (((monitorModeData.qsoInfoUpdated == false)) && (hrc.receivedSrcId != 0))) ||
								(monitorModeData.isEnabled == false))
						{
							memcpy((uint8_t *)DMR_frame_buffer, LCBuf, LC_DATA_LENGTH);

							// Don't update the screen until it's synced, or after the QSO turn has ended
							lcSent = updateLastHeard = ((slotState != DMR_STATE_IDLE) && (hrc.qsoDataTimeout > 0));
							monitorModeData.qsoInfoUpdated = true;

							if (uiDataGlobal.rxBeepState & RX_BEEP_CARRIER_HAS_STARTED)
							{
								if ((uiDataGlobal.rxBeepState & RX_BEEP_TALKER_IDENTIFIED) == 0)
								{
									uiDataGlobal.rxBeepState |= RX_BEEP_TALKER_IDENTIFIED;
								}

								if ((uiDataGlobal.rxBeepState & RX_BEEP_TALKER_HAS_STARTED) == 0)
								{
									uiDataGlobal.rxBeepState |= (RX_BEEP_TALKER_HAS_STARTED | RX_BEEP_TALKER_HAS_STARTED_EXEC);
								}
							}
						}
					}
				}
			}
		}

		if (lcSent) // only track LC data sent.
		{
			memcpy((uint8_t *)hrc.previousLCBuf, LCBuf, LC_DATA_LENGTH);
		}
	}
}

static inline void hrc7000SysSendRejectedInt(void)
{
}

static inline void hrc7000SysSendStartInt(void)
{

	/*
		The sub-status registers indicate
		seven interrupts that initiate the transmission, including:
		Bit7: Voice transmission starts

		Bit6: OACSU requests to send interrupts, including first-time send and resend requests.

		Bit5: End-to-end voice enhanced encryption interrupt, including EMB72bits update interrupt
		and voice 216bits key update interrupt, which are distinguished by Bit5~Bit4 of Register
		0x88, where 01 indicates EMB72bits update interrupt and 10 indicates voice 216bits key update interrupt.

		Bit4: 	The Vocoder configuration returns an interrupt (this interrupt is sent by the HR_C6000
				to the MCU when the MCU manually configures the AMBE3000). This interrupt is only
				valid when using the external AMBE3000 vocoder.

		Bit3: Data transmission starts

		Bit2: Data partial retransmission

		Bit1: Data retransmission

		Bit0: The vocoder is initialized to an interrupt. This interrupt is only valid when using an external AMBE3000 or AMBE1000 vocoder.

		In MSK mode, there is no sub-interrupt status.
	 */
}

static inline void hrc7000SysSendEndInt(void)
{

	/*
		In DMR mode, there is a sub-status register 0x86 at the end of the transmission, and the
		corresponding interrupt can be masked by 0x87. The sub-status register indicates six interrupts that
		generate the end of the transmission, including:

		Bit7: 	Indicates that the service transmission is completely terminated, including voice and data.
				The MCU distinguishes whether the voice or data is sent this time. Confirming that the
				data service is received is the response packet that receives the correct feedback.

		Bit6: 	Indicates that a Fragment length confirmation packet is sent in the sliding window data
				service without immediate feedback.

		Bit5: VoiceOACSU wait timeout

		Bit4: 	The Layer 2 mode handles the interrupt. The MCU sends the configuration information
				to the last processing timing of the chip to control the interrupt. If after the interrupt, the
				MCU has not written all the information to be sent in the next frame to the chip, the next
				time slot cannot be Configured to send time slots. This interrupt is only valid when the chip
				is operating in Layer 2 mode.

		Bit3: 	indicates that a Fragment that needs to be fed back confirms the completion of the data
				packet transmission. The interrupt is mainly applied to the acknowledgment message after
				all the data packets have been sent or the data packet that needs to be fed back in the sliding
				window data service is sent to the MCU to start waiting for the timing of the Response
				packet. Device.

		Bit2 : ShortLC Receive Interrupt

		Bit1: BS activation timeout interrupt

		In MSK mode, there is no substate interrupt.
	 */
}

static inline void hrc7000SysPostAccessInt(void)
{
	/*
			In DMR mode, the access interrupt has no sub-status register.
			After receiving the interrupt, it indicates that the access voice communication mode is post-access. the way.

			In MSK mode, this interrupt has no substatus registers.
	 */
	// Late entry into ongoing RX
	if ((slotState == DMR_STATE_IDLE) && hrc.ccHold && hrc7000CheckColourCodeFilter())
	{
		codecInit(false);
		LedWrite(LED_GREEN_R, 1);

		*LAYER2_TXRX_CTRL = 0x8040; //Receive only in next timeslot

		slotState = DMR_STATE_RX_1;
		hrc.hasEncodedAudio = false;
		hrc.receivedFramesCount = -1;
		hrc.insertSilenceFrame = false;
		hrc.tsAgreed = 0;
		hrc.tsDisagreed = 0;
		hrc.tsLockedTS = -1;
		hrc.qsoDataSeqCount = 0;
		hrc.hasAudioData = false;
		hrc.qsoDataTimeout = 0;
		hrc.skipOneTS = false;
		memset((uint8_t *)hrc.previousLCBuf, 0x00, LC_DATA_LENGTH); // Ensure the LC data will be send to the UI.
		lastHeardClearLastID();// Tell the LastHeard system that this is a new start

		hrc.skipCount = 2;// RC. seems to be something to do with late entry but I'm but sure what, or whether its still needed

		if (settingsUsbMode == USB_MODE_HOTSPOT)
		{
			DMR_frame_buffer[AMBE_AUDIO_LENGTH + LC_DATA_LENGTH] = HOTSPOT_RX_START_LATE;
			hrc.hotspotDMRRxFrameBufferAvailable = true;
		}
	}
}

static inline void hrc7000SysReceivedDataInt(void)
{
	/*
		In DMR mode, this interrupt has no sub-status register, but the error and receive type of its received
		data is given by the 0x51 register. The DLLRecvDataType, DLLRecvCRC are used to indicate the
		received data type and the error status, and the MCU accordingly performs the corresponding status.

		Display, you can also block the corresponding interrupt.
		In MSK mode, this interrupt has no substate interrupts.

		The FMB frame's EMB information parsing completion prompt is also the completion of the
		interrupt, which is distinguished by judging the 0x51 register SyncClass=0.
	 */
	uint8_t rxDataType;
	uint8_t rxDataType2;
	uint8_t rxSyncClass;
	uint8_t rxPrivacyIndicator;
	uint8_t rxSyncType;

	uint32_t reg51 = *RX_TYPE_INFO;
	if ((hrc.isWaking != WAKING_MODE_NONE) || hrc.transmissionEnabled)
	{
		volatile uint32_t *t = dmrWakeTrace[dmrWakeTraceIndex++ & 63];

		t[0] = systime_get_us();
		t[1] = 1 | (slotState << 8) | ((hrc.timeCode & 0xFF) << 16) | ((hrc.rxTSToggled & 0xFF) << 24);
		t[2] = reg51;
	}

	//read_SPI_page_reg_byte_SPI0(0x04, 0x57, &reg_0x57);// Kai said that the official firmware uses this register instead of 0x52 for the timecode

	rxDataType = (uint8_t)(reg51 >> 8) & 0x0F;
	rxDataType2 = (uint8_t)(reg51 >> 12) & 0x07;
	rxSyncClass = (uint8_t)(reg51 >> 0) & 0x03;
//	hrc.rxCRCisValid = (uint8_t)(((reg51 >> 26) & 0x01) == 0);
	hrc.rxCRCisValid = (uint8_t)((reg51 & 0x7F000000) == 0);
//	if (reg51 & 0x7F000000)
//	{
//		hrc.rxCRCisValid = false;
//	}else
//	{
//		hrc.rxCRCisValid = true;
//	}

	rxPrivacyIndicator = (uint8_t)(reg51 >> 17) & 0x01;

	rxSyncType = (uint8_t)(reg51 >> 2) & 0x03;

	if (codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO) == 0)
	{
		if (rxSyncType == BS_SYNC)       // if we are receiving from a base station (Repeater)
		{
			currentRadioDevice->trxDMRModeRx = DMR_MODE_RMO; // switch to RMO mode to allow reception
		}
		else
		{
			currentRadioDevice->trxDMRModeRx = DMR_MODE_DMO; // not base station so must be DMO
		}
	}

	if (((slotState == DMR_STATE_RX_1) || (slotState == DMR_STATE_RX_2)) &&
			((rxPrivacyIndicator != 0) || (hrc7000CrcIsValid() == false) || (hrc7000CheckColourCodeFilter() == false)))
	{
		// Something is not correct
		return;
	}

	hrc.tickCount = 0;

	// Wait for the repeater to wakeup and count the number of frames where the Timecode (TS number) is toggling correctly
	if (slotState == DMR_STATE_REPEATER_WAKE_3)
	{
		// A repeater Idle frame (BS sync, data type 9, no error) is the answer (stock received data handler 0x3027764)
		if ((rxSyncType == BS_SYNC) && (rxSyncClass == SYNC_CLASS_DATA) && (rxDataType == 9) && hrc7000CrcIsValid())
		{
			hrc.rxTSToggled = 4;
		}
		if (hrc.lastTimeCode != hrc.timeCode)
		{
			hrc.rxTSToggled++;// timecode has toggled correctly
		}
		else
		{
			hrc.rxTSToggled = 0;// timecode has not toggled correctly so reset the counter used in the TS state machine
		}
	}

	// Note only detect terminator frames in Active mode, because in passive we can see our own terminators echoed back which can be a problem

	if (hrc7000CrcIsValid() && (rxSyncClass == SYNC_CLASS_DATA) && (rxDataType == 2))        //Terminator with LC
	{
		if ((currentRadioDevice->trxDMRModeRx == DMR_MODE_DMO) && hrc7000CallAcceptFilter())
		{
			slotState = DMR_STATE_RX_END;
			trxIsTransmitting = false;

			if (settingsUsbMode == USB_MODE_HOTSPOT)
			{
				DMR_frame_buffer[AMBE_AUDIO_LENGTH + LC_DATA_LENGTH] = HOTSPOT_RX_STOP;
				hrc.hotspotDMRRxFrameBufferAvailable = true;
			}
			return;
		}
		else
		{
			disableAudioAmp(AUDIO_AMP_MODE_RF);
			if ((voicePromptsIsPlaying() == false) && (soundMelodyIsPlaying() == false))
			{
				soundTerminateSound(); //stop any i2s sound transfers
			}
		}
	}

	if (hrc7000CrcIsValid() && (slotState != DMR_STATE_IDLE) && (hrc.skipCount > 0) && (rxSyncClass != SYNC_CLASS_DATA) && ((rxDataType2 & 0x07) == 0x01))
	{
		hrc.skipCount--;
	}

	// Check for correct received packet
	if (hrc7000CrcIsValid() && (rxPrivacyIndicator == 0) && (slotState < DMR_STATE_TX_START_1))
	{

		// Start RX
		if (slotState == DMR_STATE_IDLE)
		{
			if (hrc7000CheckColourCodeFilter())// Voice LC Header
			{
				codecInit(false);
				LedWrite(LED_GREEN_R, 1);

				*LAYER2_TXRX_CTRL = 0x8040; //Receive only in next timeslot

				slotState = DMR_STATE_RX_1;
				hrc.timeCode = -1;
				hrc.hasEncodedAudio = false;
				hrc.receivedFramesCount = -1;
				hrc.insertSilenceFrame = false;
				hrc.tsAgreed = 0;
				hrc.tsDisagreed = 0;
				hrc.tsLockedTS = -1;
				hrc.qsoDataSeqCount = 0;
				hrc.hasAudioData = false;
				hrc.qsoDataTimeout = 0;
				hrc.skipCount = 0;
				lastHeardClearLastID();// Tell the LastHeard system that this is a new start

				if (settingsUsbMode == USB_MODE_HOTSPOT)
				{
					DMR_frame_buffer[AMBE_AUDIO_LENGTH + LC_DATA_LENGTH] = HOTSPOT_RX_START;
					hrc.hotspotDMRRxFrameBufferAvailable = true;
				}
			}
		}
		else
		{
			int sequenceNumber = (rxDataType2 & 0x07);
			// Detect/decode voice packet and transfer it into the output soundbuffer

			if (((hrc.skipCount == 0) || ((hrc.receivedSrcId != 0x00) && (hrc.receivedSrcId != trxDMRID))) &&
					(rxSyncClass != SYNC_CLASS_DATA) && ((sequenceNumber >= 0x01) && (sequenceNumber <= 0x06)) &&
					(slotState == DMR_STATE_RX_1) // We are only listening on DMR_STATE_RX_1, in both DMO and RMO
					&& HRC7000CheckTalkGroupFilter() && hrc7000CheckColourCodeFilter())
			{
				bool ccLocked = (((nonVolatileSettings.dmrCcTsFilter & DMR_CC_FILTER_PATTERN) == 0) ? (hrc.lastRxColorCodeCount == CC_PROBE_LOCKED) : true);

				// We just got a valid audio, while in Monitor Mode, hence, we need to
				// (re)send the QSO info
				if (monitorModeData.isEnabled)
				{
					if (monitorModeData.triggered && ccLocked)
					{
						if (monitorModeData.dmrIsValid == false)
						{
							monitorModeData.triggered = false;
							monitorModeData.qsoInfoUpdated = false;
							updateLastHeard = false;
							monitorModeData.dmrIsValid = true;
							// Hack: Skip the two first frames
							//   The HR-C6000 is returning, on the first iteration, the LC data from the previous
							//   transmission, hence the displayed QSO info are messed up on the screen at the beginning
							//   of the decoding.
							//   Skipping the first two frames circumvent the problem (but delay a bit the QSO info displaying).
							monitorModeData.dmrFrameSkip = 2;
							hrc.receivedFramesCount = -1;
							hrc.insertSilenceFrame = false;
							hrc.tsAgreed = 0;
							hrc.tsDisagreed = 0;
							hrc.tsLockedTS = -1;
							hrc.qsoDataSeqCount = 0;
							hrc.hasAudioData = false;
							hrc.qsoDataTimeout = 0;
						}
					}
				}

				// Mostly, we are only set the CC as hold if the CC filter is OFF, as the locking is handled
				// in hrc6000SysInterruptHandler(), except when the monitor mode is enabled.
				if (ccLocked || (monitorModeData.isEnabled && monitorModeData.dmrIsValid))
				{
					hrc.ccHold = true;				//don't allow CC to change if we are receiving audio
				}

				if((settingsUsbMode != USB_MODE_HOTSPOT) && ((getAudioAmpStatus() & AUDIO_AMP_MODE_RF) == 0) &&
						(((currentRadioDevice->trxDMRModeRx == DMR_MODE_RMO) || (currentRadioDevice->trxDMRModeRx == DMR_MODE_DMO)) && (hrc.tsAgreed > TS_STABLE_THRESHOLD)))
				{
					enableAudioAmp(AUDIO_AMP_MODE_RF);
				}

				if (sequenceNumber == 1)
				{
					if ((monitorModeData.isEnabled == false) ||
							(monitorModeData.isEnabled && monitorModeData.dmrIsValid && (monitorModeData.dmrFrameSkip == 0)))
					{
						// Once the TS is locked, we're waiting for QSODATA_THRESHOLD_COUNT frame's
						// sequence #1 before sending LC data to the UI. This avoid QSO_DISPLAY_CALLER_DATA retrigger,
						// mostly at the end of a QSO run.
						if ((hrc.tsAgreed == TS_IS_LOCKED) && (hrc.qsoDataSeqCount <= QSODATA_THRESHOLD_COUNT) &&
								((uiDataGlobal.Scan.active == false) || (uiDataGlobal.Scan.active && (uiDataGlobal.Scan.state == SCAN_STATE_PAUSED))))
						{
							hrc.qsoDataSeqCount++;
						}

						hrc7000TriggerPrivateCallQSODataDisplay(); // it also reset the qsoDataTimeout value.
					}

					if (monitorModeData.isEnabled && monitorModeData.dmrIsValid && monitorModeData.dmrFrameSkip)
					{
						monitorModeData.dmrFrameSkip--;

						if (monitorModeData.dmrFrameSkip == 0) // All frames are skipped, clear the last heard now.
						{
							HRC7000ClearActiveDMRID();
							lastHeardClearLastID();
							monitorModeData.qsoInfoUpdated = false;
						}
					}
				}

				for (int i=0; i < AMBE_AUDIO_LENGTH; i++)
				{
					DMR_frame_buffer[LC_DATA_LENGTH + i] = *(MODEM_VOICE + i);
				}

				if (settingsUsbMode == USB_MODE_HOTSPOT)
				{
					DMR_frame_buffer[AMBE_AUDIO_LENGTH + LC_DATA_LENGTH] = HOTSPOT_RX_AUDIO_FRAME;
					DMR_frame_buffer[AMBE_AUDIO_LENGTH + LC_DATA_LENGTH + 1] = (rxDataType2 & 0x07);// audio sequence number
					hrc.hotspotDMRRxFrameBufferAvailable = true;
				}
				else
				{
					if (hrc.tsAgreed == TS_IS_LOCKED)
					{
						if (monitorModeData.isEnabled == false)
						{
							// We need to count the number of received audio frames, as when a dropout happens,
							// that put the decoded audio buffering in a low state, triggering stuttering audio.
							// Hence, if we didn't get 5 frames between two sequence #1, a silence frame is
							// inserted to the buffering system, which permits to the real decoded audio to refill/recover.
							if (((hrc.hasAudioData == false) && (sequenceNumber == 1)) ||
									(hrc.hasAudioData && (sequenceNumber == 1) && (hrc.receivedFramesCount == -1)))
							{
								hrc.receivedFramesCount = 0;
							}
							else if (hrc.receivedFramesCount >= 0)
							{
								if (sequenceNumber == 1)
								{
									if ((hrc.receivedFramesCount >= 0) && (hrc.receivedFramesCount != (SUPERFRAME_NUM_FRAMES - 1)))
									{
										// Insert SILENCE frame
										hrc.insertSilenceFrame = true;
									}

									hrc.receivedFramesCount = 0;
								}
								else
								{
									hrc.receivedFramesCount++;
								}
							}
						}

						// Tell foreground that there is audio to encode
						// But not until we get a TS lock, there is no
						// need to fill the audio buffer with garbage, hence
						// over-filling could happen, triggering stuttering audio
						hrc.hasEncodedAudio = true;
						hrc.hasAudioData = true;

						if (currentRadioDevice->trxDMRModeRx == DMR_MODE_RMO)
						{
							hrc.dmrMonitorCapturedTimeout = hrc7000GetTSTimeoutValue(); // Keep resetting the TS timeout
						}
					}
				}
			}
		}
	}

	if (hrc.transmissionEnabled == false) // ignore the LC data when we are transmitting
	{
		hrc7000HandleLCData(rxSyncClass, NULL);
	}

	if (hrc.timeCode != -1)
	{
		hrc.lastTimeCode = hrc.timeCode;
	}
}

static inline void hrc7000SysReceivedInformationInt(void)
{

	/*
		In DMR mode, this interrupt has sub-status register 0x90, which has three types:
			1. 0x80 indicates that the entire information is received and verified. After the
					service data is verified, the MCU extracts the data after the address 0x30 in
					the RX terminal 1.2KRAM through the SPI port. The length of the data is defined by
					the corresponding field of the received frame header.
			2. 0x00 indicates the entire information reception check error;
			3. 0x40 indicates that a non-confirmed SMS abnormal interrupt is generated;
	 */
}

static inline void hrc7000SysAbnormalExitInt(void)
{

	/*
		In DMR mode, the cause of the abnormality in DMR mode is the unexpected abnormal voice
		interrupt generated inside the state machine. The corresponding voice exception type is obtained
		through Bit2~Bit0 of register address 0x98.
	 */

	if (hrc.ccHold && hrc7000CheckColourCodeFilter() && (hrc.transmissionEnabled == false) && (hrc.tsAgreed == TS_IS_LOCKED))
	{
		if ((settingsUsbMode != USB_MODE_HOTSPOT) && ((*LAYER3_INTERP_LIST >> 28) & 0x01)) // Check if it did happened on the TS we're locked on.
		{
			hrc.hasAbnormalExit = true; // The next audio frame will be skipped.
		}
	}
}

static inline void hrc7000SysPhysicalLayerInt(void)
{
}
void hrc7000SysInterruptHandler(void)
{

		bool reg52Result = true;
		uint32_t reg0x52;
		reg0x52 = *RX_TYPE_INFO;
		hrc.rxColorCode = (uint8_t)((reg0x52 >> 4) & 0x0F);

	if (hrc.transmissionEnabled == false)
	{
		if ((hrc.ccHold == false) && ((nonVolatileSettings.dmrCcTsFilter & DMR_CC_FILTER_PATTERN) == 0) && reg52Result
				// Only frames received without error: the colour code of a bad frame is random, and two equal ones
				// in a row changed the radio's (Tx too) colour code on noise
				&& ((reg0x52 & 0x7F000000) == 0)
				)

		{
			if (trxGetSNRMargindBm(RADIO_DEVICE_PRIMARY) >= 5) // Minimum SNR Margin
			{
				uint32_t timeDiff = (ticksGetMillis() - hrc.lastRxColorCodeTime);

				// Too much time has passed since last turn, reset hrc.lastRxColorCodeCount.
				// Also, if a CC detection failed (no ccHold), we have to reset the counter to
				// restart the whole detection process.
				if (timeDiff > 200)
				{
					hrc.lastRxColorCode = 0xFF;
				}

				if(hrc.rxColorCode == hrc.lastRxColorCode)
				{
					if (hrc.lastRxColorCodeCount < CC_PROBE_MAX_COUNT)
					{
						if (hrc.lastRxColorCodeCount % 2) // Slow down calling trxSetDMRColourCode() otherwise the FW will crash
						{
							trxSetDMRColourCode(hrc.rxColorCode);

							*LAYER2_CONTROL = 0xC0;

							if (currentRadioDevice->trxDMRModeTx == DMR_MODE_RMO) // we need to do extra config while in RMO, otherwise the chip will get stuck on a wrong CC
							{

								*LAYER2_TXRX_CTRL = 0xC00000; // Set Sync Fail Bit (Reset?))
								*LAYER2_TXRX_CTRL = 0x00; // Reset
								*LAYER2_TXRX_CTRL = 0xC00000; // Set Sync Fail Bit (Reset?)
								*LAYER2_TXRX_CTRL = 0x8040; // Receive during next Timeslot

							}

							// Give the HR-C6000 a bit of time.
							// In the modem interrupt the tick interrupt can't run: waiting for the milliseconds to
							// change hung the radio for good (colour code probe, CC filter off: after a change of
							// frequency, the first DMR frame received). A busy loop instead.
							Delay_us(3000);
						}

						hrc.lastRxColorCodeCount++;
					}
					else
					{
						hrc.ccHold = true;
						headerRowIsDirty = true; // force the UI to display the correct CC
						hrc.lastRxColorCodeCount = CC_PROBE_LOCKED;
						hrc.ccHoldTimer = 0;
					}
				}
				else
				{
					hrc.lastRxColorCodeCount = 0;
				}

				hrc.lastRxColorCode = hrc.rxColorCode;
				hrc.lastRxColorCodeTime = ticksGetMillis();
			}
		}
	}
	else
	{
		hrc.ccHold = true;  // prevent CC change when transmitting.
		hrc.lastRxColorCodeCount = 0;
	}

	if (*SYS_INTERP_LIST)
	{
//		if (*SYS_INTERP_LIST & 0x10040) USB_DEBUG_printf("%08X ", *SYS_INTERP_LIST);
		/*
			Bit7:
				In DMR mode: indicates that the transmission request rejects the interrupt without a sub-status register.
			In DMR mode, it indicates that this transmission request is rejected because the channel is busy;
		 */

		/*
			Bit6:
				In DMR mode: indicates the start of transmission.
				In MSK mode: indicates that the ping-pong buffer is half-full interrupted.
			In DMR mode, the sub-status register 0x84 is transmitted at the beginning, and the corresponding interrupt can be masked by 0x85.
		 */

		/*
			Bit5:
				In DMR mode: indicates the end of transmission.
				In MSK mode: indicates the end of transmission.
		 */

		/*
			Bit4:
				In DMR mode: indicates the access interruption;
				In MSK mode: indicates that the response response is interrupted.
		 */
		if (*SYS_INTERP_LIST & 0x02)
		{
//			hrc7000SysPostAccessInt();

		}

		/*
			Bit3:
				In DMR mode: indicates that the control frame parsing completion interrupt;
				In MSK mode: indicates the receive interrupt.
		 */
		if (*SYS_INTERP_LIST & 0x08)
		{
			hrc7000SysReceivedDataInt();
		}

		/*
			Bit2:
				In DMR mode: indicates service data reception interrupt.
				In FM mode: indicates FM function detection interrupt.
		 */

		/*
			Bit1:
				In DMR mode: indicates that the voice is abnormally exited;
			In DMR mode, the cause of the abnormality in DMR mode is the unexpected abnormal voice
			interrupt generated inside the state machine. The corresponding voice exception type is obtained
			through Bit2~Bit0 of register address 0x98.
		 */
		if (*SYS_INTERP_LIST & 0x04)
		{
			hrc7000SysReceivedEmbeddedLCInt();
//			hrc7000SysAbnormalExitInt();

		}

		/*
			Bit0:
				physical layer separate work reception interrupt
			The physical layer works independently to receive interrupts without a sub-status register. The
			interrupt is generated in the physical layer single working mode. After receiving the data, the
			interrupt is generated, and the MCU is notified to read the corresponding register to obtain the
			received data. This interrupt is typically tested in bit error rate or other performance in physical
			layer mode.
		 */
	}

//	*SYS_INTERP_CLEAR = *SYS_INTERP_LIST;

}

// Start the mic capture before the voice LC headers, as the stock firmware does (0x3020A1C): starting it
// (AUDIO_CONTROL bit 4) puts a DC step in the first samples, which are then discarded in DMR_STATE_TX_START_4.
static bool hrc7000MicCaptureStartedEarly = false;

static void hrc7000StartMicCaptureEarly(void)
{
	soundReceiveData();
	hrc7000MicCaptureStartedEarly = true;
}

static void hrc7000TransitionToTx(void)
{
	disableAudioAmp(AUDIO_AMP_MODE_RF);
	LedWrite(LED_GREEN_R, 0);

	if (settingsUsbMode != USB_MODE_HOTSPOT)
	{
		codecInit(false);
		hrc7000StartMicCaptureEarly();
	}

	*LAYER2_TXRX_CTRL = 0x00;

	slotState = DMR_STATE_TX_START_1;
	hrc.txSequence = 0;
}

//used as a delayed callback to allow time for the final Tx burst and then simulate the final Rx Interrupt and return to receive.

void hrc7000TxBurstCallback(void)
{
	trxIsTransmittingDMR = false;

	hrc7000SetInIRQHandler(true);
	hrc7000RxInterruptHandler();
	hrc7000SetInIRQHandler(false);
}

// Repeater receive, LAYER2_TXRX_CTRL for the next timeslot, as the stock firmware (its DMR task 0x2d604, 0x3022828) and
// the HR_C7000 manual (group call reception): rxnextsloten (bit 6) in every timeslot, begin_v_layer2 (bit 15, voice
// accepted) only in the timeslot of the call (locked). Receiving one timeslot out of two (0x00 in the other one) sometimes
// skipped the locked one, depending on where the timeslot interrupt falls when the HR_C7000 syncs: only the other
// timeslot's Idle frames were received for a whole call.
static inline uint32_t hrc7000RepeaterRxSlotCtrl(void)
{
	return 0x40 | (((hrc.tsAgreed == TS_IS_LOCKED) && (hrc.timeCode == hrc.tsLockedTS)) ? 0x8000 : 0);
}

void RAM_CODE hrc7000TimeslotInterruptHandler(void)
{
	// The RF of a Tx burst goes off with the HR_C7000's RF receive start interrupt (hrc7000RfRxInterruptHandler()), not
	// here 20 ms after the burst start as on the MD-UV380 (no such interrupt): from the next timeslot interrupt the burst
	// lasted ~33 ms, into the repeater's downlink timeslot of our call (a radio near ours didn't decode it)

//	if (*RX_TYPE_INFO & 0x7F000000)
//	{
//		return;
//	}

	// This only happen in RMO, when TS filter is OFF, and a locked TS became silent for dmrMonitorCapturedTimeout long,
	// but the slotState is still toggling between DMR_STATE_RX_1 and DMR_STATE_RX_2 (receiving signal).
	// In this case, we're switching to the other TS.
	if (hrc.skipOneTS)
	{
		hrc.skipOneTS = false;
		hrc.timeCode = !hrc.timeCode;
		hrc.lastTimeCode = !hrc.lastTimeCode;
		hrc.tsLockedTS = !hrc.tsLockedTS;
		hrc.qsoDataSeqCount = 0;
		hrc.receivedFramesCount = -1;
		hrc.insertSilenceFrame = false;
		hrc.hasAudioData = false;
		hrc.qsoDataTimeout = 0;
		dmrMonitorCapturedTS = !dmrMonitorCapturedTS;

		*LAYER2_TXRX_CTRL = 0x00;

		uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;

		HRC7000ClearActiveDMRID();
		lastHeardClearLastID();

		if (settingsUsbMode != USB_MODE_HOTSPOT)
		{
			codecInit(false);
		}

		return;
	}

	// NOTE:
	//      When RXing, one of the timeslot is not listened once the TS lock status is
	// acquired, hence the CACH register is not updated by the HR-C6000.
	// The following variable is only usable while tsAgreed <= 4
	int receivedTimeCode = (uint8_t)((*RX_TYPE_INFO >> 22) & 0x01);
	if ((hrc.isWaking != WAKING_MODE_NONE) || hrc.transmissionEnabled)
	{
		volatile uint32_t *t = dmrWakeTrace[dmrWakeTraceIndex++ & 63];

		t[0] = systime_get_us();
		t[1] = 2 | (slotState << 8) | ((hrc.timeCode & 0xFF) << 16) | ((hrc.rxTSToggled & 0xFF) << 24);
		t[2] = *RX_TYPE_INFO;
	}

	if ((slotState == DMR_STATE_REPEATER_WAKE_3) || (hrc.timeCode == -1))  // if we are waking up the repeater, or we don't currently have a valid value for the timecode
	{
		hrc.tsAgreed = 0;
		hrc.tsDisagreed = 0;
		hrc.timeCode = receivedTimeCode;             // use the received TC directly from the CACH
		hrc.qsoDataSeqCount = 0;
		hrc.hasAudioData = false;
		hrc.receivedFramesCount = -1;
		hrc.insertSilenceFrame = false;
		hrc.qsoDataTimeout = 0;
	}
	else
	{
		hrc.timeCode = !hrc.timeCode;                    // toggle the timecode.

		// We're entering this part of the code everytime, except when a TS lock has been acquired while RXing
		if (((((slotState == DMR_STATE_RX_1) || (slotState == DMR_STATE_RX_2)) && (hrc.tsAgreed > TS_STABLE_THRESHOLD)) || (hrc.tsAgreed == TS_IS_LOCKED)) == false)
		{

			if (hrc.timeCode == receivedTimeCode)    // if this agrees with the received version
			{
//				USB_DEBUG_printf("TimeInerr: %08X\r\n", receivedTimeCode);
				if (hrc.tsAgreed < (TS_STABLE_THRESHOLD + 1))
				{
					hrc.tsAgreed++;
				}

				if (hrc.tsDisagreed > 0)                   // decrement the disagree count
				{
					hrc.tsDisagreed--;
				}
			}
			else                                 // if there is a disagree it might be just a glitch so ignore it a couple of times.
			{
				hrc.tsDisagreed++;                         // count the number of disagrees.

				if (hrc.tsAgreed > 0)
				{
					hrc.tsAgreed--;
				}

				if (hrc.tsDisagreed > TS_DISAGREE_THRESHOLD) // if we have had four disagrees then re-sync.
				{
					hrc.timeCode = receivedTimeCode;
					hrc.tsDisagreed = 0;
					hrc.tsAgreed = 0;
					hrc.receivedFramesCount = -1;
					hrc.insertSilenceFrame = false;
					hrc.hasEncodedAudio = false;
					hrc.receivedFramesCount = -1;
					hrc.insertSilenceFrame = false;
					hrc.qsoDataSeqCount = 0;
					hrc.hasAudioData = false;
					hrc.qsoDataTimeout = 0;
				}
			}
		}
	}

	// RX/TX state machine

	switch (slotState)
	{
		case DMR_STATE_RX_1: // Start RX (first step)
			if (currentRadioDevice->trxDMRModeRx == DMR_MODE_RMO)
			{
				bool tsMatches = hrc7000CheckTimeSlotFilter();

				if(hrc.transmissionEnabled)
				{
					if (((hrc.isWaking == WAKING_MODE_NONE) || (hrc.isWaking == WAKING_MODE_AWAKEN)) && (hrc.tsDisagreed == 0) && (hrc.tsAgreed > TS_STABLE_THRESHOLD))
					{
						// Opposite TS is currently active, start transmitting on the next one, otherwise wait for the next call
						if (tsMatches == false)
						{
							hrc.tsLockedTS = !hrc.timeCode;

							hrc.tsAgreed = TS_IS_LOCKED; // Lock the TS
							hrc.isWaking = WAKING_MODE_AWAKEN;

							hrc7000TransitionToTx();
							hrc.ambeBufferCount = 0;
							hrc.deferredUpdateBufferOutPtr = deferredUpdateBuffer;
							hrc.deferredUpdateBufferInPtr = deferredUpdateBuffer;
						}
					}
					break;
				}
				else
				{
					if (LedRead(LED_GREEN_R) == 0)
					{
						LedWrite(LED_GREEN_R, 1);
					}

					// TS lock acquired
					if ((hrc.tsAgreed == (TS_STABLE_THRESHOLD + 1)) && !tsMatches)
					{
						hrc.tsLockedTS = !hrc.timeCode;
						hrc.tsAgreed = TS_IS_LOCKED; // Lock the TS
					}
				}
			}
			else
			{
				// When in Active (simplex) mode. We need to only receive on one of the 2 timeslots, otherwise we get random data for the other slot
				// and this can sometimes be interpreted as valid data, which then screws things up.
				// TS lock acquired
				if (hrc.tsLockedTS == -1)
				{
					hrc.tsLockedTS = hrc.timeCode; // Timecode is meaningless here
					hrc.tsAgreed = TS_IS_LOCKED;   // Lock the TS
				}

				if (hrc.transmissionEnabled)
				{
					hrc7000TransitionToTx();
					hrc.ambeBufferCount = 0;
					hrc.deferredUpdateBufferOutPtr = deferredUpdateBuffer;
					hrc.deferredUpdateBufferInPtr = deferredUpdateBuffer;
					break;
				}
			}

			// Once the TS lock is acquired start listening one TS out of two
			if (hrc.tsAgreed == TS_IS_LOCKED)
			{
				if ((hrc.transmissionEnabled == false) && ((uiDataGlobal.rxBeepState & RX_BEEP_CARRIER_HAS_STARTED) == 0))
				{
					uiDataGlobal.rxBeepState |= (RX_BEEP_CARRIER_HAS_STARTED | RX_BEEP_CARRIER_HAS_STARTED_EXEC);
					uiDataGlobal.rxBeepState &= ~(RX_BEEP_TALKER_IDENTIFIED | RX_BEEP_TALKER_HAS_ENDED_EXEC);
				}

				*LAYER2_TXRX_CTRL = ((currentRadioDevice->trxDMRModeRx == DMR_MODE_RMO) ? hrc7000RepeaterRxSlotCtrl() : 0x00);

				slotState = DMR_STATE_RX_2;
			}

			if (!trxDMRSynchronisedRSSIReadPending)
			{
				ticksTimerStart((ticksTimer_t *)&readDMRRSSITimer, (180 + 15)); // wait 3 * 60ms complete frames + 15 ticks (of approximately 15mS) before reading the RSSI, in the middle of a TS
				trxDMRSynchronisedRSSIReadPending = true;
			}
			break;

		case DMR_STATE_RX_2: // Start RX (second step)

			*LAYER2_TXRX_CTRL = ((currentRadioDevice->trxDMRModeRx == DMR_MODE_RMO) ? hrc7000RepeaterRxSlotCtrl() : 0x8040);

			slotState = DMR_STATE_RX_1;
			break;

		case DMR_STATE_RX_END: // Stop RX
			HRC7000ClearActiveDMRID();
			HRC7000InitDigitalDmrRx();
			disableAudioAmp(AUDIO_AMP_MODE_RF);
			if ((voicePromptsIsPlaying() == false) && (soundMelodyIsPlaying() == false))
			{
				soundTerminateSound(); //stop any i2s sound transfers
			}

			LedWrite(LED_GREEN_R, 0);

			if (uiDataGlobal.rxBeepState & RX_BEEP_CARRIER_HAS_STARTED)
			{
				uiDataGlobal.rxBeepState |= RX_BEEP_CARRIER_HAS_ENDED;

				// In DMO (HT to HT), otherwise Talker end beep won't be played.
				// The beep will be played first, followed 100ms later with carrier ending beep
				if ((uiDataGlobal.rxBeepState & RX_BEEP_TALKER_HAS_STARTED) && ((uiDataGlobal.rxBeepState & RX_BEEP_TALKER_HAS_ENDED) == 0))
				{
					uiDataGlobal.rxBeepState |= (RX_BEEP_TALKER_HAS_ENDED | RX_BEEP_TALKER_HAS_ENDED_EXEC);
				}
			}

			uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
			slotState = DMR_STATE_IDLE;
			hrc.tsLockedTS = -1;
			trxIsTransmitting = false;
			break;

		case DMR_STATE_TX_START_1: // Start TX (second step)
			LedWrite(LED_RED_R, 1); // for repeater wakeup
			hrc7000SendPcOrTgLCHeader();
			*LAYER2_TXRX_CTRL = 0x80; // Transmit during next Timeslot
			*LAYER2_SEND_TYPE = 0x10; // Set Data Type to 0001 (Voice LC Header), Data, LCSS=00
			trxIsTransmitting = true;
			*LAYER2_TXRX_CTRL |= BITSET(31);
			slotState = DMR_STATE_TX_START_2;
			break;

		case DMR_STATE_TX_START_2: // Start TX (third step)
			*LAYER2_TXRX_CTRL = 0x00; // Do nothing on the next TS
			slotState = DMR_STATE_TX_START_3;
			break;

		case DMR_STATE_TX_START_3: // Start TX (fourth step)
			*LAYER2_TXRX_CTRL = 0x80; // Transmit during Next Timeslot
			*LAYER2_SEND_TYPE = 0x10; // Set Data Type to 0001 (Voice LC Header), Data, LCSS=00
			slotState = DMR_STATE_TX_START_4;
			break;

		case DMR_STATE_TX_START_4: // Start TX (fifth step)
			*LAYER2_TXRX_CTRL = 0x00; // Do nothing on the next TS
			slotState = DMR_STATE_TX_START_5;

			if (settingsUsbMode != USB_MODE_HOTSPOT)
			{
				hrc.ambeBufferCount = 0;
				hrc.deferredUpdateBufferOutPtr = deferredUpdateBuffer;
				hrc.deferredUpdateBufferInPtr = deferredUpdateBuffer;
				if (hrc7000MicCaptureStartedEarly)
				{
					// Drop the mic samples captured during the headers (the stock firmware overwrites its voice
					// frames in the modem until they are sent, so only the latest audio goes out). Don't call
					// soundReceiveData() again: setting AUDIO_CONTROL bit 4 again restarts the capture with a DC step.
					// The encoders start over too (codecInit(): the wave buffers, our
					// encoder): the capture starts with the ADC settling from about 12000 down to the mic's DC in
					// 150 ms, and an encoder that had seen it (it encodes from 20 ms on) sent a thump and judged the
					// speech after it quiet (its level peak raised).
					codecInit(false);
					hrc7000MicCaptureStartedEarly = false;
				}
				else
				{
					soundReceiveData();
				}
			}
			break;

		case DMR_STATE_TX_START_5: // Start TX (sixth step)
			*LAYER2_TXRX_CTRL = 0x80; // Transmit during next Timeslot
			*LAYER2_SEND_TYPE = 0x10; // Set Data Type to 0001 (Voice LC Header), Data, LCSS=00
			hrc.TAPhase = 0;
			slotState = DMR_STATE_TX_1;
			break;

		case DMR_STATE_TX_1: // Ongoing TX (inactive timeslot)
			*LAYER2_TXRX_CTRL = 0x00;
			if ((hrc.transmissionEnabled == false) && (hrc.txSequence == 0))
			{
				slotState = DMR_STATE_TX_END_1;        // only exit here to ensure staying in the correct timeslot
			}
			else
			{
				slotState = DMR_STATE_TX_2;
			}
			break;

		case DMR_STATE_TX_2: // Ongoing TX (active timeslot)
			if (hrc.transmissionEnabled)
			{
				if (settingsUsbMode == USB_MODE_HOTSPOT)
				{
					if (hrc.txSequence == 0)
					{
						for (int i = 0; i < LC_DATA_LENGTH; i++) // put LC into hardware
						{
							*(MODEM_TX_RAM + i) = deferredUpdateBuffer[i];
						}
					}

					if (hrc.hotspotDMRTxFrameBufferEmpty == false)
					{
						for (int i = 0; i < AMBE_AUDIO_LENGTH; i++) // send the audio bytes to the hardware
						{
							*(MODEM_VOICE + i) = deferredUpdateBuffer[LC_DATA_LENGTH + i];
						}

						hrc.hotspotDMRTxFrameBufferEmpty = true; // we have finished with the current frame data from the hotspot
					}
					else
					{
						for (int i = 0; i < AMBE_AUDIO_LENGTH; i++) // send the audio bytes to the hardware
						{
							*(MODEM_VOICE + i) = SILENCE_AUDIO[i];
						}
					}

					if (hrc.hotspotPostponedFrameHandling > 0) // Send frames of silence until it's equal to zero
					{
						hrc.hotspotPostponedFrameHandling--;
					}
				}
				else
				{
					if((hrc.txSequence == 0) && (getCurrentTATxFlag() != TA_TX_OFF))
					{
						hrc7000TransmitTalkerAlias();
					}

					if (hrc.ambeBufferCount >= NUM_AMBE_BLOCK_PER_DMR_FRAME)
					{
						for (int i = 0; i < AMBE_AUDIO_LENGTH; i++) // send the audio bytes to the hardware
						{
							*(MODEM_VOICE + i) = hrc.deferredUpdateBufferOutPtr[i];
						}

						hrc.deferredUpdateBufferOutPtr += AMBE_AUDIO_LENGTH;

						if (hrc.deferredUpdateBufferOutPtr > DEFERRED_UPDATE_BUFFER_END)
						{
							hrc.deferredUpdateBufferOutPtr = deferredUpdateBuffer;
						}
						hrc.ambeBufferCount -= NUM_AMBE_BLOCK_PER_DMR_FRAME;
					}
					else
					{
						for (int i = 0; i < AMBE_AUDIO_LENGTH; i++)// send the audio bytes to the hardware
						{
							*(MODEM_VOICE + i) = SILENCE_AUDIO[i];
						}
					}
				}
			}
			else
			{
				for (int i = 0; i < AMBE_AUDIO_LENGTH; i++)// send the audio bytes to the hardware
				{
					*(MODEM_VOICE + i) = SILENCE_AUDIO[i];
				}
			}

			//write_SPI_page_reg_bytearray_SPI1(0x03, 0x00, (uint8_t*)(DMR_frame_buffer + LC_DATA_LENGTH), AMBE_AUDIO_LENGTH);// send the audio bytes to the hardware
			*LAYER2_TXRX_CTRL = 0x80; // Transmit during next Timeslot
			*LAYER2_SEND_TYPE = (0x08 + (hrc.txSequence << 4)); // Data Type= sequence number 0 - 5 (Voice Frame A) , Voice, LCSS = 0

			hrc.txSequence = ((hrc.txSequence + 1) % SUPERFRAME_NUM_FRAMES); // 0 .. 5

			slotState = DMR_STATE_TX_1;
			break;

		case DMR_STATE_TX_END_1: // Stop TX (first step)
			soundStopMicCapture();
			if (getCurrentTATxFlag() != TA_TX_OFF)
			{
				hrc7000SendPcOrTgLCHeader();
			}
			for (int i = 0; i < AMBE_AUDIO_LENGTH; i++) // send silence audio bytes
			{
				*(MODEM_VOICE + i) = SILENCE_AUDIO[i];
			}
			*LAYER2_TXRX_CTRL = 0x80; // Transmit during Next Timeslot
			*LAYER2_SEND_TYPE = 0x20; // Data Type =0010 (Terminator with LC), Data, LCSS=0
			slotState = DMR_STATE_TX_END_2;
			break;

		case DMR_STATE_TX_END_2: // Stop TX (second step)
			// Need to hold on this TS after Tx ends otherwise if DMR Mon TS filtering is disabled the radio may switch timeslot
			dmrMonitorCapturedTS = hrc.tsLockedTS = trxGetDMRTimeSlot();
			hrc.dmrMonitorCapturedTimeout = nonVolatileSettings.dmrCaptureTimeout * 1000;
			hrc.keepMonitorCapturedTSAfterTxing = true;
			hrc.tickCount = 0;

			if (currentRadioDevice->trxDMRModeTx == DMR_MODE_RMO)
			{
				*LAYER2_CONTROL = 0xC0; // Enable DMR Tx and Rx, Passive Timing
				*LAYER2_TXRX_CTRL = 0x00; // No Transmit or receive in next timeslot
				slotState = DMR_STATE_TX_END_3_RMO;
				addTimerCallback(hrc7000TxBurstCallback, 29, MENU_ANY, false);				//allow time for the last burst to be sent then return to receive
			}
			else
			{
				addTimerCallback(hrc7000TxBurstCallback, 29, MENU_ANY, false);				//allow time for the last burst to be sent then return to receive
				HRC7000InitDigitalDmrRx();
				slotState = DMR_STATE_TX_END_3_DMO;
			}
			break;

		case DMR_STATE_TX_END_3_DMO:
			slotState = DMR_STATE_IDLE;
			trxIsTransmitting = false;
			hrc.timeCode = -1;
			hrc.tsAgreed = 0;
			hrc.hasEncodedAudio = false;
			hrc.receivedFramesCount = -1;
			hrc.insertSilenceFrame = false;
			hrc.tsDisagreed = 0;
			hrc.lastTimeCode = -2;
			dmrMonitorCapturedTS = -1;
			hrc.keepMonitorCapturedTSAfterTxing = false;
			hrc.dmrMonitorCapturedTimeout = hrc7000GetTSTimeoutValue();
			break;

		case DMR_STATE_TX_END_3_RMO:
			LedWrite(LED_GREEN_R, 1);

			*LAYER2_TXRX_CTRL = 0x8040; // Receive during Next Timeslot And Layer2 Access success Bit
			slotState = DMR_STATE_RX_1;
			hrc.isWaking = WAKING_MODE_NONE;
			trxIsTransmitting = false;
			break;

		case DMR_STATE_REPEATER_WAKE_1:
			{
				LedWrite(LED_RED_R, 1); // Turn on the Red LED while when we transmit the wakeup frame
				// As the stock Tx slot handler (RAM 0x2d7ac): send type, the CSBK BS_Dwn_Act (24 bytes, to all), then
				// "Tx in the next timeslot" as bits 7:6 of TXRX_CTRL (bit 31, set by the voice start, stops a burst).
				// The transmitter is switched on now: switched on by the Tx interrupt, the PLL and PA took most of the
				// single 27.5 ms burst.
				uint8_t csbk[24] = { 0xB8, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF };

				csbk[7] = (trxDMRID >> 16) & 0xFF;
				csbk[8] = (trxDMRID >> 8) & 0xFF;
				csbk[9] = (trxDMRID >> 0) & 0xFF;

				if (!trxIsTransmittingDMR)
				{
					trxActivateDMRTx();
					trxDMRstartTime = ticksGetMillis();
				}

				*LAYER2_SEND_TYPE = 0x30;
				for (int i = 0; i < (int)sizeof(csbk); i++)
				{
					*(MODEM_TX_RAM + i) = csbk[i];
				}
				*LAYER2_TXRX_CTRL = (*LAYER2_TXRX_CTRL & ~0xC0) | 0x80;

				trxIsTransmitting = true;
			}
			hrc.repeaterWakeupResponseTimeout = WAKEUP_RETRY_PERIOD;
			slotState = DMR_STATE_REPEATER_WAKE_2;
			break;

		case DMR_STATE_REPEATER_WAKE_2:
			addTimerCallback(hrc7000TxBurstCallback, 29, MENU_ANY, false);				//allow time for the wakeup burst to be sent then return to receive
			LedWrite(LED_RED_R, 0); // Turn off the Red LED while we are waiting for the repeater to wakeup
			// The burst is being sent: the modem is left as it is (HRC7000InitDigitalDmrRx() here cancelled it), only
			// receiving in the next timeslots, as the stock firmware
			*LAYER2_TXRX_CTRL = (*LAYER2_TXRX_CTRL & ~0xC0) | 0x40;
			wakeSilentSlots = 0;
			hrc.rxTSToggled = 0; // Start to count the TS toggling times while waking up the repeater.
			slotState = DMR_STATE_REPEATER_WAKE_3;
			break;

		case DMR_STATE_REPEATER_WAKE_3:
			if (++wakeSilentSlots == 3)
			{
				hrc7000RepeaterRxReset(); // no answer in 3 timeslots: receive with passive timing (stock watchdog 0x302243c)
			}
			if (hrc.rxTSToggled > 3)
			{
				// wait for the signal from the repeater to have toggled timecode at least three times, i.e the signal should be stable and we should be able to go into Tx
				slotState = DMR_STATE_RX_1;
				trxIsTransmitting = false;
				hrc.isWaking = WAKING_MODE_AWAKEN;
				hrc.tsLockedTS = -1;
				hrc.qsoDataSeqCount = 0;
				hrc.hasAudioData = false;
				hrc.receivedFramesCount = -1;
				hrc.insertSilenceFrame = false;
			}
			break;

		default:
			break;
	}

	// Timeout interrupted RX
	// In RMO, we don't check for the LC Terminator (because of echo), so this timeout handles
	// the end of reception, as it resets tickCount to zero when receiving data (hrc6000SysReceivedDataInt())
	if (slotState < DMR_STATE_TX_START_1)
	{
		hrc.tickCount++;
		if (((slotState == DMR_STATE_IDLE) && (hrc.tickCount > START_TICK_TIMEOUT)) ||
				(((slotState == DMR_STATE_RX_1) || (slotState == DMR_STATE_RX_2)) && (hrc.tickCount > END_TICK_TIMEOUT)))
		{
			HRC7000InitDigitalDmrRx();
			HRC7000ClearActiveDMRID();
			disableAudioAmp(AUDIO_AMP_MODE_RF);
			if ((voicePromptsIsPlaying() == false) && (soundMelodyIsPlaying() == false))
			{
				soundTerminateSound(); //stop any i2s sound transfers
			}

			LedWrite(LED_GREEN_R, 0);

			if (uiDataGlobal.rxBeepState & RX_BEEP_CARRIER_HAS_STARTED)
			{
				uiDataGlobal.rxBeepState |= RX_BEEP_CARRIER_HAS_ENDED;

				// In DMO (HT to HT), otherwise Talker end beep won't be played.
				// The beep will be played first, followed 100ms later with carrier ending beep
				if ((uiDataGlobal.rxBeepState & RX_BEEP_TALKER_HAS_STARTED) && ((uiDataGlobal.rxBeepState & RX_BEEP_TALKER_HAS_ENDED) == 0))
				{
					uiDataGlobal.rxBeepState |= (RX_BEEP_TALKER_HAS_ENDED | RX_BEEP_TALKER_HAS_ENDED_EXEC);
				}
			}

			uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
			slotState = DMR_STATE_IDLE;
			trxIsTransmitting = false;
			hrc.tickCount = 0;
			hrc.tsLockedTS = -1;
			hrc.keepMonitorCapturedTSAfterTxing = false;
		}
	}

}

//the MDUV380 does not have the hardware for the Rx Interrupt This handler is retained for compatibility and is called from the timeslot interrupt handler.
static inline void hrc7000RxInterruptHandler(void)
{
	if(trxIsTransmittingDMR)					//if we are currently sending DMR then just turn off the RF
	{
		trxFastDMRTx(false);
	}
	else
	{
		trxActivateRx(false);	                // Else activate full Rx
	}

}

void hrc7000TxInterruptHandler(void)
{
	if ((hrc.isWaking != WAKING_MODE_NONE) || hrc.transmissionEnabled)
	{
		volatile uint32_t *t = dmrWakeTrace[dmrWakeTraceIndex++ & 63];

		t[0] = systime_get_us();
		t[1] = 3 | (slotState << 8) | ((hrc.timeCode & 0xFF) << 16) | ((hrc.rxTSToggled & 0xFF) << 24); // kind 3: Tx interrupt (RF on)
		t[2] = *LAYER2_TXRX_CTRL;
	}
	LedWrite(LED_RED_R, 1);
	if(trxIsTransmittingDMR)
	{
		trxFastDMRTx(true);							//already transmitting so just use the fast Tx method.
	}
	else
	{
		trxActivateDMRTx();							//Not currently transmitting so do a full Tx activation.
	}
	trxDMRstartTime = ticksGetMillis();				// the time the Tx burst started
}

// The PA switched by the RF task (radio.c): kind 5 at full drive, 6 off
void hrc7000TraceRf(uint32_t kind)
{
	if ((hrc.isWaking != WAKING_MODE_NONE) || hrc.transmissionEnabled)
	{
		uint32_t psr = irq_save();
		volatile uint32_t *t = dmrWakeTrace[dmrWakeTraceIndex++ & 63];

		t[0] = systime_get_us();
		t[1] = kind | (slotState << 8) | ((hrc.timeCode & 0xFF) << 16) | ((hrc.rxTSToggled & 0xFF) << 24);
		t[2] = *LAYER2_TXRX_CTRL;
		irq_restore(psr);
	}
}

// The RF receive start interrupt (vector 65, Rf_rx_inter of the HR_C7000 manual): the end of a Tx burst, the RF off
// (OpenGD77's RF_RX interrupt on the GD-77). Back to the full receive at the end of the transmission is
// hrc7000TxBurstCallback()'s.
void RAM_CODE hrc7000RfRxInterruptHandler(void)
{
	if ((hrc.isWaking != WAKING_MODE_NONE) || hrc.transmissionEnabled)
	{
		volatile uint32_t *t = dmrWakeTrace[dmrWakeTraceIndex++ & 63];

		t[0] = systime_get_us();
		t[1] = 4 | (slotState << 8) | ((hrc.timeCode & 0xFF) << 16) | ((hrc.rxTSToggled & 0xFF) << 24); // kind 4: RF receive start (RF off)
		t[2] = *LAYER2_TXRX_CTRL;
	}
	if (trxIsTransmittingDMR)
	{
		hrc7000RxInterruptHandler();
	}
}

void hrc7000SetInIRQHandler(bool in)
{
	if (in)
	{
		hrc.inIRQHandler = true;
	}
	else
	{
		hrc.interruptTimeout = 0;
		hrc.inIRQHandler = false;
	}
}

static void hrc7000InitDigitalState(void)
{
	hrc.interruptTimeout = 0;
	slotState = DMR_STATE_IDLE;
	trxIsTransmitting = false;
	hrc.tickCount = 0;
	hrc.skipCount = 0;
	hrc.tsAgreed = 0;
	hrc.hasEncodedAudio = false;
	hrc.receivedFramesCount = -1;
	hrc.insertSilenceFrame = false;
	hrc.tsDisagreed = 0;
	hrc.tsLockedTS = -1;
	hrc.qsoDataSeqCount = 0;
	hrc.hasAudioData = false;
	hrc.qsoDataTimeout = 0;
	hrc.skipOneTS = false;
	hrc.lastRxColorCode = 0xFF;
	hrc.lastRxColorCodeCount = 0;
	monitorModeData.dmrIsValid = false;
}

void HRC7000InitInterrupts(void)
{
	hrc7000InitDigitalState();

	interruptsInitC7000Interface();
}

void HRC7000InitDigitalDmrRx(void)
{
	DM32_TRACE(0x500);
	HRC7000SetDMR();						 // ensure any registers changed by FM use are restored to DMR settings
	DM32_TRACE(0x501);

	// HRC7000SetDMR() has already done the releases' layer 2 sequence (they never write 0xC00000 here)

	hrc.hasEncodedAudio = false;
	hrc.receivedFramesCount = -1;
	hrc.insertSilenceFrame = false;
	hrc.tsAgreed = 0;                     // Restart TS detection
	hrc.tsDisagreed = 0;
	hrc.qsoDataSeqCount = 0;
	hrc.hasAudioData = false;
	hrc.qsoDataTimeout = 0;
	hrc.lastRxColorCode = 0xFF;
	hrc.lastRxColorCodeCount = 0;

}

void HRC7000ResetTimeSlotDetection(void)
{
	if (hrc.keepMonitorCapturedTSAfterTxing == false)
	{
		disableAudioAmp(AUDIO_AMP_MODE_RF);
		if ((voicePromptsIsPlaying() == false) && (soundMelodyIsPlaying() == false))
		{
			soundTerminateSound(); //stop any i2s sound transfers
		}

		LedWrite(LED_GREEN_R, 0);

		hrc.timeCode = -1; // Clear current timecode synchronisation
		hrc.tsLockedTS = -1;
		hrc.hasEncodedAudio = false;
		hrc.receivedFramesCount = -1;
		hrc.insertSilenceFrame = false;
		hrc.tickCount = 0;
		hrc.tsAgreed = 0;
		hrc.tsDisagreed = 0;
		hrc.lastTimeCode = -2;
		dmrMonitorCapturedTS = -1;
		hrc.dmrMonitorCapturedTimeout = hrc7000GetTSTimeoutValue();
		hrc.qsoDataSeqCount = 0;
		hrc.hasAudioData = false;
		hrc.qsoDataTimeout = 0;
		hrc.skipOneTS = false;
		hrc.lastRxColorCode = 0xFF;
		hrc.lastRxColorCodeCount = 0;
		uiDataGlobal.rxBeepState = RX_BEEP_UNSET;
	}
}

void HRC7000InitDigital(void)
{
//	USB_DEBUG_printf("ok");
	// Partial timeslot detection reset
	disableAudioAmp(AUDIO_AMP_MODE_RF);
	if ((voicePromptsIsPlaying() == false) && (soundMelodyIsPlaying() == false))
	{
		soundTerminateSound(); //stop any i2s sound transfers
	}

	LedWrite(LED_GREEN_R, 0);

	hrc.timeCode = -1; // Clear current timecode synchronisation
	hrc.tickCount = 0;
	hrc.tsAgreed = 0;
	hrc.tsDisagreed = 0;
	hrc.lastTimeCode = -2;
	dmrMonitorCapturedTS = -1;
	hrc.qsoDataSeqCount = 0;
	hrc.hasAudioData = false;
	hrc.receivedFramesCount = -1;
	hrc.insertSilenceFrame = false;
	hrc.qsoDataTimeout = 0;
	hrc.dmrMonitorCapturedTimeout = hrc7000GetTSTimeoutValue();
	hrc.keepMonitorCapturedTSAfterTxing = false;
	hrc.skipOneTS = false;

	HRC7000InitDigitalDmrRx();
	hrc7000InitDigitalState();

	interruptsEnableC7000Interrupts();//	NVIC_EnableIRQ(PORTC_IRQn);

	codecInit(false);
}

void HRC7000TerminateDigital(void)
{
	disableAudioAmp(AUDIO_AMP_MODE_RF);
	if ((voicePromptsIsPlaying() == false) && (soundMelodyIsPlaying() == false))
	{
		soundTerminateSound(); //stop any i2s sound transfers
	}
	LedWrite(LED_GREEN_R, 0);
	hrc7000InitDigitalState();

//vk3kyy this does not seem to be needed NVIC_DisableIRQ(PORTC_IRQn);
}

static void hrc7000TriggerPrivateCallQSODataDisplay(void)
{
	// If the display is holding on the PC accept text and the incoming call is not a PC
	if ((hrc.qsoDataSeqCount == QSODATA_THRESHOLD_COUNT) &&
			(uiDataGlobal.PrivateCall.state == PRIVATE_CALL_ACCEPT) && (DMR_frame_buffer[0] == TG_CALL_FLAG))
	{
		uiDataGlobal.displayQSOState = QSO_DISPLAY_CALLER_DATA;
	}

	hrc.qsoDataTimeout = QSODATA_TIMER_TIMEOUT;
}

static void hrc7000SendPcOrTgLCHeader(void)
{
	uint8_t spi_tx[LC_DATA_LENGTH];

	spi_tx[0] = (trxTalkGroupOrPcId >> 24) & 0xFF;
	spi_tx[1] = 0x00;
	spi_tx[2] = 0x00;
	spi_tx[3] = (trxTalkGroupOrPcId >> 16) & 0xFF;
	spi_tx[4] = (trxTalkGroupOrPcId >> 8) & 0xFF;
	spi_tx[5] = (trxTalkGroupOrPcId >> 0) & 0xFF;
	spi_tx[6] = (trxDMRID >> 16) & 0xFF;
	spi_tx[7] = (trxDMRID >> 8) & 0xFF;
	spi_tx[8] = (trxDMRID >> 0) & 0xFF;
	spi_tx[9] = 0x00;
	spi_tx[10] = 0x00;
	spi_tx[11] = 0x00;

	for (int i=0; i < LC_DATA_LENGTH; i++)
	{
		*(MODEM_TX_RAM + i) = spi_tx[i];
	}

}

static bool hrc7000CallAcceptFilter(void)
{
	if (settingsUsbMode == USB_MODE_HOTSPOT)
	{
		//In Hotspot mode, we need to accept all incoming traffic, otherwise private calls won't work
		if ((DMR_frame_buffer[0] == TG_CALL_FLAG) || (DMR_frame_buffer[0] == PC_CALL_FLAG) ||
				((DMR_frame_buffer[0] >= 0x04) && (DMR_frame_buffer[0] <= 0x08))) // we maybe received TA / GPS
		{
			return true;
		}
		else
		{
			return false; // Not a PC or TG call
		}
	}
	else
	{
		return ((DMR_frame_buffer[0] == TG_CALL_FLAG) || (hrc.receivedTgOrPcId == (trxDMRID | (PC_CALL_FLAG << 24))));
	}
}

static void hrc7000ManageCCHoldState(void)
{
	uint32_t m = ticksGetMillis();

	if ((m - hrc.ccHoldReleaseTickTime) >= 1)
	{
		hrc.ccHoldReleaseTickTime = m;

		if ((nonVolatileSettings.dmrCcTsFilter & DMR_CC_FILTER_PATTERN) && (hrc.ccHold == false))
		{
			hrc.ccHold = true;
		}
		else if((hrc.transmissionEnabled == false) && ((nonVolatileSettings.dmrCcTsFilter & DMR_CC_FILTER_PATTERN) == 0) && hrc.ccHold)
		{
			if (slotState == DMR_STATE_IDLE)
			{
				if ((getAudioAmpStatus() & AUDIO_AMP_MODE_RF) == 0)
				{
					hrc.ccHoldTimer++;
				}
				else
				{
					hrc.ccHoldTimer -= ((hrc.ccHoldTimer > 0) ? 1 : 0);
				}

				if (hrc.ccHoldTimer >= CC_HOLD_TIME)
				{
					hrc.ccHold = false;
					hrc.ccHoldTimer = 0;
					hrc.lastRxColorCode = 0xFF;
					hrc.lastRxColorCodeCount = 0;
					monitorModeData.dmrIsValid = false;
				}
			}
			else
			{
				hrc.ccHoldTimer = 0;
			}
		}
	}
}

void hrc7000Tick(void)
{
	hrc7000ManageCCHoldState();

	if (hrc.transmissionEnabled && (hrc.isWaking == WAKING_MODE_NONE))
	{
		// DMO: Start transmitting
		// RMO: Waking up the repeater if it's not already listening to it.
		if (slotState == DMR_STATE_IDLE)
		{
			// Because the ISR's also write to the SPI we need to disable interrupts on Port C when doing any SPI transfers.
			// Otherwise there could be clashes in the SPI subsystem.
			// This is possibly not the ideal solution, and a better solution may be found at a later date
			// But at least it should prevent things going too badly wrong

			interruptsDisableC7000Interrupts();//			NVIC_DisableIRQ(PORTC_IRQn);

			// Ensure the ISR has exited
			while (hrc.inIRQHandler);

			if (currentRadioDevice->trxDMRModeTx == DMR_MODE_DMO)
			{
				if (settingsUsbMode != USB_MODE_HOTSPOT)
				{
					codecInit(false);
					hrc7000StartMicCaptureEarly();
				}
				else
				{
					hrc.hotspotPostponedFrameHandling = (HS_NUM_OF_SILENCE_SEQ_ON_STARTUP * 6);
					// LC and Frame data will be uplodaded in hrc6000TimeslotInterruptHandler(), DMR_STATE_TX_2 case.
					memcpy((uint8_t *)deferredUpdateBuffer, (uint8_t *)&audioAndHotspotDataBuffer.hotspotBuffer[wavbuffer_read_idx], AMBE_AUDIO_LENGTH + LC_DATA_LENGTH);
					// Note:
					//       We don't increment the buffer indexes, because this is also the first frame of audio and we need
					// it later, and LC data are needed for the silent frames
					hrc.hotspotDMRTxFrameBufferEmpty = true; // We will send silence at the beginning of the transmission
				}

				slotState = DMR_STATE_TX_START_1;
			}
			else
			{
				if (settingsUsbMode != USB_MODE_HOTSPOT)
				{
					codecInit(false);
				}

				hrc.isWaking = WAKING_MODE_WAITING;
				hrc.wakeTriesCount = 0;
				slotState = DMR_STATE_REPEATER_WAKE_1;
			}

			hrc.txSequence = 0;
			hrc.ambeBufferCount = 0;
			hrc.deferredUpdateBufferOutPtr = deferredUpdateBuffer;
			hrc.deferredUpdateBufferInPtr = deferredUpdateBuffer;

			hrc.timeCode = -1; // Clear current timecode synchronisation
			hrc.tsAgreed = 0;
			hrc.tsDisagreed = 0;
			hrc.hasEncodedAudio = false;
			hrc.receivedFramesCount = -1;
			hrc.insertSilenceFrame = false;
			hrc.lastTimeCode = -2;
			dmrMonitorCapturedTS = -1;
			hrc.dmrMonitorCapturedTimeout = TS_SYNC_STARTUP_TIMEOUT;
			hrc.keepMonitorCapturedTSAfterTxing = false;
			hrc.qsoDataSeqCount = 0;
			hrc.hasAudioData = false;
			hrc.qsoDataTimeout = 0;
			hrc.skipOneTS = false;

			interruptsEnableC7000Interrupts();//			NVIC_EnableIRQ(PORTC_IRQn);
			if (slotState == DMR_STATE_REPEATER_WAKE_1)
			{
				hrc7000RepeaterTxTiming(); // the repeater gives the timing
			}
			else
			{
				*LAYER2_CONTROL = 0xC3; // TX and RX enable, Active Timing.
			}
//			*LAYER2_SLOTON |= BITSET(2);

			hrc.interruptTimeout = 0;
		}
		else
		{
			// Note. In Tier 2 Passive (Repeater operation). The radio will already be receiving DMR frames from the repeater
			// And the transition from Rx to Tx is handled in the Timeslot ISR state machine
		}
	}

	// Timeout interrupt
	// This code appears to check whether there has been a TS ISR in the last 200 RTOS ticks
	// If not, it reinitialises the DMR subsystem
	// -> As long the ISR is called, interruptTimeout is set to 0. This permits to
	//    detect when the DMR subsystem has stopped running (no DMR signal).
	if (slotState != DMR_STATE_IDLE)
	{
		if (hrc.interruptTimeout < INTERRUPT_TIMEOUT)
		{
			hrc.interruptTimeout++;

			if (hrc.interruptTimeout == INTERRUPT_TIMEOUT)
			{
				HRC7000InitDigital();// sets 	interruptTimeout=0;
				HRC7000ClearActiveDMRID();
				if (uiDataGlobal.displayQSOState != QSO_DISPLAY_DEFAULT_SCREEN)
				{
					uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
				}
				slotState = DMR_STATE_IDLE;
				trxIsTransmitting = false;
				hrc.tickCount = 0;
				hrc.tsLockedTS = -1;
				hrc.tsAgreed = 0;
				hrc.tsDisagreed = 0;
				hrc.hasEncodedAudio = false;
				hrc.receivedFramesCount = -1;
				hrc.insertSilenceFrame = false;
				hrc.qsoDataSeqCount = 0;
				hrc.hasAudioData = false;
				hrc.qsoDataTimeout = 0;
				hrc.skipOneTS = false;
			}
		}
	}
	else
	{
		hrc.interruptTimeout = 0;
	}

	if (hrc.transmissionEnabled)
	{
		if (hrc.isWaking == WAKING_MODE_WAITING)
		{
			// The wakeup response is still expected...
			if (hrc.repeaterWakeupResponseTimeout > 0)
			{
				hrc.repeaterWakeupResponseTimeout--;
			}
			else
			{
				// The wakeup response time has expired
				// Retry, once again (limit is set into the codeplug)
				hrc.wakeTriesCount++;
				if (hrc.wakeTriesCount > codeplugGetRepeaterWakeAttempts())
				{
					hrc.isWaking = WAKING_MODE_FAILED;// signal that the Wake process has failed.
				}
				else
				{
					// Retry. Stop everything and restart.

					interruptsDisableC7000Interrupts();//NVIC_DisableIRQ(PORTC_IRQn);

					// Ensure the ISR has exited
					while (hrc.inIRQHandler);

					hrc.repeaterWakeupResponseTimeout = WAKEUP_RETRY_PERIOD;
					slotState = DMR_STATE_REPEATER_WAKE_1;
					hrc.timeCode = -1; // Clear current timecode synchronisation
					hrc.tickCount = 0;
					hrc.tsAgreed = 0;
					hrc.tsDisagreed = 0;
					hrc.hasEncodedAudio = false;
					hrc.receivedFramesCount = -1;
					hrc.insertSilenceFrame = false;
					hrc.lastTimeCode = -2;
					dmrMonitorCapturedTS = -1;
					hrc.dmrMonitorCapturedTimeout = TS_SYNC_STARTUP_TIMEOUT;
					hrc.keepMonitorCapturedTSAfterTxing = false;
					hrc.qsoDataSeqCount = 0;
					hrc.hasAudioData = false;
					hrc.qsoDataTimeout = 0;
					hrc.skipOneTS = false;

					interruptsEnableC7000Interrupts();//					NVIC_EnableIRQ(PORTC_IRQn);

					hrc7000RepeaterTxTiming(); // retry of the wake-up, as the stock firmware
//					*LAYER2_SLOTON |= BITSET(2);

					hrc.interruptTimeout = 0;
				}
			}
		}
		else
		{
			// normal operation. Not waking the repeater
			if (settingsUsbMode == USB_MODE_HOTSPOT)
			{
				if ((hrc.hotspotPostponedFrameHandling == 0) && (hrc.hotspotDMRTxFrameBufferEmpty == true) && (wavbuffer_count > 0))
				{
					memcpy((uint8_t *)deferredUpdateBuffer, (uint8_t *)&audioAndHotspotDataBuffer.hotspotBuffer[wavbuffer_read_idx], AMBE_AUDIO_LENGTH + LC_DATA_LENGTH);

					wavbuffer_read_idx = ((wavbuffer_read_idx + 1) % HOTSPOT_BUFFER_COUNT);

					wavbuffer_count--;
					hrc.hotspotDMRTxFrameBufferEmpty = false;
				}
			}
			else
			{
				// Once there are 2 buffers available they can be encoded into one AMBE block
				// The will happen  prior to the data being needed in the TS ISR, so that by the time tick_codec_encode encodes complete,
				// the data is ready to be used in the TS ISR
				// This task runs once per 10 ms OS tick and an encode takes ~13 ms, so one encode per pass
				// only reached ~45 of the 50 AMBE blocks/s: allow a second one when samples are waiting.
				for (int encodes = 0; (encodes < 2) && (wavbuffer_count >= 2); encodes++)
				{
					codecEncodeBlock((uint8_t *)hrc.deferredUpdateBufferInPtr); // was commented out, so every voice frame was silence

					hrc.deferredUpdateBufferInPtr += LENGTH_AMBE_BLOCK;

					if (hrc.deferredUpdateBufferInPtr > DEFERRED_UPDATE_BUFFER_END)
					{
						hrc.deferredUpdateBufferInPtr = deferredUpdateBuffer;
					}
					hrc.ambeBufferCount++;
				}
			}
		}
	}
	else
	{
		// receiving RF DMR
		if (settingsUsbMode == USB_MODE_HOTSPOT)
		{
			if ((hrc.hotspotDMRRxFrameBufferAvailable == true) && hrc7000CrcIsValid() && hrc.ccHold && (hrc.tsAgreed > TS_STABLE_THRESHOLD) && hrc7000CheckColourCodeFilter())
			{
				hotspotRxFrameHandler((uint8_t *)DMR_frame_buffer);
				hrc.hotspotDMRRxFrameBufferAvailable = false;
			}
		}
		else
		{

			if (monitorModeData.isEnabled && (monitorModeData.dmrTimeout > 0))
			{

				if (hrc.rxCRCisValid == false) // Here, we shouldn't use hrc6000CrcIsValid(), otherwise monitor mode will get stuck in DMR
				{
					monitorModeData.dmrTimeout--;
					if (monitorModeData.dmrTimeout == 0)
					{
						monitorModeData.dmrIsValid = false;
						// switch to analog
						trxSetModeAndBandwidth(RADIO_MODE_ANALOG, true);
						currentChannelData->sql = CODEPLUG_MIN_VARIABLE_SQUELCH;
						trxSetRxCSS(RADIO_DEVICE_PRIMARY, CODEPLUG_CSS_TONE_NONE);
						uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
						headerRowIsDirty = true;
					}
				}
				else
				{
					//found DMR signal
					monitorModeData.dmrTimeout = 0;
					monitorModeData.qsoInfoUpdated = true;
					monitorModeData.dmrIsValid = false;
					if (monitorModeData.isEnabled)
					{
						monitorModeData.triggered = true;
					}
					HRC7000ClearActiveDMRID();
					lastHeardClearLastID();
					// Detect the correct TS
					// Avoid to call HRC6000ResetTimeSlotDetection(), as it close the Audio amp and turn the LED off
					hrc.timeCode = -1;
					hrc.tsAgreed = 0;
					hrc.tsDisagreed = 0;
					hrc.hasEncodedAudio = false;
					hrc.receivedFramesCount = -1;
					hrc.insertSilenceFrame = false;
					hrc.lastTimeCode = -2;
					dmrMonitorCapturedTS = -1;
					hrc.dmrMonitorCapturedTimeout = TS_SYNC_STARTUP_TIMEOUT;
					hrc.keepMonitorCapturedTSAfterTxing = false;
					hrc.qsoDataSeqCount = 0;
					hrc.hasAudioData = false;
					hrc.qsoDataTimeout = 0;
					hrc.skipOneTS = false;
				}
			}

			// The burst to decode is taken with the interrupts off, and decoded with them on: the decode takes
			// longer than the playback's 10 ms buffer (3 frames, about 15 ms with our vocoder, 11 ms with the
			// stock one), and with the interrupts off the playback ran dry: gaps of 60 ms and clicks in the audio
			uint8_t burst[27];
			bool decodeBurst = false;
			CPU_SR_ALLOC();
			CPU_CRITICAL_ENTER();

			if (hrc.hasEncodedAudio || hrc.insertSilenceFrame)
			{

				// voice prompts take priority over incoming DMR audio
				if ((voicePromptsIsPlaying() == false) && (soundMelodyIsPlaying() == false))
				{

					if ((WAV_BUFFER_COUNT - wavbuffer_count) < 3) // If we're running low on audio decoding storage
					{
						hrc.bufferLimitReachedCount = 6; // cancels decoding of the next 6 buffers.
					}

					if (hrc.bufferLimitReachedCount > 0)
					{
						hrc.bufferLimitReachedCount--;
					}
					else
					{

						memcpy(burst, (uint8_t *)((hrc.hasAbnormalExit || hrc.insertSilenceFrame) ? SILENCE_AUDIO : (DMR_frame_buffer + LC_DATA_LENGTH)), sizeof(burst));
						decodeBurst = true;

					}
				}

				// Will process the encoded audio on the next call, silence was inserted
				if ((hrc.hasEncodedAudio && hrc.insertSilenceFrame) == false)
				{
					hrc.hasEncodedAudio = false;
				}

				hrc.insertSilenceFrame = false;
				hrc.hasAbnormalExit = false; // Clear abnormal exit
			}
			CPU_CRITICAL_EXIT();

			if (decodeBurst)
			{
				codecDecode(burst, 3);
			}

			CPU_CRITICAL_ENTER();
			soundTickRXBuffer();
			CPU_CRITICAL_EXIT();

		}

		if (hrc.qsoDataTimeout > 0)
		{
			// Only timeout the QSO data display if not displaying the Private Call Accept Yes/No text
			// if menuUtilityReceivedPcId is non zero the Private Call Accept text is being displayed
			if (uiDataGlobal.PrivateCall.state != PRIVATE_CALL_ACCEPT)
			{
				hrc.qsoDataTimeout--;
				if (hrc.qsoDataTimeout == QSODATA_RX_BEEP_TIMER_TIMEOUT)
				{
					if ((uiDataGlobal.rxBeepState & RX_BEEP_CARRIER_HAS_STARTED) && (uiDataGlobal.rxBeepState & RX_BEEP_TALKER_IDENTIFIED) &&
							(uiDataGlobal.rxBeepState & RX_BEEP_TALKER_HAS_STARTED) && ((uiDataGlobal.rxBeepState & RX_BEEP_TALKER_HAS_ENDED) == 0))
					{
						uiDataGlobal.rxBeepState |= (RX_BEEP_TALKER_HAS_ENDED | RX_BEEP_TALKER_HAS_ENDED_EXEC);
					}
				}

				if (hrc.qsoDataTimeout == 0)
				{
					hrc.qsoDataSeqCount = 0;
					hrc.tickCount = 0;
					uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
					memset((uint8_t *)hrc.previousLCBuf, 0x00, LC_DATA_LENGTH); // Some repeaters are accessed from analog, hence sending the same DMR ID for all everyone
				}
			}
		}
	}

	if ((currentRadioDevice->trxDMRModeRx == DMR_MODE_RMO) &&
				(hrc.transmissionEnabled == false) && (dmrMonitorCapturedTS != -1) && (hrc.dmrMonitorCapturedTimeout > 0))
	{
		hrc.dmrMonitorCapturedTimeout--;
		if (hrc.dmrMonitorCapturedTimeout == 0)
		{
			hrc.hasAudioData = false;
			hrc.dmrMonitorCapturedTimeout = hrc7000GetTSTimeoutValue();

			// We're still receiving something, maybe from the other TS, hence start to dance between both TS.
			if (((nonVolatileSettings.dmrCcTsFilter & DMR_TS_FILTER_PATTERN) == 0) &&
					((slotState == DMR_STATE_RX_1) || (slotState == DMR_STATE_RX_2)))
			{
				hrc.skipOneTS = true;
			}
			else
			{
				dmrMonitorCapturedTS = -1; // Reset the TS capture
				hrc.keepMonitorCapturedTSAfterTxing = false;
				hrc.tickCount = 0;
				hrc.tsAgreed = 0;
				hrc.tsDisagreed = 0;
				hrc.hasEncodedAudio = false;
				hrc.receivedFramesCount = -1;
				hrc.insertSilenceFrame = false;
				hrc.qsoDataSeqCount = 0;
				hrc.qsoDataTimeout = 0;
				if (monitorModeData.isEnabled) // Reset monitor mode startup, if enabled.
				{
					monitorModeData.triggered = true;
					monitorModeData.dmrIsValid = false;
				}
			}
		}
	}

	hrc.rxCRCisValid = false;// Reset this
}

static void hrc7000TaskFunction(void *data)
{

	while (1U)
	{
			
//		if (timer_hrc6000task == 0)       G4EML don't use PIT counter method. Use vtaskDelay() instead
//		{

			// Update our atomic transmission state
			hrc.transmissionEnabled = trxTransmissionEnabled;

			dmr_compat_run_callbacks();

			// If DIGITAL mode is active, we must handle it ;-)
			if (trxGetMode() == RADIO_MODE_DIGITAL)
			{
				hrc7000Tick();
				dmr_after_tick(); // the call to the last heard list and the UI (applicationMain.c)
			}

//			timer_hrc6000task = 1; // Reset ISR activity marker
//		}

			dhtime_delay(1);

	}
}

void HRC7000InitTask(void)
{
	xTaskCreateStatic(hrc7000TaskFunction, "hrc7000", sizeof(hrc7000TaskStack) / sizeof(hrc7000TaskStack[0]), NULL,
			HRC7000_TASK_PRIORITY, hrc7000TaskStack, &hrc7000Task);

}

// RC. I had to use accessor functions for the isWaking flag
// because the compiler seems to have problems with volatile vars as externs used by other parts of the firmware (the Tx Screen)
void HRC7000ClearIsWakingState(void)
{
	hrc.isWaking = WAKING_MODE_NONE;
}

int HRC7000GetIsWakingState(void)
{
	return hrc.isWaking;
}

void HRC7000ClearActiveDMRID(void)
{
	memset((uint8_t *)hrc.previousLCBuf, 0x00, LC_DATA_LENGTH);
	memset((uint8_t *)DMR_frame_buffer, 0x00, LC_DATA_LENGTH);
	hrc.receivedTgOrPcId 	= 0x00;
	hrc.receivedSrcId 		= 0x00;
}

void HRC7000ResyncTimeSlot(void)
{
	if (hrc.transmissionEnabled == false)
	{
		if (hrc.keepMonitorCapturedTSAfterTxing == false)
		{
			// If we are in RMO mode, without TS filtering, and currently receiving, toggles the active TS.
			if ((currentRadioDevice->trxDMRModeRx == DMR_MODE_RMO) && (dmrMonitorCapturedTS != -1) && (hrc.dmrMonitorCapturedTimeout > 0) &&
					((nonVolatileSettings.dmrCcTsFilter & DMR_TS_FILTER_PATTERN) == 0) &&
					((slotState == DMR_STATE_RX_1) || (slotState == DMR_STATE_RX_2)))
			{
				hrc.skipOneTS = true;
				hrc.keepMonitorCapturedTSAfterTxing = false;
				return;
			}

			hrc.tickCount = 0;
			hrc.tsAgreed = 0;
			hrc.tsDisagreed = 0;
			hrc.hasEncodedAudio = false;
			hrc.receivedFramesCount = -1;
			hrc.insertSilenceFrame = false;
			hrc.tsLockedTS = -1;
			hrc.qsoDataSeqCount = 0;
			hrc.hasAudioData = false;
			hrc.qsoDataTimeout = 0;
			dmrMonitorCapturedTS = -1;
			hrc.dmrMonitorCapturedTimeout = hrc7000GetTSTimeoutValue();
			hrc.skipOneTS = false;
			HRC7000ClearActiveDMRID();
		}

		hrc.keepMonitorCapturedTSAfterTxing = false;
	}
}

uint32_t HRC7000GetReceivedTgOrPcId(void)
{
	return hrc.receivedTgOrPcId;
}

uint32_t HRC7000GetReceivedSrcId(void)
{
	return hrc.receivedSrcId;
}

void HRC7000ClearTimecodeSynchronisation(void)
{
	hrc.timeCode = -1;
}

void HRC7000ClearColorCodeSynchronisation(void)
{
	if (hrc.transmissionEnabled == false)
	{
		hrc.ccHold = ((nonVolatileSettings.dmrCcTsFilter & DMR_CC_FILTER_PATTERN) != 0);
		hrc.lastRxColorCode = 0xFF;
		hrc.lastRxColorCodeCount = 0;
		hrc.ccHoldTimer = 0;
	}
}

void HRC7000SetTalkerAlias(const char *text)
{
	snprintf(hrc.talkAliasText, sizeof(hrc.talkAliasText), "%s", text);
}

void HRC7000SetTalkerAliasLocation(uint32_t Lat, uint32_t Lon)
{
	double longDouble = latLongFixed32ToDouble(Lon);
	int32_t intLong = (33554432 * longDouble) / 360;
	double latDouble = latLongFixed32ToDouble(Lat);
	int32_t intLat = (16777216 * latDouble) / 180;

	hrc.talkAliasLocation[0] = (intLong >> 24) & 0x01;
	hrc.talkAliasLocation[1] = (intLong >> 16) & 0xFF;
	hrc.talkAliasLocation[2] = (intLong >> 8) & 0xFF;
	hrc.talkAliasLocation[3] = (intLong ) & 0xFF;

	hrc.talkAliasLocation[4] = (intLat >> 16) & 0xFF;
	hrc.talkAliasLocation[5] = (intLat >> 8) & 0xFF;
	hrc.talkAliasLocation[6] = (intLat >> 0) & 0xFF;
}

bool HRC7000IRQHandlerIsRunning(void)
{
	return hrc.inIRQHandler;
}

bool HRC7000HasGotSync(void)
{
	return (hrc.timeCode != -1);
}


//restore all important registers that may have been changed by FM mode
// What the OpenDM32 releases do in radioSetMode / radioSetTx / radioSetRx when entering DMR (0x303E12C in 20260711):
// frequency calibration, DMR mic gain, DAC on, WORK_MODE. They never soft-reset the modem or rewrite the DMR
// register set there; our radioHardwareInterface.c called the full HRC7000SetDMR() at those three places.
void HRC7000SetDMRModemCalibration(void)
{
	DM32_TRACE(0x300);
	dmr_compat_frequency_calibration(); // SIG_CENTER / RF_MOD_BIAS_CTRL of the channel (the radio service's)
	DM32_TRACE(0x301);
	HRC7000SetMicGainDMR(nonVolatileSettings.micGainDMR);
	DM32_TRACE(0x302);
	*DAC_PD_CTRL &= BITCLR(1);
	DM32_TRACE(0x303);
	*WORK_MODE = 0x80;
	DM32_TRACE(0x304);
	*WORK_MODE = 0x6E;
	DM32_TRACE(0x305);
}

void HRC7000SetDMR(void)
{
	// The modem soft reset and the DMR register set are only written once (the releases do this in their init);
	// repeating them on every switch to DMR receive/transmit reset the modem each time.
	static bool dmrRegistersWritten = false;

	DM32_TRACE(0x200);
	if (!dmrRegistersWritten)
	{
	dmrRegistersWritten = true;
	// No "SYS_SOFT_RSTN = 0x190" here: the releases write 0x190 only at the very start of their init and then
	// clear bit 4 before setting up the codec (as HRC7000Init() does), so bit 4 stays 0 while running.
	// Setting it again here killed DMR receive for good: no modem interrupts on a DMR signal, even back in FM.
//	*DAC_CONTROL = 0x8FFFFFDB;
	*DAC_CONTROL = 0x8000001B;
	DM32_TRACE(0x201);
	*ADC_CONTROL |= 0x41C3;
	DM32_TRACE(0x202);
//	*AUDIO_CONTROL &= 0xFFFFFFBF;
	// The releases write AUDIO_CONTROL = 0x02 only once, early in their modem init; HRC7000SetFMRx() then writes 0x40,
	// and in DMR receive release 0711 reads back 0. Writing 0x02 here, on the first switch from FM to DMR, left bit 1
	// set while receiving (and the modem saw no receive data: ADC status bits 31:30 clear, PHASE_OUT frozen).
	*AUDIO_CONTROL &= ~(BITSET(1) | BITSET(6));
	DM32_TRACE(0x203);
	*AUDIO_BUFFER_CLR = 0x03;
	DM32_TRACE(0x204);
	*SEND_DATA_SYNC_H = 0xD5D7F7;
	*SEND_DATA_SYNC_L = 0x7fd757;
	*SEND_RC_SYNC_H = 0x77D55F;
	*SEND_RC_SYNC_L = 0x7DFD77;
	*RECV_MS_SYNC_H = 0xD5D7F7;
	*RECV_MS_SYNC_L = 0x7FD757;
	*RECV_BS_SYNC_H = 0xDFF57D;
	*RECV_BS_SYNC_L = 0x75DF5D;
	*RECV_TDMA1_SYNC_H = 0xF7FDD5;
	*RECV_TDMA1_SYNC_L = 0xDDFD55;
	*RECV_TDMA2_SYNC_H = 0xD7557F;
	*RECV_TDMA2_SYNC_L = 0x5FF7F5;
	*RECV_RC_SYNC_H = 0x0077D55F;
	*RECV_RC_SYNC_L = 0x007DFD77;
	DM32_TRACE(0x205);
	*THRESHOLD_VALUE = 0x0978786F;
	DM32_TRACE(0x206);
	*PI_CRC_MASK = 0x6969;
	*RS_H_MASK = 0x969696;
	*RS_H_INIT = 0x00;
	*RS_T_MASK = 0x999999;
	*RS_T_INIT = 0x00;
	*CSBK_CRC_MASK = 0xA5A5;
	*MBCH_CRC_MASK = 0xAAAA;
	*DATAH_CRC_MASK = 0xCCCC;
	*REG_X01 = 0xF0;
	*REG_X02 = 0x1FF;
	*REG_X03 = 0x10F;
	*REG_X04 = 0x00;
	*CRC8_INIT = 0x00;
	*VOICE_EMB_CTRL = 0x00;
	*SCRAMBLE_REG = 0x00;
	DM32_TRACE(0x207);
	*SYS_INTERP_MASK = 0x1187F;
	DM32_TRACE(0x208);
	*LAYER2_SLOT_PRE = 0x551010;
	*REG_X06 = 0x14;
	DM32_TRACE(0x209);
	*RF_RECV_DELAY = 0x36B0;
	DM32_TRACE(0x20A);
	*RX_IF_FREQ = 0xBB800;
	DM32_TRACE(0x20B);
	*TX_IF_FREQ = 0xBB800;
	DM32_TRACE(0x20C);
	*RECV_MS_SYNC_H |= BITSET(31);
	*RECV_BS_SYNC_H |= BITSET(31);
	DM32_TRACE(0x20D);
	}
	// As in the OpenDM32 releases (HRC7000SetDMR at 0x303E16C in 20260711):
	// RF_CONTROL / RF_MODE keep their HRC7000Init() values (0x00041F0A / 0x03269860) in DMR too,
	// the DAC is powered up (HRC7000SetFMRx() powers it down), the DMR mic gain is re-applied
	// (FM uses the same GCR_ADCL register), and the layer 2 timeslots are switched off and on again.
	HRC7000SetDMRModemCalibration();
	if (isReceiving)
	{
		*AUDIO_CONTROL |= BITSET(4); // AUDIO_CONTROL = 0x02 above stops the mic capture started by soundReceiveData()
	}
	DM32_TRACE(0x210);
	*VOICE_EMB_CTRL = 0x02;
	DM32_TRACE(0x211);
	HRC7000SetRepeaterMode(currentRadioDevice->trxDMRModeTx == DMR_MODE_RMO);
	if (repeaterMode)
	{
		hrc7000RepeaterRxReset();
	}
	else
	{
	*LAYER2_CONTROL = 0xC0;
	DM32_TRACE(0x212);
	*LAYER2_SLOTON = 0x00;
	DM32_TRACE(0x213);
	*LAYER2_TXRX_CTRL = 0x800000;
	DM32_TRACE(0x214);
	*LAYER2_TXRX_CTRL = 0x40;
	DM32_TRACE(0x215);
	*LAYER2_SLOTON = 0x05;
	}
	DM32_TRACE(0x216);
}

void HRC7000SetLineOut(bool isOn)
{
	if (isOn) *CR_DAC &= BITCLR(7); //Enable or disable the DAC output to Line out
	else *CR_DAC |= BITSET(7);
}

void HRC7000SetMic(bool isOn)
{
	if (isOn) *CR_ADC &= BITCLR(7); //Enable or disable the Mic Input to Line in 1
	else *CR_ADC |= BITSET(7);
//	GPIO_DRV_SetOutput( MIC_MUT, (isOn ? GPIO_PIN_SET : GPIO_PIN_RESET )); // Switch Mute Mic (low mic off/ hi mic on)
}

void HRC7000SetDmrRxGain(int8_t gain)
{

}

void HRC7000SetDmrAGCGain(int8_t gain)
{

}

bool HRC7000CCIsHeld(void)
{
	return hrc.ccHold;
}
