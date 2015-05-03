Additions to .34 Firmware:
====================================
1. Copy individual tracks between patterns.
	- select (view) the source pattern
	- press and hold 'copy'
	- press the source track (voice) button
	- press the destination pattern button

2. New global menu option for instant pattern switching
	- when enabled, switching patterns with the buttons or a program change MIDI message happens on the next sub-step (keeping the current sequencer position) rather than at the end of the bar

3. MIDI CC assignments are by voice channel rather than all on the global channel - see document in this folder for new assignments
	- this includes a morph assignment to Mod Wheel on the global channel
	- MIDI CC's also get recorded to automation slots (rather than updating voice parameters) when record is on

4. Drumkit can be changed by sending a 'Bank MSB' (CC0) signal on the global MIDI channel

5. Individual drum voices can be changed by sending a 'Bank MSB' (CC0) signal on their individual MIDI channels

6. New global menu option to make the 'shift' button a toggle rather than momentary

7. Multiple voices can be set to respond (and record) note-ons on the global channel. When the currently active track has a 'note' assignment other than 'any', note-ons to the global channel will be matched to ALL tracks that have a note assignment other than 'any', rather than just the active one.

8. MIDI channel select menus now have a '0' option that disables MIDI input.

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

10. Added pattern scaling option. This is accessed via a second page under 'click' (the transient voicing sub-page). The number is the binary exponent of the pattern multiplier. i.e., if you set this to 2, the pattern will be 2^2=4 bars long, each step is a quarter note, and each sub-step is 1/32nd. The maximum exponent is 7 (pattern is 128 bars long), beyond this there is no effect despite the control going to 15 (ran out of data types). Note that this required changing some functions so that they do not use the step counter for track 1 as a reference. A dummy step counter was introduced instead for this. It is unlikely, but possible this will cause problems with the trigger mod, but I have no way of testing this.

The control currently doesn't refresh when switching tracks for some reason, but this is purely cosmetic. The parameter is per sub-pattern, and is saved with the pattern set, just as the pattern length (1-16) is.

11. 'Performance' and 'All' save types now save the morph target parameters in addition to the drum kit and pattern. The version number for this type of file has been incremented to 3. Previous versions will load with an empty morph target, but will be re-saved as the new type. 

12. added a global option to interpret Bank Change (CC0) messages incoming on the global channel as a command to load a 'Performance' data set instead of a drum kit. Bank changes on voice MIDI channels will still change that part individually.

13. Added a 'realign pattern' shortcut. pressing the pattern button of the currently viewed pattern in 'perf' mode will align all tracks to the master clock. This also resets any 'pattern rotation' set. Also, when holding shift in 'PERF' mode, you can press the voice buttons to quickly switch the active voice for pattern rotation. This is in addition to the multiple voice-unmute shortcut. shift+current pattern will also do the re-alignment.

14. When the velocity of a step is set to '0', the step will not re-trigger envelopes. This lets you use steps as 'automation only' - a bit like 'trigless locks' on elektron kit.

15. One-shot LFO's! These are added as additional waveforms in the LFO settings. i.e. "si1", "tr1", etc are 1 shot versions of sine, triangle, and all the other normal lfo shapes. 
	- There is also a new LFO shape "exponential triangle" - "xtr" and "xt1" which are the exponential up followed immediately by exponential down. 
	-With a one-shot LFO selected, the 'offset' control instead sets a delay for the start of the LFO. It's a little weird because the delay gets scaled with 'rate' but it adds a lot of musical options. 
	- The 'noise' LFO holds a single random value on each retrigger. 
	- The 'rect' LFO is inverted in one-shot mode, so that it can be instantly on and then off, with the 'offset' delay setting an off portion at the beginning if desired.

16. Quick access to morph target parameters. When viewing a single parameter (click in and adjust with the encoder), press 'shift' to edit and view the parameter for the morph target.



Sonic Potions LXR Drumsynth Firmware
====================================
The LXR is a digital drum synthesizer based on the 32-bit Cortex-M4 processor and an Atmega644 8-bit CPU. Developed by Julian Schmidt.

    The 'front' folder contains the AVR code

    The 'mainboard' folder contains the STM32F4 code

    The 'tools' folder contains the firmware image builder tool, to combine AVR and Cortex code into a single file, usable by the bootloader.

Please note that there are libraries from ST and ARM used in the mainboard code of this project. They are all located in the Libraries subfolder of the project. Those come with their own license. The libraries are:

    ARM CMSIS library
    ST STM32_USB_Device_Library
    ST STM32_USB_OTG_Driver
    ST STM32F4xx_StdPeriph_Driver

	

Many Thanks to user Rudeog who contributet a lot of bugfixes and features for version 0.26 and 0.33 as well as Patrick Dowling for the Makefiles for the Linux build system!



Instructions for building on Linux using the provided makefiles:
----------------------------------------------------------------
You will need:
- the ARM GCC compiler 
- the AVR GCC compiler 
- the AVR c libs


 
GNU Tools for ARM Embedded Processors 
-------------------------------------
project homepage: https://launchpad.net/gcc-arm-embedded


For Ubuntu 10.04/12.04/13.04 32/64-bit user, PPA is available at https://launchpad.net/~terry.guo/+archive/gcc-arm-embedded.

otherwise you can download the 32bit binaries here
https://launchpad.net/gcc-arm-embedded/4.8/4.8-2014-q1-update/+download/gcc-arm-none-eabi-4_8-2014q1-20140314-linux.tar.bz2


--- Installing the ARM GCC binaries ----

download the binary package:
'wget https://launchpad.net/gcc-arm-embedded/4.8/4.8-2014-q1-update/+download/gcc-arm-none-eabi-4_8-2014q1-20140314-linux.tar.bz2'

extract it:
'tar xvjf gcc-arm-none-eabi-4_8-2014q1-20140314-linux.tar.bz2 '

move it to /opt/ARM:
'sudo mv gcc-arm-none-eabi-4_8-2014q1 /opt/ARM'

include it permanently in your PATH variable
'echo "PATH=$PATH:/opt/ARM/bin" >> ~/.bashrc'

IMPORTANT!
for x64 systems, you have to install the 32-bit version of libc6 or you will get an 'arm-none-eabi-gcc: not found' error when invocing arm-none-eabi-gcc:
'sudo apt-get install libc6-dev-i386'



--- Installing the AVR GCC compiler and AVR libc---

These should normaly be available from you package manager
'sudo apt-get install gcc-avr avr-libc'


Now you are ready to go.
To build the firmware, go to the LXR folder containing this file and type:
'make firmware'

you should now find a new FIRMWARE.BIN file in the 'firmware image' subfolder


Thanks a lot to Patrick Dowling and Andrew Shakinovsky for their code contributions!

Instructions for building on windows using Eclipse:
---------------------------------------------------

1.  Install Eclipse Juno CDT (You could install a later version, but this is the version I have working)

2.  Install the Eclipse GNU ARM plugin. Go into the help menu, Install new Software, add a site: http://gnuarmeclipse.sourceforge.net/updates. Then check the box to install that plugin.

3.  Download and install the GCC ARM toolchain https://launchpad.net/gcc-arm-embedded/+download

4.  Download and install gnu make: http://gnuwin32.sourceforge.net/packages/make.htm

5.  Download and install Atmel AVR toolchain from http://www.atmel.com/tools/ATMELAVRTOOLCHAINFORWINDOWS.aspx (you don't need the headers package)

6.  Ensure that the bin directory from 3, 4, and 5 are on the path. I made a batch file that adds these 3 bin directories to my path and launches eclipse.

7.  Fetch the LXR sources from github. You can either install git and do it the git way, or download a zip and unzip it.

8.  In Eclipse, create a workspace in root folder of whole tree that you unzipped (or git'd).

9.  Add two project dirs mainboard\firmware\DrumSynth_FPU and front\AVR to the workspace. To do this, use File/Import/General/Existing projects into workspace. Then select root directory and browse to these dirs. Do this step once for each of these two dirs. You will end up with two projects in your workspace.

10.  These should build. Eclipse is a bit squirrely, so you might need to do a make clean first to create the first makefiles, or rebuild indexes.

11.  I've built the firmwareimagebuilder.exe in the \tools\bin folder. I've also put a batch file that launches it and copies the binaries from the respective output directories to create FIRMWARE.BIN in that same dir. If you don't trust the .EXE I built, you will need to build it from tools\FirmwareImageBuilder\FirmwareImageBuilder. As is you will need visual studio. If you don't have it, you can try to install the free version, mingw, etc and compile the one file FirmwareImageBuilder.cpp (I've fixed it so it should build with any tool) and make your own exe and copy it to that dir.

12.  Thats it, after running the batch file you will have your firmware file. 
