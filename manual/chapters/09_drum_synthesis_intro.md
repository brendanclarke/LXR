# Introduction to Drum Synthesis

This chapter introduces the basics of drum synthesis, focused on the classic analogue sounds as a
starting point for further exploration.

## Kicks

The basis of nearly every kick sound is an oscillator combined with a decaying envelope
modulating its pitch. Voices 1–3 are ideal for this.

### Oscillator

A good starting point is a sine oscillator with a pitch around 30–35.

### Amplitude Envelope

A kick starts at maximum amplitude, so set attack to zero. The decay controls the length of the
kick — a decay in the range 20–30 with a slope around 25 (exponential) gives a nice average kick.
For booming 808-style kicks, increase the decay further.

### Pitch Envelope

Set a decay and slope similar to the amplitude envelope as a starting point. All three parameters
— decay, slope, and modulation amount — have a large impact on the character of the kick.

### Click

The attack transient is an important part of a kick sound. Try different transient generator
settings for various clicks and pops, or slightly increase the amplitude attack for softer bass
drums.

### Tips and Tricks

- You can further shape the attack using the FM oscillator.
- For additional click in the attack, enhance it with a peak filter.

## Snare

The snare is a 2-component sound: the tonal body and the noisy rattle. Voice 4 is designed with
both parts in mind — an oscillator for the tonal part and a noise generator with filter for the noise
part. Only the noise is routed through the filter, since in most cases you want highpass-filtered
noise with an unfiltered drum body.

For most snare sounds the noise should be louder than the tonal part — try setting the oscillator
page mix parameter to around 100.

### Tonal Part

Similar to a kick but less pronounced. A snare does not need a loud click or deep pitch modulation,
and its oscillator frequency should be higher than a kick since it represents a smaller drum.

### Noise Part

Highpass-filtered noise with a moderate resonance setting is sufficient for good results.

### Amplitude Envelope

Set attack to zero. For the decay:

- Longer decay times with a very exponential slope (as low as 5–10) produce a short hit with a
  nicely decaying noise tail — the tail acts like a room reverb, giving a more natural result.
- Shorter decay times with a linear or exponential slope give a very dry, direct snare.

### Tips and Tricks

- Not every snare needs both tone and noise. Try using only the tonal part with a high pitch
  modulation envelope to get the classic Kraftwerk electronic zap.

## Clap

A clap sound is noise with a characteristically fuzzy attack — simulating multiple hands clapping
with slight timing variations.

### Amplitude Envelope

Use the repeat feature to simulate this behaviour. 3–4 repeats works well for a classic clap. The
timing differences between claps are very small, so use a short repeat time (attack value below
10). This produces a short burst at the start. For the decay, use an exponential slope with a value
below 10.

### Oscillator

A single white noise oscillator set to its maximum frequency.

### Filter

A bandpass filter with cutoff 60–80 and high resonance gives good results.

## Hihat

### Oscillator

Hihats need a complex metallic noise spectrum. Use all 3 oscillators with high frequencies and
modulation amounts to build a spectrum you like. A simple white noise oscillator can also work
well.

### Amplitude Envelope

Set 2 decay times — one for the closed hat, one for the open hat. Both should be short, but the
open hihat decay should be longer than the closed. Use a very exponential slope (below 10).

### Filter

Use a highpass filter to remove all low frequencies. A high cutoff around 120 works well.

## Cymbal Ride

Cymbals are similar to hihats — the difference is in the envelope and filter settings.

### Oscillator

Oscillator settings can be similar to those of the hihat.

### Amplitude Envelope

The only real difference from the hihat is a much longer decay time. Use a strong exponential
slope. Also consider lowering the voice volume, as cymbal sounds can be very loud.

### Filter

A bandpass filter with high resonance and a cutoff above 110 gives good results.

### LFO

Cymbals benefit greatly from LFO modulation on the filter frequency. Use a low modulation
frequency and a sine wave with a gentle modulation depth — this ensures successive strikes do not
sound identical and the frequency spectrum shifts slightly over time.

## Bells

### Realistic Bells

Realistic bells are best achieved with FM oscillators. A complex metallic spectrum combined with
an exponential decay amplitude envelope and a bandpass filter produces different bell types. The
sample rate reducer is very useful for adding a more metallic character.

### 808-Style Cowbell

The 808 cowbell uses 2 detuned rectangle oscillators through a bandpass filter.

**Recommended voice:** Drum voice 1–3

**FM Page:**

- Set mode to Mix (main and FM oscillators are mixed).
- Set amount to 63 (50/50 mix ratio).
- Select rectangle waveform.
- Set frequency to approximately 78.

**OSC Page:**

- Select rectangle wave.
- Set frequency to 71.

**Amplitude Envelope:**

- Attack: 0
- Slope: very exponential (around 2–5).
- Decay: 25–50 depending on slope setting.

**Filter:**

- Bandpass filter.
- Medium resonance around 70.
- Cutoff frequency around 92.

## More Information About Drum Sound Design

Two highly recommended resources:

- The *Sound on Sound* Synth Secrets series contains excellent articles on different drum sounds:
  <http://www.soundonsound.com/search?url=%2Fsearch&Keyword=%22synth+secrets%22>

- The Waldorf Attack synthesizer owners manual contains detailed how-tos on drum synthesis:
  <http://waldorf.electro-music.com/attack/docs/attackdrumsounds.pdf>
