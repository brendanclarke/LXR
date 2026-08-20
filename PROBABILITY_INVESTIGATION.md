# Step-Probability Investigation — Song 75 / Clap-Cym Track

## Report

> When loading the attached Song (number 75), the device ignores probability settings on the Clap/Cym track (and possibly in other tracks as well). This is reoccurring on all loads, but I haven't been able to recreate when doing a track from scratch (yet). Other tracks probability seems to work ok. Using Catalyst 1.01

Attached: `probability_bug_files/P075.ALL`, `P075.PRF`, `P075.SND` (preset name `"Solumn"`, file format version `5`).

**Follow-up questions this revision answers directly:**
1. Can setting probability in the menu land on a pattern other than the one actually playing?
2. Can Save strip a probability value that really was set on a pattern?
3. "Follow" and background-loading are intentional *features*, not defects — is there a *separate defect* that interacts with them so that a probability edit appears to be accepted in the menu but never lands on any pattern that would be saved? Specifically: while playback is on the temp pattern during a background load, is there an edge case — e.g. Follow off, mid-changeover — where data keeps going into the temp pattern and gets lost?

Short answers, justified in full below:
1. **Yes** — confirmed, via two structural risk factors (Part 3).
2. **No** — traced end to end, no stripping logic exists anywhere in the save path (Part 4).
3. **Yes — a genuine, standalone code defect was found** (Part 5), independent of Follow/background-load themselves: a specific front-panel UI page silently swallows the message that would keep the STM's "which pattern are edits going to" state in sync, with no code path anywhere that ever corrects it afterward. This is the strongest single explanation found so far for how a probability edit can vanish from *every* saved pattern.

This is a static-analysis investigation only (no hardware in the loop). It has **not** been confirmed on hardware.

---

## What "Clap/Cym" maps to

`SAVE_TYPE_CYM` in `front/LxrAvr/Menu/menu.c:1090` labels this voice `"Clap/Cym"`. Cross-referencing the save-type ordering (`Drum1, Drum2, Drum3, Snare, Cym, HiHat`) against the track loop in `preset_readDrumVoice`/`preset_loadAll` (`front/LxrAvr/Preset/presetManager.c:2515-2519`) puts it at **track index 4** (0-based) out of `NUM_TRACKS = 7`. Tracks 5/6 are the closed/open hi-hat, which intentionally share one synth voice (see `sequencer.c:343-355`).

---

## Part 1 — Forensic read of the attached files

`.ALL`/`.PRF` step data is a flat, version-independent byte layout (`presetManager.c:30-58`):

```
stepOffset = STEPDATA_OFFSET + track*NUM_PATTERN*STEPS_PER_PATTERN*sizeof(StepData)
                              + pattern*STEPS_PER_PATTERN*sizeof(StepData)
StepData  = { volume, prob, note, param1Nr, param1Val, param2Nr, param2Val }  // 7 bytes/step
```

`VERSION_4_ALL_STEPDATA_OFFSET = 1097` for `.ALL`. I confirmed this offset is still correct for this **version 5** file: `1097 (header) + 50176 (7 tracks × 8 patterns × 128 steps × 7 bytes) = 51273`, which lands exactly on `VERSION_4_ALL_MAINSTEP_OFFSET`, and the full chain of offsets (`mainstep → patchain → shuffle → length → scale`) sums to exactly `51514` bytes — the real size of `P075.ALL`. Same check passes for `P075.PRF` (51515 bytes). So for this file, no offset drift — the raw bytes below can be trusted.

Parsing every `(track, pattern, step)` triple in `P075.ALL`:

- **Every** track (0–6) × **every** pattern (0–7) has exactly 16 active steps (one per main step — a plain 4/4-style fill).
- **Every single active step in the entire file has `prob == 127`** (100%), `volume == 100`, `note == 63`. Not just track 4 — all seven tracks, all eight patterns, no exceptions.
- `note = 63` is exactly `SEQ_DEFAULT_NOTE` (`PatternData.h:23`), `volume = 100` and `prob = 127` are exactly the defaults written by `pat_resetNote()` (`PatternData.c:38-47`) — the helper that initializes a step the first time a main step is turned on. **This means every step in every one of the 8 real saved patterns, on every track, is still sitting at its as-created default.** None of them has ever been captured with a customized note, volume, *or* probability — this is stronger evidence than "probability is 127," and it rules out a narrow "only the prob field got corrupted" theory: whatever happened, it swallowed *any* step-parameter edit, not something specific to the probability field.
- Track 4 (Clap/Cym) and 5/6 (hi-hats) are rotated one sub-step later than tracks 0–2, consistently across all 8 patterns — looks like an intentional per-track *rotation* setting, unrelated to probability.

**Conclusion of Part 1, confirmed:** correct — none of the 8 saved patterns has any probability set at all, on any track. Whatever the user did in the menu to lower Clap/Cym's probability never reached any of the 8 real, saved pattern slots.

### Global settings decoded from the same file

The `.ALL` header stores global settings as one byte per parameter starting right after the 8-byte name + 1-byte version (byte offset 9), in `PAR_BEGINNING_OF_GLOBALS`-relative enum order (`Parameters.h:389-428`, read by the flat loop at `presetManager.c:2454-2465`). Decoding it for `P075.ALL`:

| Parameter | Offset | Value | Meaning |
|---|---|---|---|
| `PAR_BPM` | 9 | 105 | plausible real tempo |
| `PAR_MIDI_CHAN_1..6` | 10–15 | 10,11,12,13,14,15 | — |
| `PAR_FETCH` | 16 | 0 | off |
| **`PAR_FOLLOW`** | 17 | **1 (ON)** | front panel should track the playing pattern |
| `PAR_QUANTISATION` | 18 | 2 | — |
| ... | | | |
| **`PAR_FILE_LOAD_BACKGROUND`** | 35 | **1 ("pat")** | background-swap loading enabled **only for `.pat` loads**, not `.prf`/`.all` |
| `PAR_GLOBAL_SETTINGS_VERSION` | 36 | 5 | matches file version |

These aren't leftover/default-looking values (BPM=105, sequential-but-plausible MIDI channels) — this looks like a real, in-use configuration, not a throwaway test file. Both settings above turn out to matter for Part 3.

---

## Part 2 — Transport/round-trip verification (ruled out)

The 7-byte `Step`/`StepData` struct (`volume, prob, note, param1Nr, param1Val, param2Nr, param2Val`) is packed into 7-bit SysEx bytes plus one MSB-collector byte, symmetric on both MCUs. I traced every occurrence of this pattern and they are all bit-for-bit consistent:

| Direction | Sender | Receiver |
|---|---|---|
| Load (file → STM) | `preset_readPatternStepData` (`presetManager.c:1981-2006`) | `SYSEX_BEGIN_PATTERN_TRANSMIT` (`frontPanelReceivingProtocol.c:1755-1803`) |
| Save (STM → file) | `frontPanelSending_sendStepInfo` (`frontPanelSendingProtocol.c:488-511`) | `SYSEX_REQUEST_STEP_DATA` (`avrCommsReceivingProtocol.c:286-327`) |

In both directions the MSB-shift math (`(x & 0x80) >> N` on send, `(data & (1<<i)) << (7-i)` on receive) matches field-for-field, including for `prob` specifically. **This is not where the bug is.**

Also checked and ruled out:
- The probability *trigger* check itself, `sequencer.c:1119-1131`: `seq_rndValue[i] = GetRngValue()&0x7f; ... if(seq_rndValue[i] <= stepData->prob)`. Logic is correct.
- `FRONT_SEQ_PROB`'s write `->prob = frontParser_command.data2;` (`frontPanelReceivingProtocol.c:2462-2465`) has no masking, but `data2` is always constrained to 0–127 by the AVR side before sending, so this isn't a practical issue.

---

## Part 3 — Question 1: does a menu probability edit land on the wrong pattern? **Yes.**

### The core asymmetry

Step-parameter edits from the front panel — `FRONT_SEQ_PROB`, `FRONT_SEQ_VOLUME`, `FRONT_SEQ_NOTE`, `FRONT_SET_P1_VAL`, `FRONT_SET_P2_VAL` — all write into whichever pattern is currently **displayed**:

```c
// frontPanelReceivingProtocol.c:2462-2465
case FRONT_SEQ_PROB:
   pat_getStepPtr(frontParser_shownPattern, frontParser_activeTrack, frontParser_activeStep)->prob = frontParser_command.data2;
   break;
```

But **actual playback** resolves the live pattern *per track*, through a different variable entirely:

```c
// sequencer.c:586-591
static Step* seq_liveStepForTrack(uint8_t track, uint8_t step)
{
   return pat_getStepPtr(seq_perTrackActivePattern[track], track, step);
}
```

And **Save** dumps every step by raw `(track, pattern, step)` index (`frontPanelSending_sendStepInfo`, `frontPanelSendingProtocol.c:488-511`) — it never consults `frontParser_shownPattern` either. So both playback and Save agree with each other, and both can legitimately disagree with whatever the menu is currently showing/editing.

`frontParser_shownPattern` is a *single, global* variable (`frontPanelReceivingProtocol.c:1452`); `seq_perTrackActivePattern[NUM_TRACKS]` is *per track* (`sequencer.c:125`). The two are synced only by specific, narrow code paths — they are not the same thing and nothing keeps them equal by construction. I found two independent, concrete mechanisms that separate them:

### Mechanism A — per-track pattern following (`hold VOICE + press PATTERN`)

This is a real, documented feature (README: *"hold a VOICE button and press a pattern button to assign that track to a different pattern source independently of the other tracks"*), implemented by `seq_setNextPattern(pattern, voice)` (`sequencer.c:331-360`) and triggered from `frontPanelReceivingProtocol.c:2765`.

For a single-voice call (`voice != 0x0f`), only `perTrackPendingPattern[voice]` is touched — the **global** `pendingPattern`/`seq_activePattern` is left untouched (`sequencer.c:343-357`). Yet the very same call still sets `loadPendingFlag = 1`, which unconditionally triggers `frontPanelSending_sendPatternChange(seq_activePattern)` on the next tick (`sequencer.c:956`) — an ACK that always reports the *unchanged global* pattern, never the per-voice one that actually moved.

On the AVR side, that ACK conditionally re-syncs the shown/edit pattern:

```c
// avrCommsReceivingProtocol.c:642-656
if(parameter_values[PAR_FOLLOW] || tempBoundaryAck) {
   if(menu_activePage != PATTERN_SETTINGS_PAGE)
      menu_setShownPattern(patMsg);   // patMsg == the unchanged GLOBAL pattern
   ...
```

So even with **Follow ON** (confirmed for this file), a per-track reassignment on Clap/Cym causes the AVR to *re-affirm* that the shown/edit pattern is the global one — while `seq_perTrackActivePattern[4]` now points somewhere else. There is no way, with a single global "shown pattern" variable, for step edits on a per-track-reassigned voice to land where that voice is actually playing.

### Mechanism B — the shown pattern becomes the scratch **temp** pattern

`pat_getStepPtr()` treats `SEQ_TMP_PATTERN` specially — it returns a pointer into a completely separate scratch struct, not one of the 8 real patterns:

```c
// PatternData.c:61-67
Step* pat_getStepPtr(uint8_t pattern, uint8_t track, uint8_t step)
{
   pattern = pat_normalizePatternNumber(pattern);
   if(pattern == SEQ_TMP_PATTERN)
      return &pat_tmpPattern.pat_subStepPattern[track][step & 0x7f];   // scratch, never saved
   return &pat_patternSet.pat_subStepPattern[pattern][track][step & 0x7f];  // one of the 8 real patterns
}
```

`frontParser_shownPattern` can legitimately become `SEQ_TMP_PATTERN` for real (not just in commented-out code) via the same ACK path as Mechanism A, whenever the sequencer's active pattern *is* the temp pattern:

```c
// avrCommsReceivingProtocol.c:626-654
if(patMsg != SEQ_TMP_PATTERN) patMsg &= 0x07;
...
menu_playedPattern = patMsg;                 // can legitimately be SEQ_TMP_PATTERN
...
if(parameter_values[PAR_FOLLOW] || tempBoundaryAck) {
   if(menu_activePage != PATTERN_SETTINGS_PAGE)
      menu_setShownPattern(patMsg);          // menu_setShownPattern(SEQ_TMP_PATTERN) — reachable for real
```

This is exactly what the background-load/temp-playback machinery does on purpose (Session 028/033, `MEMORY.md`): a background-swap-eligible load forces `seq_setNextPattern(SEQ_TMP_PATTERN, 0x0f)` so playback keeps running from the old pattern while the new file streams in. **If a step-parameter edit happens while `frontParser_shownPattern == SEQ_TMP_PATTERN`, it is written into `pat_tmpPattern` — a scratch buffer that Save's per-pattern loop (which only ever enumerates real patterns 0–7) never visits, and that isn't necessarily what any track is actually playing either.** The edit would appear to succeed in the menu (the AVR's own display state is self-consistent — see `FRONT_SEQ_REQUEST_STEP_PARAMS` at `frontPanelReceivingProtocol.c:2714-2725`, which reads back via the same `frontParser_shownPattern`), never affect audible playback, and **never show up anywhere in the saved file across all 8 patterns** — which is precisely what Part 1 found.

For **this specific file**, `PAR_FILE_LOAD_BACKGROUND = 1` ("pat" mode only) means loading `P075.ALL`/`P075.PRF` itself would *not* trigger this (`preset_backgroundSwapNeeded()`, `presetManager.c:168-196`, returns false for `WTYPE_ALL`/`WTYPE_PERFORMANCE` when the mode is `BACKGROUND_PAT`). So this specific mechanism is unlikely to be what happens **on load** of this exact file — but it remains fully reachable from a `.pat` load, or from any other action that drives the sequencer's active pattern to `SEQ_TMP_PATTERN`, at any point earlier in the same editing session, before this file was saved. Given Part 1 shows the edit is missing from *every* real pattern (not just relocated to a different one, which is what Mechanism A alone would produce), Mechanism B — the temp-pattern black hole — is the better fit for what actually happened to this song's probability edit specifically. Mechanism A remains a live, independently-confirmed risk for future edits.

### Corroborating detail already in the code

`FRONT_SEQ_CLEAR_AUTOM` explicitly guards on this exact condition before touching live state:

```c
// frontPanelReceivingProtocol.c:2609-2620
case FRONT_SEQ_CLEAR_AUTOM:
   if(voice < NUM_TRACKS && frontParser_shownPattern == seq_perTrackActivePattern[voice])
      seq_releaseAutomationLane(voice, automTrack ? 1 : 0);
   pat_clearAutomation(voice, frontParser_shownPattern, automTrack);
   break;
```
This proves `frontParser_shownPattern != seq_perTrackActivePattern[voice]` is a real, anticipated state in this codebase — just not one `FRONT_SEQ_PROB`/`FRONT_SEQ_VOLUME`/`FRONT_SEQ_NOTE`/`FRONT_SET_P1_VAL`/`FRONT_SET_P2_VAL` account for.

**Answer to Question 1: yes, confirmed.** Two distinct, code-verified mechanisms exist by which the pattern a step edit is written to can differ from the pattern actually driving playback (and hence from what Save records). Which one fired for this specific song is not provable from static analysis alone (see "Suggested next steps"), but both are real and neither requires an exotic user action — Mechanism A is a documented, single-gesture feature; Mechanism B is an automatic consequence of the background-load feature this user has partially enabled.

---

## Part 4 — Question 2: does Save strip probability that was actually set? **No evidence found.**

Traced the complete save round trip:

1. `preset_writePatternData()` (`presetManager.c:1148-1189`) loops `i = 0 .. 7167` (every `track × pattern × step` combination) and calls `preset_queryStepDataFromSeq(i)` then `f_write(..., &avrCommsParser_stepData, sizeof(StepData), ...)` — an unconditional, per-index write of whatever came back.
2. `preset_queryStepDataFromSeq()` (`presetManager.c:1050-1079`) sends the step index, then **blocks** (with a 31-tick retry-resend, not a give-up-and-write-stale-data timeout) until `avrCommsParser_newSeqDataAvailable` is actually set by the receive handler. There's no path that lets it fall through and write a stale/zeroed buffer.
3. On STM, `SYSEX_REQUEST_STEP_DATA` → `seq_sendStepInfoToFront()` → `frontPanelSending_sendStepInfo()` (`frontPanelSendingProtocol.c:488-511`) reads `pat_subStepPattern[currentPattern][currentTrack][currentStep]` **directly, by raw index** — no masking beyond the expected 7-bit SysEx split (verified bit-exact against the receive side in Part 2), no default substitution, no conditional skip.
4. `pat_getStepPtr()` (`PatternData.c:61-67`, shown above) is a bare address calculator. It has no side effects and nothing resets a step's contents as a side effect of being read.

I looked specifically for any "if track is muted / if step inactive / if pattern doesn't match X, report/write default" branch anywhere in this path, and found none. The only place `prob` gets programmatically reset to `127` anywhere in the STM firmware is `pat_resetNote()` (`PatternData.c:38-47`, called only from `pat_setMainStep()` when a step is newly turned on) and `seq_addNote()` (`sequencer.c:1974`, only during live MIDI/front-panel note recording) — neither of which is in the save path, and both of which only run when a step is being *(re)created*, not when it's being queried for a file write.

**Answer to Question 2: no.** There is no code path in Save that strips, clamps, or defaults a probability value that is genuinely present in `pat_patternSet`. Combined with Part 1's finding that the file's step data is byte-identical to `pat_resetNote()`'s untouched defaults (not just `prob`, but `note` and `volume` too), the far better-supported explanation is that the edit never reached `pat_patternSet` in the first place — i.e., Question 1's answer is the actual root cause, and there's nothing additionally wrong on the Save side.

---

## Part 5 — Question 3: is there a separate *defect* (not just feature interaction) that can strand edits on temp? **Yes, found one.**

Part 3 described two *structural* risk factors (per-track pattern following, and the shown pattern legitimately becoming `SEQ_TMP_PATTERN` during a background load) — both are consequences of features working as designed. This part looks specifically for an actual *bug*: a case where the sync mechanism that's supposed to keep `frontParser_shownPattern` correct fails to do its job even on its own terms.

**Found one.** It sits exactly where the user's hypothesis points: in the handshake that is supposed to keep the AVR's and STM's idea of "which pattern is shown/edited" in sync after a pattern-change ACK — including the ACK that fires when playback crosses the temp/normal boundary (i.e. exactly the background-load scenario).

### The ACK handler has a silent no-op branch

Revisiting the pattern-change ACK handler in full:

```c
// avrCommsReceivingProtocol.c:642-656
if(parameter_values[PAR_FOLLOW] || tempBoundaryAck) {

   if( menu_activePage != PATTERN_SETTINGS_PAGE)
   {
      menu_setShownPattern(patMsg);      // <- this is the ONLY thing that tells the STM
      led_clearSequencerLeds();          //    "the shown/edit pattern is now patMsg"
      avrComms_updatePatternLeds();      //    (sends SEQ_SET_SHOWN_PATTERN over UART)
      avrComms_sendData(SEQ_CC,SEQ_REQUEST_PATTERN_PARAMS,patMsg);
   } 
   else {
      //store the pending pattern update for shift button release handler
      menu_shownPattern = avrCommsParser_command.data2;   // <- LOCAL ONLY. No message sent to STM.
   }
}
```

Two things matter here:

1. `tempBoundaryAck` is computed independently of `PAR_FOLLOW` (`avrCommsReceivingProtocol.c:628-630`, `(oldPlayedPattern == SEQ_TMP_PATTERN) != (patMsg == SEQ_TMP_PATTERN)`), and is specifically designed to force a resync at the temp/normal boundary *even with Follow off* — this is the safety net the firmware already has for exactly the scenario the user is asking about. It works correctly **only if `menu_activePage != PATTERN_SETTINGS_PAGE`.**
2. If the AVR happens to be showing `PATTERN_SETTINGS_PAGE` at the moment this ACK arrives, the code takes the `else` branch: it updates the AVR's own **local** `menu_shownPattern` variable (so the AVR's own LCD/LED state looks perfectly normal and correct to the user) but never calls `menu_setShownPattern()` — so **`SEQ_SET_SHOWN_PATTERN` is never sent to the STM, and `frontParser_shownPattern` on the STM is never updated.** The comment even says the intent: *"store the pending pattern update for shift button release handler."*

### The promised flush does not exist

`PATTERN_SETTINGS_PAGE` is entered exactly one way — holding `SHIFT` while already on the PERF page (`menu_shiftPerf(1)`, `menu.c:894-913`), which repurposes the step LEDs to show the active track's rotation value (`PAR_TRACK_ROTATION`). **Correction/clarification:** this is *not* the Euclid generator page. `menu.h` defines `EUKLID_PAGE` and `PATTERN_SETTINGS_PAGE` as two separate enum values; the real Euclid page (`EUKLID_PAGE` / `SELECT_MODE_PAT_GEN`) is entered by holding `SHIFT` *then* pressing `PERF` via `menu_enterPatgenMode()` (`menu.c:851-869`), which sends `SEQ_EUKLID_RESET`/`SEQ_EUKLID_RESET_BEGIN_VISIT` — `menu_shiftPerf()` sends no such opcode. `MEMORY.md` and `COMMS_FLOW_SPEC.md` §4b already warn against exactly this mix-up; an earlier revision of this document made it. Its release handler is `menu_shiftPerf(0)`:

```c
// menu.c:915-922
else
{
   led_clearAllBlinkLeds();
   led_clearSelectLeds();
   menuIndex=menu_lastPerfIndex;
   menu_switchPage(PERFORMANCE_PAGE);
   led_initPerformanceLeds();
}
```

This is the actual "shift button release handler" the comment refers to. **It does not call `menu_setShownPattern()`, and it does not reference the locally-stashed `menu_shownPattern` value at all.** I also checked `menu_switchPage()` (`menu.c:3529-3577`) — the generic page-switch function — for any fallback flush logic on the way out of `PATTERN_SETTINGS_PAGE`; there is none. I found no other code path anywhere that reads back the value stashed by the `else` branch above and forwards it to the STM.

**Net effect:** the "pending update" described in the comment is never actually delivered. Once this branch is hit, `frontParser_shownPattern` on the STM is left wherever it was — potentially `SEQ_TMP_PATTERN` — and stays there until some *unrelated, later* pattern-change ACK happens to arrive while the AVR is off `PATTERN_SETTINGS_PAGE`. There is no guaranteed, bounded-time recovery.

### Why this directly answers the user's edge case

This defect fires on the **exact same `if(parameter_values[PAR_FOLLOW] || tempBoundaryAck)` gate** as both the Follow-driven sync and the temp-boundary safety net — so it silently defeats *both* of them at once, regardless of the Follow setting. Concretely, for the scenario the user described (Follow off, background load, pattern changeover):

1. A background-eligible load forces `seq_activePattern = SEQ_TMP_PATTERN` (Session 028/033 machinery) and playback continues from temp.
2. When the STM's active pattern later moves — either back to normal once the load settles, or into/out of temp for any other reason — it sends the `SEQ_CHANGE_PAT` ACK unconditionally (`sequencer.c:956`), and `tempBoundaryAck` correctly detects the crossing on the AVR side *regardless of Follow*.
3. **If the AVR is showing `PATTERN_SETTINGS_PAGE` at that exact moment** (e.g. the user is holding `SHIFT+PERF` to check/adjust Clap/Cym's rotation right around when the load finishes — a very plausible, ordinary thing to be doing while a file streams in), the resync silently no-ops. `menu_shownPattern` (AVR's own display) updates and looks correct; `frontParser_shownPattern` (STM's actual routing target for `FRONT_SEQ_PROB`/`VOLUME`/`NOTE`) does not.
4. The user releases `SHIFT`, goes to the step-probability page, dials in Clap/Cym's probability. The AVR's display is internally consistent (read-back uses the same stale/local state), so the edit *looks* like it worked.
5. Per Mechanism B (Part 3), if `frontParser_shownPattern` was left on `SEQ_TMP_PATTERN` specifically, the edit is written into `pat_tmpPattern` — invisible to Save's per-pattern loop (Part 4) and not necessarily what any track is actually playing. If it was left on some other stale-but-real pattern number instead, the edit lands in that pattern's real storage but still isn't what's playing (same audible symptom, but the byte would show up in a *different* pattern slot in the save file than expected — not what Part 1 found here, which is consistent with the `SEQ_TMP_PATTERN` variant of this defect specifically).
6. Nothing in the UI ever indicates this happened. The desync persists until an unrelated ordinary pattern button press generates a fresh ACK while the AVR is off `PATTERN_SETTINGS_PAGE`.

**This is not the Follow feature or the background-load feature misbehaving — both are working as designed.** It's a third, independent piece of code (the `PATTERN_SETTINGS_PAGE` special case in the ACK handler) that was evidently meant to defer-and-later-flush the sync, but the "later" half of that was never implemented. (Note: an earlier revision of this document speculated this was connected to the Session 033 `EUKLID_PAGE` rollback WIP noted in `MEMORY.md`. That connection doesn't hold up — `PATTERN_SETTINGS_PAGE` and `EUKLID_PAGE` are separate pages/features, see the correction in the previous section — so this appears to be its own, independently-introduced gap rather than fallout from that WIP.)

**Answer to Question 3: yes.** This is a genuine, independently-reproducible defect, not a consequence of Follow or background-loading being on or off. It fully explains how a probability (or volume/note) edit can be accepted by the menu, look completely normal to the user, and never reach *any* saved pattern — which is exactly what Part 1's forensic data shows.

---

## Suggested next steps

**To confirm the Part 5 defect specifically (highest priority — most concrete, most likely root cause):**
1. Enable background loading for `.all`/`.prf` (or use a `.pat` load, which this song already has background-enabled), start playback, trigger a load, and — while it's streaming in or right as it finishes — hold `SHIFT` while already on the PERF page (enters `PATTERN_SETTINGS_PAGE`, the rotation-display page — *not* the Euclid generator page) for a moment, then release, then go set a step's probability. If it fails to affect playback, the defect is confirmed.
2. Simpler bench test that doesn't even need a background load: get `frontParser_shownPattern` and `seq_activePattern`/`menu_playedPattern` to disagree by any means (e.g. use per-track pattern following, Mechanism A), hold `SHIFT+PERF` while the resulting ACK arrives, release, and check whether `frontParser_shownPattern` ever gets corrected afterward. It shouldn't, per the code read in Part 5.
3. Add a temporary debug readout of `frontParser_shownPattern` immediately before and after a `SHIFT+PERF` press/release cycle that overlaps a pattern-change ACK, to directly observe the no-op branch being taken.

**To confirm the broader Part 3 mechanisms:**
4. Reproduce the report's workflow while watching for whether the front panel is displaying the temp pattern indicator (`LED_STEP16` lights instead of a normal pattern-select LED — see `menu.c:908` / `avrCommsReceivingProtocol.c:564`) at the moment of editing. If it's lit, that's Mechanism B live.
5. Check whether the Clap/Cym VOICE button was ever held while pressing a step/pattern button in the session — that's Mechanism A.
6. Add a temporary debug readout of `frontParser_shownPattern` vs. `seq_perTrackActivePattern[4]` right after any probability edit; a mismatch confirms the edit is going to the wrong place regardless of which mechanism caused it.

**Possible fix directions, roughly in order of invasiveness:**
- **Fixes the Part 5 defect directly:** in `menu_shiftPerf(0)` (`menu.c:915-922`, the `SHIFT+PERF` release handler), call `menu_setShownPattern(menu_shownPattern)` before/while switching back to `PERFORMANCE_PAGE`, so the value stashed by the `else` branch in `avrCommsReceivingProtocol.c:653-655` actually gets flushed to the STM as the comment there promises. This is a small, targeted fix and directly closes the confirmed no-op path.
- Even more robust: don't defer at all — have the `PATTERN_SETTINGS_PAGE` branch of the ACK handler call `menu_setShownPattern(patMsg)` too (dropping the special case entirely), unless there's a UI reason found on hardware for why the Pattern Settings page specifically must not receive live shown-pattern updates.
- Closes Mechanism B as a second layer of defense: never let step-parameter edit opcodes (`FRONT_SEQ_PROB`/`VOLUME`/`NOTE`/`FRONT_SET_P1_VAL`/`P2_VAL`) resolve against `SEQ_TMP_PATTERN`; if `frontParser_shownPattern == SEQ_TMP_PATTERN` when one of these arrives, either reject it or redirect it to `seq_perTrackActivePattern[frontParser_activeTrack]`'s real pattern. This would make edits fail loudly/harmlessly instead of silently vanishing, even if some other, not-yet-found path also leaves `frontParser_shownPattern` stuck on temp.
- Closes the "leftover state" half of Mechanism A: on `FRONT_SEQ_FILE_DONE`, explicitly reset `seq_perTrackActivePattern[i]` to the loaded pattern for all tracks, so a fresh load always starts with shown == active per track.
- Most thorough, closes everything: make step-edit opcodes target `seq_perTrackActivePattern[frontParser_activeTrack]` instead of the global `frontParser_shownPattern`, or surface a clear on-screen warning whenever the viewed pattern isn't the one actually driving the active track, so an edit can never silently go somewhere invisible — this would make the whole class of bug (Part 3 and Part 5 alike) structurally impossible rather than patching each entry point.

---

## Part 6 — Fix feasibility: is "always sync regardless of page" simple and low-risk?

Short answer: **yes, but only if scoped correctly.** "Always sync" as literally stated is *not* the right fix — the `PATTERN_SETTINGS_PAGE` special case bundles two unrelated things together, and only one of them is actually broken.

### The `if` branch does two different jobs

```c
// avrCommsReceivingProtocol.c:644-650
if( menu_activePage != PATTERN_SETTINGS_PAGE)
{
   menu_setShownPattern(patMsg);         // (1) STATE SYNC — updates menu_shownPattern
                                          //     locally AND sends SEQ_SET_SHOWN_PATTERN
                                          //     to the STM (the part that's broken today)
   led_clearSequencerLeds();             // (2) LED REFRESH — repaints the step LEDs
   avrComms_updatePatternLeds();         //     for normal pattern display
   avrComms_sendData(SEQ_CC,SEQ_REQUEST_PATTERN_PARAMS,patMsg);
}
```

`PATTERN_SETTINGS_PAGE` (entered by holding `SHIFT` while already on the PERF page — see the Part 5 correction above, this is *not* the Euclid generator page) repurposes those same step LEDs to show rotation state (`menu_shiftPerf(1)`, `menu.c:902-909`). Running (2) while that page is up would visibly clobber its custom LED layout — a real, separate regression from the silent data-loss bug this investigation is about. Removing the `if`/`else` split entirely (routing *everything* through the normal path regardless of page) would fix the data-loss defect but introduce that visible one. That is **not** the low-risk version of this fix.

### The correctly-scoped fix

Keep the `if`/`else` split (so LEDs stay untouched on that page), but change only what the `else` branch does — replace the raw local assignment with the same state-sync call the `if` branch already uses, since that call has no LED side effects of its own:

```c
// avrCommsReceivingProtocol.c:651-655, proposed
else {
   // keep the STM's shown-pattern state in sync even while the Pattern
   // Settings page's LED display is active — menu_setShownPattern() only
   // updates menu_shownPattern and sends SEQ_SET_SHOWN_PATTERN; it touches
   // no LEDs, so this still doesn't disturb that page's rotation display.
   menu_setShownPattern(patMsg);   // patMsg, not avrCommsParser_command.data2 —
                                    // also fixes a pre-existing inconsistency where
                                    // this branch stored the raw, un-normalized value
}
```

This is a small, self-contained change:
- Reuses `menu_setShownPattern()`, an existing helper already exercised safely at 4+ other call sites (every file load, `buttonHandler.c:570`, and the `if` branch right above it).
- Touches only AVR-side code; the STM's `FRONT_SEQ_SET_SHOWN_PATTERN` handler is not page-aware and needs no changes.
- No opcode/protocol changes.
- Incidentally fixes the `patMsg` vs. raw `data2` inconsistency noted in Part 5.

### One interaction worth verifying on hardware, not just reasoning through

The STM's `FRONT_SEQ_SET_SHOWN_PATTERN` handler contains:

```c
if (frontParser_command.data2 == frontParser_shownPattern) {
   if(!preset_consumeTmpBoundaryPatternSwitchAck())
      seq_realign();
}
```

If the incoming value already matches what the STM has, it triggers `seq_realign()` (the documented "press the same pattern button again to realign" feature) — *unless* a temp-boundary-crossing ack is pending, in which case that's explicitly suppressed. This guard means a genuine boundary crossing handled by the fix above will **not** spuriously realign — the consume-flag check exists precisely to distinguish "boundary-crossing echo" from "user deliberately re-pressed the same pattern." The only residual gap: a *non-boundary*, Follow-driven ACK for a pattern that happens to already match, arriving while the page is up. Today that's silently dropped by the buggy `else`; after the fix it would be forwarded and could trigger a redundant realign. Worst case that's a harmless, rare, easily-observed extra realign — not data loss — but it's the one thing worth explicitly watching for during hardware testing rather than assuming away.

### Verdict

Low risk, small diff, no protocol changes, reuses proven code, and doesn't touch the LED behavior that almost certainly motivated the original special case. The only caveat is scoping it as a state-only fix (as above) rather than deleting the `if`/`else` split outright.

---

## Part 7 — Fix applied (Session 036)

The Part 6 fix was implemented, scoped exactly as described there (state-sync only, `if`/`else` split preserved).

**File changed:** `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c`, in the `SEQ_CHANGE_PAT` case of `avrComms_parseData()` (the pattern-change ACK handler discussed throughout Part 5/6).

**Change:** the `else` branch that runs while `menu_activePage == PATTERN_SETTINGS_PAGE` no longer assigns `menu_shownPattern = avrCommsParser_command.data2;` directly. It now calls `menu_setShownPattern(patMsg);` — the same helper the sibling `if` branch already uses, and the same helper used by every file-load path and `buttonHandler.c:570`. That helper only does two things: `menu_shownPattern = patternNr;` and `avrComms_sendData(SEQ_CC, SEQ_SET_SHOWN_PATTERN, menu_shownPattern);` — no LED calls, so the Pattern Settings page's repurposed rotation-indicator LEDs are still left untouched, matching the risk assessment in Part 6. This also switches the stored value from the raw, un-normalized `avrCommsParser_command.data2` to the already-normalized `patMsg` (matching the `if` branch), closing the small inconsistency noted in Part 5.

A large explanatory comment block was added directly above the changed line, covering why the branch existed, what changed, the input (`patMsg`), the output (local `menu_shownPattern` update + `SEQ_SET_SHOWN_PATTERN` sent to STM), and the affiliated STM-side code (`frontPanelReceivingProtocol.c`'s `FRONT_SEQ_SET_SHOWN_PATTERN` handler and `PatternData.c`'s `pat_getStepPtr()`), per the user's request that all changes carry adjacent comment blocks explaining the change.

**No STM-side (`mainboard/LxrStm32`) changes were needed.** `FRONT_SEQ_SET_SHOWN_PATTERN` (`frontPanelReceivingProtocol.c`) already handles `SEQ_TMP_PATTERN` and ordinary pattern values correctly and is not page-aware — it just needed to actually receive the message more often, which the AVR-side fix now ensures.

### Build verification

- `make -C front/LxrAvr avr -j4` — succeeded. Only pre-existing warnings remain (`-Wimplicit-fallthrough` on an unrelated `LED_SEQ_MAIN_*` switch case elsewhere in the same file, present before this change); no new warnings or errors from the changed code.
- `make firmware` — succeeded end to end, rebuilt `firmware image/FIRMWARE.BIN` from the freshly-built AVR binary and the existing (unchanged) STM32 binary.
- `git diff --stat` confirms only the intended files changed: `front/LxrAvr/avrComms/avrCommsReceivingProtocol.c` (the fix) and `firmware image/FIRMWARE.BIN` (rebuilt artifact). No STM32 source was touched.

### Hardware verification

**Not yet performed.** This fix has not been tested on hardware. Recommended checks, per Part 5/6's reproduction steps:
1. Reproduce the Part 5 scenario (get `frontParser_shownPattern` out of sync — e.g. via per-track pattern following or a background load's temp-boundary crossing — while holding `SHIFT+PERF`) and confirm a subsequent probability/volume/note edit now takes effect immediately after releasing `SHIFT`.
2. Confirm the `PATTERN_SETTINGS_PAGE` LED display (rotation indicator, `menu_shiftPerf(1)` — *not* the Euclid generator page) still looks correct while a pattern-change ACK arrives during that page — i.e. confirm no LED regression was introduced.
3. Watch for the narrow residual case flagged in Part 6 (a redundant same-pattern ACK while on this page could now trigger a harmless `seq_realign()` that previously no-opped) — confirm it's inaudible/inconsequential if it occurs.
4. General regression pass on ordinary pattern switching, Follow on/off, and the `PATTERN_SETTINGS_PAGE` rotation-display flow (hold `SHIFT` while on PERF), since this is a shared code path.

This fix addresses the Part 5 defect specifically. It does not by itself close Mechanism A (per-track pattern following) or fully harden Mechanism B (editing while shown pattern is legitimately `SEQ_TMP_PATTERN` outside of this specific stuck-state defect) — those remain open, lower-priority hardening items per the "Possible fix directions" list above if further robustness is wanted later.

---

## Appendix — things checked and ruled out

- `.ALL`/`.PRF` fixed step-data byte offsets (`VERSION_4_*_STEPDATA_OFFSET`) are stale in *name* (file version is 5, constants say "VERSION_4") but were verified byte-exact for this specific file via total-size cross-check. Not the bug here, but worth a defensive review if the `.prf`/`.all` header format changes again.
- Full AVR↔STM SysEx pack/unpack of the 7-byte step struct, in both load and save directions — verified bit-for-bit symmetric.
- The RNG-vs-probability trigger comparison in `seq_process()` — logically correct.
- The one-sub-step rotation difference on tracks 4/5/6 in the attached file — looks like an intentional per-track rotation setting, unrelated to probability.
- The full Save round trip (`preset_writePatternData` → STM `SYSEX_REQUEST_STEP_DATA` → `frontPanelSending_sendStepInfo`) — no stripping/defaulting logic found anywhere in it (Part 4).
- Whether `tempBoundaryAck` itself is computed correctly on the AVR side (`avrCommsReceivingProtocol.c:628-630`) — logic is correct and does not depend on `PAR_FOLLOW`; it's the `PATTERN_SETTINGS_PAGE` branch downstream of it that drops the resync (Part 5).
- Whether any code path other than `menu_shiftPerf(0)` and `menu_switchPage()` could flush the `PATTERN_SETTINGS_PAGE`-deferred `menu_shownPattern` value to the STM — none found.
