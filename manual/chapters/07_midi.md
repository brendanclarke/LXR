# MIDI

The synthesizer has 2 MIDI interfaces: a standard serial interface using DIN connectors, and a
MIDI USB interface for use with a computer. Both provide an input/output pair.

## MIDI Channel

The MIDI channel (1–16) is selected in the settings menu. The synth listens to incoming messages
on this channel and sends sequencer notes to the MIDI output on the same channel.

## MIDI Sync

The LXR can be synced to external gear using MIDI clock signals. Set the BPM parameter in the
settings menu to `0` to enable slave mode. The LXR will then sync its tempo to incoming MIDI
clocks, and the Play button has no function — the synth listens for MIDI start/stop signals instead.

When not in MIDI slave mode, the LXR sends its internal clock as MIDI clock messages on both
the USB and the DIN MIDI output. This way the LXR can act as a MIDI master and sync other
devices. MIDI start/stop messages are sent when the sequencer starts or stops.

## Record Incoming MIDI

You can trigger all voices with external MIDI gear (drum pads, keyboard, etc.). If record mode is
active, the played MIDI notes are directly recorded into the corresponding patterns. Note
recording can be quantised to a grid.

The default MIDI note numbers for the 7 tracks are:

\begin{voicemiditbl}
1 & 36 \\
2 & 37 \\
3 & 38 \\
4 & 39 \\
5 & 40 \\
6 & 41 \\
7 & 42 \\
\end{voicemiditbl}

## Transmitting Sequencer Data to the MIDI Output

The sequencer currently transmits note-on trigger messages to the MIDI output. The ability to
send complete sequencer tracks with pitch information on separate MIDI channels is planned for a
future release.

## MIDI CC List

Parameters can be remote-controlled with MIDI CC messages. See the tables below.

## MIDI NRPN List

Since the LXR has more than 127 parameters, some are controlled using NRPN messages.
