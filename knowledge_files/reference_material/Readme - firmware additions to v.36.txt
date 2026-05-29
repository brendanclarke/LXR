Brendan Clarke
Additions to LXR .36 firmware
LXR by Julian Schmidt (www.sonic-potions.com)

!!!BACK UP YOUR SD CARD!!!

Latest additions are at the top of the list.
23. Track Transpose - in the PERF menu, when SHIFT is held. This is per track. Note that in the custom firmware, shift+track selects, and the active track LED will flash while shift is held.
	There is a 'transpose amount' and a 'transpose on/off' control available. Transpose is temporary, but will be applied to the sequencer track if RECORD is turned on. While in RECORD mode, transpose is stored per-step in a temporary parameter, and is not applied until RECORD is switched OFF. Therefore, you can turn on record, set some transpose value, and quickly toggle transpose on/off to apply transpose to only a few steps. Choose a new transpose value, and repeat until you have a pattern you like, then turn record OFF to make it permanent. 

22. Expanded Performance Menu. 
There are now 4 pages, the top two can be switched with multiple presses of the 'perf' button, all can be accessed with the encoder.
page 1: (mac mac mrp srt)
	In addition to the usual morph and sample-rate controls there are two 'Performance Macros'. These are assignable controls that can change up to 2 parameters each. They work in the same way as the lfo's - the actual kit parameters are unaffected, only the sound changes.

page 2: (shu rol rln rlv)
	In addition to the usual shuffle and roll rate controls, there are now 'roll note' and 'roll velocity' controls. These allow the sound of a roll to be changed, and help speed up recording to the sequencer with rolls.

page 3: (1d1 1a1 1d2 1a2)
	These are the assignments for 'Performance Macro 1' on page 1.
page 4: (2d1 2a1 2d2 2a2)
	These are the assignments for 'Performance Macro 2' on page 1.

	In addition, to make note entry easier with the new roll functions, there is a new entry under the 'sequencer record' menu (the one you get to with shift+record). 'nte' - Sequencer Note Record - can be set to on or off. This exists to let you record 'note' and 'velocity' from roll in separate passes. basically:
1. turn record on, 'sequencer note record' on
2. record notes by pressing roll buttons, turning roll note knob.
3. turn 'sequencer note record' off.
4. press roll buttons, turn the roll velocity knob to record velocity only.
	It may help to turn quantization on for this process, too.

	Pro tip: when record is OFF, setting 'nte' to 'off' here will force rolls to use the sequencer data instead of the 2 roll control knobs.

21. Voices can be changed by pattern automation. There is a new step automation parameter "Voice LoadKit#". When this is set, and the step is activated, the voice will do an individual voice load from the kit file number specified by the automation value. The load is not instantaneous, so it is recommended the load not be done on a step that you want to sound (though this can sometimes sound nicely glitchy), but done on a silent (velocity 0) intermediate step.

The new parameter shouldn't cause old patterns to load strangely, but to be on the safe side you might not want to overwrite patterns or kits that you want re-use with older versions of the OS.

20. Step copy and sub-step copy:
	- Press and hold 'copy', press a step, press another step. Copies all the substep data from one main step to another
	- Press and hold 'copy', press a step, press one of the 8 'select' buttons, then press a different main step, then press another 'select' button. this will copy individual sub-steps between or within main steps. You can also press two 'select' buttons in sequence to copy within the same main step.

Note that the copy function does not copy main step on/off (ie if one of the sequencer lights is lit for the step) - this be set manually.

19. Kit versions - are stored as a parameter. Versioning prevents kits with filter set to 'off' being inadvertently changed to 'LP2'. This does not apply to individual voice loads.

18. Merged .36 stock firmware changes - adds new 'LP2' filter and fix for kick transient

17. Fixes to roll: roll now does not 'reset' at the end of a bar - it acts independently of bar, track length, or scale. It also takes the 'record quantization' (shift+rec) parameter into account when triggering. Record quantization is fixed to take track scale into account also.

16. Quick access to morph target parameters. When viewing a single parameter (click in and adjust with the encoder), press 'shift' to edit and view the parameter for the morph target.

15. One-shot LFO's! These are added as additional waveforms in the LFO settings. i.e. "si1", "tr1", etc are 1 shot versions of sine, triangle, and all the other normal lfo shapes. 
	- There is also a new LFO shape "exponential triangle" - "xtr" and "xt1" which are the exponential up followed immediately by exponential down. 
	-With a one-shot LFO selected, the 'offset' control instead sets a delay for the start of the LFO. It's a little weird because the delay gets scaled with 'rate' but it adds a lot of musical options. 
	- The 'noise' LFO holds a single random value on each retrigger. If there is a delay (offset) phase set, it holds the previous value during this time.
	- The 'rect' LFO is inverted in one-shot mode (normal phase is 0 first, then 1), so that it can be instantly on and then off after a time set by 'frq', with the 'offset' delay setting an off portion at the beginning if desired.

14. When the velocity of a step is set to '0', the step will not re-trigger envelopes. This lets you use steps as 'automation only' - a bit like 'trigless locks' on elektron kit.

13. Added a 'realign pattern' shortcut. pressing the pattern button of the currently viewed pattern in 'perf' mode will align all tracks to the master clock. This also resets any 'pattern rotation' set. Also, when holding shift in 'PERF' mode, you can press the voice buttons to quickly switch the active voice for pattern rotation. This is in addition to the multiple voice-unmute shortcut. shift+current pattern will also do the re-alignment.

12. added a global option to interpret Bank Change (CC0) messages incoming on the global channel as a command to load a 'Performance' data set instead of a drum kit. Bank changes on voice MIDI channels will still change that part individually.

11. 'Performance' and 'All' save types now save the morph target parameters in addition to the drum kit and pattern. The version number for this type of file has been incremented to 3. Previous versions will load with an empty morph target, but will be re-saved as the new type. 

10. Added pattern scaling option. This is accessed via a second page under 'click' (the transient voicing sub-page). Setting this increases track length by the shown multiplier, at the expense of step resolution. Note that this required changing some functions so that they do not use the step counter for DRUM1 as a reference. A dummy step counter was introduced instead for this (at the end of the step index array). It is unlikely, but possible this will cause problems with the trigger mod, but I have no way of testing this.

9. 'Load' menu now includes entries to load individual drum voices. This includes a number of load/save menu changes to deal with this:
	- The name and number of drum voices will reflect the drum kit the voice was derived from, even if this was changed via a MIDI bank change request.
	- The shortcut functions of the knobs has changed (knobs 1-4, from left to right):
	LOAD MENU
		- knob 1: change load type (kit, drum 1, pattern, etc)
		- knob 2: change preset number NB: auto-loads for all kit and voice types
		- knob 3: no function, except disables auto-load if turned
		- knob 4: disables auto-load, moves cursor between load type and 'ok' if it exists
	SAVE MENU
		- knob 1: change save type (same as for load menu)
		- knob 2: change cursor position (ie cycles through type, preset, alphanums, and 'ok')
		- knob 3: change cursor value
		- knob 4: for alphanumeric values, cycles through more characters
		note that the order of characters for the shortcut knobs has changed. Capital letters are all the way on the left of knob 3, numbers are in the middle of knob 3, and lower case is all the way to the left of knob 4.

8. MIDI channel select menus now have a '0' option that disables MIDI input.

7. Multiple voices can be set to respond (and record) note-ons on the global channel. When the currently active track has a 'note' assignment other than 'any', note-ons to the global channel will be matched to ALL tracks that have a note assignment other than 'any', rather than just the active one.

6. New global menu option to make the 'shift' button a toggle rather than momentary

5. Individual drum voices can be changed by sending a 'Bank MSB' (CC0) signal on their individual MIDI channels
	- bug fix - when 2 or more voices are stacked on a single channel, all will respond

4. Drumkit can be changed by sending a 'Bank MSB' (CC0) signal on the global MIDI channel

3. MIDI CC assignments are by voice channel rather than all on the global channel - see document in this folder for new assignments
	- this includes a morph assignment to Mod Wheel on the global channel
	- MIDI CC's also get recorded to automation slots (rather than updating voice parameters) when record is on
	- NRPN's are banished to the forbidden zone

2. New global menu option for instant pattern switching
	- when enabled, switching patterns with the buttons or a program change MIDI message happens on the next sub-step (keeping the current sequencer position) rather than at the end of the bar

1. Copy individual tracks between patterns.
	- select (view) the source pattern
	- press and hold 'copy'
	- press the source track (voice) button
	- press the destination pattern button

