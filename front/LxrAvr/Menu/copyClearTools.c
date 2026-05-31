/*
 * copyClearTools.c
 *
 * Created: 26.01.2013 08:45:55
 *  Author: Julian
 */ 
#include "copyClearTools.h"
#include "../frontPanelParser.h"
#include "../buttonHandler.h"
#include <avr/io.h>
#include "../Hardware/lcd.h"
#include "../ledHandler.h"
#include "menu.h"
#include "../Preset/PresetManager.h"

uint8_t copyClear_Mode = MODE_NONE;
#define SRC_DST_NONE -1
int8_t buttonHandler_copySrc = SRC_DST_NONE;
int8_t buttonHandler_copyDst = SRC_DST_NONE;
static uint8_t copyClear_clearTarget = CLEAR_TRACK;

//-----------------------------------------------------------------------------
uint8_t copyClear_getCopyMode()
{
   return copyClear_Mode;
}
//-----------------------------------------------------------------------------
void copyClear_clearTrackAutom(uint8_t automTrack)
{
   uint8_t voice = menu_getActiveVoice();
   frontPanel_sendData(SEQ_CC,SEQ_CLEAR_AUTOM,(uint8_t)((voice<<4)|(automTrack&0x0f)));
};
//-----------------------------------------------------------------------------
void copyClear_clearCurrentPattern()
{
   uint8_t pattern = menu_getViewedPattern();
   led_clearSequencerLeds();
   frontPanel_sendData(SEQ_CC,SEQ_CLEAR_PATTERN,pattern);
};
//-----------------------------------------------------------------------------
void copyClear_executeClear()
{
   switch(copyClear_clearTarget)
   {
      default:
      case CLEAR_TRACK:
         copyClear_clearCurrentTrack();
         break;
   	
      case CLEAR_PATTERN:
         copyClear_clearCurrentPattern();
         break;
   	
      case CLEAR_AUTOMATION1:
         copyClear_clearTrackAutom(0);
         break;
   	
      case CLEAR_AUTOMATION2:
         copyClear_clearTrackAutom(1);
         break;
   }
	
   copyClear_armClearMenu(0);
   copyClear_Mode = MODE_NONE;
};
//-----------------------------------------------------------------------------
void copyClear_reset()
{
   copyClear_Mode = MODE_NONE;
   led_clearAllBlinkLeds();
   led_clearSelectLeds();
   buttonHandler_copySrc = buttonHandler_copyDst = SRC_DST_NONE;
   menu_repaintAll();
	
};
//-----------------------------------------------------------------------------
void copyClear_setSrc(int8_t src, uint8_t type)
{
   buttonHandler_copySrc = src;
   copyClear_Mode = type;
};
//-----------------------------------------------------------------------------
int8_t copyClear_getSrc()
{
   return buttonHandler_copySrc;
};
//-----------------------------------------------------------------------------
void copyClear_setDst(int8_t dst, uint8_t type)
{
   buttonHandler_copyDst = dst;
   copyClear_Mode = type;
};
//-----------------------------------------------------------------------------
uint8_t copyClear_srcSet()
{
   return buttonHandler_copySrc != SRC_DST_NONE;
}
//-----------------------------------------------------------------------------
void copyClear_clearCurrentTrack()
{
   uint8_t voice = menu_getActiveVoice();
   led_clearSequencerLeds();
   frontPanel_sendData(SEQ_CC,SEQ_CLEAR_TRACK,voice);
	
};
//-----------------------------------------------------------------------------
void copyClear_copyTrack()
{
   if(copyClear_Mode != MODE_COPY_TRACK)
   {
      return;
   }
   uint8_t value = (uint8_t)(((buttonHandler_copySrc&0xf)<<4) | (buttonHandler_copyDst&0xf));
   led_clearSequencerLeds();
   frontPanel_sendData(SEQ_CC,SEQ_COPY_TRACK,value);
	
   buttonHandler_copySrc = buttonHandler_copyDst = SRC_DST_NONE;
};
//-----------------------------------------------------------------------------
void copyClear_copyPattern()
{
   if(copyClear_Mode != MODE_COPY_PATTERN)
   {
      return;
   }

   /* RESTORE: If we are copying to the temporary sequence slot, first dump the current
      AVR raw menu endpoints to the STM so it can populate its symmetric kit structures. */
   if (buttonHandler_copyDst == SEQ_TMP_PATTERN)
   {
      preset_dumpNormalEndpointsToStm();
   }

   uint8_t value = (uint8_t)(((buttonHandler_copySrc&0xf)<<4) | (buttonHandler_copyDst&0xf));
   led_clearSequencerLeds();
   frontPanel_sendData(SEQ_CC,SEQ_COPY_PATTERN,value);

   buttonHandler_copySrc = buttonHandler_copyDst = SRC_DST_NONE;
};

//-----------------------------------------------------------------------------
void copyClear_copyTrackPattern()
{
   if(copyClear_Mode != MODE_COPY_TRACK_PATTERN)
   {
      return;
   }
   uint8_t value = (uint8_t)(((buttonHandler_copySrc&0xf)<<4) | (buttonHandler_copyDst&0xf));
   led_clearSequencerLeds();
   frontPanel_sendData(SEQ_CC,SEQ_COPY_TRACK_PATTERN,value);
	
   buttonHandler_copySrc = buttonHandler_copyDst = SRC_DST_NONE;
};
//-----------------------------------------------------------------------------
void copyClear_copyStep()
{

   uint8_t srcStep = (uint8_t)(buttonHandler_copySrc);
   uint8_t dstStep = (uint8_t)(buttonHandler_copyDst);
   uint8_t i;
   
   //led_clearSequencerLeds();
   // send substep copy commands
   for (i=0;i<8;i++)
   {
      frontPanel_sendData(SEQ_CC,SEQ_COPY_STEP_SET_SRC,(uint8_t)(srcStep+i));
      frontPanel_sendData(SEQ_CC,SEQ_COPY_STEP_SET_DST,(uint8_t)(dstStep+i));
      
   }
   // turn on destination main step
   uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
   uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
   uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
   frontPanel_sendData(MAIN_STEP_CC, value, (uint8_t)( (buttonHandler_copyDst / 8) | (0x40) ) );
   
   led_clearAllBlinkLeds();
	copyClear_Mode = MODE_NONE;
   buttonHandler_copySrc = buttonHandler_copyDst = SRC_DST_NONE;
   led_clearSelectLeds();
};
//-----------------------------------------------------------------------------
void copyClear_copySubStep()
{

   //led_clearSequencerLeds();
   frontPanel_sendData(SEQ_CC,SEQ_COPY_STEP_SET_SRC,(uint8_t)buttonHandler_copySrc);
   frontPanel_sendData(SEQ_CC,SEQ_COPY_STEP_SET_DST,(uint8_t)buttonHandler_copyDst);
   
   // turn on destination main step
   uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
   uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
   uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
   frontPanel_sendData(MAIN_STEP_CC, value, (uint8_t)( (buttonHandler_copyDst / 8) | (0x40) ) );
   
   led_clearAllBlinkLeds();
   copyClear_Mode = MODE_NONE;
   buttonHandler_copySrc = buttonHandler_copyDst = SRC_DST_NONE;
   led_clearSelectLeds();
};
//-----------------------------------------------------------------------------
uint8_t copyClear_isClearModeActive() 
{
   return (copyClear_Mode == MODE_CLEAR);
}
//-----------------------------------------------------------------------------
uint8_t copyClear_getClearTarget() 
{
   return copyClear_clearTarget;
}	
//-----------------------------------------------------------------------------
void copyClear_setClearTarget(uint8_t mode)
{
   copyClear_clearTarget = mode;
   if(copyClear_Mode == MODE_CLEAR)
   {
   	//repaint
      copyClear_armClearMenu(1);
   }
}
//-----------------------------------------------------------------------------
void copyClear_armClearMenu(uint8_t isShown)
{
   if(isShown)
   {
   	//TODO this wastes RAM!!!
      lcd_clear();
      lcd_setcursor(0,1);
      lcd_string("clear [");
   	
      switch(copyClear_clearTarget)
      {
         default:
         case CLEAR_TRACK:
            lcd_string("track");
            break;
      	
         case CLEAR_PATTERN:
            lcd_string("pattern");
            break;
      	
         case CLEAR_AUTOMATION1:
            lcd_string("autom.1");
            break;
      	
         case CLEAR_AUTOMATION2:
            lcd_string("autom.2");
            break;
      }
      lcd_string("]?");
      led_clearAllBlinkLeds();
      led_setValue(1,LED_COPY);
   
   	
   }
   else
   {
      led_setValue(0,LED_COPY);
      menu_repaintAll();
   }
};
//-----------------------------------------------------------------------------
