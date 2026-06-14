/*
 * lfo.c
 *
 *  Created on: 14.01.2013
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

//-------------------------------------------------------------
#include "lfo.h"
#include "random.h"
#include <math.h>

#include "DrumVoice.h"
#include "CymbalVoice.h"
#include "HiHat.h"
#include "Snare.h"
#include "valueShaper.h"
//-------------------------------------------------------------
void lfo_init(Lfo *lfo)
{
	lfo->phase 			= 0;
	lfo->phaseInc 		= 1;
	lfo->phaseOffset	= 0;
	lfo->retrigger		= 0;
	lfo->waveform		= SINE;
	lfo->sync			= 0;
	lfo->freq			= 1;
	lfo->modNodeValue	= 1;
   lfo->delay        = 0;

	modNode_init(&lfo->modTarget);
}
//-------------------------------------------------------------
float lfo_calc(Lfo *lfo)
{
	uint32_t oldPhase 		= lfo->phase;
	float inc = lfo->phaseInc*lfo->modNodeValue;
	uint32_t incInt = inc;
	lfo->phase 			   += incInt;//lfo->phaseInc*lfo->modNodeValue;
	const uint8_t overflow 	= oldPhase>lfo->phase;

   if(lfo->waveform>=LFO_SINE_ONE) // sort cases where one shots are pre or post activity
   {
      if(overflow) // either we just hit the end of the delay phase, or one shot is past
      {
         if(lfo->delay) // we just hit the end of the delay phase
         {
            lfo->delay = 0;
            if (lfo->waveform==LFO_NOISE_ONE) // if there was delay, we generate a random now
            {
               lfo->rnd = GetRngValue();
			      lfo->rnd = lfo->rnd  / (float)0xffffffff ;
            }
         }
         else // the one-shot ended
         {
            lfo->phase=oldPhase;
            switch(lfo->waveform)
            {
            case LFO_EXP_UP_ONE: // these run up to 1 and hold
            case LFO_SAW_UP_ONE:
               return 1;
               break;
            case LFO_NOISE_ONE: // hold the random value until next retrigger
               return lfo->rnd;
               break;
            default:             // all other waveforms hold at 0
               return 0;
               break;
            }
          }
       }
       else if (lfo->delay) // one shot is in delay phase. hold at pre value 
       {
         switch(lfo->waveform)
         {
         case LFO_EXP_DOWN_ONE: // these start at 1 and run down while playing
         case LFO_SAW_DOWN_ONE:
            return 1;
            break;
         case LFO_NOISE_ONE:     // hold the random lfo at previous value
            return lfo->rnd;
            break;
         default:             // everything else starts at 0 during the delay phase
            return 0;
            break;
         }
       }
   }

	switch(lfo->waveform)
	{
	//---
		case LFO_SINE:
      case LFO_SINE_ONE:
			return ((sine_table[(lfo->phase>>20)]/32767.f) + 1)/2.f;
		break;
	//---
		case LFO_TRI:
      case LFO_TRI_ONE:
			return (1.f-fabsf( (lfo->phase/(float)0xffffffff)*2-1 ) );
			break;
	//---
		case LFO_SAW_UP:
      case LFO_SAW_UP_ONE:
		return lfo->phase/(float)0xffffffff ;
		break;
	//---
		case LFO_REC:
		if(lfo->phase > 0x7fffffff)
		{
			return 1.f;
		}
		else
		{
			return 0.f;
		}
		break;
   //---   
      case LFO_REC_ONE: // one shot rec gets reversed to start high
		if(lfo->phase > 0x7fffffff)
		{
			return 0.f;
		}
		else
		{
			return 1.f;
		}
		break;
	//---
		case LFO_NOISE:
      case LFO_NOISE_ONE:
		if(overflow)
		{
			lfo->rnd = GetRngValue();
			lfo->rnd = lfo->rnd  / (float)0xffffffff ;
		}
			return lfo->rnd;
		break;
	//---
		case LFO_SAW_DOWN:
      case LFO_SAW_DOWN_ONE:
		return (0xffffffff-(lfo->phase)) / (float)0xffffffff;
		break;
	//---
		case LFO_EXP_UP:
      case LFO_EXP_UP_ONE:
			{
				float x = lfo->phase/(float)0xffffffff;
				return x*x*x;
			}
			break;
	//---
		case LFO_EXP_DOWN:
      case LFO_EXP_DOWN_ONE:
			{
				float x = (0xffffffff-(lfo->phase)) / (float)0xffffffff;
				return x*x*x;//valueShaperF2F(x,-0.9f);
			}
			break;
	//---
      case LFO_EXP_TRI:
      case LFO_EXP_TRI_ONE:
         {
			   float x = (1.f-fabsf( (lfo->phase/(float)0xffffffff)*2-1 ) );
            return x*x*x;
         }
			break;
   //---
		default:
		return 0;
		break;
	}
	return 0;
}//-------------------------------------------------------------
void lfo_dispatchNextValue(Lfo* lfo)
{
	float val = lfo_calc(lfo);
	modNode_updateValue(&lfo->modTarget,val);
}
//-------------------------------------------------------------
uint32_t lfo_calcPhaseInc(float freq, uint8_t sync)
{
	if(sync==0)//no sync
	{
		return freq/(LFO_SR) * 0xffffffff;
	}

	//sync is on

	float bpm =  seq_getBpm();

	float tempoAsFrequency = bpm > 0.0 ? bpm / 60.f /4: 0.0f; //bpm/60 = beat duration /4 = bar duration

	float scaler;

	switch(sync)
	{
		default: //no sync

		return freq/(LFO_SR) * 0xffffffff;
		break;

		case 1: // 4/1
		scaler = 0.25f;//NoteTypeScalers[Quadruple] * NoteDivisionScalers[Whole];
		break;

		case 2: // 2/1
		scaler = 0.5f;//NoteTypeScalers[Double] * NoteDivisionScalers[Whole];
		break;

		case 3: // 1/1
		scaler = 1;//NoteTypeScalers[Regular] * NoteDivisionScalers[Whole];
		break;

		case 4: // 1/2
		scaler = 2;// NoteTypeScalers[Regular] * NoteDivisionScalers[Half];
		break;

		case 5: //1/3
		scaler = 3;// NoteTypeScalers[Regular] * NoteDivisionScalers[Third];
		break;

		case 6: // 1/4
		scaler = 4;// NoteTypeScalers[Regular] * NoteDivisionScalers[Quarter];
		break;

		case 7: // 1/6
		scaler = 6;// NoteTypeScalers[Regular] * NoteDivisionScalers[Sixth];
		break;

		case 8: // 1/8
		scaler = 8;// NoteTypeScalers[Regular] * NoteDivisionScalers[Eighth];
		break;

		case 9: // 1/12
		scaler = 12;//NoteTypeScalers[Regular] * NoteDivisionScalers[Twelfth];
		break;

		case 10: // 1/16
		scaler = 16;//NoteTypeScalers[Regular] * NoteDivisionScalers[Sixteenth];
		break;

		case 11: // 1/32
		scaler = 32;//NoteTypeScalers[Regular] * NoteDivisionScalers[Sixteenth];
		break;
	}

	float lfoSyncfreq = tempoAsFrequency * scaler;
	return (lfoSyncfreq / (LFO_SR)) * 0xffffffff;
}
//-------------------------------------------------------------
void lfo_setFreq(Lfo *lfo, float f)
{
	f += 1;
	f = f/128.f;
	f = f*f*f;
	lfo->freq = f*LFO_MAX_F;
	lfo->phaseInc = lfo_calcPhaseInc(lfo->freq,lfo->sync);
}
//-------------------------------------------------------------
void lfo_setSync(Lfo* lfo, uint8_t sync)
{
	lfo->sync = sync;
	lfo->phaseInc = lfo_calcPhaseInc(lfo->freq,lfo->sync);
}
//-------------------------------------------------------------
void lfo_recalcSync()
{
	Lfo* lfo = &voiceArray[0].lfo;
	lfo->phaseInc = lfo_calcPhaseInc(lfo->freq,lfo->sync);

	lfo = &voiceArray[1].lfo;
	lfo->phaseInc = lfo_calcPhaseInc(lfo->freq,lfo->sync);

	lfo = &voiceArray[2].lfo;
	lfo->phaseInc = lfo_calcPhaseInc(lfo->freq,lfo->sync);

	lfo = &snareVoice.lfo;
	lfo->phaseInc = lfo_calcPhaseInc(lfo->freq,lfo->sync);

	lfo = &cymbalVoice.lfo;
	lfo->phaseInc = lfo_calcPhaseInc(lfo->freq,lfo->sync);

	lfo = &hatVoice.lfo;
	lfo->phaseInc = lfo_calcPhaseInc(lfo->freq,lfo->sync);
}
//-------------------------------------------------------------
void lfo_retrigger(uint8_t voice)
{
   uint8_t isOneShot;
   /* -bc- for one shots, offset acts as a delay. for these, 'phase' is loaded with
      negative phase offset and runs up to 0, at which point the envelope will play.
      for the 'noise' signal, we generate one random value after the delay and hold 
      until the next retrigger. if there is no lfo delay, we need to generate it here.
   */
	if(voiceArray[0].lfo.retrigger == voice+1)
	{
      isOneShot = (voiceArray[0].lfo.waveform>=LFO_SINE_ONE);
      voiceArray[0].lfo.delay = isOneShot&&(voiceArray[0].lfo.phaseOffset);
      
      if (isOneShot)
         voiceArray[0].lfo.phase = -voiceArray[0].lfo.phaseOffset;
      else
         voiceArray[0].lfo.phase = voiceArray[0].lfo.phaseOffset;
      if (voiceArray[0].lfo.waveform==LFO_NOISE_ONE&&!voiceArray[0].lfo.phaseOffset)
      {
         voiceArray[0].lfo.rnd = GetRngValue();
			voiceArray[0].lfo.rnd = voiceArray[0].lfo.rnd  / (float)0xffffffff ;
      }
	}
	if(voiceArray[1].lfo.retrigger == voice+1)
	{
      isOneShot = (voiceArray[1].lfo.waveform>=LFO_SINE_ONE);
      voiceArray[1].lfo.delay = isOneShot&&(voiceArray[1].lfo.phaseOffset);
      
		if (isOneShot)
         voiceArray[1].lfo.phase = -voiceArray[1].lfo.phaseOffset;
      else
         voiceArray[1].lfo.phase = voiceArray[1].lfo.phaseOffset;
      if (voiceArray[1].lfo.waveform==LFO_NOISE_ONE&&!voiceArray[1].lfo.phaseOffset)
      {
         voiceArray[1].lfo.rnd = GetRngValue();
			voiceArray[1].lfo.rnd = voiceArray[1].lfo.rnd  / (float)0xffffffff ;
      }
	}
	if(voiceArray[2].lfo.retrigger == voice+1)
	{
      isOneShot = (voiceArray[2].lfo.waveform>=LFO_SINE_ONE);
      voiceArray[2].lfo.delay = isOneShot&&(voiceArray[2].lfo.phaseOffset);
      
		if (isOneShot)
         voiceArray[2].lfo.phase = -voiceArray[2].lfo.phaseOffset;
      else
         voiceArray[2].lfo.phase = voiceArray[2].lfo.phaseOffset;
      if (voiceArray[2].lfo.waveform==LFO_NOISE_ONE&&!voiceArray[2].lfo.phaseOffset)
      {
         voiceArray[2].lfo.rnd = GetRngValue();
			voiceArray[2].lfo.rnd = voiceArray[2].lfo.rnd  / (float)0xffffffff ;
      }
	}
	if(snareVoice.lfo.retrigger == voice+1)
	{
      isOneShot = (snareVoice.lfo.waveform>=LFO_SINE_ONE);
      snareVoice.lfo.delay = isOneShot&&(snareVoice.lfo.phaseOffset);
      
		if (isOneShot)
         snareVoice.lfo.phase = -snareVoice.lfo.phaseOffset;
      else
         snareVoice.lfo.phase = snareVoice.lfo.phaseOffset;
      if (snareVoice.lfo.waveform==LFO_NOISE_ONE&&!snareVoice.lfo.phaseOffset)
      {
         snareVoice.lfo.rnd = GetRngValue();
			snareVoice.lfo.rnd = snareVoice.lfo.rnd  / (float)0xffffffff ;
      }
	}
	if(cymbalVoice.lfo.retrigger == voice+1)
	{
      isOneShot = (cymbalVoice.lfo.waveform>=LFO_SINE_ONE);
      cymbalVoice.lfo.delay = isOneShot&&(cymbalVoice.lfo.phaseOffset);
      
		if (isOneShot)
         cymbalVoice.lfo.phase = -cymbalVoice.lfo.phaseOffset;
      else
         cymbalVoice.lfo.phase = cymbalVoice.lfo.phaseOffset;
      if (cymbalVoice.lfo.waveform==LFO_NOISE_ONE&&!cymbalVoice.lfo.phaseOffset)
      {
         cymbalVoice.lfo.rnd = GetRngValue();
			cymbalVoice.lfo.rnd = cymbalVoice.lfo.rnd  / (float)0xffffffff ;
      }
	}
	if(hatVoice.lfo.retrigger == voice+1)
	{
      isOneShot = (hatVoice.lfo.waveform>=LFO_SINE_ONE);
      hatVoice.lfo.delay = isOneShot&&(hatVoice.lfo.phaseOffset);
      
		if (isOneShot)
         hatVoice.lfo.phase = -hatVoice.lfo.phaseOffset;
      else
         hatVoice.lfo.phase = hatVoice.lfo.phaseOffset;
      if (hatVoice.lfo.waveform==LFO_NOISE_ONE&&!hatVoice.lfo.phaseOffset)
      {
         hatVoice.lfo.rnd = GetRngValue();
			hatVoice.lfo.rnd = hatVoice.lfo.rnd  / (float)0xffffffff ;
      }
	}
   
}//-------------------------------------------------------------
