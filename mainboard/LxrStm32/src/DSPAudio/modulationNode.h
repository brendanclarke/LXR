/*
 * VelocityModulation.h
 *
 *  Created on: 06.01.2013
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


#ifndef VELOCITYMODULATION_H_
#define VELOCITYMODULATION_H_

#include "stm32f4xx.h"
#include "Oscillator.h"
#include "Preset/ParameterArray.h"

typedef struct ModulatorStruct
{

	uint16_t	destination;	/**< dest param nr */
	uint8_t		type;			/**< pointer type */
	ptrValue	originalValue;	/**< stores the original value of the parameter*/
	float		amount;			/**< modulation amount*/
	float 		lastVal;

} ModulationNode;

//TODO move into corresponding voice
extern ModulationNode velocityModulators[6];

extern ModulationNode macroModulators[4];

//void modNode_updateMacro(ModulationNode* macroNode, float amount);
void modNode_resetMacros();
void modNode_reassignMacroMod();
void modNode_init(ModulationNode* vm);

void modNode_resetTargets();
void modNode_reassignVeloMod();
void modNode_vMorph(ModulationNode* vm, float val);

/** if multiple nodes address the same target we need to update the other modNodes if one of them changes the destionation*/
//void modNode_originalValueModulated(uint16_t idx, ModulationNode* modSource);
void modNode_originalValueChanged(uint16_t idx);
void modNode_setDestination(ModulationNode* vm, uint16_t dest);
void modNode_updateValue(ModulationNode* vm, float val);

/* Maximum number of oscillators that receive waveform blend state per DSP block.
   Limits the double-render cost: the first OSC_WAVE_INTERP_MAX_ACTIVE eligible
   waveform-parameter modulation targets get fractional blend state; later ones
   fall through to integer waveform IDs as normal. */
#define OSC_WAVE_INTERP_MAX_ACTIVE 1u

/* Enable / disable waveform-parameter interpolation.
   When enabled, modNode_updateValue() writes fractional blend state to oscillators
   for TYPE_UINT8 waveform parameters instead of truncating to an integer ID.
   Called from frontParser_handleSeqCC() on FRONT_SEQ_OSC_WAVE_INTERPOLATION. */
void modNode_setWaveInterpEnabled(uint8_t enabled);

/* Returns the current waveform interp enable flag (0 or 1). Read by
   osc_waveInterpActive() in the audio render path. */
uint8_t modNode_getWaveInterpEnabled(void);

/* Returns the current DSP block generation counter. Read by osc_waveInterpActive()
   to detect and reject stale per-oscillator blend state. */
uint32_t modNode_getWaveInterpGeneration(void);

#endif /* VELOCITYMODULATION_H_ */
