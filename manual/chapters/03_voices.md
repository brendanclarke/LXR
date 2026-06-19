# Voices

The LXR offers 6 voices, each optimised for different drum sounds. The type of a voice cannot be
changed. There are 3 drum voices, a subtractive clap/snare voice, an FM percussion voice, and a
hi-hat voice.

## Drum Voices (voices 1–3)

The drum voices are suited to — but not limited to — kick drums, toms, cowbells, and other
percussive sounds.

\begin{figure}[H]
  \centering
  \includegraphics[width=\linewidth]{voice_drum_signal_flow.jpeg}
  \caption{Drum voice signal flow (voices 1–3).}
\end{figure}

## Snare Voice (voice 4)

This voice is good for snare drum and clap sounds. A noise source and a pitched oscillator can be
mixed. Only the noise source is routed through the filter. There is no FM capability on this voice.

\begin{figure}[H]
  \centering
  \includegraphics[width=\linewidth]{voice_snare_signal_flow.jpeg}
  \caption{Snare voice signal flow (voice 4).}
\end{figure}

## Cymbal/Clap Voice (voice 5)

This voice uses a 3-operator FM synthesis to generate the sound. The main oscillator is modulated
by 2 modulation oscillators. It is excellent for metallic and noisy sounds.

\begin{figure}[H]
  \centering
  \includegraphics[width=\linewidth]{voice_cymbal_signal_flow.jpeg}
  \caption{Cymbal/Clap voice signal flow (voice 5).}
\end{figure}

## Hihat Voice (voice 6)

The hihat voice is nearly identical to the Cymbal voice, but it offers 2 different decay times for
the amplitude envelope. The voice is shared between sequencer tracks 6 and 7, each using one of
the 2 available decay times. This allows open and closed hihats to be played. The closed hat
(track 6) chokes the open hat (track 7).

\begin{figure}[H]
  \centering
  \includegraphics[width=\linewidth]{voice_hihat_signal_flow.jpeg}
  \caption{Hihat voice signal flow (voice 6).}
\end{figure}

## Voice Parameter Menu Sections

The synthesis parameters of each voice can be altered in voice edit mode. The synthesis engine is
grouped into 8 sections selectable with the select buttons.

### Oscillator Page (OSC)

Provides access to the main oscillator parameters — frequency and waveform.

**Drum 1–3, Clap, Hihat**

\begin{paramtable}
coa & Coarse tune & Coarse tuning of the main oscillator in semitones. \\
fin & Fine tune   & Fine tuning of the main oscillator. $\pm$50 cents. \\
wav & Waveform    & The waveform of the main oscillator. \\
\end{paramtable}

**Snare**

\begin{paramtable}
coa & Coarse tune    & Coarse tuning of the main oscillator in semitones. \\
fin & Fine tune      & Fine tuning of the main oscillator. $\pm$50 cents. \\
noi & Noise frequency & Coarse tuning of the noise oscillator. \\
mix & Oscillator mix & Mix ratio between oscillator and noise source. \\
wav & Waveform       & The waveform of the main oscillator. \\
\end{paramtable}

### Amplitude Envelope Page (AEG)

Each voice has an amplitude envelope. The common parameters are attack time, decay time, and
slope.

**Drum 1–3**

\begin{paramtable}
att & Amplitude envelope attack & Rise time of the envelope. \\
dec & Amplitude envelope decay  & Fall time of the envelope. \\
slp & Amplitude envelope slope  & Variable slope from exponential to linear to logarithmic. \\
\end{paramtable}

**Snare/Cymbal**

\begin{paramtable}
att & Amplitude envelope attack & Rise time of the envelope. \\
dec & Amplitude envelope decay  & Fall time of the envelope. \\
rpt & Repeat count              & Number of retriggers. \\
slp & Amplitude envelope slope  & Variable slope from exponential to linear to logarithmic. \\
\end{paramtable}

**HiHat**

\begin{paramtable}
att & Amplitude envelope attack & Rise time of the envelope. \\
d1  & Closed hihat decay time   & Fall time of the envelope for the closed hihat. \\
d2  & Open hihat decay time     & Fall time of the envelope for the open hihat. \\
slp & Amplitude envelope slope  & Variable slope from exponential to linear to logarithmic. \\
\end{paramtable}

### Modulation Page (MOD)

Contains velocity modulation parameters and, where available, the second envelope parameters.

**Drum 1–3, Snare**

\begin{paramtable}
dec & Modulation envelope decay  & Fall time of the modulation envelope. \\
slp & Modulation envelope slope  & Variable slope from exponential to linear to logarithmic. \\
mod & Modulation envelope amount & Controls how strongly the envelope modulates its target.
                                   The envelope is hardwired to pitch; this controls the modulation
                                   intensity. In FM mode the EG is additionally hardwired to the FM
                                   amount. \\
dst & Velocity mod. destination  & The note velocity can be routed to any voice parameter.
                                   Select the target from a list. \\
amt & Velocity mod. amount       & Amount of the velocity modulation. \\
vol & Velocity to volume         & On or Off. When active, note velocity controls voice volume. \\
\end{paramtable}

**Clap, Hihat**

\begin{paramtable}
dst & Velocity mod. destination & Route note velocity to any voice parameter. \\
amt & Velocity mod. amount      & Amount of the velocity modulation. \\
vol & Velocity to volume        & On or Off. When active, note velocity controls voice volume. \\
\end{paramtable}

### Frequency Modulation Page (FM)

Hosts frequency, waveform, and modulation amount settings for the FM oscillators.

**Drum 1–3**

\begin{paramtable}
amt & Modulation amount / mix ratio & FM mode: modulation amount.
                                      Mix mode: mix ratio of the 2 oscillators. \\
frq & Frequency of FM OSC           & Coarse tuning of the FM oscillator in semitones. \\
wav & Waveform of FM OSC            & Waveform of the FM oscillator. \\
mod & Oscillator mode               & FM mode: main OSC is modulated by FM OSC.
                                      Mix mode: main OSC and FM OSC are mixed. \\
\end{paramtable}

**Snare** — Voice 4 has no FM capability; this page is empty.

**Clap, Hihat**

\begin{paramtable}
f1  & Frequency 1  & Frequency of the first modulation oscillator. \\
f2  & Frequency 2  & Frequency of the second modulation oscillator. \\
g1  & Gain 1       & Gain of the first modulation oscillator. \\
g2  & Gain 2       & Gain of the second modulation oscillator. \\
wav & Waveform 1   & Waveform of the first modulation oscillator. \\
wav & Waveform 2   & Waveform of the second modulation oscillator. \\
\end{paramtable}

### Transient Generator Page (Click)

Parameters of the transient generator: waveform, volume, and frequency of a short attack sample
mixed into the voice output.

\begin{paramtable}
wav & Transient wave shape  & Selects the transient sample to play. \\
vol & Transient volume      & Volume of the transient sample. \\
frq & Transient frequency   & Frequency of the transient sample. \\
\end{paramtable}

### Filter Page (FIL)

All filter parameters on one page.

\begin{paramtable}
frq & Filter frequency  & Changes the cutoff frequency of the filter. \\
res & Filter resonance  & Adjusts the filter resonance. \\
typ & Filter type       & Selects the filter characteristic. \\
drv & Filter drive      & Increases the input gain of the filter. \\
\end{paramtable}

### LFO Page (LFO)

Each voice has an LFO editable on this page.

\begin{paramtable}
frq & LFO frequency        & Manual LFO rate control. Only available when sync is off. \\
snc & LFO sync             & Activates clock sync. Sync rates: $\frac{1}{2}$, $\frac{1}{4}$, etc. \\
mod & Modulation amount    & Controls how strongly the LFO signal affects the target. \\
wav & LFO waveform         & Sine (sin), Triangle (tri), Saw up (sup), Saw down (sdn),
                             Square (sqr), Random (rnd), Exponential saw up (xup),
                             Exponential saw down (xdn). \\
rtg & LFO retrigger        & Allows the LFO to be retriggered from a sequencer track (v1–v6).
                             The LFO resets its phase whenever a note plays on the selected track. \\
off & Phase offset         & Phase offset applied when the LFO is retriggered. \\
voi & Destination voice    & Select the target voice for LFO modulation. \\
dst & Destination select   & Select the parameter to modulate (list of all parameters for the
                             selected target voice). \\
\end{paramtable}

### Mixer Page (Mix)

Volume, panning, routing, voice effects, and sequencer track length.

\begin{paramtable}
vol & Voice volume  & Adjusts the volume of the voice. \\
pan & Voice panning & Voice panning. Only active when output is set to a stereo channel. \\
sr  & Sample rate   & Sample rate decimation. \\
drv & Drive         & Soft clipping amount. \\
out & Output        & Selects the hardware audio output for this voice: one of the 2 stereo
                      channels, or one of the 4 individual mono outputs. \\
len & Track length  & The length of the sequencer track in $\frac{1}{16}$ steps. \\
\end{paramtable}
