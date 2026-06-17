/*
 * EuklidGenerator.c
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
#include "EuklidGenerator.h"
#include "PatternData.h"

extern uint8_t seq_activePattern;

/* Per-track Euclid generator caches. These names remain legacy in this pass. */
static uint8_t euklid_length[NUM_TRACKS];         /* Cached working length for each track. */
static uint8_t euklid_steps[NUM_TRACKS];          /* Cached working step count for each track. */
static uint8_t euklid_rotation[NUM_TRACKS];       /* Cached main-step rotation for each track. */
static uint8_t euklid_subStepRotation[NUM_TRACKS];/* Cached sub-step rotation for each track. */

static uint8_t euklid_workingPattern = 0;         /* Pattern whose rotation cache is currently valid. */

static uint16_t euklid_patternBuffer;             /**< Generated 16-step mask before transfer. */

static uint16_t euklid_nextCnt1 = 0;              /* Recursive object counter for the current build. */
static uint16_t euklid_originalLength = 0;        /* Original track length for the current build. */
//-----------------------------------------------------
void euklid_init()
{
	int i;
	for(i=0;i<NUM_TRACKS;i++)
	{
		euklid_length[i] = 16;
		euklid_steps[i] = 4;
		euklid_rotation[i] = 0;
      euklid_subStepRotation[i] = 0;
	}

	euklid_patternBuffer = 0;
}
//-----------------------------------------------------
/** calculate how many elements are in one group.
 *  the leading zeroes up to the first 1 are counted using the _CLZ(uint32) function starting at MSB
 *  since a group has always at least 1 element we don't return 0
 */
inline uint8_t getObjSize(uint16_t group)
{
	uint8_t ret = 32 - __CLZ((uint32_t)group);
	if(ret==0) return 1;
	return ret;
}
//-----------------------------------------------------
void euklid_calcRecursive(uint8_t length, uint8_t steps, uint8_t iteration, uint16_t group1, uint16_t group2)
{
	int i,j;
	uint16_t remainder;

	//omit division by zero
	if(steps!=0)
		remainder 	= length%steps;
	else
		remainder = 0;

	//In the first iteration we have to set the next count value to the number of steps
	if(euklid_nextCnt1==0)euklid_nextCnt1=steps;

	//calculate the objects per group and their size
	int objectCnt1 = euklid_nextCnt1;
	int objSize1 = getObjSize(group1);
	int objSize2 = getObjSize(group2);
	int objCnt2 = (euklid_originalLength - (objectCnt1*objSize1))  / (objSize2);

	//the number of group 1 objects in the next iteration equals the count of objects in the smaller group
	euklid_nextCnt1 = objectCnt1>objCnt2?objCnt2:objectCnt1;

	//if the 2nd group has only 1 or less objects left
	//the algorithm has finished
	//--> write the generated pattern to euklid_patternBuffer
	if(objCnt2<=1 )
	{
		uint8_t bufferPos=0;
		//group1 objects
		for(i=0;i<objectCnt1;i++)
		{
			for(j=objSize1-1;j>=0;j--)
			{
				if(group1&(1<<j))
				{
					euklid_patternBuffer |= (1<<bufferPos);
				}
				bufferPos++;
			}
		}
		//group 2 objects
		for( i=0;i<objCnt2;i++)
		{
			for( j=objSize2-1;j>=0;j--)
			{
				if(group2&(1<<j))
				{
					euklid_patternBuffer |= (1<<bufferPos);
				}
				bufferPos++;
			}
		}

		return;
	}

	//shift the group 1 value to make space for the new group2 part
	//and save the original group content
	uint16_t originalGroup1 = group1;
	group1 = group1 << objSize2;


	//paste the group 2 values to the free space at the end of group 1
	for(i=0;i<objSize2;i++)
	{
		//if group2 contains a 1, set in in group1
		if(group2 & (1<<i) )
		{
			group1 |= (1<<i);
		}
	}

	//if there are group 2 objects left group 2 stays the same
	//if no group 2 objects are left the rest of the old group1 will be the new group 2
	group2 = objCnt2-objectCnt1>0?group2:originalGroup1;

	//start the next iteration
	euklid_calcRecursive(steps,remainder,iteration+1,group1,group2);

}
//-----------------------------------------------------
void euklid_generate(uint8_t trackNr, uint8_t patternNr)
{
	uint8_t length,steps,rotation;
	length = euklid_length[trackNr];
	steps = euklid_steps[trackNr];
	rotation = euklid_rotation[trackNr];
   
   if (patternNr != euklid_workingPattern)
   {
      euklid_clearRotation();
      euklid_workingPattern = patternNr;
   }

	//todo ist das hier n�tig? zero steps sollte doch gehen...
	if(steps==0)steps++;

	//reset the global values
	euklid_nextCnt1 = 0;
	euklid_originalLength = length;

	//clear the buffer
	euklid_patternBuffer = 0;

	//let's calculate the new pattern
	euklid_calcRecursive(length,steps,1,1,0); //always start with 1st iterations
	//rotate resulting pattern
	//euklid_rotatePattern(length, seq_getTrackRotation(trackNr));
	euklid_rotatePattern(trackNr, patternNr, length, rotation, 0/*substeps*/);
	//and store it in the active track
	euklid_transferPattern(trackNr, patternNr);
}
//-----------------------------------------------------
uint8_t euklid_getLength(uint8_t trackNr)
{
   euklid_length[trackNr] = pat_getLengthRotatePtr(seq_activePattern, trackNr)->length;
   if (!euklid_length[trackNr])
      euklid_length[trackNr]=16;
	return euklid_length[trackNr];
}
//-----------------------------------------------------
uint8_t euklid_getSteps(uint8_t trackNr)
{
	return euklid_steps[trackNr];
};

//-----------------------------------------------------
void euklid_setLength(uint8_t trackNr, uint8_t patternNr, uint8_t value)
{
   if(patternNr != euklid_workingPattern)
   {
      euklid_clearRotation();
      euklid_workingPattern = patternNr;
   }
   
	if(value<=0)value=1;
	euklid_length[trackNr] = value;
   pat_getLengthRotatePtr(patternNr, trackNr)->length = value;
}
//-----------------------------------------------------
void euklid_setSteps(uint8_t trackNr, uint8_t value, uint8_t patternNr)
{
   if(patternNr != euklid_workingPattern)
   {
      euklid_clearRotation();
      euklid_workingPattern = patternNr;
   }
   
	if(value<=0)value=1;

	if(value>euklid_length[trackNr]) value= euklid_length[trackNr];

	euklid_steps[trackNr] = value;
	euklid_generate(trackNr, patternNr);
}
//-----------------------------------------------------
uint8_t euklid_getRotation(uint8_t trackNr)
{
	return euklid_rotation[trackNr];
}
//-----------------------------------------------------
uint8_t euklid_getSubStepRotation(uint8_t trackNr)
{
	return euklid_subStepRotation[trackNr];
}
//-----------------------------------------------------
void euklid_setRotation(uint8_t trackNr, uint8_t value, uint8_t patternNr)
{
   if(patternNr != euklid_workingPattern)
   {
      euklid_clearRotation();
      euklid_workingPattern = patternNr;
   }
   
   uint8_t length = pat_getLengthRotatePtr(patternNr, trackNr)->length;
   
	euklid_rotatePattern(trackNr, patternNr, length, (int8_t)(value-euklid_rotation[trackNr]), 0);
   
   euklid_rotation[trackNr] = value;
   
   
}
//-----------------------------------------------------
void euklid_setSubStepRotation(uint8_t trackNr, uint8_t value, uint8_t patternNr)
{
   if(patternNr != euklid_workingPattern)
   {
      euklid_clearRotation();
      euklid_workingPattern = patternNr;
   }
   
   uint8_t length = pat_getLengthRotatePtr(patternNr, trackNr)->length;
   
	euklid_rotatePattern(trackNr, patternNr, length, 0, (int8_t)(value-euklid_subStepRotation[trackNr]) );
   
   euklid_subStepRotation[trackNr] = value;
 
}
//-----------------------------------------------------
void euklid_rotatePattern(uint8_t trackNr, uint8_t patternNr, uint8_t length, int8_t mainSteps, int8_t subSteps)
{
   if(patternNr != euklid_workingPattern)
   {
      euklid_clearRotation();
      euklid_workingPattern = patternNr;
   }
   
   Step tempTrack[NUM_STEPS];
   Step *psrc, *pdst;
   uint8_t i;
   uint16_t mainStepTemp = pat_getMainSteps(patternNr, trackNr);
   int8_t rotationSteps;
   
   // get correct length, mainSteps, and substeps - if hey are negative, make positive
   
   if (length==0)
      length=16;
      
   if (subSteps>7)
   { 
      mainSteps=(int)(mainSteps+subSteps/8);
      subSteps=subSteps%8;
   }   
      
   else if (subSteps<-7)
   {
      mainSteps=(int)(mainSteps+subSteps/8);
      subSteps=-( (-subSteps)%8 );
   }
               
   if (subSteps<0)
   {
      subSteps = 8-((-subSteps)%8);
      mainSteps--;
   }
   
   if (mainSteps<0)
   {
      mainSteps = length-((-mainSteps)%length);
   }
     
   // if rotating by any main steps, rotate the active main step index    
   if (!subSteps)
   {
      pat_setMainSteps(patternNr, trackNr, 0x00);
      for (i=0;i<length;i++)
      {
         if (mainStepTemp&(0x01<<i))
         {
            pat_setMainSteps(patternNr, trackNr,
               (uint16_t)(pat_getMainSteps(patternNr, trackNr)|(0x01<<((i+mainSteps)%length))));
         }   
      }
   }   
   
   // deal with rotating the step data structure
   rotationSteps = mainSteps*8 + subSteps;
   for (i=0;i<(length*8);i++)
   { 
      psrc=pat_getStepPtr(patternNr, trackNr, i);
      pdst=&tempTrack[(i+rotationSteps)%(length*8)];
      euklid_copySubStep(psrc,pdst);
   }
   
   for (i=0;i<(length*8);i++)
   { 
      pdst=pat_getStepPtr(patternNr, trackNr, i);
      psrc=&tempTrack[i];
      euklid_copySubStep(psrc,pdst);
      //update front sub step LED
      //seq_sendStepInfoToFront(i);
   }

}
//-----------------------------------------------------
void euklid_copySubStep(Step *psrc, Step *pdst)
{

         pdst->note			= psrc->note;
         pdst->param1Nr 		= psrc->param1Nr;
         pdst->param1Val 	= psrc->param1Val;
         pdst->param2Nr		= psrc->param2Nr;
         pdst->param2Val		= psrc->param2Val;
         pdst->prob			= psrc->prob;
         pdst->volume		= psrc->volume;

}
//-----------------------------------------------------
void euklid_transferPattern(uint8_t trackNr, uint8_t patternNr)
{
   if(patternNr != euklid_workingPattern)
   {
      euklid_clearRotation();
      euklid_workingPattern = patternNr;
   }
   
	uint8_t len=euklid_length[trackNr];
	pat_setMainSteps(patternNr, trackNr, euklid_patternBuffer);
// **PATROT - pattern end is now stored differently

	if(len == 16)
		len=0;
	pat_getLengthRotatePtr(patternNr, trackNr)->length=len;

}
//-----------------------------------------------------
void euklid_clearRotation()
{
   // reset euklid rotation amounts for all tracks
   uint8_t i;
   for (i=0;i<NUM_TRACKS;i++)
   {
      euklid_clearTrackRotation(i);
   }   

}//-----------------------------------------------------
void euklid_clearTrackRotation(uint8_t trackNr)
{
   // reset euklid rotation for a single track
   euklid_rotation[trackNr]=0;
   euklid_subStepRotation[trackNr]=0;

}//-----------------------------------------------------
void euklid_resetRotation()
{
   // reset euklid rotation amounts for all tracks
   uint8_t i;
   for (i=0;i<NUM_TRACKS;i++)
   {
      euklid_resetTrackRotation(i);
   } 

}//-----------------------------------------------------
void euklid_resetTrackRotation(uint8_t trackNr)
{
   euklid_rotation[trackNr]=0;
   euklid_subStepRotation[trackNr]=0;
}
