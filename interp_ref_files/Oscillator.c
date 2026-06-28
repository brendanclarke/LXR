/*
 * Oscillator.c
 *
 *  Created on: 02.04.2012
 * ------------------------------------------------------------------------------------------------------------------------
 *  Copyright 2013 Julian Schmidt
 *  Julian@sonic-potions.com
 * ------------------------------------------------------------------------------------------------------------------------
 *  This file is part of the Sonic Potions LXR drumsynth firmware.
 * ------------------------------------------------------------------------------------------------------------------------
 *  Redistribution and use of the LXR code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *       - The code may not be sold, nor may it be used in a commercial product or activity.
 *
 *       - Redistributions that are modified from the original source must include the complete
 *         source code, including the source code for all components used by a binary built
 *         from the modified sources. However, as a special exception, the source code distributed
 *         need not include anything that is normally distributed (in either source or binary form)
 *         with the major components (compiler, kernel, and so on) of the operating system on which
 *         the executable runs, unless that component itself accompanies the executable.
 *
 *       - Redistributions must reproduce the above copyright notice, this list of conditions and the
 *         following disclaimer in the documentation and/or other materials provided with the distribution.
 * ------------------------------------------------------------------------------------------------------------------------
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 *   WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *   USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ------------------------------------------------------------------------------------------------------------------------
 */

/*
 *  Modified on: 17.05.2026
 * ------------------------------------------------------------------------------------------------------------------------
 *  Modifications Copyright 2026 Brendan Clarke
 *  brendanpaulclarke@gmail.com
 *  https://www.brendanclarke.com
 * ------------------------------------------------------------------------------------------------------------------------
 *  The modifications to this file are part of the LXR02 Open-Source software.
 *  The same license and restrictions on use for the LXR software apply.
 * ------------------------------------------------------------------------------------------------------------------------
 */



#include "Oscillator.h"
#include "random.h"
#include "Samples.h"
#include "MidiParser.h"
#include "MidiNoteNumbers.h"
#include "modulationNode.h"
#include <math.h>
// TODO DSP_PORT
// #include "sequencer.h"


//TODO die phaseInc berechnung kann man doch sicher per LUT machen!
//-----------------------------------------------------------
static inline uint8_t freqToTableIndex(const float f)
{
	if (f <= 0.f) {
		return 0;
	}

	// Use a real Hz->MIDI mapping so frequencies below 440Hz don't collapse
	// to a bogus table index due to integer truncation.
	const float midi = 69.f + (12.f * log2f(f / 440.f));
	int tableIndex = (int)(midi / 12.f);

	if (tableIndex < 0) {
		tableIndex = 0;
	} else if (tableIndex > 10) {
		tableIndex = 10;
	}

	return (uint8_t)tableIndex;
}
//-----------------------------------------------------------
static inline uint32_t freq2PhaseIncr(const float f) //4096
{
	return (((TABLESIZE*f)/REAL_FS))*1048576 ; // 1048576 <-> (<<20)
}
//-----------------------------------------------------------
static inline uint32_t freq2PhaseIncr1024(const float f)
{
	return (((1024*f)/REAL_FS))*4194304 ; //(<<22)
}
//-----------------------------------------------------------
static inline uint32_t freq2PhaseIncr32767(const float f)
{
	return (((1024*f)/REAL_FS))*131072 ; //(<<17)
}
//-----------------------------------------------------------
static INDTCMZ int16_t osc_interp_a[OUTPUT_DMA_SIZE];
static INDTCMZ int16_t osc_interp_b[OUTPUT_DMA_SIZE];

/* Waveform interpolation is intentionally block-based here. The old scalar
** approach evaluated both neighboring waveforms through the normal dispatcher
** for every output sample. Rendering each neighbor once into a 32-frame scratch
** buffer keeps the audible interpolation behavior but turns the extra work into
** two ordinary block renders plus a cheap blend loop. */
static inline void osc_clearWaveInterp(OscInfo* osc)
{
	osc->waveInterpFrac = 0.f;
	osc->waveInterpNext = osc->waveform;
	osc->waveInterpGeneration = 0u;
}
//-----------------------------------------------------------
static inline void osc_updateSampleCache(OscInfo* osc, uint8_t sampleIndex)
{
	/* User sample metadata lives in flash-backed SampleMemory tables. The
	** generation counter lets oscillators keep cached pointers/sizes across
	** audio blocks and invalidate them after a sample/loop install refreshes
	** the manifest. */
	const uint32_t generation = sampleMemory_getGeneration();

	if (osc->sampleCacheData &&
		osc->sampleCacheIndex == sampleIndex &&
		osc->sampleCacheGeneration == generation) {
		return;
	}

	SampleInfo info = sampleMemory_getSampleInfo(sampleIndex);
	osc->sampleCacheIndex = sampleIndex;
	osc->sampleCacheGeneration = generation;
	osc->sampleCacheSize = info.size;
	osc->sampleCacheData = (const int16_t*)((const int8_t*)info.offset);
	osc->sampleCacheLooped = sampleMemory_isLooped(sampleIndex);
}
//-----------------------------------------------------------
static inline uint8_t osc_waveInterpActive(const OscInfo* osc)
{
	const uint8_t maxWave = (uint8_t)(OSC_SAMPLE_START + sampleMemory_getNumSamples() - 1u);

	if (!modNode_getWaveInterpEnabled()) {
		return 0u;
	}
	if (osc->waveInterpGeneration != modNode_getWaveInterpGeneration()) {
		return 0u;
	}
	if (osc->waveInterpFrac <= 0.f || osc->waveInterpFrac >= 1.f) {
		return 0u;
	}
	if (osc->waveInterpNext <= osc->waveform) {
		return 0u;
	}
	if (osc->waveform > maxWave || osc->waveInterpNext > maxWave) {
		return 0u;
	}
	return 1u;
}
//-----------------------------------------------------------
static inline int16_t osc_evalWaveAtPhase(const OscInfo* src, const uint8_t waveform, const uint32_t phase)
{
	OscInfo tmp = *src;
	int16_t out = 0;

	tmp.waveform = waveform;
	tmp.phase = phase;
	tmp.waveInterpFrac = 0.f;
	tmp.waveInterpNext = waveform;
	tmp.waveInterpGeneration = 0u;
	calcNextOscSampleBlock(&tmp, &out, 1u, 1.f);
	return out;
}
//-----------------------------------------------------------
static inline int16_t osc_evalWaveAtPhaseFm(const OscInfo* src, const uint8_t waveform, const uint32_t phase, const int16_t modSample)
{
	OscInfo tmp = *src;
	int16_t out = 0;
	int16_t modBuf[1] = { modSample };

	tmp.waveform = waveform;
	tmp.phase = phase;
	tmp.waveInterpFrac = 0.f;
	tmp.waveInterpNext = waveform;
	tmp.waveInterpGeneration = 0u;
	calcNextOscSampleFmBlock(&tmp, modBuf, &out, 1u, 1.f);
	return out;
}
//-----------------------------------------------------------
static void calcPeriodicInterpBlock(OscInfo* osc, int16_t* buf, const uint8_t size, const float gain)
{
	const uint8_t base = osc->waveform;
	const uint8_t next = osc->waveInterpNext;
	const float frac = osc->waveInterpFrac;
	const uint32_t startPhase = osc->phase;
	OscInfo oscA = *osc;
	OscInfo oscB = *osc;
	uint8_t i;

	oscA.waveform = base;
	osc_clearWaveInterp(&oscA);
	oscB.waveform = next;
	osc_clearWaveInterp(&oscB);

	calcNextOscSampleBlock(&oscA, osc_interp_a, size, 1.f);
	calcNextOscSampleBlock(&oscB, osc_interp_b, size, 1.f);

	for (i = 0; i < size; i++) {
		const int16_t a = osc_interp_a[i];
		const int16_t b = osc_interp_b[i];
		const int16_t out = (int16_t)(a + frac * (b - a));
		buf[i] = out * gain;
	}
	osc->phase = startPhase + (osc->phaseInc * (uint32_t)size);
}
//-----------------------------------------------------------
static int16_t calcPeriodicInterp(OscInfo* osc)
{
	const uint8_t base = osc->waveform;
	const uint8_t next = osc->waveInterpNext;
	const float frac = osc->waveInterpFrac;
	const uint32_t phase = osc->phase;

	const int16_t a = osc_evalWaveAtPhase(osc, base, phase);
	const int16_t b = osc_evalWaveAtPhase(osc, next, phase);
	const int16_t out = (int16_t)(a + frac * (b - a));

	osc->phase = phase + osc->phaseInc;
	osc->output = out;
	return out;
}
//-----------------------------------------------------------
static void calcPeriodicInterpFmBlock(OscInfo* osc, int16_t* modBuffer, int16_t* buf, const uint8_t size, const float gain)
{
	const uint8_t base = osc->waveform;
	const uint8_t next = osc->waveInterpNext;
	const float frac = osc->waveInterpFrac;
	const uint32_t startPhase = osc->phase;
	OscInfo oscA = *osc;
	OscInfo oscB = *osc;
	int16_t lastOut = 0;
	uint8_t i;

	oscA.waveform = base;
	osc_clearWaveInterp(&oscA);
	oscB.waveform = next;
	osc_clearWaveInterp(&oscB);

	calcNextOscSampleFmBlock(&oscA, modBuffer, osc_interp_a, size, 1.f);
	calcNextOscSampleFmBlock(&oscB, modBuffer, osc_interp_b, size, 1.f);

	for (i = 0; i < size; i++) {
		const int16_t a = osc_interp_a[i];
		const int16_t b = osc_interp_b[i];
		lastOut = (int16_t)(a + frac * (b - a));
		buf[i] = lastOut * gain;
	}
	osc->phase = startPhase + (osc->phaseInc * (uint32_t)size);
	osc->output = lastOut;
}
//-----------------------------------------------------------
static int16_t calcPeriodicInterpFm(OscInfo* osc, OscInfo* modOsc)
{
	const uint8_t base = osc->waveform;
	const uint8_t next = osc->waveInterpNext;
	const float frac = osc->waveInterpFrac;
	const uint32_t phase = osc->phase;

	const int16_t a = osc_evalWaveAtPhaseFm(osc, base, phase, modOsc->output);
	const int16_t b = osc_evalWaveAtPhaseFm(osc, next, phase, modOsc->output);
	const int16_t out = (int16_t)(a + frac * (b - a));

	osc->phase = phase + osc->phaseInc;
	osc->output = out;
	return out;
}
//-----------------------------------------------------------
INITCM void calcSineBlock(OscInfo* osc, int16_t* buf, const uint8_t size ,const float gain)
{
	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase = osc->phase;
		int16_t oscOut;

		const uint32_t index =  oscPhase;
		uint32_t  itg	= index>>20;

		#if INTERPOLATE_OSC
			oscOut = sine_table[itg++];
			const float frac = (index & 0xfffff) * 0.00000095367431640625f;
			oscOut += frac*(sine_table[itg] - oscOut);
		#else
			oscOut = sine_table[itg];
		#endif

		osc->phase = oscPhase + osc->phaseInc;

		buf[i] = oscOut * gain;
	}
}
//-----------------------------------------------------------
int16_t calcSine(OscInfo* osc)
{
	const uint32_t oscPhase = osc->phase;
	int16_t oscOut;

	const uint32_t index =  oscPhase;
	uint32_t  itg	= index>>20;

#if INTERPOLATE_OSC
	oscOut = sine_table[itg++];
	float frac	= (index & 0xfffff) * 0.00000095367431640625f;
	oscOut += frac*(sine_table[itg] - oscOut);
#else
	oscOut = sine_table[itg];
#endif

	osc->phase = oscPhase + osc->phaseInc;
	osc->output = oscOut;
	return oscOut;
}

//-----------------------------------------------------------
INITCM void calcFmSineBlock(OscInfo* osc, int16_t* modBuffer, int16_t* buf, uint8_t size,const float gain)
{
	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase = osc->phase;
		int16_t oscOut ;

		const uint32_t index =  oscPhase + (((uint32_t)(modBuffer[i]*osc->fmMod))<<17);
		uint32_t  itg	= index>>20;

	#if INTERPOLATE_FM_OSC
		oscOut = sine_table[itg++];
		const float frac	= (index & 0xfffff) * 0.00000095367431640625f;
		oscOut += frac*(sine_table[itg] - oscOut);
	#else
		oscOut = sine_table[itg];
	#endif

		osc->phase = oscPhase + osc->phaseInc;
		buf[i] =  oscOut * gain;
	}
}
//-----------------------------------------------------------
int16_t calcFmSine(OscInfo* osc, OscInfo* modOsc)
{
	const uint32_t oscPhase 	= osc->phase;
	int16_t oscOut ;

	const uint32_t index =  oscPhase + (((uint32_t)(modOsc->output*osc->fmMod))<<17);
	uint32_t  itg	= index>>20;

#if INTERPOLATE_FM_OSC
	oscOut = sine_table[itg++];
	const float frac	= (index & 0xfffff) * 0.00000095367431640625f;
	oscOut += frac*(sine_table[itg] - oscOut);
#else
	oscOut = sine_table[itg];
#endif

	osc->phase = oscPhase + osc->phaseInc;
	osc->output = oscOut;
	return oscOut;
};
//---------------------------------------------------------------
INITCM void calcFmBlock(OscInfo* osc, const int16_t table[][1024], int16_t* modBuffer, int16_t* buf, uint8_t size ,const float gain)
{
	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase = osc->phase;
		int16_t oscOut ;
		const uint32_t index =  oscPhase + (((uint32_t)(modBuffer[i]*osc->fmMod))<<19);

		uint32_t  itg	= index>>22;

	#if INTERPOLATE_FM_OSC
		const uint32_t next = (itg + 1u) & 0x000003FFu;
		oscOut = table[osc->tableOffset][itg];
		const float frac	= (index&0x003FFFFF)*2.38418579101562e-07f;
		oscOut += frac*(table[osc->tableOffset][next] - oscOut);

	#else
		oscOut = table[overtoneIndex][itg];
	#endif

		osc->phase = oscPhase + osc->phaseInc;
		osc->output = oscOut;
		buf[i] = oscOut * gain;
	}
}
//-----------------------------------------------------------
int16_t calcFm(OscInfo* osc, OscInfo* modOsc, const int16_t table[][1024])
{

	const uint32_t oscPhase = osc->phase;
	int16_t oscOut ;

	const uint32_t index =  oscPhase + (((uint32_t)(modOsc->output*osc->fmMod))<<19);

	uint32_t  itg	= index>>22;

#if INTERPOLATE_FM_OSC
	const uint32_t next = (itg + 1u) & 0x000003FFu;
	oscOut = table[osc->tableOffset][itg];
	const float frac	= (index&0x003FFFFF)*2.38418579101562e-07f;
	oscOut += frac*(table[osc->tableOffset][next] - oscOut);
#else
	oscOut = table[overtoneIndex][itg];
#endif

	osc->phase = oscPhase + osc->phaseInc;
	osc->output = oscOut;
	return oscOut;
}
//-----------------------------------------------------------
INITCM void calcNextOscSampleFmBlock(OscInfo* osc, int16_t* modBuffer, int16_t* buf, uint8_t size ,const float gain)
{
	if (osc_waveInterpActive(osc)) {
		calcPeriodicInterpFmBlock(osc, modBuffer, buf, size, gain);
		return;
	}

	switch(osc->waveform)
		{
		case SINE:
			calcFmSineBlock(osc,modBuffer,buf,size,gain);
			break;

		case SAW:
			calcFmBlock(osc,sawTable,modBuffer,buf,size,gain);
			break;

		case TRI:
			calcFmBlock(osc, triTable, modBuffer,buf,size,gain);
			break;

		case REC:
			calcFmBlock(osc,recTable,modBuffer,buf,size,gain);
			break;

		case NOISE:
			//TODO FM noise - needed since its tuned digi noise!
			calcNoiseBlock(osc,buf,size,gain);
			break;

		case CRASH:
			calcSampleOscFmBlock(osc,modBuffer,buf,size,gain);
			break;

		default:
			calcUserSampleOscFmBlock(osc,modBuffer,buf,size,gain);
			break;

		}
}
//-----------------------------------------------------------
int16_t calcNextOscSampleFm(OscInfo* osc,OscInfo* modOsc)
{
	if (osc_waveInterpActive(osc)) {
		return calcPeriodicInterpFm(osc, modOsc);
	}

	switch(osc->waveform)
	{
	case SINE:
		return calcFmSine(osc,modOsc);
		break;

	case SAW:
		return calcFm(osc,modOsc,sawTable);
		break;

	case TRI:
		return calcFm(osc,modOsc, triTable);
		break;

	case REC:
		return calcFm(osc,modOsc,recTable);
		break;

	case NOISE:
		return calcNoise(osc);
		break;

	case CRASH:
		return calcSampleOscFm(osc,modOsc);
		break;

	default:

		return 0;
		break;

	}
	return 0;
}
//-----------------------------------------------------------
INITCM void calcNoiseBlock(OscInfo* osc, int16_t* buf, const uint8_t size ,const float gain)
{
	int i;
	for(i=0;i<size;i++)
	{
		const uint32_t lastPhase = osc->phase;
		osc->phase += osc->phaseInc;

		if(lastPhase > osc->phase)
		{
			//overflow happened -> phaseWrapped
			// osc->output = (int16_t)(GetRngValue()& 0x7FFF); //normal pitched white noise
			osc->output = (int16_t)(GetRngValue()); //normal pitched white noise
		}

		buf[i] = osc->output * gain;
	}
}
//-----------------------------------------------------------
int16_t calcNoise(OscInfo* osc)
{
	osc->phase += osc->phaseInc;

	//check overflow flag, ( if osc->phase == osc->phaseInc a reset occured ==> retrigger, too
	APSR_Type apsr;
	apsr.w = __get_APSR();
	if(apsr.b.V ||  (osc->phase == osc->phaseInc))
	{
		//overflow happened -> phaseWrapped

		// uint16_t rnd = (int16_t)(GetRngValue()& 0x7FFF);
		uint16_t rnd = (int16_t)(GetRngValue());
		// uint16_t rnd = 0;
		if( rnd > 0x00ff)
		{
			if( rnd > 0x000f)
			{
				osc->output = 32767;
			}
			else
			{
				osc->output = -32768;
			}

		}
	}
	else
	{
			osc->output = 0;
	}
	return osc->output;
}
//-----------------------------------------------------------
INITCM void calcNextOscSampleBlock(OscInfo* osc, int16_t* buf, const uint8_t size, const float gain)
{
	if (osc_waveInterpActive(osc)) {
		calcPeriodicInterpBlock(osc, buf, size, gain);
		return;
	}

	switch(osc->waveform)
		{
		case SINE:
			calcSineBlock(osc,buf,size, gain);
			break;

		case SAW:
			calcWavetableOscBlock(osc,sawTable,buf,size, gain);
			break;

		case TRI:
			calcWavetableOscBlock(osc,triTable,buf,size, gain);
			break;

		case REC:
			calcWavetableOscBlock(osc,recTable,buf,size, gain);
			break;

		case NOISE:
			calcNoiseBlock(osc,buf,size, gain);
			break;

		case CRASH:
			calcSampleOscBlock(osc,buf,size, gain);
			break;

		default://sample playback
			if( osc->waveform - OSC_SAMPLE_START > sampleMemory_getNumSamples() ) return; // return if out of range

			calcUserSampleOscBlock(osc,buf,size, gain);
			break;
		}
}
//-----------------------------------------------------------
int16_t calcNextOscSample(OscInfo* osc)
{
	if (osc_waveInterpActive(osc)) {
		return calcPeriodicInterp(osc);
	}

	switch(osc->waveform)
	{
	case SINE:
		return calcSine(osc);
		break;

	case SAW:
		return calcWavetableOsc(osc,sawTable);
		break;

	case TRI:
		return calcWavetableOsc(osc,triTable);
		break;

	case REC:
		return calcWavetableOsc(osc,recTable);
		break;

	case NOISE:
		return calcNoise(osc);
		break;

	case CRASH:
		return calcSampleOsc(osc);
		break;

	default:

		return 0;
		break;

	}
	return 0;
};
//---------------------------------------------------------------
INITCM void calcWavetableOscBlock(OscInfo* osc, const int16_t table[][1024], int16_t* buf, const uint8_t size ,const float gain)
{
	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase = osc->phase;
			int16_t oscOut ;
			uint32_t  itg	= oscPhase>>22;
		#if INTERPOLATE_OSC
			//todo use ldm instead of ldr to laod multiple values from memory
			const uint32_t next = (itg + 1u) & 0x000003FFu;
			oscOut = table[osc->tableOffset][itg];
			const float frac = (oscPhase & 0x003FFFFF) * 2.384185791015625e-07f; // 1 / 2^22
			oscOut += frac*(table[osc->tableOffset][next] - oscOut);
		#else
			oscOut = table[osc->tableOffset][itg];
		#endif

			osc->phase = oscPhase + osc->phaseInc;
			buf[i] = oscOut * gain;
	}
}

//---------------------------------------------------------------
int16_t calcWavetableOsc(OscInfo* osc,  const int16_t table[][1024])
{
	const uint32_t oscPhase = osc->phase;
	int16_t oscOut ;

	uint32_t  itg	= oscPhase>>22;

#if INTERPOLATE_OSC
	//todo use ldm instead of ldr to laod multiple values from memory
	const uint32_t next = (itg + 1u) & 0x000003FFu;
	oscOut = table[osc->tableOffset][itg];
	const float frac = (oscPhase & 0x003FFFFF) * 2.384185791015625e-07f; // 1 / 2^22
	oscOut += frac*(table[osc->tableOffset][next] - oscOut);
#else
	oscOut = table[osc->tableOffset][itg];
#endif

	osc->phase = oscPhase + osc->phaseInc;
	osc->output = oscOut;
	return oscOut;

};

//------------------------------------------------------------------
INITCM void calcSampleOscFmBlock(OscInfo* osc,int16_t* modBuffer, int16_t* buf, uint8_t size ,const float gain)
{
	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase 	= osc->phase;
		int16_t oscOut ;

		const uint32_t index =  oscPhase + (((uint32_t)(modBuffer[i]*osc->fmMod))<<14);
		uint32_t  itg	= index>>17;

	#if INTERPOLATE_FM_OSC
		oscOut = crashSample[itg++];
		const float frac	= (index & 0x0001FFFF) * 0.00000762939453125f; // 1 / 2^17
		oscOut += frac*(crashSample[itg] - oscOut);
	#else
		oscOut = crashSample[itg];
	#endif

		oscOut = (oscOut - 127)*256;

		osc->phase = oscPhase + osc->phaseInc;
		osc->output = oscOut;
		buf[i] = oscOut * gain;
	}
}
//------------------------------------------------------------------
int16_t calcSampleOscFm(OscInfo* osc, OscInfo* modOsc)
{

	const uint32_t oscPhase 	= osc->phase;
	int16_t oscOut ;

	const uint32_t index =  oscPhase + (((uint32_t)(modOsc->output*osc->fmMod))<<14);
	uint32_t  itg	= index>>17;

#if INTERPOLATE_FM_OSC
	oscOut = crashSample[itg++];
	const float frac	= (index & 0x0001FFFF) * 0.00000762939453125f; // 1 / 2^17
	oscOut += frac*(crashSample[itg] - oscOut);
#else
	oscOut = crashSample[itg];
#endif

	oscOut = (oscOut - 127)*256;

	osc->phase = oscPhase + osc->phaseInc;
	osc->output = oscOut;
	return oscOut;
};
//------------------------------------------------------------------
INITCM void calcUserSampleOscFmBlock(OscInfo* osc,int16_t* modBuffer, int16_t* buf, uint8_t size ,const float gain)
{
	uint8_t sampleIndex = osc->waveform - OSC_SAMPLE_START;
	osc_updateSampleCache(osc, sampleIndex);
	uint32_t sampleSize = osc->sampleCacheSize;
	uint8_t looped = osc->sampleCacheLooped;

	//cast sample data to signed int16_t array
	const int16_t* sampleData = osc->sampleCacheData;
	if(sampleSize == 0) return;

	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase 	= osc->phase;
		int16_t oscOut ;

		const uint32_t index =  oscPhase + (((uint32_t)(modBuffer[i]*osc->fmMod))<<14);
		uint32_t  itg	= index>>17;

		if(looped && itg >= sampleSize)
			itg %= sampleSize;

	#if INTERPOLATE_FM_OSC
		oscOut = sampleData[itg];
		const float frac	= (index & 0x0001FFFF) * 0.00000762939453125f; // 1 / 2^17
		if(looped && (itg + 1u) >= sampleSize)
			oscOut += frac*(sampleData[0] - oscOut);
		else
			oscOut += frac*(sampleData[itg + 1u] - oscOut);
		itg++;
	#else
		oscOut = sampleData[itg];
	#endif

		//one shot
		if(looped)
		{
			uint32_t nextPhase = oscPhase + osc->phaseInc;
			if(sampleSize < 32768u) {
				uint32_t loopPhase = sampleSize << 17;
				if(nextPhase >= loopPhase)
					nextPhase %= loopPhase;
			}
			osc->phase = nextPhase;
		}
		else if(itg < sampleSize)
		{
			osc->phase = oscPhase + osc->phaseInc;
		}
		osc->output = oscOut;
		buf[i] = oscOut * gain;
	}
}
//---------------------------------------------------------------
INITCM void calcUserSampleOscBlock(OscInfo* osc, int16_t* buf, const uint8_t size ,const float gain)
{
	uint8_t sampleIndex = osc->waveform - OSC_SAMPLE_START;
	osc_updateSampleCache(osc, sampleIndex);
	uint32_t sampleSize = osc->sampleCacheSize;
	uint8_t looped = osc->sampleCacheLooped;

	//cast sample data to signed int16_t array
	const int16_t* sampleData = osc->sampleCacheData;
	if(sampleSize == 0) return;

	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase = osc->phase;
		int16_t oscOut ;
		uint32_t  itg	= oscPhase>>17;

		if(looped && itg >= sampleSize)
			itg %= sampleSize;

	#if INTERPOLATE_OSC

		oscOut = sampleData[itg];
		const float frac = (oscPhase & 0x0001FFFF) * 0.00000762939453125f; // 1 / 2^17
		if(looped && (itg + 1u) >= sampleSize)
			oscOut += frac*(sampleData[0] - oscOut);
		else
			oscOut += frac*(sampleData[itg + 1u] - oscOut);
		itg++;
	#else
		oscOut = sampleData[itg];
	#endif
	//	oscOut = (oscOut - 127)*256;

		//one shot
		if(looped)
		{
			uint32_t nextPhase = oscPhase + osc->phaseInc;
			if(sampleSize < 32768u) {
				uint32_t loopPhase = sampleSize << 17;
				if(nextPhase >= loopPhase)
					nextPhase %= loopPhase;
			}
			osc->phase = nextPhase;
		}
		else if(itg < sampleSize)
		{
			osc->phase = oscPhase + osc->phaseInc;
		}
		buf[i] = oscOut * gain;
	}
}
//---------------------------------------------------------------
INITCM void calcSampleOscBlock(OscInfo* osc, int16_t* buf, const uint8_t size ,const float gain)
{
	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase = osc->phase;
		int16_t oscOut ;
		uint32_t  itg	= oscPhase>>17;


	#if INTERPOLATE_OSC
		//todo use ldm instead of ldr to laod multiple values from memory
		oscOut = crashSample[itg++];
		const float frac = (oscPhase & 0x0001FFFF) * 0.00000762939453125f; // 1 / 2^17
		oscOut += frac*(crashSample[itg] - oscOut);
	#else
		oscOut = crashSample[itg];
	#endif
		oscOut = (oscOut - 127)*256;

		osc->phase = oscPhase + osc->phaseInc;
		buf[i] = oscOut * gain;
	}
}
//---------------------------------------------------------------
int16_t calcSampleOsc(OscInfo* osc)
{
	const uint32_t oscPhase = osc->phase;
	int16_t oscOut ;

	uint32_t  itg	= oscPhase>>17;

#if INTERPOLATE_OSC
	//todo use ldm instead of ldr to laod multiple values from memory
	oscOut = crashSample[itg++];
	const float frac = (oscPhase & 0x0001FFFF) * 0.00000762939453125f; // 1 / 2^17
	oscOut += frac*(crashSample[itg] - oscOut);
#else
	oscOut = crashSample[itg];
#endif
	oscOut = (oscOut - 127)*256;

	osc->phase = oscPhase + osc->phaseInc;
	osc->output = oscOut;
	return oscOut;

};
 //-----------------------------------------------------------
static void osc_calcSineFreqValue(OscInfo* osc, const float currentFreq)
{
	osc->phaseInc = freq2PhaseIncr(currentFreq);
}
//-----------------------------------------------------------
static void osc_calcNoiseFreqValue(OscInfo* osc, const float currentFreq)
{
	osc_calcSineFreqValue(osc, currentFreq);
}
//-----------------------------------------------------------
static void osc_calcWavetableFreqValue(OscInfo* osc, const float currentFreq)
{
	osc->phaseInc = freq2PhaseIncr1024(currentFreq);

	const uint8_t overtoneIndex = freqToTableIndex(currentFreq);
	osc->tableOffset = overtoneIndex>10?10:overtoneIndex;
}
//-----------------------------------------------------------
static void osc_calcSampleFreqValue(OscInfo* osc, const float currentFreq)
{
	osc->phaseInc = freq2PhaseIncr32767(currentFreq);
}
//-----------------------------------------------------------
static void osc_calcUserSampleFreqValue(OscInfo* osc, const float currentFreq)
{
	osc_calcSampleFreqValue(osc, currentFreq);
}
//-----------------------------------------------------------
 INITCM void osc_setFreq(OscInfo* osc)
 {
		const float currentFreq = osc->freq*osc->pitchMod*osc->modNodeValue;
		/* osc_setFreq() is called frequently by the block dispatcher. Cache the
		** effective frequency+waveform tuple so unchanged blocks skip log2f(),
		** phase-increment recalculation, and wavetable octave selection. */
		if (osc->freqCacheValid &&
			osc->freqCacheWaveform == osc->waveform &&
			osc->freqCacheValue == currentFreq) {
			return;
		}

		switch(osc->waveform)
		{
		case SINE:
			osc_calcSineFreqValue(osc, currentFreq);
			break;

		case SAW:
			osc_calcWavetableFreqValue(osc, currentFreq);
			break;

		case TRI:
			osc_calcWavetableFreqValue(osc, currentFreq);
			break;

		case REC:
			osc_calcWavetableFreqValue(osc, currentFreq);
			break;

		case NOISE:
			osc_calcNoiseFreqValue(osc, currentFreq);
			break;


		case CRASH:
			osc_calcSampleFreqValue(osc, currentFreq);
			break;

		//samples
		default:
			osc_calcUserSampleFreqValue(osc, currentFreq);
			break;

		}
		osc->freqCacheValue = currentFreq;
		osc->freqCacheWaveform = osc->waveform;
		osc->freqCacheValid = 1u;
 }
 //-----------------------------------------------------------
 void osc_setBaseNote(OscInfo* osc, uint8_t baseNote)
 {

	 //get fine tune
	 const float cent = midiParser_calcDetune(osc->midiFreq&0xff);
	 //calc coarse tune
	 int16_t note =  (osc->midiFreq>>8) + (baseNote-SEQ_DEFAULT_NOTE);
	 if(note>127)note=127;
	 if(note<0)note=0;

	 osc->freq = MidiNoteFrequencies[note]*cent;
	 osc->baseNote = baseNote;
 };

 //-----------------------------------------------------------
 void osc_recalcFreq(OscInfo* osc)
 {
	 //get fine tune
	 const float cent = midiParser_calcDetune(osc->midiFreq&0xff);
	 //calc coarse tune
	 int16_t note =  (osc->midiFreq>>8) + (osc->baseNote-SEQ_DEFAULT_NOTE);

	 if(note>127)note=127;
 	 if(note<0)note=0;

	 osc->freq = MidiNoteFrequencies[note]*cent;
 }
 //-----------------------------------------------------------
