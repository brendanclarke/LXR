# Overview

In this first chapter we will focus on the physical appearance of the LXR, describing the front
panel controls as well as the connection jacks on the back. The basic menu navigation is also
explained.

## The Front

\begin{figure}[H]
  \centering
  \includegraphics[width=\linewidth]{front_panel.png}
  \caption{LXR front panel with numbered control regions.}
\end{figure}

### 1) Display

The display shows parameter values from the selected menu page.

### 2) Knobs

The 4 knobs edit the values shown in the display. A parameter fetch option — which prevents
value jumps when switching menu pages — can be activated in the settings menu.

### 3) Record/Play Buttons

**The Play button** starts and stops playback. The current pattern is reset to the first step when
the sequencer is stopped. \[Alternative SHIFT function: reset last sound parameters loaded from file.\]

**The Record button** activates the recording function. When the LED is lit, all incoming MIDI
notes are recorded into the active pattern. Knob movements are also recorded to the selected
automation track. \[Alternative SHIFT function: record settings.\]

### 4) Shift Button

Activates an alternate function set for the buttons. Hold SHIFT while pressing another button to activate the alternative function. A global setting can change the SHIFT button to work as a toggle instead, in which case the led indicates whether SHIFT is active or not.  

### 4) Copy/Clear Button

Used to copy and \[clear (alternative SHIFT function)\] sequencer data.

### 5) 16 Sequencer Buttons

Function depends on the active mode:

- **Voice mode:** set/reset the 16 main steps in the sequencer.
- **Performance mode:** buttons 1–7 trigger manual rolls. Buttons 9–16 control the looper.
- **Step Edit mode:** select one of the 16 main steps for editing.
- **Menu mode:** no function.

### 6) Encoder

Used to navigate through the menu. Press the encoder to activate a selected option or enter a single-parameter view.
\[Hold shift while in single-parameter view to view and edit the **morph** parameter where applicable.\]  

### 7) 8 Select Buttons

Function depends on the active mode:

- **Voice Edit mode:** select the synth section to edit (Oscillators, Mix, Envelopes, etc.)
- **Performance mode:** change the playing pattern.
- **Step Edit mode:** select/edit sub steps.
- **Menu/Pattern generator mode:** no function.

### 8) 4 Mode Buttons

Switch between the 4 operating modes. Modes shown in brackets are selected using the shift
button.

- Button 1 — Voice edit mode
- Button 2 — Performance mode \[Pattern generator mode\]
- Button 3 — Step Edit mode
- Button 4 — Load/Save \[Settings menu\]

### 9) 7 Voice Buttons

The voice buttons select the active track to edit. Together with the shift key they mute/unmute
the voices. They behave the same in all operation modes except performance mode — see the
performance mode chapter for details.

## The Back

\begin{figure}[H]
  \centering
  \includegraphics[width=0.85\linewidth]{back_panel.jpeg}
  \caption{LXR rear panel connections.}
\end{figure}

### USB

The synthesizer provides a class-compliant USB MIDI interface.

### Memory

Insert an SD card to load and save preset and pattern data. The card must be inserted with the
label facing downward.

\attention{The SD card must use a FAT32 filesystem for reliable operation. The synth will work
with FAT16 cards too, but \textbf{firmware updates are only possible from FAT32 cards}.}

### 4 Audio Outputs

4 mono audio outputs at line level. Can also be used as 2 stereo output pairs.

### MIDI

MIDI in and output via standard DIN connectors.

### Power Connector

**7–9V DC** power plug, **centre pin positive, at least 600 mA**.

## Menu Navigation

The upper row of the display shows the short name of the parameter. The bottom row shows its
current value. Use one of the 4 knobs to change it.

\oled{frq \quad Res \quad typ \quad drv >}{32 \quad\; 120 \quad LP \quad\;\; 0}

- The encoder selects a menu parameter. A capital letter at the beginning of the name
  indicates the selected parameter. In the example above, `Res` is selected.
- If there are more than 4 entries in the current menu, a `>` or `<` sign in the upper-right
  corner indicates a second page. The two pages toggle by pressing the menu button again.

Pushing the encoder opens the detail page:

\oled{\textbf{Filter}}{Resonance \hfill 120}

On the detail page you can see the full parameter name and change its value with the encoder.
This is useful for fine adjustments where the knob control is too coarse. When viewing a drumkit
parameter in VOICE mode, hold SHIFT in this mode to view and edit the corresponding **morph**
parameter. Push the encoder again to return to the normal menu mode.
