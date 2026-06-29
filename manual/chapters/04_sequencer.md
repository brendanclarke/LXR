# Sequencer

The LXR has a sequencer for playing and programming drum patterns. The basic functionality is
similar to classic TR-x0x machines, with additional features.

## Tracks

There are 7 sequencer tracks — one per voice, plus an additional track to control the open hihat
of voice 6.

[**Brendan: To Review**] Pattern scaling per track is accessible via a second page under the 'click' (transient voicing) sub-page. This lets you run a track at a different rhythmic subdivision from the rest of the pattern.

### Select Tracks

The active track is changed with the 7 voice buttons. A lit LED next to the button shows the
selected track.

### Mute Tracks

Hold the shift button and press a voice button to mute/unmute that track. The LEDs of unmuted
voices light up.

\tip{In performance mode the function of the voice buttons is inverted — they mute/unmute
without needing to hold shift. This allows single-handed operation.}

### Unmute All Tracks

To unmute all muted tracks simultaneously, go to performance mode and hold shift. The voice
buttons then unmute all tracks up to and including the pressed button.

Example:

- Shift + Voice Button 4 = tracks 1, 2, 3 and 4 will be unmuted.
- Shift + Voice Button 7 = all tracks are unmuted.

## Steps

There are 16 steps per track. Steps are set or removed using the 16 sequencer buttons.

### Sub Steps

Each of the 16 main steps consists of 8 sub steps, giving a total of 128 steps per pattern.

\begin{figure}[H]
  \centering
  \includegraphics[width=\linewidth]{substep_diagram.png}
  \caption{Sub steps shown within the step buttons.}
\end{figure}

Sub steps define a step pattern played when the corresponding main step is active. They can be
used to create flam or roll sounds on a particular step, or to place a note between the
$\frac{1}{16}$th notes of the main steps.

If the main step is inactive, none of its sub steps will be played. Each sub step can have its own
pitch, volume, probability, and automation settings.

### Step Scale and Step Length

Each track in a pattern can have an independent step scale and step length. Step scale makes the track run slower than the master clock by multiples of 2 and is accessed from the secondary page under Voice Mode: Click. Step length is set from the Patgen mode or from the secondary page under Voice Mode: Mix. Note that even though these parameters are listed under Voice Mode, they are saved with the pattern data and can change across patterns in the pattern set. 

## Pattern

A set of all 7 tracks is called a pattern. Different patterns can be chained to play one after
another — see the Performance Mode section for details.

## Pattern Set

A pattern set is a group of 8 patterns saved together on the SD card and directly accessible in
performance mode.

## Basic Pattern Editing

Basic pattern editing is done in voice edit mode. Select the track with the voice buttons. The step
data is displayed on the 16 sequencer button LEDs. Use the sequencer buttons to set and remove
steps.

## Step Parameters

Each step has a set of parameters. To edit them in voice edit mode, hold the shift button. The
display then shows the step parameter page and the LED of the selected step starts flashing.

### Available Step Parameters

\begin{paramtable}
vel & Step Velocity    & Note-on velocity (0–127). Can be used as a mod source on the
                         voice modulation page. \\
nte & Step Note        & MIDI note number to play (--63 to +63). Default 0 = MIDI note 63. \\
prb & Step Probability & Chance of the step being played. 0 = never (0\%), 63 = 50\%,
                         127 = always (100\%). \\
\end{paramtable}

### Step Probability

The step probability value sets the chance of a step being played, from 0 to 127 (0–100%).

On each main step, the sequencer generates a new random number between 0 and 127. If the
random number is less than or equal to the step's probability value, the step plays.

\tip{The random number is generated once per main step and shared across all 8 of its sub steps.
This allows interesting sub-step cluster randomisation — the 1st sub step always plays, the 5th
plays with 50\% chance, and the 3rd and 7th play with 25\% chance.}

## Automation

Parameter values can be recorded to the sequencer steps. Each step can store 2 parameter
changes. They only affect that single step and are ignored if the step is inactive. The modulation
targets can be different on every step.

When a step's velocity is set to 0, it does not retrigger the voice envelope but still plays back
any automation recorded on that step. This can be useful for assigning automation to a different
voice than the track assignment, or changing the character of a voice as the sound decays.

### Choosing the Active Automation Track

There are 2 automation tracks per voice. Select the active track before recording.

- Hold shift and press the record button to open the recording options menu.
- The \param{trk} parameter selects the active automation track.

### How to Assign Parameter Automations

**Realtime recording:**

1. Press the record button. The record LED lights up.
2. While the pattern is playing, tweak sound parameters with the knobs. Changes are recorded
   to the played steps.

\attention{Activate the quantisation grid before recording automations. Without quantisation,
data is recorded to all 128 sub steps, and most of it will land on inactive sub steps if you are
only using the 16 main steps.}

**Lock parameter values to a single step:**

1. In voice edit mode, hold down the step button.
2. The step LED starts blinking.
3. Use one of the 4 knobs to assign a parameter value to that step.
4. Release the step button.

**Edit automation manually via the step edit menu:**

1. Go to the step parameter menu (voice mode: shift + step button).
2. Use the encoder to scroll to page 2. You will see:

\begin{paramtable}
p1d & Parameter 1 Destination & Select the target voice parameter from a list. \\
p1v & Parameter 1 Value       & Set the parameter to this value on this step. \\
p2d & Parameter 2 Destination & Select the target voice parameter from a list. \\
p2v & Parameter 2 Value       & Set the parameter to this value on this step. \\
\end{paramtable}

Manual editing is the only way to assign parameter automations from one track to another. Edit
the target parameter in detail mode (push encoder) to see full names and destination voice.

### Clear Automation Data

- Hold shift and press the copy/clear button.
- Use the encoder to select `Clear [autom.1/2]?`
- Press copy/clear again to confirm, or release shift to abort.

## Recording Options Menu

Hold shift and press the record button to open the recording options menu:

\oled{Trk \quad qnt}{0 \hspace{2.5em} 16}

### Automation Track

The \param{trk} parameter selects which of the 2 automation tracks is active. Parameter
automations are recorded to the active track.

### Quantisation

Selects the quantisation grid. All recorded notes and automation data will be quantised to the
selected grid. Quantisation cannot be applied retroactively — it must be on before recording.

| Parameter value | Description             |
|-----------------|-------------------------|
| off             | No quantisation         |
| 8               | Quantise to 1/8 notes   |
| 16              | Quantise to 1/16 notes  |
| 32              | Quantise to 1/32 notes  |
| 64              | Quantise to 1/64 notes  |

## Copy Sequencer Data

It is possible to copy sequencer tracks and patterns to another position.

1. Press and hold the Copy button. Its LED starts flashing to indicate copy mode is active.
2. Select the source data by pressing a voice button (track copy) or a pattern button (pattern
   copy). The source LED starts flashing.
3. Press the target voice/pattern button. The data is copied immediately, overwriting the
   destination.
4. To abort, release the copy button before selecting a target.

It is possible to copy sequencer main steps and sub-steps to another position:

1. Press and hold the Copy button.
2. Select the source data by pressing a sequencer button (main step copy) or a select button (sub-
   step copy). The source LED starts flashing.
3. Press the target sequencer button. If a main step was copied it will be applied immediately.
4. If a sub-step was copied, press the select button to paste the sub-step.

## Clear Sequencer Data

Sequencer data can be cleared separately for track, pattern, or automation data.

1. Press Shift + Copy/Clear.
2. The display shows `Clear [track]?`
3. Use the encoder to select what to delete: `[track]`, `[pattern]`, `[autom.1]`, or `[autom.2]`.
4. Press the clear button again to confirm, or release shift to abort.
