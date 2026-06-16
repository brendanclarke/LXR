/*
 * SomGenerator.h
 *
 *  Created on: 30.04.2013
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


#ifndef SOMGENERATOR_H_
#define SOMGENERATOR_H_

#include "stm32f4xx.h"
#include "random.h"

/* Legacy SOM generator API.
   The public names stay on som_* in this pass so the rename work stays
   isolated to Pattern storage. */
typedef struct SomGeneratorStruct
{
	/* Current XY position in the SOM field. */
	float x;
	float y;

	/* Per-voice trigger thresholds derived from the SOM nodes. */
	uint8_t frequency[7];

	/* Randomness applied while interpolating node values. */
	float flux;


}SomGenerator;

/* Initialize the SOM generator state. */
void som_init();
/* Advance the SOM generator on a sequencer tick. */
void som_tick(uint8_t stepNr, uint8_t mutedTracks);

/* Set the X coordinate used for SOM interpolation. */
void som_setX(uint8_t x);
/* Set the Y coordinate used for SOM interpolation. */
void som_setY(uint8_t y);
/* Set the flux amount used for SOM interpolation. */
void som_setFlux(float flux);
/* Set the trigger threshold for one voice. */
void som_setFreq(uint8_t freq, uint8_t voice);

/* Shared SOM generator state. */
extern SomGenerator somGenerator;


#endif /* SOMGENERATOR_H_ */
