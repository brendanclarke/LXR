/*
 * buttonHandler.c
 *
 * Created: 24.04.2012 16:04:29
 *  Author: Julian
 */
#include "buttonHandler.h"
#include "Menu/menu.h"
#include "Preset/PresetManager.h"
#include "ledHandler.h"
#include "Hardware/lcd.h"
#include <stdio.h>
#include "frontPanelParser.h"
#include "IO/din.h"
#include "Menu/screensaver.h"
#include "Menu/copyClearTools.h"
#include "Hardware/timebase.h"
#include "config.h"
#include "front.h"
#include <util/delay.h>
//volatile uint8_t buttonHandler_selectButtonMode; 

//used to remember the last active page when entering the sequencer mode
//needed to return to the previously shown page when seq mode button is released
static uint8_t lastActivePage = 0;
static uint8_t lastActiveSubPage = 0;
/** selected step for sequencer mode*/
uint8_t buttonHandler_selectedStep = 0; //TODO ist das gleiche wie der parameter PAR_ACTIVE_STEP

static uint16_t buttonHandler_buttonTimer = 0;

#define TIMER_ACTION_OCCURED -2
static int8_t buttonHandler_buttonTimerStepNr = NO_STEP_SELECTED;
uint16_t buttonHandler_originalParameter = 0;//saves parameter number for step automation reset (stgep assign)
uint8_t buttonHandler_originalValue = 0;//saves the parameter value for reset
uint8_t buttonHandler_resetLock = 0;
uint8_t buttonHandler_heldVoiceButtons = 0;
uint8_t buttonHandler_muteTag=0;
uint8_t buttonHandler_heldSeqLoopButton = 0;
uint8_t buttonHandler_seqLoopLength=0;

uint8_t buttonHandler_recDown=0;
uint8_t buttonHandler_recArmed=0;

uint8_t shiftMode=0;
uint8_t shiftState=0;

uint8_t subStepCopy_mainStep=0;

volatile struct {
   unsigned selectButtonMode :3; // 0-3 for unshifted, 4-7 for shifted
   unsigned seqRunning :1;
   unsigned seqRecording :1;
   unsigned seqErasing :1; //--AS **RECORD

} buttonHandler_stateMemory;

static uint8_t buttonHandler_mutedVoices = 0;
// static uint8_t buttonHandler_mutesFromSolo = 0;
static int8_t buttonHandler_armedAutomationStep = NO_STEP_SELECTED;

void buttonHandler_copySubStep(uint8_t selectButtonPressed);
void buttonHandler_copyStep(uint8_t seqButtonPressed);
void buttonHandler_enterSeqMode();
void buttonHandler_leaveSeqMode();
void buttonHandler_handleShift(uint8_t shift);

#define ARM_AUTOMATION		0x40
#define DISARM_AUTOMATION	0x00
#define TMP_PATTERN_SEQ_BUTTON 15

static uint8_t buttonHandler_patternLed(uint8_t patternNr)
{
   return (uint8_t)((patternNr == SEQ_TMP_PATTERN) ? LED_STEP16 : (LED_PART_SELECT1 + patternNr));
}

static void buttonHandler_queryShownPattern()
{
   uint8_t trackNr = menu_getActiveVoice();
   uint8_t patternNr = menu_getViewedPattern();
   uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
   frontPanel_sendData(LED_CC, LED_QUERY_SEQ_TRACK, value);
   frontPanel_sendData(SEQ_CC, SEQ_REQUEST_PATTERN_PARAMS, patternNr);
   frontPanel_sendData(SEQ_CC, SEQ_REQUEST_EUKLID_PARAMS, menu_activePage);
}

static void buttonHandler_selectTmpPatternForEdit()
{
   menu_setShownPattern(SEQ_TMP_PATTERN);
   led_clearSelectLeds();
   led_clearAllBlinkLeds();
   led_setBlinkLed(LED_STEP16, 1);
   buttonHandler_queryShownPattern();
}

static void buttonHandler_changeTmpPattern()
{
   if(buttonHandler_heldVoiceButtons)
   {
      uint8_t i;
      for(i=0;i<7;i++)
      {
         if(buttonHandler_heldVoiceButtons&(0x01<<i))
            frontPanel_sendData(SEQ_CC, SEQ_CHANGE_TMP_PAT, (uint8_t)(i<<3));
      }
      buttonHandler_muteTag=0;
   }
   else
   {
      frontPanel_sendData(SEQ_CC, SEQ_CHANGE_TMP_PAT, 0x78);
      led_clearAllBlinkLeds();
      led_setBlinkLed(LED_STEP16, 1);
   }
}


//--------------------------------------------------------
void buttonHandler_copySubStep(uint8_t selectButtonPressed)
{
      //led_setSubStepLeds(0xff);
   if (copyClear_Mode == MODE_COPY_STEP)
   {  // user pressed copy - then seq step, then select button:  
         // switch source to sub-step
      copyClear_Mode = MODE_COPY_SUB_STEP;
         // get main source step and add source substep position
      int8_t subStepSource = (int8_t)(subStepCopy_mainStep*8+selectButtonPressed);
         // send source to copyClear agent
      copyClear_setSrc((int8_t)(subStepSource), MODE_COPY_SUB_STEP);
         // blink the selected substep
      led_setBlinkLed((uint8_t) (LED_PART_SELECT1 + selectButtonPressed), 1);
   }
      // this is the second time around copySubStep gets called - we have a
      // valid main step, add to main step and transmit
   else if (copyClear_Mode == MODE_COPY_SUB_STEP&&copyClear_srcSet())
   {
         // if user selected a new main step, this will update the destination,
         // otherwise same main step is used
      int8_t dst = (int8_t)(subStepCopy_mainStep*8+selectButtonPressed);
         // set the destination
      copyClear_setDst(dst, MODE_COPY_SUB_STEP);
         // execture copy
      copyClear_copySubStep();
   }

}

//--------------------------------------------------------
void buttonHandler_copyStep(uint8_t seqButtonPressed)
{     
   if (copyClear_srcSet()) 
   { // if we have already selected a source, do the copy operation
       	
         // select dest
      copyClear_setDst((int8_t)(seqButtonPressed*8), MODE_COPY_STEP);
         // execute - actually sends 8 'substep copy' operations to main
      copyClear_copyStep();
   }
   else
   {
         // first seq button pressed - set the source 
      copyClear_setSrc((int8_t)(seqButtonPressed*8), MODE_COPY_STEP);
      frontPanel_updatePatternLeds();
      led_setBlinkLed((uint8_t) (LED_STEP1 + seqButtonPressed), 1);
   
   }
}

//--------------------------------------------------------
static void buttonHandler_armTimerActionStep(int8_t stepNr) {
	//check if sub or main step
   if (buttonHandler_buttonTimerStepNr == BUT_REC)
      return;
   const uint8_t isMainStep = ((stepNr % 8) == 0);
   buttonHandler_armedAutomationStep = stepNr;

   if (isMainStep) {
      const uint8_t mainStepNr = (uint8_t) (stepNr / 8);
      led_setBlinkLed((uint8_t) (LED_STEP1 + mainStepNr), 1);
   } 
   else {
      const uint8_t selectButtonNr = (uint8_t) (stepNr % 8);
      led_setBlinkLed((uint8_t) (LED_PART_SELECT1 + selectButtonNr), 1);
   }

   frontPanel_sendData(ARM_AUTOMATION_STEP, (uint8_t) (stepNr),
      	menu_getActiveVoice() | ARM_AUTOMATION);

}
//--------------------------------------------------------
static void buttonHandler_disarmTimerActionStep() 
{
   if (buttonHandler_buttonTimerStepNr == BUT_REC)
   {
      buttonHandler_armedAutomationStep = NO_STEP_SELECTED;
      buttonHandler_buttonTimerStepNr = NO_STEP_SELECTED;
   }
   else if (buttonHandler_armedAutomationStep != NO_STEP_SELECTED) 
   {  
      
      const uint8_t isMainStep =
      		((buttonHandler_armedAutomationStep % 8) == 0);
      if (isMainStep) {
         const uint8_t mainStepNr =
         		(uint8_t) (buttonHandler_armedAutomationStep / 8);
         led_setBlinkLed((uint8_t) (LED_STEP1 + mainStepNr), 0);
      } 
      else {
         const uint8_t selectButtonNr =
         		(uint8_t) (buttonHandler_armedAutomationStep % 8);
         led_setBlinkLed((uint8_t) (LED_PART_SELECT1 + selectButtonNr), 0);
      }
   
   	//revert to original sound
   	//make changes temporary while an automation step is armed - revert to original value
      if (buttonHandler_resetLock == 1) {
         parameter_values[buttonHandler_originalParameter] =
            	buttonHandler_originalValue;
      }
   
      buttonHandler_armedAutomationStep = NO_STEP_SELECTED;
      frontPanel_sendData(ARM_AUTOMATION_STEP, 0, DISARM_AUTOMATION);
   
      if (buttonHandler_resetLock == 1) {
         buttonHandler_resetLock = 0;
      	//&revert to original value
         if (buttonHandler_originalParameter < 128) // => Sound Parameter
         {
            frontPanel_sendData(MIDI_CC,
               	(uint8_t) (buttonHandler_originalParameter),
               	buttonHandler_originalValue);
         } 
         else if (buttonHandler_originalParameter >= 128
         		&& (buttonHandler_originalParameter
         				< END_OF_SOUND_PARAMETERS)) // => Sound Parameter above 127
         {
            frontPanel_sendData(CC_2,
               	(uint8_t) (buttonHandler_originalParameter - 128),
               	buttonHandler_originalValue);
         } 
         else {
            menu_parseGlobalParam(buttonHandler_originalParameter,
               	parameter_values[buttonHandler_originalParameter]);
         }
         menu_repaintAll();
      }
      return;
     
   }// end if no step selected

   buttonHandler_armedAutomationStep = NO_STEP_SELECTED;
   frontPanel_sendData(ARM_AUTOMATION_STEP, 0, DISARM_AUTOMATION);

}
;
//--------------------------------------------------------
int8_t buttonHandler_getArmedAutomationStep() {
   return buttonHandler_armedAutomationStep;
}
;
//--------------------------------------------------------
static uint8_t buttonHandler_TimerActionOccured() {
   buttonHandler_disarmTimerActionStep();
   if (buttonHandler_buttonTimerStepNr == TIMER_ACTION_OCCURED) //NO_STEP_SELECTED)
   {
   	//a timed action apeared -> do nothing
      return 1;
   }
   buttonHandler_buttonTimerStepNr = NO_STEP_SELECTED;
   return 0;
}
//--------------------------------------------------------
static void buttonHandler_setTimeraction(uint8_t buttonNr) {
   buttonHandler_buttonTimer = time_sysTick + BUTTON_TIMEOUT;
   buttonHandler_buttonTimerStepNr = (int8_t) buttonNr;

}
//--------------------------------------------------------
/**returns 1 is the mode 2 select button is pressed*/
/*
 uint8_t buttonHandler_isModeButtonPressed()
 {
 const uint8_t arrayPos	= BUT_MODE/8;
 const uint8_t bitPos	= BUT_MODE&7;
 if(din_inputData[arrayPos] & (1<<bitPos))
 {
 return 0;
 }
 return 1;
 }
 */
//--------------------------------------------------------
//periodically called handler for timed actions
//-> hold a step button for a long time to select/rec automation
void buttonHandler_tick() 
{
   if ((time_sysTick > buttonHandler_buttonTimer)) {
      if (buttonHandler_buttonTimerStepNr == BUT_REC&&buttonHandler_recDown)
      {
         buttonHandler_recArmed=1;
         led_setBlinkLed(LED_REC, 1);
      }
      else if (buttonHandler_buttonTimerStepNr >= 0) //!=NO_STEP_SELECTED)
      {
      	//select step
         buttonHandler_armTimerActionStep(buttonHandler_buttonTimerStepNr);
      	//reset
         buttonHandler_buttonTimerStepNr = TIMER_ACTION_OCCURED; //NO_STEP_SELECTED;
      }
   }
}
//--------------------------------------------------------
/**returns 1 is the shift button is pressed*/
uint8_t buttonHandler_getShift() {
   const uint8_t arrayPos = BUT_SHIFT / 8;
   const uint8_t bitPos = BUT_SHIFT & 7;

   if (din_inputData[arrayPos] & (1 << bitPos)) {
      return 0;
   }
   return 1;
}
//--------------------------------------------------------
void buttonHandler_handleModeButtons(uint8_t mode) {
   buttonHandler_heldVoiceButtons = 0;// this is here for individ. pattern switch - if the mode
   buttonHandler_muteTag=0;           // is changed, clear any held buttons
   
   if (buttonHandler_getShift()) 
   {
   	//set the new mode
      buttonHandler_stateMemory.selectButtonMode = (uint8_t) ((mode + 4) & 0x07);
   } 
   else {
   	//set the new mode
      buttonHandler_stateMemory.selectButtonMode = (uint8_t) (mode & 0x07);
   }

   led_clearAllBlinkLeds();

	//update the status LED
   led_clearModeLeds();

   switch (buttonHandler_stateMemory.selectButtonMode) 
   {
      case SELECT_MODE_VOICE2:
         buttonHandler_stateMemory.selectButtonMode = (uint8_t) (mode & 0x07); // no voice2 mode yet
      case SELECT_MODE_VOICE:
         menu_enterVoiceMode();
         led_setValue(1,LED_MODE1);
         break;
         
      case SELECT_MODE_PERF: 
         if (menu_activePage==PERFORMANCE_PAGE)
         {
            menu_switchSubPage(0);
            menu_repaint();
         }   
         else
            menu_enterPerfMode();  
         led_setValue(1,LED_MODE2);   
         break;
      
      case SELECT_MODE_STEP2:
         menu_enterActiveStepMode();
         break;
      case SELECT_MODE_STEP:
         if (menu_activePage==SEQ_PAGE)
            menu_switchSubPage(menu_getSubPage()); //to enable toggle
         menu_enterStepMode();
         break;
         
      case SELECT_MODE_LOAD_SAVE:
         if (menu_activePage==LOAD_PAGE)
            menu_switchPage(SAVE_PAGE);
         else
            menu_switchPage(LOAD_PAGE);
         led_setValue(1,LED_MODE4);   
         break;
   
      case SELECT_MODE_MENU:
         menu_switchPage(MENU_MIDI_PAGE);
         led_setBlinkLed(LED_MODE4,1);
         break;
         
      case SELECT_MODE_PAT_GEN:
         menu_enterPatgenMode();
         break;
   
      default:
         break;
   }
}
//--------------------------------------------------------
void buttonHandler_muteVoice(uint8_t voice, uint8_t isMuted) {
   if (isMuted) {
      buttonHandler_mutedVoices |= (uint8_t)(0x01 << voice);
   
   } 
   else {
      buttonHandler_mutedVoices &= (uint8_t)~(0x01 << voice);
   
   }

	//muted tracks are lit
   if (menu_muteModeActive) {
      led_setActiveVoiceLeds((uint8_t) (~buttonHandler_mutedVoices));
   }
}
;
//--------------------------------------------------------
void buttonHandler_showMuteLEDs() {
   led_setActiveVoiceLeds((uint8_t) (~buttonHandler_mutedVoices));
   menu_muteModeActive = 1;
}
   //--------------------------------------------------------
static void buttonHandler_handleSelectButton(uint8_t buttonNr) {

   if (buttonHandler_getShift()) {
   
      switch (buttonHandler_stateMemory.selectButtonMode) {
         case SELECT_MODE_STEP2:
            break;
         case SELECT_MODE_PAT_GEN:
            {
            //select buttons represent sub steps
            
               uint8_t stepNr = (uint8_t) (buttonHandler_selectedStep + buttonNr);
            //uint8_t ledNr = LED_PART_SELECT1 + buttonNr;
            
            //request step parameters from sequencer
               frontPanel_sendData(SEQ_CC, SEQ_REQUEST_STEP_PARAMS, stepNr);
               
               if (parameter_values[PAR_ACTIVE_STEP]==stepNr)
                  menu_switchSubPage(menu_getSubPage()); //to enable toggle
               else       
                  parameter_values[PAR_ACTIVE_STEP] = stepNr;
            
            //buttonHandler_armTimerActionStep(stepNr);
               menu_resetBlinkLeds();
            //re set the main step led
               led_setBlinkLed((uint8_t) (LED_STEP1 + (stepNr / 8)), 1);
            
               const uint8_t selectButtonNr = stepNr % 8;
               led_setBlinkLed((uint8_t) (LED_PART_SELECT1 + selectButtonNr), 1);
            
            }
            break;
         case SELECT_MODE_STEP:
         case SELECT_MODE_VOICE:
            {
            //select buttons represent sub steps
            
               uint8_t stepNr = (uint8_t) (buttonHandler_selectedStep + buttonNr);
               uint8_t ledNr = (uint8_t) (LED_PART_SELECT1 + buttonNr);
            //toggle the led
               led_toggle(ledNr);
            //TODO lastActivePage zeigt nur auf den currentTrack wenn man im stepMode ist... tut nicht in den anderen modes :-/
            //set sequencer step on soundchip
            
               uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
               uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
               uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
               frontPanel_sendData(STEP_CC, value, stepNr);
            
            //request step parameters from sequencer
               frontPanel_sendData(SEQ_CC, SEQ_REQUEST_STEP_PARAMS, stepNr);
            
               parameter_values[PAR_ACTIVE_STEP] = stepNr;
            }
            break;
         case SELECT_MODE_PERF:
         //select shownPattern
            menu_setShownPattern(buttonNr);
            led_clearSelectLeds();
            led_clearAllBlinkLeds();
            led_setBlinkLed((uint8_t) (LED_PART_SELECT1 + buttonNr), 1);
         //led_clearSequencerLeds();
         //query current sequencer step states and light up the corresponding leds
            uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
            uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
            uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
            frontPanel_sendData(LED_CC, LED_QUERY_SEQ_TRACK, value);
            frontPanel_sendData(SEQ_CC, SEQ_REQUEST_PATTERN_PARAMS, patternNr);
            frontPanel_sendData(SEQ_CC, SEQ_REQUEST_EUKLID_PARAMS, menu_activePage);
            break;
      }
   } 
   else {
      switch (buttonHandler_stateMemory.selectButtonMode) {
         
         case SELECT_MODE_STEP2:
            break;
            
         case SELECT_MODE_PAT_GEN:
            {
            //select buttons represent sub steps
            
               uint8_t stepNr = (uint8_t) (buttonHandler_selectedStep + buttonNr);
               uint8_t ledNr = (uint8_t) (LED_PART_SELECT1 + buttonNr);
            //toggle the led
               led_toggle(ledNr);
            //TODO lastActivePage zeigt nur auf den currentTrack wenn man im stepMode ist... tut nicht in den anderen modes :-/
            //set sequencer step on soundchip
            
               uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
               uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
               uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
               frontPanel_sendData(STEP_CC, value, stepNr);
            
            //request step parameters from sequencer
               frontPanel_sendData(SEQ_CC, SEQ_REQUEST_STEP_PARAMS, stepNr);
            
               parameter_values[PAR_ACTIVE_STEP] = stepNr;
            }
            break;
         case SELECT_MODE_STEP: 
            {
            //select buttons represent sub steps
            
               uint8_t stepNr = (uint8_t) (buttonHandler_selectedStep + buttonNr);
            //uint8_t ledNr = LED_PART_SELECT1 + buttonNr;
            
            //request step parameters from sequencer
               frontPanel_sendData(SEQ_CC, SEQ_REQUEST_STEP_PARAMS, stepNr);
               
               if (parameter_values[PAR_ACTIVE_STEP]==stepNr)
                  menu_switchSubPage(menu_getSubPage()); //to enable toggle
               else       
                  parameter_values[PAR_ACTIVE_STEP] = stepNr;
            
            //buttonHandler_armTimerActionStep(stepNr);
               led_clearAllBlinkLeds();
            //re set the main step led
               led_setBlinkLed((uint8_t) (LED_STEP1 + (stepNr / 8)), 1);
            
               const uint8_t selectButtonNr = stepNr % 8;
               led_setBlinkLed((uint8_t) (LED_PART_SELECT1 + selectButtonNr), 1);
            
            }
            break;
      
         case SELECT_MODE_VOICE:
            {
            //change sub page -> osc, filter, mod etc...
               menu_switchSubPage(buttonNr);
            //go to 1st parameter on sub page
               menu_resetActiveParameter();
               led_setActiveSelectButton(buttonNr);
               menu_repaintAll();
            }
            break;
      
         case SELECT_MODE_PERF:
         
         //check if euklid or perf mode is active
            if (menu_getActivePage() == PERFORMANCE_PAGE) {
            //todo
            //change pattern
            //if shift -> select pattern for edit
            
               if (buttonHandler_getShift()) {
               
               } 
               else 
               {
               //individual pattern
                  if(buttonHandler_heldVoiceButtons)
                  {
                     uint8_t i;
                     for(i=0;i<7;i++)
                     {
                        if(buttonHandler_heldVoiceButtons&(0x01<<i))
                           frontPanel_sendData(SEQ_CC, SEQ_CHANGE_PAT, (uint8_t)( (i<<3)|buttonNr) );
                     }   
                     buttonHandler_muteTag=0;
                  }
                  else
                  {
                     //tell sequencer to change pattern
                     frontPanel_sendData(SEQ_CC, SEQ_CHANGE_PAT, (0x78|buttonNr) );
                     //flash corresponding LED until ACK (SEQ_CHANGE_PAT) received
                     uint8_t ledNr = (uint8_t) (LED_PART_SELECT1 + buttonNr);
                     led_clearAllBlinkLeds();
                  
                     led_setBlinkLed(ledNr, 1);
                  
                     //request the pattern info for the selected pattern (bar cnt, next...)
                     //	frontPanel_sendData(SEQ_CC,SEQ_REQUEST_PATTERN_PARAMS,buttonNr);
                  }
               
               }
            
            }
            //----- Euklid Mode -----
            else {
            /* //moved to voice button
             //tell the sequencer the new active track
             //TODO muss nicht f�r button 8 gesendet werden
             frontPanel_sendData(SEQ_CC,SEQ_SET_ACTIVE_TRACK,buttonNr);
             //request the parameters for the new track
             frontPanel_sendData(SEQ_CC,SEQ_REQUEST_EUKLID_PARAMS,buttonNr);
             //the currently active track button is lit
             led_setActivePage(buttonNr);
             */
            
            }
            break;
      
         case SELECT_MODE_LOAD_SAVE:
         //the currently active button is lit
            led_setActivePage(buttonNr);
         
         //	menu_switchPage(LFO1_PAGE+buttonNr);
            break;
      }
   }

}

//--------------------------------------------------------
// when in voice mode you press shift, you enter sequencer mode
void buttonHandler_enterSeqMode() {
   lastActivePage = menu_activePage;
   lastActiveSubPage = menu_getSubPage();
   menu_switchSubPage(0);
   menu_switchPage(SEQ_PAGE);

   frontPanel_updatePatternLeds();

   led_setBlinkLed(menu_selectedStepLed, 1);
}
//--------------------------------------------------------
void buttonHandler_leaveSeqMode() {
	//stop blinking active step led
   led_setBlinkLed(menu_selectedStepLed, 0);
   led_setValue(0, menu_selectedStepLed);

	//reset select leds
   led_clearSelectLeds();

   menu_switchPage(lastActivePage);
   menu_switchSubPage(lastActiveSubPage);

}
//--------------------------------------------------------
//uint8_t buttonHandler_getMutedVoices() {
//	return buttonHandler_mutedVoices;
//}
//--------------------------------------------------------
static void buttonHandler_selectActiveStep(uint8_t ledNr, uint8_t seqButtonPressed) {

   led_setBlinkLed(menu_selectedStepLed, 0);
   led_setValue(0, menu_selectedStepLed);

   if (buttonHandler_selectedStep == (seqButtonPressed * 8)) {
      menu_switchSubPage(menu_getSubPage()); //to enable toggle
   }

	//update active step
   buttonHandler_selectedStep = (uint8_t) (seqButtonPressed * 8);
   menu_selectedStepLed = ledNr;

   parameter_values[PAR_ACTIVE_STEP] = buttonHandler_selectedStep;

	//blink new step
   led_setBlinkLed(ledNr, 1);

	//update sub steps
	//request step parameters from sequencer 
   frontPanel_sendData(SEQ_CC, SEQ_REQUEST_STEP_PARAMS,
      	(uint8_t) (seqButtonPressed * 8));
}
//--------------------------------------------------------
void buttonHandler_setRemoveStep(uint8_t ledNr, uint8_t seqButtonPressed) {
	//led_toggle(ledNr); //handled by cortex
   led_setValue(0, ledNr);
	//we have 128 steps, the main buttons are only for multiples of 8
   DISABLE_CONV_WARNING
   seqButtonPressed *= 8;
   END_DISABLE_CONV_WARNING
   //which track is active
   //uint8_t currentTrack = menu_getActivePage();
   
   //update active step (so that seq mode always shows the last set step)
   buttonHandler_selectedStep = seqButtonPressed;
   parameter_values[PAR_ACTIVE_STEP] = buttonHandler_selectedStep;
   menu_selectedStepLed = ledNr;

	//request step parameters from sequencer
   frontPanel_sendData(SEQ_CC, SEQ_REQUEST_STEP_PARAMS, seqButtonPressed);

	//set sequencer step on soundchip
   uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
   uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
   uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
	//frontPanel_sendData(STEP_CC,value,seqButtonPressed);
   frontPanel_sendData(MAIN_STEP_CC, value, seqButtonPressed / 8);
}
//--------------------------------------------------------
// **PATROT set track rotation to a value between 0 and 15
// 0 means not rotated.
// **PATROT todo need to update seq leds with current rotation for this track
static void buttonHandler_setTrackRotation(uint8_t seqButtonPressed)
{
   parameter_values[PAR_TRACK_ROTATION]=seqButtonPressed;
   frontPanel_sendData(SEQ_CC, SEQ_TRACK_ROTATION, seqButtonPressed);
	// update the value right now (this is also updated in the code that handles the shift button press)
   menu_resetBlinkLeds();
   led_setBlinkLed((uint8_t) (LED_STEP1 + seqButtonPressed), 1);
   led_setBlinkLed((uint8_t) (LED_VOICE1 + menu_getActiveVoice()), 1);
   led_setBlinkLed(buttonHandler_patternLed(menu_getViewedPattern()) ,1);
}

//--------------------------------------------------------
static void buttonHandler_copyPatternButton(uint8_t patternNr)
{
   if (copyClear_Mode == MODE_COPY_TRACK)
   {
      if (copyClear_srcSet())
      {
         copyClear_setDst((int8_t)patternNr, MODE_COPY_TRACK_PATTERN);
         copyClear_copyTrackPattern();
         menu_resetBlinkLeds();
         buttonHandler_queryShownPattern();
      }
      else
      {
         copyClear_setSrc((int8_t)patternNr, MODE_COPY_PATTERN);
         led_setBlinkLed(buttonHandler_patternLed(patternNr), 1);
      }
   }
   else if (copyClear_Mode == MODE_COPY_PATTERN)
   {
      if (copyClear_srcSet())
      {
         copyClear_setDst((int8_t)patternNr, MODE_COPY_PATTERN);
         copyClear_copyPattern();
         menu_resetBlinkLeds();
         buttonHandler_queryShownPattern();
      }
      else
      {
         copyClear_setSrc((int8_t)patternNr, MODE_COPY_PATTERN);
         led_setBlinkLed(buttonHandler_patternLed(patternNr), 1);
      }
   }
}

//--------------------------------------------------------
static void buttonHandler_seqButtonPressed(uint8_t seqButtonPressed)
{
   if (copyClear_Mode>MODE_NONE)
   {
      return; // do nothing - act on seqButtonReleased
   }
   uint8_t ledNr;

   ledNr = (uint8_t) (seqButtonPressed + LED_STEP1);

   if (buttonHandler_getShift()) {
      switch(buttonHandler_stateMemory.selectButtonMode) {
         case SELECT_MODE_PAT_GEN:
            menu_resetBlinkLeds();;
            buttonHandler_selectActiveStep(ledNr, seqButtonPressed);
            frontPanel_updatePatternLeds();
            //reset sub step to 1 (== main step parameters)
            led_setBlinkLed(LED_PART_SELECT1, 1);
            break;
         case SELECT_MODE_STEP2:
            break;
         case SELECT_MODE_VOICE: //sequencer mode -> buttons select active step because shift is down
            buttonHandler_selectActiveStep(ledNr, seqButtonPressed);
            frontPanel_updatePatternLeds();
            break;
         case SELECT_MODE_STEP: // step edit mode -> adds/removes step because shift is down
            buttonHandler_setRemoveStep(ledNr, seqButtonPressed);
            frontPanel_updateSubstepLeds();
            break;
         case SELECT_MODE_PERF: // **PATROT shift is held while in perf mode and a seq button is pressed
            if(seqButtonPressed == TMP_PATTERN_SEQ_BUTTON)
            {
               buttonHandler_selectTmpPatternForEdit();
               break;
            }
            buttonHandler_setTrackRotation(seqButtonPressed);
            break;
         default:
            break;
      }
      
   } 
   else { // shift is not down
      switch (buttonHandler_stateMemory.selectButtonMode) {
         case SELECT_MODE_STEP2:
            break;
         case SELECT_MODE_PAT_GEN:
            buttonHandler_setRemoveStep(ledNr, seqButtonPressed);
            frontPanel_updateSubstepLeds();
            break;   
         case SELECT_MODE_VOICE:
            // button is held down, start a timer. If the button is held we
            // can record automation for the one button. The regular handling for
            // adding/removing step is handled in the button release
            buttonHandler_setTimeraction((uint8_t) (seqButtonPressed * 8));
            break;
         case SELECT_MODE_STEP:
            led_clearAllBlinkLeds();
            buttonHandler_selectActiveStep(ledNr, seqButtonPressed);
            frontPanel_updatePatternLeds();
            //reset sub step to 1 (== main step parameters)
            led_setBlinkLed(LED_PART_SELECT1, 1);
            break;
         case SELECT_MODE_PERF: //--- buttons 1-8 initiate a manual roll
            if(seqButtonPressed == TMP_PATTERN_SEQ_BUTTON)
            {
               buttonHandler_changeTmpPattern();
               break;
            }
            if (seqButtonPressed < 8) 
            {
            //turn roll on
               frontPanel_sendData(SEQ_CC, SEQ_ROLL_ON_OFF,(uint8_t) ((seqButtonPressed & 0xf) + 0x10));
            //turn button led on
               led_setValue(1, ledNr);
            } 
            else 
            {
               buttonHandler_heldSeqLoopButton |= (uint8_t)(0x01<<(seqButtonPressed-8));
               switch(seqButtonPressed)
               {
                  case 9:
                     buttonHandler_seqLoopLength=64;
                     break;
                  case 10:
                     buttonHandler_seqLoopLength=32;
                     break;
                  case 11:
                     buttonHandler_seqLoopLength=16;
                     break;
                  case 12:
                     buttonHandler_seqLoopLength=8;
                     break;
                  case 13:
                     buttonHandler_seqLoopLength=4;
                     break;
                  case 14:
                     buttonHandler_seqLoopLength=2;
                     break;
                  case 15:
                     buttonHandler_seqLoopLength=1;
                     break;
                  default:
                     break;
               }   
            
               if(buttonHandler_heldSeqLoopButton&0x01)
               {
                  buttonHandler_seqLoopLength=(uint8_t)
                     (buttonHandler_seqLoopLength+(buttonHandler_seqLoopLength>>1)); // dot the length of loop
               }
               if(buttonHandler_seqLoopLength)
                  frontPanel_sendData(SEQ_CC, SEQ_SET_LOOP,(uint8_t) (buttonHandler_seqLoopLength));
            }
            led_setValue(1, ledNr);
            break;
      
      	//--- unused (maybe lfo clock sync? ---
         case SELECT_MODE_LOAD_SAVE:
         default:
            break;
      }
   } // is shift being held down
}
//--------------------------------------------------------
static void buttonHandler_seqButtonReleased(uint8_t seqButtonPressed)
{
   uint8_t ledNr = (uint8_t) (seqButtonPressed + LED_STEP1);

   if ((buttonHandler_stateMemory.selectButtonMode == SELECT_MODE_PERF)
      && (seqButtonPressed == TMP_PATTERN_SEQ_BUTTON)
      && ((copyClear_Mode == MODE_COPY_PATTERN) || (copyClear_Mode == MODE_COPY_TRACK)))
   {
      buttonHandler_copyPatternButton(SEQ_TMP_PATTERN);
      return;
   }

   if (buttonHandler_getShift()) // shift click is fully handled in "pressed"
      return;

	// --AS I don't think that the timer would be armed in this case
	//{
		//do nothing if timer action occured
		//if (buttonHandler_TimerActionOccured())
			//return;

		//if ((buttonHandler_getMode() == SELECT_MODE_VOICE)) {
			//sequencer mode -> buttons select active step
			//buttonHandler_selectActiveStep(ledNr, seqButtonPressed);
		//} else if ((buttonHandler_getMode() == SELECT_MODE_STEP)) {
			//buttonHandler_setRemoveStep(ledNr,seqButtonPressed);
		//}

	//} else {

   switch (buttonHandler_stateMemory.selectButtonMode) {
      case SELECT_MODE_PAT_GEN:
      case SELECT_MODE_STEP:
         if (copyClear_Mode == MODE_COPY_SUB_STEP){
            buttonHandler_selectActiveStep(ledNr, seqButtonPressed);
            subStepCopy_mainStep=seqButtonPressed;
            led_clearSelectBlinkLeds();
            led_setBlinkLed((uint8_t)(LED_STEP1+seqButtonPressed), 1);
            frontPanel_updatePatternLeds();
         }
         else if (copyClear_Mode > MODE_CLEAR){
            buttonHandler_selectActiveStep(ledNr, seqButtonPressed);
            subStepCopy_mainStep=seqButtonPressed;
            led_setBlinkLed((uint8_t)(LED_STEP1+seqButtonPressed), 1);
            buttonHandler_copyStep(seqButtonPressed);
            frontPanel_updatePatternLeds();
         }
         //do nothing if timer action occurred - its already handled in the timer
         else if (buttonHandler_TimerActionOccured())
            return;
         break;
   	//--- edit the pattern -> button sets and removes a step ---
      case SELECT_MODE_VOICE:            
         // copy step and substep functions   
         if (copyClear_Mode == MODE_COPY_SUB_STEP){
            buttonHandler_selectActiveStep(ledNr, seqButtonPressed);
            subStepCopy_mainStep=seqButtonPressed;
            led_clearSelectBlinkLeds();
            led_setBlinkLed((uint8_t)(LED_STEP1+seqButtonPressed), 1);
            frontPanel_updatePatternLeds();
         }
         else if (copyClear_Mode > MODE_CLEAR){
            buttonHandler_selectActiveStep(ledNr, seqButtonPressed);
            subStepCopy_mainStep=seqButtonPressed;
            led_setBlinkLed((uint8_t)(LED_STEP1+seqButtonPressed), 1);
            buttonHandler_copyStep(seqButtonPressed);
            frontPanel_updatePatternLeds();
         }
         //do nothing if timer action occured
         else if (buttonHandler_TimerActionOccured())
            return;
         else{
            buttonHandler_setRemoveStep(ledNr, seqButtonPressed);
         }
      
         break;
      case SELECT_MODE_PERF:
         if (seqButtonPressed == TMP_PATTERN_SEQ_BUTTON)
         {
            return;
         }
         if (seqButtonPressed < 8) 
         {
         //turn roll off
            frontPanel_sendData(SEQ_CC, SEQ_ROLL_ON_OFF, (seqButtonPressed & 0xf));
         //turn button led off
            led_setValue(0, ledNr);
         }
         else
         {
            buttonHandler_heldSeqLoopButton &= (uint8_t)~(0x01<<(seqButtonPressed-8));
            if(seqButtonPressed==8)
            {
               buttonHandler_seqLoopLength=(uint8_t)(buttonHandler_seqLoopLength-(buttonHandler_seqLoopLength/3));
               frontPanel_sendData(SEQ_CC, SEQ_SET_LOOP,buttonHandler_seqLoopLength);
            }
            if (!buttonHandler_heldSeqLoopButton)
            {
               buttonHandler_seqLoopLength=0;
               frontPanel_sendData(SEQ_CC, SEQ_SET_LOOP,0);
            }   
            led_setValue(0, ledNr);
         }
         break;
   
   	//--- unused (maybe lfo clock sync? ---
      case SELECT_MODE_LOAD_SAVE:
         break;
   
      default:
         break;
   
   }
}

//--------------------------------------------------------
// one of the 8 part buttons is pressed
static void buttonHandler_partButtonPressed(uint8_t partNr)
{
   if (copyClear_Mode>=MODE_COPY_STEP)
   {
      buttonHandler_copySubStep(partNr);
   } 
   else if (copyClear_Mode == MODE_COPY_TRACK) 
   {
      // user has previously selected a track and now selects
      // a pattern - escape from pattern copy and instead
      // do track pattern copy
      if (copyClear_srcSet()) 
      { // if we have already selected a source, do the copy operation
      	//select dest
         copyClear_setDst((int8_t)partNr, MODE_COPY_TRACK_PATTERN);
         copyClear_copyTrackPattern();
         menu_resetBlinkLeds();
      
      	//query current sequencer step states and light up the corresponding leds
         uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
         uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
         uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
         frontPanel_sendData(LED_CC, LED_QUERY_SEQ_TRACK, value);
      } 
      else 
      {
      	//no source selected, so select src (this shouldn't happen)
         copyClear_setSrc((int8_t)partNr, MODE_COPY_PATTERN);
         led_setBlinkLed((uint8_t)(LED_PART_SELECT1 + partNr), 1);
      }
   }
   else if (copyClear_Mode >= MODE_COPY_PATTERN) 
   {
   	//copy mode
      if (copyClear_srcSet()) 
      { // if we have already selected a source, do the copy operation
      	//select dest
         copyClear_setDst((int8_t)partNr, MODE_COPY_PATTERN);
         copyClear_copyPattern();
         menu_resetBlinkLeds();
      
      	//query current sequencer step states and light up the corresponding leds
         uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
         uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
         uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
         frontPanel_sendData(LED_CC, LED_QUERY_SEQ_TRACK, value);
      } 
      else 
      {
      	//no source selected, so select src
         copyClear_setSrc((int8_t)partNr, MODE_COPY_PATTERN);
         led_setBlinkLed((uint8_t)(LED_PART_SELECT1 + partNr), 1);
      }
   } 
   else 
   { // none or clear
   	// the code that handles most of this case is in the button release
   	// if shift is held and a part button is held down, we will enter automation record mode
   	// for that sub-step on the currently selected step.
   	// if shift is not held, we just do normal handling
      if ( buttonHandler_stateMemory.selectButtonMode == SELECT_MODE_VOICE && buttonHandler_getShift()) 
      {
      	//TODO hier muss selektiert werden!
         buttonHandler_setTimeraction((uint8_t)(buttonHandler_selectedStep * 8 + partNr));
      } 
      else 
      {
         buttonHandler_handleSelectButton(partNr);
      }
   
   }

}

//--------------------------------------------------------
// one of the 8 part buttons is released
static void buttonHandler_partButtonReleased(uint8_t partNr)
{
   if (copyClear_Mode >= MODE_COPY_PATTERN) {
   // don't do anything if copying steps or sub-steps
   } 
   else {
   	//if(buttonHandler_stateMemory.selectButtonMode == SELECT_MODE_VOICE) return;
   	//do nothing if timer action occured
      if (buttonHandler_TimerActionOccured())
         return;
   
      buttonHandler_buttonTimerStepNr = NO_STEP_SELECTED;
      if (buttonHandler_getShift() && buttonHandler_stateMemory.selectButtonMode == SELECT_MODE_VOICE) {
         buttonHandler_handleSelectButton(partNr);
      }
   }
}
//--------------------------------------------------------
static void buttonHandler_voiceButtonReleased(uint8_t voiceNr)
{
   buttonHandler_heldVoiceButtons &= (uint8_t)(~(0x01<<voiceNr));
   if (buttonHandler_stateMemory.selectButtonMode == SELECT_MODE_PERF) 
   {
   
      if (buttonHandler_muteTag) 
      {
         //un/mute
         if (buttonHandler_mutedVoices & (1 << voiceNr)) 
         {
         	//unmute tracks 0-7
            buttonHandler_muteVoice(voiceNr, 0);
            frontPanel_sendData(SEQ_CC, SEQ_UNMUTE_TRACK, voiceNr);
         } 
         else 
         {
         	//mute tracks 0-7
            buttonHandler_muteVoice(voiceNr, 1);
            frontPanel_sendData(SEQ_CC, SEQ_MUTE_TRACK, voiceNr);
         }
         
      }
      
   }
   else if( (menu_activePage==LOAD_PAGE) &&
   (menu_getWhat()==SAVE_TYPE_PERFORMANCE||menu_getWhat()==SAVE_TYPE_PATTERN||
   menu_getWhat()==SAVE_TYPE_ALL) )
   {
      if(parameter_values[PAR_FILE_LOAD_FAST])
      {
      
         if(!buttonHandler_heldVoiceButtons)
         {
            preset_loadPerf(menu_currentPresetNr[SAVE_TYPE_PERFORMANCE],menu_voiceArray);
            menu_resetSaveParameters();
            buttonHandler_handleModeButtons(SELECT_MODE_PERF);
         }
      }
   }
   if(!buttonHandler_heldVoiceButtons)
   {
      buttonHandler_muteTag=0;
   }   

}
//--------------------------------------------------------
// one of the 7 voice buttons pressed
static void buttonHandler_voiceButtonPressed(uint8_t voiceNr)
{
   
   if (copyClear_Mode >= MODE_COPY_PATTERN) 
   {
   	//copy mode -> voice buttons select track copy src/dst
      if (copyClear_srcSet()) {
      	//select dest
         copyClear_setDst((int8_t)voiceNr, MODE_COPY_TRACK);
         copyClear_copyTrack();
         menu_resetBlinkLeds();
      	//query current sequencer step states and light up the corresponding leds
      
         uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
         uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
         uint8_t value = (uint8_t) ((trackNr << 4) | (patternNr & 0x0f));
         frontPanel_sendData(LED_CC, LED_QUERY_SEQ_TRACK, value);
      
      } 
      else {
      	// no source selected so select src
         copyClear_setSrc((int8_t)voiceNr, MODE_COPY_TRACK);
         led_setBlinkLed((uint8_t) (LED_VOICE1 + voiceNr), 1);
      }
   } 
   else if( (menu_activePage==LOAD_PAGE) &&
   (menu_getWhat()==SAVE_TYPE_PERFORMANCE||menu_getWhat()==SAVE_TYPE_PATTERN||
   menu_getWhat()==SAVE_TYPE_ALL) )
   {
      if(parameter_values[PAR_FILE_LOAD_FAST])
      {
         if(!buttonHandler_heldVoiceButtons)
            menu_voiceArray=0;
         menu_voiceArray |= (uint8_t)(0x01<<voiceNr);
         led_setBlinkLed((uint8_t)(LED_VOICE1+voiceNr),(uint8_t)(menu_voiceArray&(uint8_t)(0x01<<voiceNr)));
      }
      else
      {
         if(menu_voiceArray==0x7f)
         {
            menu_voiceArray = 0;
            led_clearVoiceLeds();
            led_clearAllBlinkLeds();
         }
      
         menu_voiceArray ^= (uint8_t)(0x01<<voiceNr);
         led_setBlinkLed((uint8_t)(LED_VOICE1+voiceNr),(uint8_t)(menu_voiceArray&(uint8_t)(0x01<<voiceNr)));
      
      }
   }
   else {
   	// mode none or clear
   	// determine if we are in mute/unmute mode -
   	// in perf mode we are in mute mode unless shift is held
   	// in any other mode we are in mute mode when shift is held
      uint8_t muteModeActive=0;
      uint8_t shiftMode=buttonHandler_getShift();
      if(buttonHandler_stateMemory.selectButtonMode == SELECT_MODE_PERF)
      {
         if (shiftMode) 
         {
            led_setBlinkLed((uint8_t)(LED_VOICE1+menu_getActiveVoice()),0);
            menu_setActiveVoice(voiceNr);
            menu_updateMainStepDisplay();
            frontPanel_sendData(SEQ_CC, SEQ_SET_ACTIVE_TRACK, voiceNr);
            buttonHandler_showMuteLEDs();
            menu_updateMainStepDisplay();
            led_setBlinkLed((uint8_t)(LED_VOICE1+voiceNr),1);
         }
         else
         {
            buttonHandler_muteTag=1;
         }
      }
      else // not PERF MODE
      {
         muteModeActive = shiftMode;
      
         if (muteModeActive) 
         {
         	//un/mute
            if (buttonHandler_mutedVoices & (1 << voiceNr)) {
            	//unmute tracks 0-7
               buttonHandler_muteVoice(voiceNr, 0);
               frontPanel_sendData(SEQ_CC, SEQ_UNMUTE_TRACK, voiceNr);
            } 
            else 
            {
            	//mute tracks 0-7
               buttonHandler_muteVoice(voiceNr, 1);
               frontPanel_sendData(SEQ_CC, SEQ_MUTE_TRACK, voiceNr);
            }
         
         }
         else if (buttonHandler_stateMemory.selectButtonMode == SELECT_MODE_STEP2)//activestep mode
         {
            
         }
         else
         {
            //select active voice
         	//the currently active button is lit
            led_setActiveVoice(voiceNr);
            
            frontPanel_sendData(SEQ_CC, SEQ_SET_ACTIVE_TRACK, voiceNr);
         
            menu_setActiveVoice(voiceNr);
         	//change voice page on display if in voice mode
            if (buttonHandler_stateMemory.selectButtonMode == SELECT_MODE_VOICE) 
            {
               menu_switchPage(voiceNr);
            }
            else if (buttonHandler_stateMemory.selectButtonMode == SELECT_MODE_STEP) 
            {
            	//reactivate sequencer mode
               menu_resetBlinkLeds();
               menu_switchPage(SEQ_PAGE);   
               led_setBlinkLed(menu_selectedStepLed, 1);
               frontPanel_sendData(SEQ_CC, SEQ_REQUEST_STEP_PARAMS,(uint8_t) ((menu_selectedStepLed-LED_STEP1) * 8));
               frontPanel_updatePatternLeds();
               
               menu_repaintAll();
            }
            else if (buttonHandler_stateMemory.selectButtonMode == SELECT_MODE_PAT_GEN)
            {
               menu_enterPatgenMode();
            
            }
         }
      } // not PERF MODE      
   } // not copyclear mode
   
   buttonHandler_heldVoiceButtons|=(uint8_t)(0x01<<voiceNr);
}

//--------------------------------------------------------
void buttonHandler_buttonPressed(uint8_t buttonNr) {
   
   if(frontParser_rxDisable)
      return;
      
   screensaver_touch();

   if(buttonNr < BUT_SELECT1) { // BUT_SEQ* buttons
      buttonHandler_seqButtonPressed(buttonNr);
      return;
   } 
   else if(buttonNr < BUT_VOICE_1) { // BUT_SELECT* buttons
      buttonHandler_partButtonPressed((uint8_t)(buttonNr-BUT_SELECT1));
      return;
   } 
   else if(buttonNr < BUT_COPY) { // BUT_VOICE_* buttons
      buttonHandler_voiceButtonPressed((uint8_t)(buttonNr-BUT_VOICE_1));
      return;
   }

   switch (buttonNr) {
      case BUT_MODE1:
      case BUT_MODE2:
      case BUT_MODE3:
      case BUT_MODE4:
      // voice/perf patgen/step/load save menu
         buttonHandler_handleModeButtons((uint8_t) (buttonNr - BUT_MODE1));
         break;
   
      case BUT_START_STOP:
      //Sequencer Start Stop button
      //because the output shift registers are full, this buttons LED is on a single uC pin
      //toggle the state and update led
         if (buttonHandler_getShift())
         {
            menu_reloadKit();
         }
         else
         {
            buttonHandler_setRunStopState( (uint8_t) (1 - buttonHandler_stateMemory.seqRunning));
            //send run/stop command to soundchip
            frontPanel_sendData(SEQ_CC, SEQ_RUN_STOP, buttonHandler_stateMemory.seqRunning);
            menu_sequencerRunning=buttonHandler_stateMemory.seqRunning;
         }      
         break;
   
      case BUT_REC:
         if (buttonHandler_getShift()) {
            menu_switchPage(RECORDING_PAGE);
         } 
         else {
         //toggle the led
            buttonHandler_stateMemory.seqRecording = (uint8_t) ((1
               - buttonHandler_stateMemory.seqRecording) & 0x01);
            led_setValue(buttonHandler_stateMemory.seqRecording, LED_REC);
         //send run/stop command sequencer
            frontPanel_sendData(SEQ_CC, SEQ_REC_ON_OFF,buttonHandler_stateMemory.seqRecording);
         }
         buttonHandler_setTimeraction(BUT_REC);
         buttonHandler_recDown=1;
         buttonHandler_recArmed=0;
         break;
   
      case BUT_COPY:
         if (buttonHandler_getShift()) 
         {
         //with shift -> clear mode. unless we are record active and playing. In this case,
         // holding down the copy button does a realtime erase on the active voice
            if(buttonHandler_stateMemory.seqRecording && buttonHandler_stateMemory.seqRunning) {
               buttonHandler_stateMemory.seqErasing=1;
               frontPanel_sendData(SEQ_CC, SEQ_ERASE_ON_OFF,
                  buttonHandler_stateMemory.seqErasing);
            // --AS **RECORD todo update the display
            } 
            else {
            
               if (copyClear_Mode == MODE_CLEAR) {
               //execute
                  copyClear_executeClear();
               /*
                copyClear_clearCurrentTrack();
                copyClear_armClearMenu(0);
                copyClear_Mode = MODE_NONE;
                */
               } 
               else {
                  copyClear_Mode = MODE_CLEAR;
                  copyClear_armClearMenu(1);
               }
            }
         
         } 
         else {
         //copy mode
            copyClear_Mode = MODE_COPY_TRACK;
            menu_resetBlinkLeds();
            led_setBlinkLed(LED_COPY, 1);
            led_clearSelectLeds();
            led_clearVoiceLeds();
         
         }
         break;
   
   	//the mode selection for the 16 seq buttons
      case BUT_SHIFT:
         buttonHandler_handleShift(1);
         break;
      default:
         break;
   }
}
//--------------------------------------------------------
void buttonHandler_handleShift(uint8_t isDown)
{
   if(isDown)
   {
      shiftState=!(shiftMode&shiftState);
      if (shiftState)
      {
         led_setValue(1, LED_SHIFT);
         switch(buttonHandler_stateMemory.selectButtonMode) 
         {
            case SELECT_MODE_VOICE:
            case SELECT_MODE_VOICE2:
               if ((menu_activePage<=VOICE7_PAGE)&&(editModeActive))
                  menu_repaintAll();
               else
                  menu_shiftVoice(1); // display step params and substeps while shift held
               break;
            case SELECT_MODE_PERF:
               {
                  menu_shiftPerf(1);
               }
               break;
            
            case SELECT_MODE_PAT_GEN:
               menu_shiftPatgen(1);
               break;
         
            case SELECT_MODE_STEP:
               menu_shiftStep(1);
               break;
            case SELECT_MODE_STEP2:
               menu_shiftActiveStep(1);
               break;
            default:
               break;
         }
      
      //show muted voices if pressed
         buttonHandler_showMuteLEDs();
      }
   } // end if isDown
   else
   { // button release actions
      shiftState=(shiftMode&shiftState);
      if (!shiftState)
      {
         if(buttonHandler_stateMemory.seqErasing) 
         {
         // --AS **RECORD if we are in erase mode, exit that mode
            buttonHandler_stateMemory.seqErasing=0;
            frontPanel_sendData(SEQ_CC, SEQ_ERASE_ON_OFF,
               	buttonHandler_stateMemory.seqErasing);
         }
         
         if (copyClear_Mode == MODE_CLEAR) 
         {
            copyClear_armClearMenu(0);
            copyClear_Mode = MODE_NONE;
         }
         led_setValue(0, LED_SHIFT);
         
         if (menu_activePage!=RECORDING_PAGE)
         {
            switch(buttonHandler_stateMemory.selectButtonMode) 
            {
               case SELECT_MODE_VOICE:
               case SELECT_MODE_VOICE2:
                  if ((menu_activePage<=VOICE7_PAGE)&&(editModeActive))
                     menu_repaintAll();
                  else
                     menu_shiftVoice(0);
                  break;
               case SELECT_MODE_PERF:
                  menu_shiftPerf(0);
                  break;
               case SELECT_MODE_PAT_GEN:
                  menu_shiftPatgen(0);
                  break;
               case SELECT_MODE_STEP:
                  menu_shiftStep(0);
                  break;
               case SELECT_MODE_STEP2:
                  menu_shiftActiveStep(0);
                  break;
               default:
                  break;
            }
            
            //show active voice if released
            if (buttonHandler_stateMemory.selectButtonMode != SELECT_MODE_PERF) 
            {
               led_setActiveVoice(menu_getActiveVoice());
            } 
            else 
            {
            // --AS TODO this code is never reached (see return above) ???
               buttonHandler_showMuteLEDs();
            }
         }
      } // if !shiftstate
   } // end button up actions
}
//--------------------------------------------------------
void buttonHandler_buttonReleased(uint8_t buttonNr) {

   if(frontParser_rxDisable)
      return;

   if(buttonNr < BUT_SELECT1) { // BUT_SEQ* buttons
      buttonHandler_seqButtonReleased(buttonNr);
      return;
   } 
   else if(buttonNr < BUT_VOICE_1) { // BUT_SELECT* buttons
      buttonHandler_partButtonReleased((uint8_t)(buttonNr - BUT_SELECT1));
      return;
   }
   else if(buttonNr < BUT_COPY) { // BUT_VOICE_* buttons
      buttonHandler_voiceButtonReleased((uint8_t)(buttonNr-BUT_VOICE_1));
      return;
   }

	// for the rest...
   switch (buttonNr) {
      case BUT_COPY:
         {
            if(buttonHandler_stateMemory.seqErasing) 
            {
            // --AS **RECORD if we are in erase mode, exit that mode
               buttonHandler_stateMemory.seqErasing=0;
               frontPanel_sendData(SEQ_CC, SEQ_ERASE_ON_OFF,
                  				buttonHandler_stateMemory.seqErasing);
            } 
            else if (!buttonHandler_getShift()) 
            {//copy mode abort/exit
               copyClear_reset();
               // get previous led modes after copyclear
               switch(buttonHandler_stateMemory.selectButtonMode) 
               {
                  case SELECT_MODE_VOICE:
                     if ((menu_activePage<=VOICE7_PAGE)&&(editModeActive))
                        menu_repaintAll();
                     else
                        menu_enterVoiceMode();
                     break;
                  case SELECT_MODE_PAT_GEN:
                     menu_enterPatgenMode();
                     break;
                  case SELECT_MODE_STEP:
                     menu_enterStepMode();
                     break;
                  case SELECT_MODE_STEP2:
                     menu_enterActiveStepMode();
                     break;   
                  case SELECT_MODE_MENU:
                     led_setBlinkLed(LED_MODE4, 1);
                     break;  
                  default:
                     break;
               }
            }
         }
         break;
   
      case BUT_SHIFT: // when this button is released, revert back to normal operating mode
         buttonHandler_handleShift(0);
         break;
      case BUT_REC:
         if(buttonHandler_recArmed)
         {
            // record held - write transpose action
            led_setBlinkLed(LED_REC, 0);
            frontPanel_sendData(SEQ_CC, SEQ_TRANSPOSE_ON_OFF,0x0f);    
            
            // reset the record button
            buttonHandler_stateMemory.seqRecording = 0;
            led_setValue(0, LED_REC);
            //send run/stop command sequencer
            frontPanel_sendData(SEQ_CC, SEQ_REC_ON_OFF,0);  
         }
         buttonHandler_recDown=0;  
         buttonHandler_recArmed=0; 
      default:
         break;
   }
}

//--------------------------------------------------------------
//void buttonHandler_toggleEuklidMode() {
//	if (menu_getActivePage() == PERFORMANCE_PAGE) {
//		//switch to Euklid
//		menu_switchPage(EUKLID_PAGE);
//	} else {
//		//switch back to mute mode
//		menu_switchPage(PERFORMANCE_PAGE);
//	}
//
//}
//--------------------------------------------------------------
uint8_t buttonHandler_getMode() {
   return buttonHandler_stateMemory.selectButtonMode;
}
//--------------------------------------------------------------

void buttonHandler_setRunStopState(uint8_t running) {
	// set the state
   buttonHandler_stateMemory.seqRunning = (uint8_t) (running & 0x01);
   menu_sequencerRunning = (uint8_t)(running&0x01);
	// set the led
   led_setValue(buttonHandler_stateMemory.seqRunning, LED_START_STOP);  
}
