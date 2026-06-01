
/*
 * presetManager.c
 *
 * Created: 09.05.2012 16:06:19
 *  Author: Julian
 */ 
#include "../config.h"
#include "PresetManager.h"
#include "../Hardware/SD/ff.h"
#include <stdio.h>
#include "../Menu/CcNr2Text.h"

#include <util/delay.h>
#include "../Hardware/lcd.h"
#include <avr/pgmspace.h>
#include "../frontPanelParser.h"
#include <stdlib.h>
#include <util/atomic.h> 
#include "../IO/uart.h"
#include "../IO/din.h"
#include "../IO/dout.h"
#include "../Hardware/timebase.h"
#include <string.h>


#define FILE_VERSION 5

// bc - version 4 offsets remain valid for version 5
#define VERSION_4_PERF_SCALE_OFFSET 51459    // (56)
#define VERSION_4_PERF_LENGTH_OFFSET 51403   // (56)
#define VERSION_4_PERF_SHUFFLE_OFFSET 51402  // (1)
#define VERSION_4_PERF_PATCHAIN_OFFSET 51386 // (16)
#define VERSION_4_PERF_MAINSTEP_OFFSET 51274 // (112)
#define VERSION_4_PERF_STEPDATA_OFFSET 1098  // (50176)

#define VERSION_1_PERF_KIT_OFFSET 74
#define VERSION_4_PERF_KIT_OFFSET 74
#define VERSION_4_PERF_MORPH_OFFSET 586

#define VERSION_4_ALL_SCALE_OFFSET 51458    // (56)
#define VERSION_4_ALL_LENGTH_OFFSET 51402   // (56)
#define VERSION_4_ALL_SHUFFLE_OFFSET 51401  // (1)
#define VERSION_4_ALL_PATCHAIN_OFFSET 51385 // (16)
#define VERSION_4_ALL_MAINSTEP_OFFSET 51273 // (112)
#define VERSION_4_ALL_STEPDATA_OFFSET 1097  // (50176)
 
#define VERSION_4_ALL_KIT_OFFSET 73
#define VERSION_1_ALL_KIT_OFFSET 73
#define VERSION_4_ALL_MORPH_OFFSET 585

// version 0 offsets remain valid for version 5
#define VERSION_0_PAT_SCALE_OFFSET 50369    // (56)
#define VERSION_0_PAT_LENGTH_OFFSET 50313   // (56)
#define VERSION_0_PAT_SHUFFLE_OFFSET 50312  // (1)
#define VERSION_0_PAT_PATCHAIN_OFFSET 50296 // (16)
#define VERSION_0_PAT_MAINSTEP_OFFSET 50184 // (112)
#define VERSION_0_PAT_STEPDATA_OFFSET 8     // (50176)

#define VERSION_0_KIT_KIT_OFFSET 8

#define GEN_BUF_LEN 9

#define NUM_TRACKS 7
#define NUM_PATTERN 8
#define STEPS_PER_PATTERN 128
#define VOICE_PARAM_LENGTH 36
#define FEXT_SOUND 	0
#define FEXT_PAT 	1
#define FEXT_ALL 	2
#define FEXT_PERF 	3

// fill buffer (length 9 total) with a filename. type is one of above
// eg p001.snd

// defines for sorting out multiple voice change signals
#define BANK_1 0x01
#define BANK_2 0x02
#define BANK_3 0x04
#define BANK_4 0x08
#define BANK_5 0x10
#define BANK_6 0x20
#define BANK_7 0x40
#define BANK_GLOBAL 0x80

FATFS preset_Fatfs;		/* File system object for the logical drive */
FIL preset_File;		/* place to hold 1 file*/

uint8_t preset_workingPreset;
uint8_t preset_workingType;
uint8_t preset_workingVersion;
uint8_t preset_workingVoiceArray;

enum workingTypeEnum
{
   WTYPE_KIT=0,
   WTYPE_DRUM1,
   WTYPE_DRUM2,
   WTYPE_DRUM3,
   WTYPE_SNARE,
   WTYPE_CYM,
   WTYPE_HIHAT,
   WTYPE_PATTERN,
   WTYPE_PERFORMANCE,
   WTYPE_ALL,
   WTYPE_GLOBAL,   
};
uint16_t totalBytes;
char filename[GEN_BUF_LEN];

char preset_currentName[8];
char preset_currentSaveMenuName[8];

uint8_t parameter_values_temp[END_OF_SOUND_PARAMETERS];
uint8_t parameters2_temp[END_OF_SOUND_PARAMETERS];
static uint8_t preset_savedParameterValues[END_OF_SOUND_PARAMETERS];
static uint8_t preset_savedParameters2[END_OF_SOUND_PARAMETERS];

static uint8_t voice1presetMask[VOICE_PARAM_LENGTH]={1,8,9,20,      37,43,49,50,   62,70,74,78,  82,83,88,94,   102,108,115,121,     128,134,137,143,    149,155,161,167,    173,179,185,191,    197,203,209,215}; 
static uint8_t voice2presetMask[VOICE_PARAM_LENGTH]={2,10,11,21,    38,44,51,52,   63,71,75,79,  84,85,89,95,   103,109,116,122,     129,135,138,144,    150,156,162,168,    174,180,186,192,    198,204,210,216}; 
static uint8_t voice3presetMask[VOICE_PARAM_LENGTH]={3,12,13,22,    39,45,53,54,   64,72,76,80,  86,87,90,96,   104,110,117,123,     130,136,139,145,    151,157,163,169,    175,181,187,193,    199,205,211,217}; 
static uint8_t voice4presetMask[VOICE_PARAM_LENGTH]={4,14,15,27,28, 40,46,55,      56,65,68,73,  77,81,91,99,   105,111,118,124,     131,140,146,152,        158,164,170,    176,182,188,194,    200,206,212,218}; 
static uint8_t voice5presetMask[VOICE_PARAM_LENGTH]={6,16,17,23,    24,29,30,31,   32,41,47,57,  58,66,69,92,   100,106,112,119,125, 132,141,147,153,        159,165,171,    177,183,189,195,    201,207,213,219}; 
static uint8_t voice6presetMask[VOICE_PARAM_LENGTH]={7,18,19,25,    26,33,34,35,   36,42,48,59,  60,61,67,93,   101,107,113,120,126, 133,142,148,154,        160,166,172,    178,184,190,196,    202,208,214,220};         

static void preset_makeFileName(char *buf, uint8_t num, uint8_t type);
static void preset_dumpEndpointsToStm(uint8_t endpointMode);
static uint8_t preset_shouldPreserveMenuEndpointsDuringFileLoad(void);
static void preset_saveMenuEndpointsDuringFileLoad(void);
static void preset_restoreMenuEndpointsDuringFileLoad(void);

static void preset_showLoadingPerf()
{
   lcd_clear();
   lcd_home();
   lcd_string_F(PSTR("Loading Perf"));
}

//----------------------------------------------------
static uint8_t preset_shouldPreserveMenuEndpointsDuringFileLoad(void)
{
   return menu_playedPattern == SEQ_TMP_PATTERN;
}

//----------------------------------------------------
static void preset_saveMenuEndpointsDuringFileLoad(void)
{
   memcpy(preset_savedParameterValues, parameter_values, END_OF_SOUND_PARAMETERS);
   memcpy(preset_savedParameters2, parameters2, END_OF_SOUND_PARAMETERS);
}

//----------------------------------------------------
static void preset_restoreMenuEndpointsDuringFileLoad(void)
{
   memcpy(parameter_values, preset_savedParameterValues, END_OF_SOUND_PARAMETERS);
   memcpy(parameters2, preset_savedParameters2, END_OF_SOUND_PARAMETERS);
}


//----------------------------------------------------
void preset_init()
{

	//mount the filesystem	
   f_mount(0,(FATFS*)&preset_Fatfs);
	
	
}

//----------------------------------------------------
static void preset_writeDrumsetData(uint8_t isMorph)
{
   uint16_t i;
   unsigned int bytesWritten;
	
   if(isMorph>0)
   {
      for(i=0;i<END_OF_SOUND_PARAMETERS;i++)
      {
         uint8_t value;
      	//Mod targets are not morphed!!!
         if( (i >= PAR_VEL_DEST_1) && (i <= PAR_VEL_DEST_6) )
         {
            value = parameter_values[i];
         } 
         else if( (i >= PAR_TARGET_LFO1) && (i <= PAR_TARGET_LFO6) )
         {
            value = parameter_values[i];
         } 
         else if( (i >= PAR_VOICE_LFO1) && (i <= PAR_VOICE_LFO6) )
         {
            value = parameter_values[i];
         } 
         else 
         {
            if (isMorph==2) // -bc- added this to be able to write full morph params for 'performance' files
               value = preset_getMorphValue(i,255);
            else
               value = preset_getMorphValue(i,parameter_values[PAR_MORPH]);
         }					
         f_write((FIL*)&preset_File,&value,1,&bytesWritten);	
      }
   } 
   else {
      for(i=0;i<END_OF_SOUND_PARAMETERS;i++)
      {
         f_write((FIL*)&preset_File,&parameter_values[i],1,&bytesWritten);
      }
   }
}

//----------------------------------------------------
void preset_saveDrumset(uint8_t presetNr, uint8_t isMorph)
{
#if USE_SD_CARD
   unsigned int bytesWritten;
	//filename in 8.3  format
   char filename[9];
   preset_makeFileName(filename,presetNr,FEXT_SOUND);
	//sprintf(filename,"p%03d.snd",presetNr);

	//open the file
   f_open((FIL*)&preset_File,filename,FA_CREATE_ALWAYS | FA_WRITE);
   totalBytes=0;
	//write preset name to file
   f_write((FIL*)&preset_File,(void*)preset_currentSaveMenuName,8,&bytesWritten);
	//write the preset data
   preset_writeDrumsetData(isMorph);
	
	//close the file
   f_close((FIL*)&preset_File);
#else
#endif
};

//----------------------------------------------------
static void preset_writeGlobalData()
{
   unsigned int bytesWritten;
   int i;
   for(i=PAR_BEGINNING_OF_GLOBALS;(i<NUM_PARAMS);i++)
   {
      f_write((FIL*)&preset_File,&parameter_values[i],1,&bytesWritten);
   }
}

//----------------------------------------------------
void preset_saveGlobals()
{
#if USE_SD_CARD	
	//open the file
   f_open((FIL*)&preset_File,"glo.cfg",FA_CREATE_ALWAYS | FA_WRITE);
   totalBytes=0;
   preset_writeGlobalData();

	//close the file
   f_close((FIL*)&preset_File);
#else
#endif	
}
//----------------------------------------------------
// returns 1 on success
static uint8_t preset_readGlobalData()
{
#if USE_SD_CARD
   int i;
   UINT bytesRead;
   for(i=PAR_BEGINNING_OF_GLOBALS;(i<NUM_PARAMS) &&( bytesRead!=0);i++) {
      f_read((FIL*)&preset_File,&parameter_values[i],1,&bytesRead);
      if(!bytesRead)
         parameter_values[i]=0; // set to 0 by default
         
   }
   return 1;
#endif
}

//----------------------------------------------------
void preset_loadGlobals()
{
#if USE_SD_CARD
	//open the file
   uint8_t i;
   FRESULT res = f_open((FIL*)&preset_File,"glo.cfg",FA_OPEN_EXISTING | FA_READ);
   totalBytes=0;
   if(res!=FR_OK)
      return; //file open error... maybe the file does not exist?

   preset_readGlobalData();
   f_close((FIL*)&preset_File);
   
   switch(parameter_values[PAR_GLOBAL_SETTINGS_VERSION])
   {
      case 0: // config file created before setting versions. set macro to defaults
              // get midi note values from kit 0 file
         preset_loadDrumset(0,0x7f,0);
         
         // track MIDI was not contained in globals - get them from drumkit 0
         for(i=0;i<7;i++)
         {
            parameter_values[PAR_MIDI_NOTE1+i]=parameter_values_temp[PAR_ENVELOPE_POSITION_1+i];
         }
         
         lcd_clear();
         lcd_home();
         lcd_string_F(PSTR("old config!"));
         lcd_setcursor(0,2);
         lcd_string_F(PSTR("re-save settings"));
         _delay_ms(2000);
         
         break;
      default:
         break;
   }
      
   parameter_values[PAR_GLOBAL_SETTINGS_VERSION]=FILE_VERSION;
   
   menu_sendAllGlobals();
	
#endif	
}
//----------------------------------------------------
// read the whole kit data from file to appropriate temp, fix values   
void preset_readKitToTemp(uint8_t isMorph)
{

  #if USE_SD_CARD		
   FIL kitRead_File;
   char kit_filename[GEN_BUF_LEN];
   uint16_t bytesRead;
   int16_t i;
   FRESULT res;
   uint16_t kitOffset;
   uint8_t *para = parameter_values_temp;
   
   if(isMorph)
      para=parameters2_temp;
   
   switch(preset_workingType)
   {
      case WTYPE_PERFORMANCE:
         if(preset_workingVersion>=3)
         {
            if(isMorph)
            {
               kitOffset=VERSION_4_PERF_MORPH_OFFSET;
            }
            else
            {
               kitOffset=VERSION_4_PERF_KIT_OFFSET;
            }
         }
         else
         {
            kitOffset=VERSION_1_PERF_KIT_OFFSET;
         }
         
         preset_makeFileName(kit_filename,preset_workingPreset,FEXT_PERF);
         break;
      case WTYPE_ALL:
         if(preset_workingVersion>=3)
         {
            if(isMorph)
            {
               kitOffset=VERSION_4_ALL_MORPH_OFFSET;
            }
            else
            {
               kitOffset=VERSION_4_ALL_KIT_OFFSET;
            }
         }
         else
         {
            kitOffset=VERSION_1_ALL_KIT_OFFSET;
         }
         
         preset_makeFileName(kit_filename,preset_workingPreset,FEXT_ALL);
         break;
      case WTYPE_KIT:
         kitOffset=VERSION_0_KIT_KIT_OFFSET;
         preset_makeFileName(kit_filename,preset_workingPreset,FEXT_SOUND);
         break;
      default:
         return;
   }
   
   res = f_open((FIL*)&kitRead_File,kit_filename,FA_OPEN_EXISTING | FA_READ);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("kitOpen err "));
      lcd_string(text);
      while(1){;}
   }
   
   res = f_lseek((FIL*)&kitRead_File,kitOffset);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("kitSeek err "));
      lcd_string(text);
      while(1){;}
   }
   
   res=f_read((FIL*)&kitRead_File, para,END_OF_SOUND_PARAMETERS, &bytesRead);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("kitread err "));
      lcd_string(text);
      while(1){;}
   }
   

   f_close((FIL*)&kitRead_File);
   
   // set to 0 for any that were not read from the file
   if(END_OF_SOUND_PARAMETERS-bytesRead)
      memset(para+bytesRead,0,END_OF_SOUND_PARAMETERS-bytesRead);
   	
 //special case mod targets. normalize.
   const uint8_t nmt=getNumModTargets();
   for(i=0;i<6;i++) {
      	// --AS since I've changed the meaning of these, I'll ensure that it's valid for kits saved prior to the
      	// change
      if(para[PAR_VEL_DEST_1+i] >= nmt )
         para[PAR_VEL_DEST_1+i] = 0;
      if(para[PAR_TARGET_LFO1+i] >= nmt )
         para[PAR_TARGET_LFO1+i] = 0;
   }
   if(para[PAR_KIT_VERSION]<FILE_VERSION) // file version is ouf of date - put any corrections here
   {
      if(para[PAR_KIT_VERSION]<2) // kit versioning started at version 2, with addition of LP2 Filter
      {
         for (i=PAR_FILTER_TYPE_1;i<=PAR_FILTER_TYPE_6;i++)
         {
            if (para[i]==6)
            {
               para[i]=7;
            }
         }
      }
         // end of corrections - save the version as a param so it gets written with kit on save
      para[PAR_KIT_VERSION]=FILE_VERSION; 
   }

#endif
}
//----------------------------------------------------
// read the data into either the main parameters or the morph parameters.
void preset_readDrumVoice(uint8_t track, uint8_t isMorph)
{

   if(track>NUM_TRACKS-1)
      return;
      
   uint8_t i, value, upper, lower;
   uint8_t *paramMask;
   
   frontPanel_holdForBuffer();
   frontPanel_sendData(SEQ_CC,SEQ_LOAD_VOICE,track);
   
   switch(track)
   {
      case 0:
         paramMask=voice1presetMask;
         break;
      case 1:
         paramMask=voice2presetMask;
         break;
      case 2:
         paramMask=voice3presetMask;
         break;
      case 3:
         paramMask=voice4presetMask;
         break;
      case 4:
         paramMask=voice5presetMask;
         break;
      case 6:
         track--;
      case 5:
         paramMask=voice6presetMask;
         break;
      default:
         return;
   }

   if(!frontPanel_flowBegin(FLOW_CH_VOICE_PARAM))
      return;
   
   if(isMorph)
   {
      for (i=0;i<VOICE_PARAM_LENGTH;i++)
      {
         parameters2[paramMask[i]]=parameters2_temp[paramMask[i]];
      }
   }
   else
   {
      for (i=0;i<VOICE_PARAM_LENGTH;i++)
      {
         parameter_values[paramMask[i]]=parameter_values_temp[paramMask[i]];
      }
   }
    
    // send velocity targets
    
   value = (uint8_t)pgm_read_word(&modTargets[parameter_values[PAR_VEL_DEST_1+track]].param);
   upper = (uint8_t)(((value&0x80)>>7) | (((track)&0x3f)<<1));
   lower = value&0x7f;
   frontPanel_sendData(CC_VELO_TARGET,upper,lower);
   
   // ensure lfo target voice # is valid
   if(parameter_values[PAR_VOICE_LFO1+track] < 1 || parameter_values[PAR_VOICE_LFO1+track] > 6 )
      parameter_values[PAR_VOICE_LFO1+track]=track;
   
   // **LFO par_target_lfo will be an index into modTargets, but we need a parameter number to send
   value = (uint8_t)pgm_read_word(&modTargets[parameter_values[PAR_TARGET_LFO1+track]].param);
   upper = (uint8_t)(((value&0x80)>>7) | (((track)&0x3f)<<1));
   lower = value&0x7f;
   frontPanel_sendData(CC_LFO_TARGET,upper,lower);
   
   // --AS todo will this morph (and fuck up) our modulation targets?
   // send parameters (possibly combined with morph parameters) to back
   
   // bc: output dests aren't morphed anymore - they need to be a special case
   frontPanel_sendData(CC_2,(uint8_t)(PAR_AUDIO_OUT1+track-128),parameter_values[track+PAR_AUDIO_OUT1]);
   frontPanel_holdForBuffer();
   preset_morph((uint8_t)(0x01<<track),parameter_values[PAR_MORPH]);  
   
   frontPanel_holdForBuffer();
   frontPanel_sendData(SEQ_CC,SEQ_UNHOLD_VOICE,track);

   if(!frontPanel_flowFailed())
      (void)frontPanel_flowEnd(FLOW_CH_VOICE_PARAM);
   
}
 
//----------------------------------------------------
// read from temp any kit data not associated with a voice.
void preset_readDrumsetMeta(uint8_t isMorph)
{
   int16_t i;
   uint8_t value;
   
   if(isMorph)
   {
      /* FILE LOAD: Keep the morph parameter endpoint image complete. This is
         endpoint storage only; it does not change live morph destination behavior. */
      for (i=0;i<END_OF_SOUND_PARAMETERS-END_OF_INDIVIDUAL_VOICE_PARAMS;i++)
      {
         parameters2[END_OF_INDIVIDUAL_VOICE_PARAMS+i]=
            parameters2_temp[END_OF_INDIVIDUAL_VOICE_PARAMS+i];
      }
      parameters2[PAR_KIT_VERSION]=preset_workingVersion; // version is updated in readToTemp
      
   }
   else
   {
      if(!frontPanel_flowBegin(FLOW_CH_DRUM_META))
         return;

   // copy values from temp to where they are supposed to be - normal params or morph
    
      for (i=0;i<END_OF_SOUND_PARAMETERS-END_OF_INDIVIDUAL_VOICE_PARAMS;i++)
      {
         parameter_values[END_OF_INDIVIDUAL_VOICE_PARAMS+i]=
            parameter_values_temp[END_OF_INDIVIDUAL_VOICE_PARAMS+i];
      }
   
   // bc: special case macro targets - re-send targets on kit load
   /* MACRO_CC message structure
   byte1 - status byte 0xaa as above
   byte2, data1 byte: xtta aa-b : x= MIDI message, only 7 bits; -= as yet unassigned
                                  tt= top level macro value sent (2 macros exist now, we can do 2 more if we want)
                                  aaa= macro destination value sent (4 destinations exist now, can do 8)
                                  b=macro mod target value top bit
                                  I have left a blank bit above this to make it easier to make more than 255 kit parameters
                                  if we ever want to take on that can of worms
                                 
   byte3, data2 byte: xbbb bbbb : b=macro mod target value lower 7 bits or top level value full
   */
      for(i=0;i<7; i=(uint8_t)(i+2) ) // 0,2,4,6
      {
         value =  (uint8_t)pgm_read_word(&modTargets[parameter_values[PAR_MAC1_DST1+i]].param); // the value of the mod target
         uint8_t lower = value&0x7f;
         uint8_t upper = (uint8_t)
                      ( ( ( ( i ) //  MAC1_DST1=0, M1D2=2, M2D1=4, M2D2=6
                           >>1 )  //  MAC1_DST1=0, M1D2=1, M2D1=2, M2D2=3
                           <<2 )  //  shift over 2 to make room for upper mod target bit
                           |(value>>7) );
                           
         frontPanel_sendData(MACRO_CC,upper,lower);
      }
   
         
   // send macro amounts as special cases
      frontPanel_sendData(CC_2,(uint8_t)(PAR_MAC1_DST1_AMT-128),parameter_values[PAR_MAC1_DST1_AMT]);
      frontPanel_sendData(CC_2,(uint8_t)(PAR_MAC1_DST2_AMT-128),parameter_values[PAR_MAC1_DST2_AMT]);
      frontPanel_sendData(CC_2,(uint8_t)(PAR_MAC2_DST1_AMT-128),parameter_values[PAR_MAC2_DST1_AMT]);
      frontPanel_sendData(CC_2,(uint8_t)(PAR_MAC2_DST2_AMT-128),parameter_values[PAR_MAC2_DST2_AMT]);

      if(!frontPanel_flowFailed())
         (void)frontPanel_flowEnd(FLOW_CH_DRUM_META);
   }
}

//----------------------------------------------------
static void preset_dumpAutomationTargetsToStm(const uint8_t *params)
{
   uint16_t i;
   uint8_t value, upper, lower;

   /* RESTORE: Resolved automation target sidebands are not ordinary parameter
      bytes. The caller brackets this helper with SEQ_TMP_KIT_AUTOMATION_PHASE
      so the STM knows whether these destinations belong to the kit/front
      endpoint image or the morph automation target endpoint image. */
   for (i = 0; i < 6; i++)
   {
      value = (uint8_t)pgm_read_word(&modTargets[params[PAR_VEL_DEST_1 + i]].param);
      upper = (uint8_t)(((value & 0x80) >> 7) | (((i) & 0x3f) << 1));
      lower = value & 0x7f;
      frontPanel_sendData(CC_VELO_TARGET, upper, lower);

      value = (uint8_t)pgm_read_word(&modTargets[params[PAR_TARGET_LFO1 + i]].param);
      upper = (uint8_t)(((value & 0x80) >> 7) | (((i) & 0x3f) << 1));
      lower = value & 0x7f;
      frontPanel_sendData(CC_LFO_TARGET, upper, lower);
   }

   for (i = 0; i < 7; i = (uint8_t)(i + 2)) // 0, 2, 4, 6
   {
      value = (uint8_t)pgm_read_word(&modTargets[params[PAR_MAC1_DST1 + i]].param);
      lower = value & 0x7f;
      upper = (uint8_t)((((i) >> 1) << 2) | (value >> 7));
      frontPanel_sendData(MACRO_CC, upper, lower);
   }
}

//----------------------------------------------------
void preset_dumpNormalEndpointsToStm()
{
   preset_dumpEndpointsToStm(SEQ_TMP_KIT_ENDPOINT_BOTH);
}

//----------------------------------------------------
static void preset_dumpEndpointsToStm(uint8_t endpointMode)
{
   uint16_t i;

   /* RESTORE: Endpoint capture BEGIN. The data byte selects which normal
      endpoint image the STM clears and receives. This is used both for
      copy-to-temp full dumps and file-load normal endpoint storage. */
   frontPanel_sendData(SEQ_CC, SEQ_TMP_KIT_ENDPOINT_BEGIN, endpointMode);

   if(endpointMode != SEQ_TMP_KIT_ENDPOINT_MORPH_ONLY)
   {
      // 1. Send parameter_values[] (Original/Front Panel endpoints)
      for (i = 0; i < END_OF_SOUND_PARAMETERS; i++)
      {
         if (i < 128)
            frontPanel_sendData(PRF_RESTORE_PARAM_CC, (uint8_t)i, parameter_values[i]);
         else
            frontPanel_sendData(PRF_RESTORE_PARAM_CC2, (uint8_t)(i - 128), parameter_values[i]);
      }

      /* RESTORE: Resolved automation targets for parameter_values[] are bracketed
         immediately after the kit/front endpoint bytes they describe. */
      frontPanel_sendData(SEQ_CC, SEQ_TMP_KIT_AUTOMATION_PHASE, SEQ_TMP_KIT_AUTOMATION_FRONT_ENDPOINT);
      preset_dumpAutomationTargetsToStm(parameter_values);
   }

   if(endpointMode != SEQ_TMP_KIT_ENDPOINT_FRONT_ONLY)
   {
      // 2. Send parameters2[] (Morph parameter endpoints)
      for (i = 0; i < END_OF_SOUND_PARAMETERS; i++)
      {
         if (i < 128)
            frontPanel_sendData(PRF_RESTORE_MORPH_CC, (uint8_t)i, parameters2[i]);
         else
            frontPanel_sendData(PRF_RESTORE_MORPH_CC2, (uint8_t)(i - 128), parameters2[i]);
      }

      /* RESTORE: Resolved automation targets for parameters2[] are sent as the
         morph automation target endpoint image, separate from kit/front endpoints. */
      frontPanel_sendData(SEQ_CC, SEQ_TMP_KIT_AUTOMATION_PHASE, SEQ_TMP_KIT_AUTOMATION_MORPH_ENDPOINT);
      preset_dumpAutomationTargetsToStm(parameters2);
   }

   frontPanel_sendData(SEQ_CC, SEQ_TMP_KIT_AUTOMATION_PHASE, SEQ_TMP_KIT_AUTOMATION_NONE);

   /* RESTORE: Handshake END. Inform STM we have finished the dump. */
   frontPanel_sendData(SEQ_CC, SEQ_TMP_KIT_ENDPOINT_END, 0);
}

//----------------------------------------------------
uint8_t preset_loadDrumset(uint8_t presetNr, uint8_t voiceArray, uint8_t isMorph)
{
   #if USE_SD_CARD
	//filename in 8.3  format
   
   UINT bytesRead;
   uint8_t trkNum;

   uart_clearRxFifo();
   frontParser_rxDisable=1;
   
   preset_workingPreset=presetNr;
   preset_workingType=WTYPE_KIT;
   preset_workingVoiceArray = voiceArray;

   preset_makeFileName(filename,presetNr,FEXT_SOUND);
   
	//open the file
   FRESULT res = f_open((FIL*)&preset_File,filename,FA_OPEN_EXISTING | FA_READ);
   if(res!=FR_OK)
      return 1; //file open error... maybe the file does not exist?
   
	//first the preset name
   f_read((FIL*)&preset_File,(void*)preset_currentName,8,&bytesRead);
   if(!bytesRead)
      goto closeFile;
   
   frontPanel_sendData(SEQ_CC,SEQ_FILE_BEGIN,WTYPE_KIT);
   preset_readKitToTemp(isMorph);

   
   for (trkNum=0;trkNum<NUM_TRACKS;trkNum++)
   {
      if(voiceArray&(0x01<<trkNum))
      {
         
         if(trkNum<6)
         {
            frontPanel_sendData(SEQ_CC,SEQ_LOAD_VOICE,trkNum); 
            preset_readDrumVoice(trkNum, isMorph);
         }
         
         if(trkNum<5)
            frontPanel_sendData(SEQ_CC,SEQ_UNHOLD_VOICE,trkNum);
            
      }
      
   }
   
   frontPanel_sendData(SEQ_CC,SEQ_UNHOLD_VOICE,5); 
   
   if( (voiceArray>=0x7f) || (voiceArray==0) )
   {
      preset_readDrumsetMeta(isMorph);
   }

   preset_dumpEndpointsToStm(isMorph
                             ? SEQ_TMP_KIT_ENDPOINT_MORPH_ONLY
                             : SEQ_TMP_KIT_ENDPOINT_FRONT_ONLY);
   
   frontPanel_sendData(SEQ_CC,SEQ_FILE_DONE,WTYPE_KIT);
   
	//force complete repaint
   menu_repaintAll();

#else
	frontPanel_sendData(PRESET,PRESET_LOAD,presetNr);
#endif

closeFile:
	//close the file handle
   f_close((FIL*)&preset_File);
   
   frontParser_rxDisable=0;
   uart_clearRxFifo();
   
   return 0;
}

//----------------------------------------------------
char* preset_loadName(uint8_t presetNr, uint8_t what, uint8_t loadSave)
{
// loadSave - 0 is load, send name to standard preset slot
//             1 is save, send to separate save name slot to prevent it being
//                updated by bank and instrument changes that might arrive
   uint8_t type;
// DO ME NOW - MAKE LOAD SAVE CASES FOR ALL RETURNS
   switch(what) {
      
      case SAVE_TYPE_KIT:
      case SAVE_TYPE_MORPH:
      case SAVE_TYPE_DRUM1:
      case SAVE_TYPE_DRUM2:
      case SAVE_TYPE_DRUM3:
      case SAVE_TYPE_SNARE:
      case SAVE_TYPE_CYM:
      case SAVE_TYPE_HIHAT:
         type=FEXT_SOUND;
         break;
      case SAVE_TYPE_PATTERN:
         type=FEXT_PAT;
         break;
      case SAVE_TYPE_ALL:
         type=FEXT_ALL;
         break;
      case SAVE_TYPE_PERFORMANCE:
         type=FEXT_PERF;
         break;
      default:
      // 
         if (loadSave==0)
         {
            memcpy_P((void*)preset_currentName,PSTR("Empty   "),8);
         }
         else
         {
            memcpy_P((void*)preset_currentSaveMenuName,PSTR("Empty   "),8);
         }   
         return NULL; // for glo and sample we don't load a name
   }
	

#if USE_SD_CARD		
	//filename in 8.3  format
   char filename[9];
   preset_makeFileName(filename,presetNr,type);

	//try to open the file
   FRESULT res = f_open((FIL*)&preset_File,filename,FA_OPEN_EXISTING | FA_READ);
   totalBytes=0;
	
   if(res!=FR_OK)
   {
   	//error opening the file
      if (loadSave==0)
      {
         memcpy_P((void*)preset_currentName,PSTR("Empty   "),8);
         return (char*)preset_currentName;
      }
      else
      {
         memcpy_P((void*)preset_currentSaveMenuName,PSTR("Empty   "),8);
         return (char*)preset_currentSaveMenuName;
      } 
      
      	
   }
	
	//file opened correctly -> extract name (first 8 bytes)
   unsigned int bytesRead;
   if (loadSave==0)
   {
      f_read((FIL*)&preset_File,(void*)preset_currentName,8,&bytesRead);
      totalBytes+=bytesRead;
      //close the file handle
      f_close((FIL*)&preset_File);
   
      return (char*)preset_currentName;
   }
   else
   {
      f_read((FIL*)&preset_File,(void*)preset_currentSaveMenuName,8,&bytesRead);
      totalBytes+=bytesRead;
      //close the file handle
      f_close((FIL*)&preset_File);
   
      return (char*)preset_currentSaveMenuName;
   }
   

#else

return "ToDo";
#endif
}

//----------------------------------------------------
/** request step data from the cortex via uart and save it in the provided step struct*/
void preset_queryStepDataFromSeq(uint16_t stepNr)
{
   frontParser_newSeqDataAvailable = 0;

	//request step data
	//the max number for 14 bit data is 16383!!!
	//current max step nr is 128*7*8 = 7168
   frontPanel_sendByte((stepNr>>7)&0x7f);		//upper nibble 7 bit
   frontPanel_sendByte(stepNr&0x7f);			//lower nibble 7 bit

	//wait until data arrives
   uint8_t newSeqDataLocal = 0;
   uint16_t now = time_sysTick;
   while((newSeqDataLocal==0))
   {
   	//we have to call the uart parser to handle incoming messages from the sequencer
      uart_checkAndParse();
   	
      newSeqDataLocal = frontParser_newSeqDataAvailable;
   	
      if(time_sysTick-now >= 31)
      {
      	//timeout
         now = time_sysTick;
      	//request step again
         frontPanel_sendByte((stepNr>>7)&0x7f);		//upper nibble 7 bit
         frontPanel_sendByte(stepNr&0x7f);
      }
   }
}
    
 //----------------------------------------------------
void preset_queryPatternInfoFromSeq(uint8_t patternNr, uint8_t* next, uint8_t* repeat)
{
   frontParser_newSeqDataAvailable = 0;
	//request pattern info
   frontPanel_sendByte(patternNr);	

	//wait until data arrives
   uint8_t newSeqDataLocal = 0;
   uint16_t now = time_sysTick;
   while((newSeqDataLocal==0))
   {
   	//we have to call the uart parser to handle incoming messages from the sequencer
      uart_checkAndParse();
   	
      newSeqDataLocal = frontParser_newSeqDataAvailable;
   	
      if(time_sysTick-now >= 31)
      {
      	//timeout
         now = time_sysTick;
      	//request step again
         frontPanel_sendByte(patternNr);	
      }
   }
	
	//the stepdata struct is used as buffer for the data
   *next = frontParser_stepData.volume; 
   *repeat =  frontParser_stepData.prob;
}
 //----------------------------------------------------
void preset_queryMainStepDataFromSeq(uint16_t stepNr, uint16_t *mainStepData, uint8_t *length, uint8_t *scale)
{
   frontParser_newSeqDataAvailable = 0;

	//request step data
	//the max number for 14 bit data is 16383!!!
	//current max step nr is 7*8 = 56
   frontPanel_sendByte((stepNr>>7)&0x7f);		//upper nibble 7 bit
   frontPanel_sendByte(stepNr&0x7f);	//lower nibble 7 bit

	//wait until data arrives
   uint8_t newSeqDataLocal = 0;
   uint16_t now = time_sysTick;
   while((newSeqDataLocal==0))
   {
   	//we have to call the uart parser to handle incoming messages from the sequencer
      uart_checkAndParse();
   	
      newSeqDataLocal = frontParser_newSeqDataAvailable;
   	
      if(time_sysTick-now >= 31)
      {
      	//timeout
         now = time_sysTick;
      	//request step again
         frontPanel_sendByte((stepNr>>7)&0x7f);		//upper nibble 7 bit
         frontPanel_sendByte(stepNr&0x7f);
      }
   }
	
	// we are reusing these members for purposes other than those that were originally intended
   *mainStepData =(uint16_t) ((frontParser_stepData.volume<<8) | frontParser_stepData.prob);
   *length=frontParser_stepData.note;
   *scale=frontParser_stepData.param1Nr;
};
 //----------------------------------------------------
static void preset_writePatternData()
{
   uint16_t bytesWritten;
   uint8_t length;
   uint8_t scale;

   frontPanel_sendData(SEQ_CC,SEQ_EUKLID_RESET,0x01);

	//write the preset data
	//initiate the sysex mode
	
   while( (frontParser_midiMsg.status != SYSEX_START))
   {
      frontPanel_sendByte(SYSEX_START);
      uart_checkAndParse();
   }		
   _delay_ms(10);
	//enter step data mode
   frontPanel_sendByte(SYSEX_REQUEST_STEP_DATA);
   frontPanel_sysexMode = SYSEX_REQUEST_STEP_DATA;
	
   uint8_t percent=0;
   char text[5];
   uint16_t i;
   for(i=0;i<(STEPS_PER_PATTERN*NUM_PATTERN*NUM_TRACKS);i++)
   {
      if((i&0x80) == 0)
      {
      	//set cursor to beginning of row 2
         lcd_setcursor(0,2);
      	//print percent value
         percent = (uint8_t)(i * (100.f/(STEPS_PER_PATTERN*NUM_PATTERN*NUM_TRACKS)));
         itoa(percent,text,10);
         lcd_string(text);
      }			
   	
   	
   	
   	//get next data chunk and write it to file
      preset_queryStepDataFromSeq(i);
      f_write((FIL*)&preset_File,(const void*)&frontParser_stepData,sizeof(StepData),&bytesWritten);	
   }
	
	//end sysex mode
   frontPanel_sendByte(SYSEX_END);
   frontParser_midiMsg.status = 0;
	//now the main step data
   while( (frontParser_midiMsg.status != SYSEX_START))
   {
      frontPanel_sendByte(SYSEX_START);
      uart_checkAndParse();
   }	
   _delay_ms(50);	
   frontPanel_sendByte(SYSEX_REQUEST_MAIN_STEP_DATA);
   frontPanel_sysexMode = SYSEX_REQUEST_MAIN_STEP_DATA;
	
   uint16_t mainStepData;
   for(i=0;i<(NUM_PATTERN*NUM_TRACKS);i++)
   {
   	//get next data chunk and write it to file (length here is ignored)
      preset_queryMainStepDataFromSeq(i, &mainStepData, &length, &scale);
      f_write((FIL*)&preset_File,(const void*)&mainStepData,sizeof(uint16_t),&bytesWritten);	
   }
		
	//end sysex mode
   frontPanel_sendByte(SYSEX_END);
   frontParser_midiMsg.status = 0;
	
	//----- pattern info (next/repeat) ------
	
   while( (frontParser_midiMsg.status != SYSEX_START))
   {
      frontPanel_sendByte(SYSEX_START);
      uart_checkAndParse();
   }	
   _delay_ms(50);	
   frontPanel_sendByte(SYSEX_REQUEST_PATTERN_DATA);
   frontPanel_sysexMode = SYSEX_REQUEST_PATTERN_DATA;
	
   uint8_t next;
   uint8_t repeat;
   for(i=0;i<(NUM_PATTERN);i++)
   {
   	//get next data chunk and write it to file
      preset_queryPatternInfoFromSeq((uint8_t)i, &next, &repeat);
      f_write((FIL*)&preset_File,(const void*)&next,sizeof(uint8_t),&bytesWritten);	
      f_write((FIL*)&preset_File,(const void*)&repeat,sizeof(uint8_t),&bytesWritten);
   }
		
	//end sysex mode
   frontPanel_sendByte(SYSEX_END);
   frontParser_midiMsg.status = 0;
	
	
	//----- shuffle setting ------
   f_write((FIL*)&preset_File,(const void*)&parameter_values[PAR_SHUFFLE],sizeof(uint8_t),&bytesWritten);

	//----- pattern/track lengths ------
	// --AS we reuse the same call from above (when saving main step data)
	// but we only use the length info retrieved. We want to store it at the end
	// to avoid breaking compatibility with save file
   while( (frontParser_midiMsg.status != SYSEX_START))
   {
      frontPanel_sendByte(SYSEX_START);
      uart_checkAndParse();
   }
   _delay_ms(50);
   frontPanel_sendByte(SYSEX_REQUEST_MAIN_STEP_DATA);
   frontPanel_sysexMode = SYSEX_REQUEST_MAIN_STEP_DATA;

   for(i=0;i<(NUM_PATTERN*NUM_TRACKS);i++)
   {
   	//get next data chunk and write it to file
      preset_queryMainStepDataFromSeq(i, &mainStepData, &length, &scale);
      f_write((FIL*)&preset_File,(const void*)&length,sizeof(uint8_t),&bytesWritten);
   }
   

   for(i=0;i<(NUM_PATTERN*NUM_TRACKS);i++)
   {
   	//get next data chunk and write it to file
      preset_queryMainStepDataFromSeq(i, &mainStepData, &length, &scale);
      f_write((FIL*)&preset_File,(const void*)&scale,sizeof(uint8_t),&bytesWritten);
   }
   

	//end sysex mode
   frontPanel_sendByte(SYSEX_END);
   frontParser_midiMsg.status = 0;

}
 //----------------------------------------------------
void preset_savePattern(uint8_t presetNr)
{
#if USE_SD_CARD

   frontPanel_sendData(SEQ_CC,SEQ_EUKLID_RESET,0x01);
   
   uint16_t bytesWritten;
   lcd_clear();
   lcd_home();
   lcd_string_F(PSTR("Saving pattern"));
	
     
	
	//filename in 8.3  format
   char filename[9];
	//sprintf(filename,"p%03d.pat",presetNr);
   preset_makeFileName(filename,presetNr,FEXT_PAT);
	//open the file
   f_open((FIL*)&preset_File,filename,FA_CREATE_ALWAYS | FA_WRITE);

	//write preset name to file
   f_write((FIL*)&preset_File,(void*)preset_currentSaveMenuName,8,&bytesWritten);
   
   preset_writePatternData();

	//close the file
   f_close((FIL*)&preset_File);
	
	//reset the lcd
   menu_repaintAll();
	
#else
#endif
}
 //----------------------------------------------------
static void preset_readPatternScale()
{
   FIL scaleread_File;
   char sca_filename[GEN_BUF_LEN];
   UINT bytesRead;
   uint8_t i, trkNum, patNum;
   FRESULT res;
   uint16_t scaleOffset=0;
   uint8_t infoByte;
   
   switch(preset_workingType)
   {
      case WTYPE_PERFORMANCE:
         scaleOffset=VERSION_4_PERF_SCALE_OFFSET;
         preset_makeFileName(sca_filename,preset_workingPreset,FEXT_PERF);
         break;
      case WTYPE_ALL:
         scaleOffset=VERSION_4_ALL_SCALE_OFFSET;
         preset_makeFileName(sca_filename,preset_workingPreset,FEXT_ALL);
         break;
      case WTYPE_PATTERN:
         scaleOffset=VERSION_0_PAT_SCALE_OFFSET;
         preset_makeFileName(sca_filename,preset_workingPreset,FEXT_PAT);
         break;
      default:
         return;
   }   
   
   res = f_open((FIL*)&scaleread_File,sca_filename,FA_OPEN_EXISTING | FA_READ);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("open err "));
      lcd_string(text);
      while(1){;}
   }
   res = f_lseek((FIL*)&scaleread_File,scaleOffset);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("seek err "));
      lcd_string(text);
      while(1){;}
   }
   
   uint8_t scale[NUM_PATTERN*NUM_TRACKS];
   
   frontParser_midiMsg.status = 0;
   frontPanel_sendByte(SYSEX_START);
   while(frontParser_midiMsg.status != SYSEX_START)
   {
      uart_checkAndParse();
   }
   
   frontPanel_sendByte(SYSEX_SEND_PAT_SCALE_DATA);
   frontPanel_sysexMode = SYSEX_SEND_PAT_SCALE_DATA;
   frontParser_sysexCallback=NO_CALLBACK;
      
   for(i=0;i<(NUM_PATTERN*NUM_TRACKS);i++)
   {
      res=f_read((FIL*)&scaleread_File,(void*)&scale[i],sizeof(uint8_t),&bytesRead);
      if(res) {
         scale[i]=0; // default to a length of 16 since we didn't read anything
         {
            //print received data on LCD
            char text[2];
            lcd_clear();
            lcd_home();
            itoa((int)i,text,10);
            lcd_string(text);
            lcd_string_F(PSTR("scale err "));
            itoa(res,text,10);
            lcd_string(text);
            while(1){;}
         }
      }
   }
   
   f_close((FIL*)&scaleread_File);  
   
   for(patNum=0;patNum<NUM_PATTERN;patNum++)
   {
      for(trkNum=0;trkNum<NUM_TRACKS;trkNum++)
      {
         if(preset_workingVoiceArray&(0x01<<trkNum))
         {
            infoByte = (uint8_t)((trkNum&0x07)<<3);
            infoByte = (uint8_t)( (infoByte)|(patNum&0x07) );
            frontPanel_sendByte(infoByte);
            
            frontPanel_sendByte(scale[patNum*NUM_TRACKS+trkNum]);
           
         // wait to get ack from cortex
            while(frontParser_sysexCallback!=SCALE_CALLBACK)
            {
               uart_checkAndParse();
            }
            frontParser_sysexCallback=NO_CALLBACK;
         }
            
      }         
   }

   // end sysex mode
   frontParser_midiMsg.status = 0;
   frontPanel_sendByte(SYSEX_END);
   while(frontParser_midiMsg.status != SYSEX_END)
   {
      uart_checkAndParse();
   }
   
   
}
 //----------------------------------------------------
static void preset_readPatternLength()
{
   FIL lengthread_File;
   char len_filename[GEN_BUF_LEN];
   UINT bytesRead;
   uint8_t i, trkNum, patNum;
   FRESULT res;
   uint16_t lengthOffset=0;
   uint8_t infoByte;
   
   switch(preset_workingType)
   {
      case WTYPE_PERFORMANCE:
         lengthOffset=VERSION_4_PERF_LENGTH_OFFSET;
         preset_makeFileName(len_filename,preset_workingPreset,FEXT_PERF);
         break;
      case WTYPE_ALL:
         lengthOffset=VERSION_4_ALL_LENGTH_OFFSET;
         preset_makeFileName(len_filename,preset_workingPreset,FEXT_ALL);
         break;
      case WTYPE_PATTERN:
         lengthOffset=VERSION_0_PAT_LENGTH_OFFSET;
         preset_makeFileName(len_filename,preset_workingPreset,FEXT_PAT);
         break;
      default:
         return;
   }  
   
   res = f_open((FIL*)&lengthread_File,len_filename,FA_OPEN_EXISTING | FA_READ);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("lopen err "));
      lcd_string(text);
      while(1){;}
   }
   res = f_lseek((FIL*)&lengthread_File,lengthOffset);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("lseek err "));
      lcd_string(text);
      while(1){;}
   }
   
   uint8_t length[NUM_PATTERN*NUM_TRACKS];
   
   frontParser_midiMsg.status = 0;
   frontPanel_sendByte(SYSEX_START);
   while(frontParser_midiMsg.status != SYSEX_START)
   {
      uart_checkAndParse();
   }
   
   frontPanel_sendByte(SYSEX_SEND_PAT_LEN_DATA);
   frontPanel_sysexMode = SYSEX_SEND_PAT_LEN_DATA;
   frontParser_sysexCallback=NO_CALLBACK;
      
   for(i=0;i<(NUM_PATTERN*NUM_TRACKS);i++)
   {
      res=f_read((FIL*)&lengthread_File,(void*)&length[i],sizeof(uint8_t),&bytesRead);
      if(res) {
         length[i]=0; // default to a length of 16 since we didn't read anything
         {
               //print received data on LCD
            char text[2];
            lcd_clear();
            lcd_home();
            itoa((int)i,text,10);
            lcd_string(text);
            lcd_string_F(PSTR("len err "));
            itoa(res,text,10);
            lcd_string(text);
            while(1){;}
         }
      }
   }
   
   f_close((FIL*)&lengthread_File);  

   for(patNum=0;patNum<NUM_PATTERN;patNum++)
   {
      for(trkNum=0;trkNum<NUM_TRACKS;trkNum++)
      {
         if(preset_workingVoiceArray&(0x01<<trkNum))
         {
            infoByte = (uint8_t)((trkNum&0x07)<<3);
            infoByte = (uint8_t)( (infoByte)|(patNum&0x07) );
            frontPanel_sendByte(infoByte);
            
            frontPanel_sendByte(length[patNum*NUM_TRACKS+trkNum]);
           
         // wait to get ack from cortex
            while(frontParser_sysexCallback!=LENGTH_CALLBACK)
            {
               uart_checkAndParse();
            }
            frontParser_sysexCallback=NO_CALLBACK;
         }
            
      }         
   }
   
   // end sysex mode
   frontParser_midiMsg.status = 0;
   frontPanel_sendByte(SYSEX_END);
   while(frontParser_midiMsg.status != SYSEX_END)
   {
      uart_checkAndParse();
   }

   
}
 //----------------------------------------------------
static void preset_readShuffle()
{
   FIL shuffread_File;
   char shuff_filename[GEN_BUF_LEN];
   UINT bytesRead;
   FRESULT res;
   uint16_t shuffOffset=0;
   
   switch(preset_workingType)
   {
      case WTYPE_PERFORMANCE:
         shuffOffset=VERSION_4_PERF_SHUFFLE_OFFSET;
         preset_makeFileName(shuff_filename,preset_workingPreset,FEXT_PERF);
         break;
      case WTYPE_ALL:
         shuffOffset=VERSION_4_ALL_SHUFFLE_OFFSET;
         preset_makeFileName(shuff_filename,preset_workingPreset,FEXT_ALL);
         break;
      case WTYPE_PATTERN:
         shuffOffset=VERSION_0_PAT_SHUFFLE_OFFSET;
         preset_makeFileName(shuff_filename,preset_workingPreset,FEXT_PAT);
         break;
      default:
         return;
   }  
   
   res = f_open((FIL*)&shuffread_File,shuff_filename,FA_OPEN_EXISTING | FA_READ);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("shopen err "));
      lcd_string(text);
      while(1){;}
   }
   res = f_lseek((FIL*)&shuffread_File,shuffOffset);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("shseek err "));
      lcd_string(text);
      while(1){;}
   }
   
   // ------------ shuffle settings
   	//load the shuffle settings
   f_read((FIL*)&shuffread_File,(void*)&parameter_values[PAR_SHUFFLE],sizeof(uint8_t),&bytesRead);
   if(res) 
   {
      
               //print received data on LCD
      char text[2];
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("shuff err "));
      itoa(res,text,10);
      lcd_string(text);
      while(1){;}
      
   }
   else
      frontPanel_sendData(SEQ_CC,SEQ_SHUFFLE,parameter_values[PAR_SHUFFLE]);

   f_close((FIL*)&shuffread_File);  

}
 //----------------------------------------------------
static void preset_readPatternChain()
{
   FIL chainread_File;
   char chain_filename[GEN_BUF_LEN];
   UINT bytesRead;
   uint8_t i;
   FRESULT res;
   uint16_t chainOffset=0;
   
   switch(preset_workingType)
   {
      case WTYPE_PERFORMANCE:
         chainOffset=VERSION_4_PERF_PATCHAIN_OFFSET;
         preset_makeFileName(chain_filename,preset_workingPreset,FEXT_PERF);
         break;
      case WTYPE_ALL:
         chainOffset=VERSION_4_ALL_PATCHAIN_OFFSET;
         preset_makeFileName(chain_filename,preset_workingPreset,FEXT_ALL);
         break;
      case WTYPE_PATTERN:
         chainOffset=VERSION_0_PAT_PATCHAIN_OFFSET;
         preset_makeFileName(chain_filename,preset_workingPreset,FEXT_PAT);
         break;
      default:
         return;
   }  
   
   res = f_open((FIL*)&chainread_File,chain_filename,FA_OPEN_EXISTING | FA_READ);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("chopen err "));
      lcd_string(text);
      while(1){;}
   }
   res = f_lseek((FIL*)&chainread_File,chainOffset);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("chseek err "));
      lcd_string(text);
      while(1){;}
   }
   
   uint8_t next[NUM_PATTERN];
   uint8_t repeat[NUM_PATTERN];
   
   frontParser_midiMsg.status = 0;
   frontPanel_sendByte(SYSEX_START);
   while(frontParser_midiMsg.status != SYSEX_START)
   {
      uart_checkAndParse();
   }
   
   frontPanel_sendByte(SYSEX_SEND_PAT_CHAIN_DATA);
   frontPanel_sysexMode = SYSEX_SEND_PAT_CHAIN_DATA;
   frontParser_sysexCallback=NO_CALLBACK;
      
   for(i=0;i<(NUM_PATTERN);i++)
   {
      res=f_read((FIL*)&chainread_File,(void*)&next[i],sizeof(uint8_t),&bytesRead);
      if(res) {
         next[i]=i; // default to a length of 16 since we didn't read anything
         {
               //print received data on LCD
            char text[2];
            lcd_clear();
            lcd_home();
            itoa((int)i,text,10);
            lcd_string(text);
            lcd_string_F(PSTR("next err "));
            itoa(res,text,10);
            lcd_string(text);
            while(1){;}
         }
      }
      res=f_read((FIL*)&chainread_File,(void*)&repeat[i],sizeof(uint8_t),&bytesRead);
      if(res) {
         repeat[i]=0; // default to a length of 16 since we didn't read anything
         {
               //print received data on LCD
            char text[2];
            lcd_clear();
            lcd_home();
            itoa((int)i,text,10);
            lcd_string(text);
            lcd_string_F(PSTR("rpt err "));
            itoa(res,text,10);
            lcd_string(text);
            while(1){;}
         }
      }
   
   }
   
   f_close((FIL*)&chainread_File);  

   for(i=0;i<(NUM_PATTERN);i++)
   { 
      frontPanel_sendByte(next[i]);    
      // wait to get ack from cortex  
      while(frontParser_sysexCallback!=PATCHAIN_CALLBACK)
      {
         uart_checkAndParse();
      }
      frontParser_sysexCallback=NO_CALLBACK;
      
      frontPanel_sendByte(repeat[i]);    
      // wait to get ack from cortex  
      while(frontParser_sysexCallback!=PATCHAIN_CALLBACK)
      {
         uart_checkAndParse();
      }
      frontParser_sysexCallback=NO_CALLBACK;
            
   }
   // end sysex mode
   frontParser_midiMsg.status = 0;
   frontPanel_sendByte(SYSEX_END);
   while(frontParser_midiMsg.status != SYSEX_END)
   {
      uart_checkAndParse();
   }

   
}
 //----------------------------------------------------
static void preset_readPatternMainStep()
{
   FIL mainStpRead_File;
   char mainStp_filename[GEN_BUF_LEN];
   UINT bytesRead;
   uint8_t patNum;
   uint8_t trkNum;
   FRESULT res;
   uint16_t mainStpOffset=0;
   uint8_t infoByte;
   
   switch(preset_workingType)
   {
      case WTYPE_PERFORMANCE:
         mainStpOffset=VERSION_4_PERF_MAINSTEP_OFFSET;
         preset_makeFileName(mainStp_filename,preset_workingPreset,FEXT_PERF);
         break;
      case WTYPE_ALL:
         mainStpOffset=VERSION_4_ALL_MAINSTEP_OFFSET;
         preset_makeFileName(mainStp_filename,preset_workingPreset,FEXT_ALL);
         break;
      case WTYPE_PATTERN:
         mainStpOffset=VERSION_0_PAT_MAINSTEP_OFFSET;
         preset_makeFileName(mainStp_filename,preset_workingPreset,FEXT_PAT);
         break;
      default:
         return;
   }  
   
   res = f_open((FIL*)&mainStpRead_File,mainStp_filename,FA_OPEN_EXISTING | FA_READ);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("msopen err "));
      lcd_string(text);
      while(1){;}
   }
   res = f_lseek((FIL*)&mainStpRead_File,mainStpOffset);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("msseek err "));
      lcd_string(text);
      while(1){;}
   }
   
   uint16_t mainStep[NUM_PATTERN*NUM_TRACKS];
   
   frontParser_midiMsg.status = 0;
   frontPanel_sendByte(SYSEX_START);
   while(frontParser_midiMsg.status != SYSEX_START)
   {
      uart_checkAndParse();
   }
   
   frontPanel_sendByte(SYSEX_SEND_MAIN_STEP_DATA);
   frontPanel_sysexMode = SYSEX_SEND_MAIN_STEP_DATA;
   frontParser_sysexCallback=NO_CALLBACK;
      
   for(patNum=0;patNum<NUM_PATTERN;patNum++)
   {
      for(trkNum=0;trkNum<NUM_TRACKS;trkNum++)
      {
         res=f_read((FIL*)&mainStpRead_File,(void*)&mainStep[patNum*NUM_TRACKS+trkNum],sizeof(uint16_t),&bytesRead);
         if(res) {
            mainStep[patNum*NUM_TRACKS+trkNum]=0; // default to a length of 16 since we didn't read anything
            {
               //print received data on LCD
               char text[2];
               lcd_clear();
               lcd_home();
               itoa((int)(patNum*NUM_TRACKS+trkNum),text,10);
               lcd_string(text);
               lcd_string_F(PSTR("mStp err "));
               itoa(res,text,10);
               lcd_string(text);
               while(1){;}
            }
         }
      }
   }
   
   f_close((FIL*)&mainStpRead_File);  

   for(patNum=0;patNum<NUM_PATTERN;patNum++)
   {
      for(trkNum=0;trkNum<NUM_TRACKS;trkNum++)
      {
         if(preset_workingVoiceArray&(0x01<<trkNum))
         {
            infoByte = (uint8_t)((trkNum&0x07)<<3);
            infoByte = (uint8_t)( (infoByte)|(patNum&0x07) );
            frontPanel_sendByte(infoByte);
         
            frontPanel_sendByte(mainStep[patNum*NUM_TRACKS+trkNum] & 0x7f);
            frontPanel_sendByte((mainStep[patNum*NUM_TRACKS+trkNum]>>7) & 0x7f);
            frontPanel_sendByte((uint8_t)((mainStep[patNum*NUM_TRACKS+trkNum]>>14) & 0x7f));
         // wait to get ack from cortex
            while(frontParser_sysexCallback!=MAINSTEP_CALLBACK)
            {
               uart_checkAndParse();
            }
            frontParser_sysexCallback=NO_CALLBACK;     
         }
      }
   }
   
   // end sysex mode
   frontParser_midiMsg.status = 0;
   frontPanel_sendByte(SYSEX_END);
   while(frontParser_midiMsg.status != SYSEX_END)
   {
      uart_checkAndParse();
   }

   
}

//----------------------------------------------------
static void preset_readPatternStepData(uint8_t track, uint8_t pattern)
{
   FIL stepRead_File;
   char step_filename[GEN_BUF_LEN];
   UINT bytesRead;
   FRESULT res;
   uint16_t stepOffset=0;
   volatile StepData preset_stepData;
   uint8_t i;
   
   switch(preset_workingType)
   {
      case WTYPE_PERFORMANCE:
         stepOffset=VERSION_4_PERF_STEPDATA_OFFSET
            +(uint16_t)(track*(uint16_t)NUM_PATTERN*(uint16_t)STEPS_PER_PATTERN*(uint16_t)sizeof(StepData))
            +(uint16_t)(pattern*(uint16_t)STEPS_PER_PATTERN*(uint16_t)sizeof(StepData));
         preset_makeFileName(step_filename,preset_workingPreset,FEXT_PERF);
         break;
      case WTYPE_ALL:
         stepOffset=VERSION_4_ALL_STEPDATA_OFFSET
            +(uint16_t)(track*(uint16_t)NUM_PATTERN*(uint16_t)STEPS_PER_PATTERN*(uint16_t)sizeof(StepData))
            +(uint16_t)(pattern*(uint16_t)STEPS_PER_PATTERN*(uint16_t)sizeof(StepData));
         preset_makeFileName(step_filename,preset_workingPreset,FEXT_ALL);
         break;
      case WTYPE_PATTERN:
         stepOffset=VERSION_0_PAT_STEPDATA_OFFSET
            +(uint16_t)(track*(uint16_t)NUM_PATTERN*(uint16_t)STEPS_PER_PATTERN*(uint16_t)sizeof(StepData))
            +(uint16_t)(pattern*(uint16_t)STEPS_PER_PATTERN*(uint16_t)sizeof(StepData));
         preset_makeFileName(step_filename,preset_workingPreset,FEXT_PAT);
         break;
      default:
         return;
   }  
   
   res = f_open((FIL*)&stepRead_File,step_filename,FA_OPEN_EXISTING | FA_READ);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("stpopen err "));
      lcd_string(text);
      while(1){;}
   }
   res = f_lseek((FIL*)&stepRead_File,stepOffset);
   if(res)
   {
      //print received data on LCD
      char text[2];
      itoa(res,text,10);
      lcd_clear();
      lcd_home();
      lcd_string_F(PSTR("stpseek err "));
      lcd_string(text);
      while(1){;}
   }
   
   frontParser_midiMsg.status = 0;
   frontPanel_sendByte(SYSEX_START);
   while(frontParser_midiMsg.status != SYSEX_START)
   {
      uart_checkAndParse();
   }
   
   frontPanel_sendByte(SYSEX_BEGIN_PATTERN_TRANSMIT);
   frontPanel_sysexMode = SYSEX_BEGIN_PATTERN_TRANSMIT;
   frontParser_sysexCallback=NO_CALLBACK;
   
   for(i=0;i<128;i++)
   {
      res=f_read((FIL*)&stepRead_File,(void*)&preset_stepData,sizeof(StepData),&bytesRead);
      if(res) {
         {
               //print received data on LCD
            char text[2];
            lcd_clear();
            lcd_home();
            lcd_string(text);
            lcd_string_F(PSTR("step err "));
            itoa(res,text,10);
            lcd_string(text);
            while(1){;}
         }
      }
      // break step data down into 7-bit sysex messages and transmit
      {
         uint8_t infoByte = (uint8_t)((track&0x07)<<3);
         infoByte = (uint8_t)( (infoByte)|(pattern&0x07) );
         frontPanel_sendByte(infoByte);
      
         frontPanel_sendByte(preset_stepData.volume	& 0x7f);
         frontPanel_sendByte(preset_stepData.prob	& 0x7f);
         frontPanel_sendByte(preset_stepData.note	& 0x7f);
      
         frontPanel_sendByte(preset_stepData.param1Nr	& 0x7f);
         frontPanel_sendByte(preset_stepData.param1Val	& 0x7f);
      
         frontPanel_sendByte(preset_stepData.param2Nr	& 0x7f);
         frontPanel_sendByte(preset_stepData.param2Val	& 0x7f);
      
      //now the MSBs from all 7 values
         frontPanel_sendByte((uint8_t)(
                     ((preset_stepData.volume 	& 0x80)>>7) |
            			((preset_stepData.prob	 	& 0x80)>>6) |
            			((preset_stepData.note	 	& 0x80)>>5) |
            			((preset_stepData.param1Nr	& 0x80)>>4) |
            			((preset_stepData.param1Val	& 0x80)>>3) |
            			((preset_stepData.param2Nr	& 0x80)>>2) |
            			((preset_stepData.param2Val	& 0x80)>>1))
            			);
      }
      
      // must receive an ack from main before sending next step to avoid overflow
      frontParser_sysexCallback=NO_CALLBACK;
      while(frontParser_sysexCallback==NO_CALLBACK)
      {  
         uart_checkAndParse();
      }
   
   }
   f_close((FIL*)&stepRead_File); 
   
   // wait callback - step done on cortex
   
   //menu_debug("WAIT STEP CALL", 14, track, pattern, 0);
   
   while(frontParser_sysexCallback!=STEP_CALLBACK)
   {  
      uart_checkAndParse();
   }
   frontParser_sysexCallback=NO_CALLBACK;
   
   // end sysex mode
   frontParser_midiMsg.status = 0;
   frontPanel_sendByte(SYSEX_END);
   
   //menu_debug("WAIT SYSX", 9, track, pattern, 0);
   
   while(frontParser_midiMsg.status != SYSEX_END)
   {
      uart_checkAndParse();
   }

}
   
 
//----------------------------------------------------
uint8_t preset_loadPattern(uint8_t presetNr, uint8_t voiceArray)
{
   #if USE_SD_CARD
   UINT bytesRead;
   uint8_t trkNum;
   uint8_t patNum;

   uart_clearRxFifo();
   frontParser_rxDisable=1;
   
   preset_workingPreset=presetNr;
   preset_workingType=WTYPE_PATTERN;
   preset_workingVoiceArray = voiceArray;

   preset_makeFileName(filename,presetNr,FEXT_PAT);
   
   //open the file
   FRESULT res = f_open((FIL*)&preset_File,filename,FA_OPEN_EXISTING | FA_READ);
   if(res!=FR_OK)
      return 1; //file open error... maybe the file does not exist?
   
	//first the preset name
   f_read((FIL*)&preset_File,(void*)preset_currentName,8,&bytesRead);
   if(!bytesRead)
      goto closeFile;
   

   preset_workingVersion = 0;
   
   f_close((FIL*)&preset_File);
   
   lcd_clear();
   lcd_home();
   lcd_string_F(PSTR("Loading Patrn"));
   
   frontPanel_sendData(SEQ_CC,SEQ_FILE_BEGIN,WTYPE_PATTERN);
   frontPanel_sendData(SEQ_CC,SEQ_EUKLID_RESET,0x01);
   
   // bc - NB: if enabled, this will lock the track
   preset_readPatternMainStep();
   

   if( (preset_workingVoiceArray>=0x7f) || (preset_workingVoiceArray==0x7f) )
   {
      preset_readShuffle();
   }
   
   preset_readPatternLength();
   preset_readPatternScale();
   
   if( (preset_workingVoiceArray>=0x7f) || (preset_workingVoiceArray==0x7f) )
   {
      preset_readPatternChain();
   }
   // calling this twice ensures pattern realign
   menu_setShownPattern(menu_playedPattern);
   menu_setShownPattern(menu_playedPattern);
   
   
   for (trkNum=0;trkNum<NUM_TRACKS;trkNum++)
   {
      if(voiceArray&(0x01<<trkNum))
      {
         // if track is locked, this will also unlock it for playing
         preset_readPatternStepData(trkNum,menu_playedPattern);
         
            
      }
      
   }
   

   for (trkNum=0;trkNum<NUM_TRACKS;trkNum++)
   {
      if(preset_workingVoiceArray&(0x01<<trkNum))
      {
         for (patNum=0;patNum<NUM_PATTERN;patNum++)
         {
            if(patNum!=menu_playedPattern)
               preset_readPatternStepData(trkNum,patNum);
         }
      }
   }


   frontPanel_sendData(SEQ_CC,SEQ_FILE_DONE,WTYPE_PATTERN);
   
	//force complete repaint
   menu_repaintAll();

#else
	frontPanel_sendData(PRESET,PRESET_LOAD,presetNr);
#endif

closeFile:
	//close the file handle
   f_close((FIL*)&preset_File);
   
   _delay_ms(50);
   frontParser_rxDisable=0;
   uart_clearRxFifo();
   
   return 0;
}

//----------------------------------------------------
//val is interpolation value between 0 and 255 
//uses bankers rounding
static uint8_t interpolate(uint8_t a, uint8_t b, uint8_t x)
{
   uint16_t fixedPointValue = (uint16_t)(((a*256) + (b-a)*x));
   uint8_t result = (uint8_t)(fixedPointValue/256);
	
   return (uint8_t)((fixedPointValue&0xff) < 0x7f ? result : result+1);
	//return ((a*255) + (b-a)*x)/255;
}
//----------------------------------------------------
// This will determine an interpolated value between parameters
// and morph parameters and send the data to the back
// if the morph value is 0 it will just send the parameter values
// to the back
void preset_morph(uint8_t voiceArray, uint8_t morph)
{
   uint8_t i, k;
   uint8_t *parArray;

   if(frontParser_isRestoreActive())
      return;

   if (morph>=127)
      morph=255;
   else
      morph=(uint8_t)(morph<<1);
         
   for(k=0;k<NUM_TRACKS-1;k++)
   {      
   
      if(voiceArray&(0x01))
      {
         parArray=voice1presetMask;
         voiceArray &= (uint8_t)~(0x01);
      } 
      else if(voiceArray&(0x02))
      {
         parArray=voice2presetMask;
         voiceArray &= (uint8_t)~(0x02);
      }
      else if(voiceArray&(0x04))
      {
         parArray=voice3presetMask;
         voiceArray &= (uint8_t)~(0x04);
      }
      else if(voiceArray&(0x08))
      {
         parArray=voice4presetMask;
         voiceArray &= (uint8_t)~(0x08);
      }	
      else if(voiceArray&(0x10))
      {
         parArray=voice5presetMask;
         voiceArray &= (uint8_t)~(0x10);
      }	
      else if((voiceArray&(0x20))||(voiceArray&(0x40)))
      {
         parArray=voice6presetMask;
         voiceArray &= (uint8_t)~(0x60);
      }	
      else 
         goto EndLoop;
      
      for(i=0;i<VOICE_PARAM_LENGTH;i++)
      {
         uint8_t paramNumber = parArray[i];
         uint8_t val;
         	
         val = interpolate(parameter_values[paramNumber],parameters2[paramNumber],morph);
         if(paramNumber<128) 
         {
            frontPanel_sendData(MIDI_CC,(uint8_t)paramNumber,val);
         } 
         else 
         {
            frontPanel_sendData(CC_2,(uint8_t)(paramNumber-128),val);
         }		
         	
         		
         	//to omit front panel button/LED lag we have to process din dout and uart here
         	//read next button
         din_readNextInput();
         	//update LEDs
         dout_updateOutputs();
         	//read uart messages from sequencer
         uart_checkAndParse();
      }	
   EndLoop: ;
   }	



}

//----------------------------------------------------
uint8_t preset_getMorphValue(uint16_t index, uint8_t morph)
{
   return interpolate(parameter_values[index],parameters2[index],morph);
};
//----------------------------------------------------

//----------------------------------------------------
//--AS **SAVEALL
// save all settings (global, pattern, kit)
// or save performance settings (pattern, kit)

void preset_saveAll(uint8_t presetNr, uint8_t isAll)
{
#if USE_SD_CARD
   uint16_t remain;
   uint8_t siz;
	//filename in 8.3  format
   char filename[GEN_BUF_LEN];
   if(isAll)
      preset_makeFileName(filename,presetNr,FEXT_ALL);
   else
      preset_makeFileName(filename,presetNr,FEXT_PERF);
	//open the file
   f_open((FIL*)&preset_File,filename,FA_CREATE_ALWAYS | FA_WRITE);
   totalBytes=0;


   unsigned int bytesWritten;
	//write preset name to file
   f_write((FIL*)&preset_File,(void*)preset_currentSaveMenuName,8,&bytesWritten);

	//write the version
   filename[0]=FILE_VERSION;
   f_write((FIL*)&preset_File, filename, 1, &bytesWritten);

   memset(filename,0xFF,GEN_BUF_LEN); // reuse this as our buffer for padding

   if(isAll) {
   	// write global settings
      preset_writeGlobalData();
   
   	// write some padding (we'll allocate 64 bytes for globals, right now we have +/- 18) so that
   	// we don't need to change the file format later
      remain=	64- (NUM_PARAMS - PAR_BEGINNING_OF_GLOBALS);
   
   } 
   else {
   	// write the bpm
      f_write((FIL*)&preset_File, &parameter_values[PAR_BPM], 1, &bytesWritten);
   	// bar reset mode
      f_write((FIL*)&preset_File, &parameter_values[PAR_BAR_RESET_MODE], 1, &bytesWritten);
      // pattern change time
      f_write((FIL*)&preset_File, &parameter_values[PAR_SEQ_PC_TIME], 1, &bytesWritten);
      // track midi channel settings
      f_write((FIL*)&preset_File, &parameter_values[PAR_MIDI_CHAN_1], 1, &bytesWritten);
      f_write((FIL*)&preset_File, &parameter_values[PAR_MIDI_CHAN_2], 1, &bytesWritten);
      f_write((FIL*)&preset_File, &parameter_values[PAR_MIDI_CHAN_3], 1, &bytesWritten);
      f_write((FIL*)&preset_File, &parameter_values[PAR_MIDI_CHAN_4], 1, &bytesWritten);
      f_write((FIL*)&preset_File, &parameter_values[PAR_MIDI_CHAN_5], 1, &bytesWritten);
      f_write((FIL*)&preset_File, &parameter_values[PAR_MIDI_CHAN_6], 1, &bytesWritten);
      f_write((FIL*)&preset_File, &parameter_values[PAR_MIDI_CHAN_7], 1, &bytesWritten);
      // track midi note settings
      f_write((FIL*)&preset_File, &parameter_values[PAR_MIDI_NOTE1], 7, &bytesWritten);
      
      remain=	64 - (1+1)-14;
   }

	// write padding after globals/perf data
   while(remain){
      if(remain < GEN_BUF_LEN)
         siz=(uint8_t)remain;
      else
         siz=GEN_BUF_LEN;
      f_write((FIL*)&preset_File, filename, siz, &bytesWritten);
      remain -=siz;
   }

	// save kit
   preset_writeDrumsetData(0);

   // check remain from drumkit data
   remain = 512 - (END_OF_SOUND_PARAMETERS);
   
   // write some more padding (we'll allocate 512 bytes for kit data. This should be more than
	// we'll ever need. Right now we use about 228 bytes for kit plus morph params).
   
   while(remain){
      if(remain < GEN_BUF_LEN)
         siz=(uint8_t)remain;
      else
         siz=GEN_BUF_LEN;
      f_write((FIL*)&preset_File, filename, siz, &bytesWritten);
      remain -=siz;
   }

   // save morph parameter endpoint
   preset_writeDrumsetData(2);

   // check remain from drumkit data
   remain = 512 - (END_OF_SOUND_PARAMETERS);
   
   // write some more padding (we'll allocate 512 bytes for kit data. This should be more than
	// we'll ever need. Right now we use about 228 bytes for kit plus morph params).
   
   while(remain){
      if(remain < GEN_BUF_LEN)
         siz=(uint8_t)remain;
      else
         siz=GEN_BUF_LEN;
      f_write((FIL*)&preset_File, filename, siz, &bytesWritten);
      remain -=siz;
   }



	// write pattern data
   preset_writePatternData();
   
   
	//close the file
   f_close((FIL*)&preset_File);
#else
#endif
}
   
//----------------------------------------------------
uint8_t preset_loadAll(uint8_t presetNr, uint8_t voiceArray)
{

   #if USE_SD_CARD
	//filename in 8.3  format
   
   UINT bytesRead;
   uint8_t version=0;
   uint8_t trkNum;
   uint8_t patNum;
   uint8_t i;
   uint8_t flowSessionStarted=0;
   uint8_t flowSessionOk=1;
   uint8_t fileBeginSent=0;
   uint8_t fileDoneSent=0;
   uint8_t menuEndpointsSaved=0;

   uart_clearRxFifo();
   if(!frontPanel_flowBeginSession())
      return 1;
   flowSessionStarted=1;
   frontParser_rxDisable=1;
   
   preset_workingPreset=presetNr;
   preset_workingType=WTYPE_ALL;
   preset_workingVoiceArray = voiceArray;

   preset_makeFileName(filename,presetNr,FEXT_ALL);
   
   //open the file
   FRESULT res = f_open((FIL*)&preset_File,filename,FA_OPEN_EXISTING | FA_READ);
   if(res!=FR_OK) {
      (void)frontPanel_flowEndSession();
      frontParser_rxDisable=0;
      uart_clearRxFifo();
      return 1; //file open error... maybe the file does not exist?
   }
   
	//first the preset name
   f_read((FIL*)&preset_File,(void*)preset_currentName,8,&bytesRead);
   if(!bytesRead)
      goto closeFile;
   
	// read version number and make sure its valid
   f_read((FIL*)&preset_File,&version,1,&bytesRead);
   if(!bytesRead || version > FILE_VERSION)
      goto closeFile;
   
   preset_workingVersion = version;
   frontPanel_sendData(SEQ_CC,SEQ_FILE_BEGIN,WTYPE_ALL);
   fileBeginSent=1;

   if(preset_shouldPreserveMenuEndpointsDuringFileLoad())
   {
      preset_saveMenuEndpointsDuringFileLoad();
      menuEndpointsSaved=1;
   }
   
   if( (preset_workingVoiceArray>=0x7f) || (preset_workingVoiceArray==0x7f) )
   {
      #if USE_SD_CARD
      for(i=0;(i<(NUM_PARAMS-PAR_BEGINNING_OF_GLOBALS)) &&( bytesRead!=0);i++) 
      {
         f_read((FIL*)&preset_File,&parameter_values[(PAR_BEGINNING_OF_GLOBALS+i)],1,&bytesRead);
         totalBytes+=bytesRead;
         if(!bytesRead)
            goto closeFile;
         // if anything other than bpm is 0xff, we are reading filler
         // set global to 0 by default
         else if((parameter_values[(PAR_BEGINNING_OF_GLOBALS+i)]==0xff) && i)
            parameter_values[(PAR_BEGINNING_OF_GLOBALS+i)]=0;
            
      }
      #endif
      
      // send global params
      menu_sendAllGlobals();
   }
 //close the file handle
   f_close((FIL*)&preset_File);
   
   lcd_clear();
   lcd_home();
   lcd_string_F(PSTR("Loading All"));
   
   frontPanel_sendData(SEQ_CC,SEQ_EUKLID_RESET,0x01);
   
   // bc - NB: if enabled, this will lock the track
   preset_readPatternMainStep();
   

   if( (preset_workingVoiceArray>=0x7f) || (preset_workingVoiceArray==0x7f) )
   {
      preset_readShuffle();
   }
   
   
   preset_readPatternLength();
   preset_readPatternScale();
   
   if( (preset_workingVoiceArray>=0x7f) || (preset_workingVoiceArray==0x7f) )
   {
      preset_readPatternChain();
   }
   // calling this twice ensures pattern realign
   menu_setShownPattern(menu_playedPattern);
   menu_setShownPattern(menu_playedPattern);
   
   preset_readKitToTemp(1);
   preset_readKitToTemp(0);
   
   for (trkNum=0;trkNum<NUM_TRACKS;trkNum++)
   {
      if(voiceArray&(0x01<<trkNum))
      {
         
         if(trkNum<6)
         {
            frontPanel_sendData(SEQ_CC,SEQ_LOAD_VOICE,trkNum); 
            preset_readDrumVoice(trkNum, 1);
            preset_readDrumVoice(trkNum, 0);
         }
         
         // if track is locked, this will also unlock it for playing
         preset_readPatternStepData(trkNum,menu_playedPattern);
         
         if(trkNum<5)
            frontPanel_sendData(SEQ_CC,SEQ_UNHOLD_VOICE,trkNum);
            
      }
      
   }
   
   frontPanel_sendData(SEQ_CC,SEQ_UNHOLD_VOICE,5); 
   
   if( (voiceArray>=0x7f) || (voiceArray==0) )
   {
      preset_readDrumsetMeta(0);
      preset_readDrumsetMeta(1);
   }

   preset_dumpEndpointsToStm(SEQ_TMP_KIT_ENDPOINT_BOTH);

   /* TEMP PATTERN: .all/.prf loads still dump the loaded endpoints to STM normal
      storage, but temp playback keeps AVR menu endpoint arrays from the active
      temporary image until playback leaves the temporary pattern. */
   if(menuEndpointsSaved)
   {
      preset_restoreMenuEndpointsDuringFileLoad();
      menuEndpointsSaved=0;
   }
   
   for (trkNum=0;trkNum<NUM_TRACKS;trkNum++)
   {
      if(preset_workingVoiceArray&(0x01<<trkNum))
      {
         for (patNum=0;patNum<NUM_PATTERN;patNum++)
         {
            if(patNum!=menu_playedPattern)
               preset_readPatternStepData(trkNum,patNum);
         }
      }
   }

   frontPanel_sendData(SEQ_CC,SEQ_FILE_DONE,WTYPE_ALL);
   fileDoneSent=1;
   
	//force complete repaint
   menu_repaintAll();

#else
	frontPanel_sendData(PRESET,PRESET_LOAD,presetNr);
#endif

closeFile:
   if(menuEndpointsSaved)
   {
      preset_restoreMenuEndpointsDuringFileLoad();
      menuEndpointsSaved=0;
   }

	//close the file handle
   f_close((FIL*)&preset_File);

   if(fileBeginSent && !fileDoneSent)
      frontPanel_sendData(SEQ_CC,SEQ_FILE_DONE,WTYPE_ALL);
   
   _delay_ms(50);
   if(flowSessionStarted)
      flowSessionOk = frontPanel_flowEndSession();
   frontParser_rxDisable=0;
   uart_clearRxFifo();
   
   return flowSessionOk ? 0 : 1;
}

   
//----------------------------------------------------
uint8_t preset_loadPerf(uint8_t presetNr, uint8_t voiceArray)
{
   #if USE_SD_CARD
	//filename in 8.3  format
   
   UINT bytesRead;
   uint8_t version=0;
   uint8_t trkNum;
   uint8_t patNum;
   uint8_t flowSessionStarted=0;
   uint8_t flowSessionOk=1;
   uint8_t fileBeginSent=0;
   uint8_t fileDoneSent=0;
   uint8_t menuEndpointsSaved=0;

   uart_clearRxFifo();
   if(!frontPanel_flowBeginSession())
      return 1;
   flowSessionStarted=1;
   frontParser_rxDisable=1;
   
   preset_workingPreset=presetNr;
   preset_workingType=WTYPE_PERFORMANCE; // wtype is held until file load is done
   preset_workingVoiceArray = voiceArray; // bit register of voies to load this perf

   preset_makeFileName(filename,presetNr,FEXT_PERF);
   
	//open the file
   FRESULT res = f_open((FIL*)&preset_File,filename,FA_OPEN_EXISTING | FA_READ);
   if(res!=FR_OK) {
      (void)frontPanel_flowEndSession();
      frontParser_rxDisable=0;
      uart_clearRxFifo();
      return 1; //file open error... maybe the file does not exist?
   }
   
	//first the preset name
   f_read((FIL*)&preset_File,(void*)preset_currentName,8,&bytesRead);
   if(!bytesRead)
      goto closeFile;
   
	// read version number and make sure its valid
   f_read((FIL*)&preset_File,&version,1,&bytesRead);
   if(!bytesRead || version > FILE_VERSION)
      goto closeFile;
   
   preset_workingVersion = version;
   preset_showLoadingPerf();

   frontPanel_sendData(SEQ_CC,SEQ_FILE_BEGIN,WTYPE_PERFORMANCE);
   fileBeginSent=1;

   if(preset_shouldPreserveMenuEndpointsDuringFileLoad())
   {
      preset_saveMenuEndpointsDuringFileLoad();
      menuEndpointsSaved=1;
   }
   
   if(preset_workingVoiceArray>=0x3f) // all voices - load perf metadata too
   {
   // read bpm
      f_read((FIL*)&preset_File,&parameter_values[PAR_BPM],1,&bytesRead);
      if(!bytesRead)
         goto closeFile;
   
      if(version>1) {
      // read pat reset bit
         f_read((FIL*)&preset_File,&parameter_values[PAR_BAR_RESET_MODE],1,&bytesRead);
      // read seq time bit
         f_read((FIL*)&preset_File,&parameter_values[PAR_SEQ_PC_TIME],1,&bytesRead);
         if(!bytesRead)
            goto closeFile;
      }
       
      if(preset_workingVersion>4) {
         uint8_t noteData;
      // versions 5+ store MIDI channel and note for the voices
      
      // 7 channels for the 7 tracks are stored first
         for(trkNum=0;trkNum<NUM_TRACKS;trkNum++)
         {
            {
               if(trkNum==6)// nb, chan7 is non-contiguous parameter
               {
                  f_read((FIL*)&preset_File,&noteData,1,&bytesRead);
                  if(bytesRead&&(noteData!=0xff))
                     parameter_values[PAR_MIDI_CHAN_7]=noteData;
               }
               else
               {
                  f_read((FIL*)&preset_File,&noteData,1,&bytesRead);
                  if(bytesRead&&(noteData!=0xff))
                     parameter_values[PAR_MIDI_CHAN_1+trkNum]=noteData;
               }
            }
         }
      
      // 7 note settings are stored second
         for(trkNum=0;trkNum<NUM_TRACKS;trkNum++)
         {
            {
               f_read((FIL*)&preset_File,&noteData,1,&bytesRead);
               if(bytesRead&&(noteData!=0xff))
                  parameter_values[PAR_MIDI_NOTE1+trkNum]=noteData;
            }
         }
      }
   
   // send global params
      menu_sendAllGlobals();
       
   }
   

	//close the file handle
   f_close((FIL*)&preset_File);
   
   preset_showLoadingPerf();
   
   frontPanel_sendData(SEQ_CC,SEQ_EUKLID_RESET,0x01);
   
   // bc - NB: if enabled, this will lock the track (bit reg. seq_tracksLocked)
   // 'locking' the track takes place only in the mainboard, in the case
   // of loading mainstep data, it is only relevant if 'load fast' mode is on
   // in which case the 'lock track' flag is set if the mainboard receives main
   // step data for the currently playing sequence. it is used in triggervoice
   // to prevent the voice from playing (return from function)
   preset_readPatternMainStep();
   
   // nb there is a sysex callback after mainstep, mainboard is caught up
   
   if(preset_workingVoiceArray>=0x3f)
   {
      preset_readShuffle();
   }
      
   preset_readPatternLength();
   preset_readPatternScale();
   
   if(preset_workingVoiceArray>=0x3f)
   {
      preset_readPatternChain();
   }
   
   preset_readKitToTemp(1);
   preset_readKitToTemp(0);

   for (trkNum=0;trkNum<NUM_TRACKS;trkNum++)
   {
      if(voiceArray&(0x01<<trkNum))
      {
         if(trkNum<6)
         {
            frontPanel_sendData(SEQ_CC,SEQ_LOAD_VOICE,trkNum);
            preset_readDrumVoice(trkNum, 1);
            preset_readDrumVoice(trkNum, 0);
         }

         if(trkNum<5)
            frontPanel_sendData(SEQ_CC,SEQ_UNHOLD_VOICE,trkNum);
      }
   }

   frontPanel_sendData(SEQ_CC,SEQ_UNHOLD_VOICE,5);
   
   if( (voiceArray>=0x7f) || (voiceArray==0) )
   {
      preset_readDrumsetMeta(0);
      preset_readDrumsetMeta(1);
   }

   preset_dumpEndpointsToStm(SEQ_TMP_KIT_ENDPOINT_BOTH);

   /* TEMP PATTERN: forward loaded .prf endpoints to STM normal storage, then
      restore AVR menu endpoint arrays for the currently-playing temporary kit. */
   if(menuEndpointsSaved)
   {
      preset_restoreMenuEndpointsDuringFileLoad();
      menuEndpointsSaved=0;
   }
   
   for (trkNum=0;trkNum<NUM_TRACKS;trkNum++)
   {
      if(preset_workingVoiceArray&(0x01<<trkNum))
      {
         for (patNum=0;patNum<NUM_PATTERN;patNum++)
         {
         
            preset_readPatternStepData(trkNum,patNum); 
            
         }
      }
   }
   
   frontPanel_sendData(SEQ_CC,SEQ_FILE_DONE,WTYPE_PERFORMANCE);
   fileDoneSent=1;
   
#else
	// frontPanel_sendData(PRESET,PRESET_LOAD,presetNr);
#endif

closeFile:
   if(menuEndpointsSaved)
   {
      preset_restoreMenuEndpointsDuringFileLoad();
      menuEndpointsSaved=0;
   }

	//close the file handle
   f_close((FIL*)&preset_File);

   if(fileBeginSent && !fileDoneSent)
      frontPanel_sendData(SEQ_CC,SEQ_FILE_DONE,WTYPE_PERFORMANCE);
   
   if(flowSessionStarted)
      flowSessionOk = frontPanel_flowEndSession();
   frontParser_rxDisable=0;
   uart_clearRxFifo();
   
   return flowSessionOk ? 0 : 1;
}

//----------------------------------------------------
static void preset_makeFileName(char *buf, uint8_t num, uint8_t type)
{
   const char *ext;
   buf[0]='p';
   numtostrpu(&buf[1],num,'0');
   switch(type) {
      case 0:
         ext=PSTR(".snd");
         break;
      case 1:
         ext=PSTR(".pat");
         break;
      case 2:
         ext=PSTR(".all");
         break;
      default:
      case 3:
         ext=PSTR(".prf");
         break;
   }
   strcpy_P(&buf[4],ext);
}
