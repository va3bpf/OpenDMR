/*
 * Copyright (C) 2019 Kai Ludwig, DG4KLU
 *           (C) 2020-2024 Roger Clark, VK3KYY / G4KYF
 *           (C) 2026 Junqiang Shi, VA3BPF / BG8AM
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

/*
 * OpenDMR, DMR: the codec calls of the HR_C7000 engine (the main firmware's dmr_codec/codec.c and
 * codec_interface.c), on our own AMBE+2 vocoder (dmr/vocoder.c, vocoder/c/ambe). The stock codec is not supported.
 * A 9 byte AMBE frame is 160 samples, two 80 sample wave buffers of dmr_sound.c.
 */

#include <string.h>
#include "dmr_compat.h"
#include "codec.h"
#include "vocoder.h"

void codecInit(bool fromVoicePrompts)
{
	// Need to prevent the DMR side of the code initialising the codec and sound buffers when the voice prompts are playing
	// This could be done in every location this function is called, but it saves space if the check is done inside the function.
	if ((fromVoicePrompts == false) && voicePromptsIsPlaying())
	{
		return;
	}
	vocoder_sync();
	vocoderReset();
	soundInit();
}

void codecDecode(uint8_t *indata_ptr, int numbBlocks)
{
	static int16_t pcm[160];

	vocoder_sync();
	for (int idx = 0; idx < numbBlocks; idx++)
	{
		vocoderDecodeFrame(indata_ptr + 9 * idx, pcm);
		for (int half = 0; half < 2; half++)
		{
			soundSetupBuffer();
			memcpy(currentWaveBuffer, &pcm[80 * half], 160);
			soundStoreBuffer();
		}
	}
}

void codecEncodeBlock(uint8_t *outdata_ptr)
{
	static int16_t pcm[160];

	vocoder_sync();
	for (int half = 0; half < 2; half++)
	{
		soundRetrieveBuffer();
		memcpy(&pcm[80 * half], currentWaveBuffer, 160);
	}
	vocoderEncodeFrame(pcm, outdata_ptr);
}
