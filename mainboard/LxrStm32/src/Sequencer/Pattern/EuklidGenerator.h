/*
 * EuklidGenerator.h
 *
 *  Created on: 04.08.2012
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


#ifndef EUKLIDGENERATOR_H_
#define EUKLIDGENERATOR_H_

#include "stm32f4xx.h"

/* Legacy Euclid generator API.
   The public names stay on euklid_* in this pass so the Pattern rename can
   settle without touching the generator contract yet. */
/* Initialize the Euclid working buffers and cached track values. */
void euklid_init();
/* Read the current working length for a track. */
uint8_t euklid_getLength(uint8_t trackNr);
/* Read the current working step count for a track. */
uint8_t euklid_getSteps(uint8_t trackNr);
/* Read the cached main rotation for a track. */
uint8_t euklid_getRotation(uint8_t trackNr);
/* Read the cached sub-step rotation for a track. */
uint8_t euklid_getSubStepRotation(uint8_t trackNr);
/* Clear all cached rotation deltas. */
void euklid_clearRotation();
/* Clear the cached rotation for one track. */
void euklid_clearTrackRotation(uint8_t trackNr);
/* Reset all Euclid rotation caches to zero. */
void euklid_resetRotation();
/* Reset one track's Euclid rotation caches to zero. */
void euklid_resetTrackRotation(uint8_t trackNr);
/* Store a new working length and propagate it into pattern storage. */
void euklid_setLength(uint8_t trackNr, uint8_t patternNr, uint8_t value);
/* Store a new working step count and regenerate the pattern. */
void euklid_setSteps(uint8_t trackNr, uint8_t value, uint8_t patternNr);
/* Store a new working rotation and apply it to the pattern. */
void euklid_setRotation(uint8_t trackNr, uint8_t value, uint8_t patternNr);
/* Store a new working sub-step rotation and apply it to the pattern. */
void euklid_setSubStepRotation(uint8_t trackNr, uint8_t value, uint8_t patternNr);
/* Restore the cached Euclid UI state for one track without rotating the
   pattern again. Used when SHIFT+PERF rolls a touched track back from temp. */
void euklid_restoreTrackState(uint8_t trackNr,
                              uint8_t patternNr,
                              uint8_t steps,
                              uint8_t rotation,
                              uint8_t subStepRotation);
/* Generate and rotate one Euclid track into the target pattern. */
void euklid_rotatePattern(uint8_t trackNr, uint8_t patternNr, uint8_t length, int8_t mainSteps, int8_t subSteps);
/* Transfer the generated Euclid mask into the target pattern. */
void euklid_transferPattern(uint8_t trackNr, uint8_t patternNr);
/* Copy one generated Euclid step into a destination step. */
void euklid_copySubStep();
#endif /* EUKLIDGENERATOR_H_ */
