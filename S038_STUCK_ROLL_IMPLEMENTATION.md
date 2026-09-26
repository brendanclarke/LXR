# S038 — Stuck MIDI Roll: Implementation Schedule

**Date**: 2026-09-26
**Branch**: `dev-roll-midi`
**Baseline**: every line number below refers to the files as they are at commit `9576ae5` (current HEAD, clean tree).
**Companion plan**: [S038_STUCK_MIDI_ROLL_BUG.md](S038_STUCK_MIDI_ROLL_BUG.md) (root cause and rationale).
**Status**: schedule only. No code has been changed.

> **How to apply**: within each file, apply the changes **from the bottom of the file upward**, so that the baseline line numbers cited for earlier changes remain valid. Every code block below includes its documentation comment. That comment is meant to go into the source verbatim, next to the code it describes.

---

## 0. Decisions Settled by the User (2026-09-26)

| # | Decision | Consequence in this schedule |
|---|---|---|
| D1 | **Drop Settled Decision #4.** A Note-On with velocity 0 is a Note-Off (MIDI 1.0). | §2 C5: `isNoteOff` in `midiParser_parseMidiMessage()`. |
| D2 | MIDI roll holds are keyed per MIDI key (channel + note) and release exactly the voices their note-on claimed. | §2 C1–C4 and C6: the held-key table replaces the per-voice counters. |
| D3 | **MIDI-held rolls release when the sequencer stops.** | §4 S4: `seq_setRunning(0)` calls `midiParser_clearMidiRollHolds()`. |
| D4 | **MIDI rolls can be re-triggered, and are audible, while the sequencer is stopped.** | §4 S1–S3, S5, S6: a stopped-transport roll clock inside `sequencer.c`. |

### Behaviour the implementation must produce

1. Pressing a roll key starts the roll. Releasing it (with `8n kk vv` **or** `9n kk 00`) stops it.
2. Several roll keys may hold the same voice. The voice keeps rolling until the **last** of those keys is released.
3. A note-off releases the voices that its own note-on claimed. It does not recompute them from the current routing, so changing the active track mid-hold cannot strand a roll.
4. A repeated note-on for a key that is already held is not counted twice.
5. When the sequencer stops, every MIDI roll hold is cleared. Later note-offs for those keys are ignored.
6. While the sequencer is stopped, a new roll-key press rolls immediately. The repeats are locked to the current tempo and roll rate. Releasing the key stops the roll.
7. If a MIDI roll is still held when the sequencer starts, it is handed to the normal roll engine and continues on the quantize grid.
8. Front-panel (manual) rolls are unchanged in every respect, including their stopped-transport behaviour: they are silent until start.

---

## 1. File Map

| File | Changes | Section |
|---|---|---|
| `mainboard/LxrStm32/src/MIDI/MidiParser.c` | C1–C7 | §2 |
| `mainboard/LxrStm32/src/MIDI/MidiParser.h` | H1 | §3 |
| `mainboard/LxrStm32/src/Sequencer/sequencer.c` | S1–S6 | §4 |
| `mainboard/LxrStm32/src/Sequencer/sequencer.h` | SH1–SH4 | §5 |
| `knowledge_files/comms_spec_reference/MIDI_TABLE.md` | D-1 | §6 |
| `MIDI_ROLL_TRIGGER.md` | D-2 | §6 |
| `MIDI_ROLL_TRIGGER_IMPLEMENTATION.md` | D-3 | §6 |
| `S038_STUCK_MIDI_ROLL_BUG.md` | D-4 | §6 |
| `MEMORY.md` | D-5 | §6 |

**These files are confirmed to need no change** (each was checked):

- `MIDI/ChannelMidiParser.c`. `channelMidiParser_noteOff()` (line 78) already forces `vel = 0` and delegates to `channelMidiParser_noteOn()` (line 45), which treats `vel == 0` as "no trigger, record with `isNoteOff = 1`, echo velocity 0". Routing a velocity-0 Note-On to `noteOff()` instead of `noteOn()` is therefore **behaviourally identical** for normal notes. The Session 037 comment at lines 64–67 stays accurate.
- `MIDI/GlobalMidiParser.c`. NRPN 93 is unchanged. The MTC-timeout stop at line 507 calls `seq_setRunning(0)` and so inherits D3 automatically.
- `Sequencer/clockSync.c`. `sync_midiStartStop()` (lines 91–99) calls `seq_setRunning()` and inherits D3 and D4. `sync_tick()` (line 77) keeps `seq_tempo` updated from the external clock even while stopped, so the stopped roll clock follows external tempo.
- `uARTFrontSYX/frontPanelReceivingProtocol.c`. The existing clear points stay as they are: channel change at line 2593, channel off at line 2610, note override at line 2980, offset at line 2761. `FRONT_SEQ_ROLL_ON_OFF` (line 2773) remains the manual source.
- Everything on the AVR side (`front/LxrAvr/**`). No protocol or menu change.

---

## 2. `mainboard/LxrStm32/src/MIDI/MidiParser.c`

### C7 — lines 830–839 — MODIFY `midi_setFilter()`: release MIDI roll holds when note reception is filtered off

Apply first (it is the bottom-most change).

**Remove** lines 836–837:

```c
   else // set the low nibble to value
      midiParser_txRxFilter = (value & 0x0F) | (midiParser_txRxFilter & 0xF0);
```

**Add** in their place:

```c
   else // set the low nibble to value
   {
      /* MIDI ROLL HOLD RELEASE ON RX NOTE-FILTER DISABLE
         WHAT: when the RX nibble changes so that bit 0 (receive notes) goes
               from enabled to disabled, every parser-owned MIDI roll hold is
               released before the new filter value is installed.
         WHY:  midiParser_parseMidiMessage() only enters its note block while
               (midiParser_txRxFilter & 0x01) is set. A roll key held at the
               moment note reception is switched off would never see its
               note-off, so its roll would stick until the next stop or
               mapping change. Releasing here keeps the rule "every accepted
               roll key has a guaranteed release path".
         INPUT:  value, the new RX nibble (low 4 bits significant).
         OUTPUT: midiParser_txRxFilter updated. MIDI-owned rolls released
                 through midiParser_clearMidiRollHolds() when bit 0 turns off.
         ACCESSORS: called from the front-panel MIDI filter setting path.
         AFFILIATES: midiParser_clearMidiRollHolds() (this file),
                     seq_rollMidiChange() (Sequencer/sequencer.c). */
      if((midiParser_txRxFilter & 0x01) && !(value & 0x01))
         midiParser_clearMidiRollHolds();

      midiParser_txRxFilter = (value & 0x0F) | (midiParser_txRxFilter & 0xF0);
   }
```

---

### C6 — lines 486–489 and line 547 — MODIFY the last-consumer roll section of `midiParser_parseMidiMessage()`

The voice-mask builder at lines 490–545 is **kept unchanged**. Only its entry condition and its final call change. A release must never depend on the current routing, the offset, or `normalConsumed`, so the release path becomes an unconditional key lookup that runs *before* the note-on claim path.

**Remove** lines 486–489:

```c
            /* Last-consumer MIDI roll-note path. Normal note routing wins; only
               unconsumed literal NOTE_ON/NOTE_OFF messages are tested against
               the positive-offset roll map. */
            if(!normalConsumed && midiParser_rollOffsetEnabled())
```

**Add** in their place:

```c
            /* MIDI ROLL-NOTE PATH (release first, then last-consumer claim)
               WHAT: a note-off (including NOTE_ON velocity 0, see isNoteOff
                     above) releases the held roll key (chanonly, note) if one
                     exists, and does nothing else on the roll path. A note-on
                     that no normal route consumed is matched against the
                     positive-offset roll map. The resulting voice mask is
                     claimed by that key in the held-key table.
               WHY:  Session 038 stuck-roll bug. Releases used to recompute a
                     voice mask from the *current* routing and decrement
                     per-voice counters, so any sender using NOTE_ON velocity 0
                     as note-off (e.g. the Octatrack) added a second hold
                     instead of releasing. A routing drift between press and
                     release (such as an active-track change on the global
                     chromatic route) also released the wrong voice. A release
                     is now a lookup of what the press claimed, and it is
                     deliberately not gated by normalConsumed, the offset
                     state, or routing: any accepted roll key must always be
                     releasable.
               INPUT:  chanonly (1..16), msg.data1 (incoming note), isNoteOff,
                       normalConsumed, and the offset/channel/override
                       tables read by the unchanged mask builder below.
               OUTPUT: held-key table updated; seq_rollMidiChange() called for
                       each voice whose MIDI hold state changes (or, on a
                       press, is re-asserted).
               ACCESSORS: the only caller of midiParser_rollKeyOn() and
                          midiParser_rollKeyOff().
               AFFILIATES: midiParser_rollKeyOn/Off(),
                           midiParser_rollSyncVoices() (this file);
                           seq_rollMidiChange() (Sequencer/sequencer.c). */
            if(isNoteOff)
            {
               midiParser_rollKeyOff(chanonly, msg.data1);
            }
            else if(!normalConsumed && midiParser_rollOffsetEnabled())
```

Lines 490–545 (from `{` through the closing `}` of the voice-channel `for`) stay exactly as they are.

**Remove** line 547:

```c
               midiParser_applyRollVoiceMask(rollVoiceMask, isNoteOff);
```

**Add** in its place:

```c
               /* Claim the computed voices for this key. An empty mask (no
                  voice maps to this shifted note) claims nothing. */
               midiParser_rollKeyOn(chanonly, msg.data1, rollVoiceMask);
```

---

### C5 — line 401 — MODIFY: a Note-On with velocity 0 is a Note-Off

**Remove** line 401:

```c
            const uint8_t isNoteOff = (msgonly == NOTE_OFF);
```

**Add** in its place:

```c
            /* NOTE-OFF CLASSIFICATION (MIDI 1.0: NOTE_ON velocity 0 == NOTE_OFF)
               WHAT: a message is a note-off if its status is NOTE_OFF, or if
                     its status is NOTE_ON and its velocity (data2) is 0.
               WHY:  many senders transmit releases as NOTE_ON velocity 0 so
                     that running status stays unbroken (the Octatrack is the
                     Session 038 reproduction). The MIDI roll feature
                     originally read statuses literally, so every such release
                     was counted as a second press and the roll stuck. That
                     "literal status" design decision is withdrawn (Session
                     038, D1). Do NOT reintroduce literal-status handling.
                     For normal notes this changes nothing observable:
                     channelMidiParser_noteOff() forces vel = 0 and delegates
                     to channelMidiParser_noteOn(), which already treats
                     vel == 0 as "no trigger, record as note-off, echo vel 0".
               INPUT:  msgonly (status high nibble), msg.data2 (velocity).
               OUTPUT: isNoteOff selects noteOff() on the normal routes and
                       the release branch on the MIDI roll path.
               ACCESSORS: read only inside this note block.
               AFFILIATES: channelMidiParser_noteOn/Off()
                           (MIDI/ChannelMidiParser.c); seq_addNote()'s
                           isNoteOff contract (Sequencer/sequencer.c) is
                           unaffected, since both routes already pass 1. */
            const uint8_t isNoteOff = (msgonly == NOTE_OFF)
                                   || (msgonly == NOTE_ON && msg.data2 == 0);
```

---

### C4 — lines 210–224 — MODIFY `midiParser_clearMidiRollHolds()`

**Remove** lines 210–224 (the comment and the whole function body that loops over `midiParser_rollHoldCount`).

**Add** in their place:

```c
/* RELEASE EVERY MIDI ROLL HOLD
   WHAT: empties the held-key table, then releases every voice that MIDI was
         holding, through seq_rollMidiChange(voice, 0).
   WHY:  some events invalidate the mapping between a held key and its
         eventual note-off, or are defined as "all MIDI rolls stop". A
         cleared key's later note-off finds no entry and is ignored, so it
         can neither strand nor wrongly release a roll. Clearing the parser
         table (not only the sequencer mask) is also what makes a key
         re-triggerable after a stop: a stale entry would make the next
         press of that key look like a duplicate.
   INPUT:  none.
   OUTPUT: midiParser_rollHeldKeys[] all free; midiParser_rollHeldVoiceMask
           = 0; seq_rollMidiHeld cleared bit by bit via seq_rollMidiChange().
           Manual (front-panel) roll ownership is untouched.
   ACCESSORS (all existing unless marked):
     - midi_clearCache() (this file; reached from seq_init())
     - midiParser_setRollNoteOffset() (this file; offset change)
     - midi_setFilter() (this file; RX note filter disabled) [S038 new]
     - FRONT_SEQ_MIDI_CHAN / FRONT_SEQ_MIDI_CHAN_OFF / FRONT_SEQ_TRACK_NOTE1..7
       (uARTFrontSYX/frontPanelReceivingProtocol.c)
     - seq_setRunning(0) (Sequencer/sequencer.c; sequencer stop) [S038 new]
   AFFILIATES: midiParser_rollSyncVoices() (this file), seq_rollMidiChange()
               (Sequencer/sequencer.c). Declared in MidiParser.h. */
void midiParser_clearMidiRollHolds(void)
{
   uint8_t slot;

   for(slot = 0; slot < MIDI_ROLL_MAX_HELD_KEYS; ++slot)
   {
      midiParser_rollHeldKeys[slot].chan = 0;
      midiParser_rollHeldKeys[slot].voiceMask = 0;
   }

   midiParser_rollSyncVoices(0);
}
```

`midi_clearCache()` (lines 192–208) and `midiParser_setRollNoteOffset()` (lines 226–238) stay as they are. Both already call this function.

---

### C3 — lines 153–190 — REMOVE the per-voice counter helpers; ADD the held-key helpers

**Remove** lines 153–190 completely:

- `midiParser_rollVoiceOn()` (153–163)
- `midiParser_rollVoiceOff()` (165–173)
- `midiParser_applyRollVoiceMask()` (175–190)

**Add** in their place (between `midiParser_chromaticRollCandidate()`, which ends at line 151, and `midi_clearCache()` at line 192):

```c
/* FIND A HELD ROLL KEY
   WHAT: linear search of the held-key table for an occupied slot whose
         (chan, note) matches.
   WHY:  releases and duplicate presses are resolved by MIDI key identity,
         not by voice (Session 038).
   INPUT:  chan 1..16 (the parser's chanonly), note 0..127.
   OUTPUT: slot index 0..MIDI_ROLL_MAX_HELD_KEYS-1, or -1 if not held.
   ACCESSORS: midiParser_rollKeyOn(), midiParser_rollKeyOff().
   AFFILIATES: midiParser_rollHeldKeys[] (this file). */
static int8_t midiParser_rollFindHeldKey(uint8_t chan, uint8_t note)
{
   int8_t slot;

   for(slot = 0; slot < MIDI_ROLL_MAX_HELD_KEYS; ++slot)
   {
      if(midiParser_rollHeldKeys[slot].chan == chan
         && midiParser_rollHeldKeys[slot].note == note)
         return slot;
   }
   return -1;
}

/* FIND A FREE HELD-KEY SLOT
   WHAT: returns the first slot whose chan is 0 (free).
   WHY:  a new roll key needs storage for the voices it claims. The caller
         treats a full table as "do not start the roll", because a roll that
         cannot be recorded here would have no release path.
   INPUT:  none.
   OUTPUT: free slot index, or -1 if all MIDI_ROLL_MAX_HELD_KEYS are in use.
   ACCESSORS: midiParser_rollKeyOn().
   AFFILIATES: midiParser_rollHeldKeys[] (this file). */
static int8_t midiParser_rollFindFreeKeySlot(void)
{
   int8_t slot;

   for(slot = 0; slot < MIDI_ROLL_MAX_HELD_KEYS; ++slot)
   {
      if(midiParser_rollHeldKeys[slot].chan == 0)
         return slot;
   }
   return -1;
}

/* UNION OF ALL HELD-KEY VOICE MASKS
   WHAT: ORs the voiceMask of every occupied slot.
   WHY:  a voice is MIDI-held while *any* held key claims it. This is the
         single source of truth for the sequencer's MIDI ownership bit.
   INPUT:  none.
   OUTPUT: bit v set means voice v is claimed by at least one held key.
   ACCESSORS: midiParser_rollSyncVoices().
   AFFILIATES: midiParser_rollHeldKeys[] (this file). */
static uint8_t midiParser_rollHeldKeysUnion(void)
{
   uint8_t slot;
   uint8_t mask = 0;

   for(slot = 0; slot < MIDI_ROLL_MAX_HELD_KEYS; ++slot)
   {
      if(midiParser_rollHeldKeys[slot].chan != 0)
         mask |= midiParser_rollHeldKeys[slot].voiceMask;
   }
   return mask;
}

/* SYNCHRONISE SEQUENCER MIDI OWNERSHIP WITH THE HELD-KEY TABLE
   WHAT: recomputes the union of claimed voices. Voices that left the union
         are released with seq_rollMidiChange(v, 0). Voices in assertMask
         that are still in the union are (re)asserted with
         seq_rollMidiChange(v, 1).
   WHY:  it replaces the per-voice 0->1 / 1->0 counter transitions. The
         deliberate re-assert on every press (not only on 0->1) is what lets
         a new key press re-fire a voice that is already MIDI-held:
           - running, one-shot rate: seq_setRoll() clears seq_rollTriggered
             after its single hit, so re-asserting re-arms it;
           - stopped: seq_rollMidiChange() re-arms the stopped roll clock
             for an immediate hit (Session 038, D4).
         For a running continuous roll, re-asserting a bit that is already
         set is a no-op in seq_rollApplyAggregate().
   INPUT:  assertMask, the voices claimed by the press being processed
           (0 for a release or a clear).
   OUTPUT: midiParser_rollHeldVoiceMask updated; seq_rollMidiChange() calls
           for released and asserted voices.
   ACCESSORS: midiParser_rollKeyOn(), midiParser_rollKeyOff(),
              midiParser_clearMidiRollHolds().
   AFFILIATES: seq_rollMidiChange() / seq_rollApplyAggregate()
               (Sequencer/sequencer.c). */
static void midiParser_rollSyncVoices(uint8_t assertMask)
{
   const uint8_t newMask = midiParser_rollHeldKeysUnion();
   const uint8_t releaseMask =
      (uint8_t)(midiParser_rollHeldVoiceMask & (uint8_t)~newMask);
   uint8_t voice;

   midiParser_rollHeldVoiceMask = newMask;

   for(voice = 0; voice < 7; ++voice)
   {
      const uint8_t voiceBit = (uint8_t)(1u << voice);

      if(releaseMask & voiceBit)
         seq_rollMidiChange(voice, 0);
      else if(assertMask & newMask & voiceBit)
         seq_rollMidiChange(voice, 1);
   }
}

/* CLAIM VOICES FOR A PRESSED ROLL KEY
   WHAT: records (chan, note) as held and ORs voiceMask into its claimed
         voices, then synchronises sequencer ownership, asserting voiceMask.
   WHY:  the note-off for this key must later release exactly these voices,
         whatever the routing looks like by then. A repeated note-on for a
         key that is already held does not take a second slot, so one
         note-off always suffices.
   INPUT:  chan 1..16, note 0..127, voiceMask (bits 0..6) produced by the
           last-consumer matcher in midiParser_parseMidiMessage().
   OUTPUT: table slot occupied or updated; seq_rollMidiChange(v, 1) for each
           claimed voice. With an empty mask, or a full table for a new key,
           nothing happens and no roll starts.
   ACCESSORS: midiParser_parseMidiMessage() note-on branch.
   AFFILIATES: midiParser_rollFindHeldKey(), midiParser_rollFindFreeKeySlot(),
               midiParser_rollSyncVoices() (this file). */
static void midiParser_rollKeyOn(uint8_t chan, uint8_t note, uint8_t voiceMask)
{
   int8_t slot;

   voiceMask &= 0x7f;
   if(!voiceMask)
      return;

   slot = midiParser_rollFindHeldKey(chan, note);
   if(slot < 0)
   {
      slot = midiParser_rollFindFreeKeySlot();
      if(slot < 0)
         return;

      midiParser_rollHeldKeys[slot].chan = chan;
      midiParser_rollHeldKeys[slot].note = note;
      midiParser_rollHeldKeys[slot].voiceMask = 0;
   }

   midiParser_rollHeldKeys[slot].voiceMask |= voiceMask;
   midiParser_rollSyncVoices(voiceMask);
}

/* RELEASE A ROLL KEY
   WHAT: frees the slot for (chan, note) if it is held, then synchronises
         sequencer ownership. Only voices that no other held key still
         claims are released.
   WHY:  a release must undo exactly what its own press claimed (Session 038).
         An unmatched note-off (never held, or cleared by a stop or a
         mapping change) is ignored instead of decrementing some other
         press's hold.
   INPUT:  chan 1..16, note 0..127.
   OUTPUT: slot freed; seq_rollMidiChange(v, 0) for each voice that became
           unheld. No effect for an unknown key.
   ACCESSORS: midiParser_parseMidiMessage() note-off branch, which includes
              NOTE_ON velocity 0.
   AFFILIATES: midiParser_rollFindHeldKey(), midiParser_rollSyncVoices()
               (this file). */
static void midiParser_rollKeyOff(uint8_t chan, uint8_t note)
{
   const int8_t slot = midiParser_rollFindHeldKey(chan, note);

   if(slot < 0)
      return;

   midiParser_rollHeldKeys[slot].chan = 0;
   midiParser_rollHeldKeys[slot].voiceMask = 0;
   midiParser_rollSyncVoices(0);
}
```

---

### C1/C2 — lines 91–95 — MODIFY the parser-owned MIDI roll state declarations

**Remove** lines 91–95:

```c
/* Parser-owned MIDI roll state. Raw 0 disables shifted roll notes; raw 1..127
   is the positive semitone offset above the normal trigger note. Hold counters
   allow overlapping roll-note presses for the same voice. */
static uint8_t midiParser_rollNoteOffsetRaw = 0;
static uint8_t midiParser_rollHoldCount[7] = {0};
```

**Add** in their place:

```c
/* PARSER-OWNED MIDI ROLL STATE
   midiParser_rollNoteOffsetRaw:
     raw 0 disables shifted roll notes; raw 1..127 is the positive semitone
     offset above the normal trigger note. Set only through
     midiParser_setRollNoteOffset() (FRONT_SEQ_ROLL_NOTE_OFFSET).

   HELD-KEY TABLE (Session 038; replaces the per-voice hold counters)
   WHAT: one slot per physically held MIDI roll key, identified by
         (chan, note), storing the voices that key claimed at note-on.
         midiParser_rollHeldVoiceMask caches the union of all slot masks,
         which equals the sequencer's seq_rollMidiHeld mask.
   WHY:  per-voice counters could not tell which press a release belonged
         to. A NOTE_ON velocity 0 release was counted as a press, releases
         recomputed their voices from the current routing, and duplicate
         presses needed duplicate releases. Each of these could leave a roll
         stuck. Keying on the MIDI key makes release exact and idempotent.
   SIZE: MIDI_ROLL_MAX_HELD_KEYS = 16 simultaneous roll keys (3 bytes each).
         If the table is full, a new press is ignored rather than starting a
         roll with no release path.
   ENCODING: chan 1..16 matches the parser's chanonly; chan 0 marks a free
             slot. Zero-initialised storage therefore starts with every slot
             free.
   ACCESSORS: midiParser_rollFindHeldKey(), midiParser_rollFindFreeKeySlot(),
              midiParser_rollHeldKeysUnion(), midiParser_rollSyncVoices(),
              midiParser_rollKeyOn(), midiParser_rollKeyOff(),
              midiParser_clearMidiRollHolds() (all in this file).
   AFFILIATES: seq_rollMidiChange() / seq_rollMidiHeld
               (Sequencer/sequencer.c). */
#define MIDI_ROLL_MAX_HELD_KEYS 16

typedef struct
{
   uint8_t chan;       /* 1..16 = held on that MIDI channel; 0 = free slot */
   uint8_t note;       /* incoming (shifted) roll note number, 0..127      */
   uint8_t voiceMask;  /* voices 0..6 claimed by this key at note-on        */
} MidiRollHeldKey;

static uint8_t midiParser_rollNoteOffsetRaw = 0;
static MidiRollHeldKey midiParser_rollHeldKeys[MIDI_ROLL_MAX_HELD_KEYS];
static uint8_t midiParser_rollHeldVoiceMask = 0;
```

The helper functions in C3 and C4 need forward visibility of `midiParser_rollSyncVoices()` only in the order given. C3 defines `midiParser_rollSyncVoices()` before `midiParser_rollKeyOn/Off()`, and C4's `midiParser_clearMidiRollHolds()` comes after C3 in the file, so **no additional prototypes are required**. `midi_clearCache()` (line 192) calls `midiParser_clearMidiRollHolds()` before its definition, which is already legal through the declaration in `MidiParser.h`.

---

## 3. `mainboard/LxrStm32/src/MIDI/MidiParser.h`

### H1 — lines 100–104 — MODIFY the MIDI roll API contract comment

**Remove** lines 100–104:

```c
/* MIDI roll-note offset and hold ownership.
   MidiParser owns these because it is the only layer that knows which incoming
   note/channel pairs map to shifted roll triggers. */
void midiParser_setRollNoteOffset(uint8_t rawOffset);
void midiParser_clearMidiRollHolds(void);
```

**Add** in their place:

```c
/* MIDI ROLL-NOTE OFFSET AND HOLD OWNERSHIP
   MidiParser owns these because it is the only layer that knows which
   incoming note/channel pairs map to shifted roll triggers.

   Hold model (Session 038): each pressed roll key (MIDI channel + note) is
   stored with the voices it claimed. Its note-off, which may be NOTE_OFF or
   NOTE_ON velocity 0, releases exactly those voices. A voice stays
   MIDI-held while any held key claims it. MIDI ownership is reported to the
   sequencer through seq_rollMidiChange() and is kept separate from manual
   front-panel roll ownership.

   midiParser_setRollNoteOffset()
     INPUT:  rawOffset. 0 = off, 1..127 = positive semitone offset (masked to
             7 bits).
     OUTPUT: the stored offset. If the value changed, every MIDI roll hold is
             released first.
     ACCESSOR: FRONT_SEQ_ROLL_NOTE_OFFSET
               (uARTFrontSYX/frontPanelReceivingProtocol.c).

   midiParser_clearMidiRollHolds()
     WHAT: releases every MIDI roll hold and empties the held-key table.
           Later note-offs for cleared keys are ignored, and a fresh press of
           any key starts a new roll.
     ACCESSORS: offset change, MIDI channel change or off, track note
                override change (frontPanelReceivingProtocol.c);
                midi_clearCache() and the RX note-filter disable
                (MidiParser.c); sequencer stop, seq_setRunning(0)
                (Sequencer/sequencer.c). */
void midiParser_setRollNoteOffset(uint8_t rawOffset);
void midiParser_clearMidiRollHolds(void);
```

---

## 4. `mainboard/LxrStm32/src/Sequencer/sequencer.c`

Apply the changes bottom-up in this order: S6, S5, S4, S3, S2, S1.

### S6 — insert after line 1660 (after the closing `}` of `seq_rollMidiChange()`) — ADD `seq_tickStoppedRolls()`

```c
//-------------------------------------------------------------------------------
/* STOPPED-TRANSPORT MIDI ROLL CLOCK (Session 038, D4)
   WHAT: while the sequencer is stopped, plays rolls for every voice MIDI
         currently holds (seq_rollMidiHeld):
           - a newly held (or re-armed) voice fires immediately;
           - further hits repeat every seq_tempRate sub-steps, timed from
             systick_ticks at the current tempo (the same sub-step length
             seq_nextStep() uses: 3 internal 96ppq ticks, SEQ_PRESCALER_MASK);
           - one-shot rate (0xff) fires once per press, then parks;
           - a voice that MIDI stops holding is dropped at once.
         Hits use the front-panel stopped-preview voicing,
         seq_triggerVoice(voice, seq_rollVelocity, seq_rollNote). The same
         call is made by FRONT_SEQ_SET_ACTIVE_TRACK when stopped. This keeps
         hihat choke, the note-off-before-note-on sequence, track locking,
         trigger-out and MIDI echo behaviour identical to other stopped hits.
   WHY:  the normal roll engine lives in seq_nextStep(), which returns
         immediately while stopped, so a MIDI roll pressed while stopped
         would be silent until start. The user asked for MIDI rolls to stay
         re-triggerable with the sequencer stopped. This clock is completely
         separate from the transport timing (seq_deltaT, seq_lastTick,
         seq_prescaleCounter, seq_stepIndex, seq_rollState, seq_rollCounter),
         so it cannot disturb start alignment or external sync.
   SCOPE: MIDI ownership only. Manual front-panel roll buttons keep their
          existing behaviour (silent while stopped).
   MODE NOTE: seq_rollMode is deliberately not consulted. TRIG, NOTE, VEL
              and BOTH read the step under the playhead, which is frozen
              while stopped, so the stopped roll always uses roll velocity
              and roll note.
   RATE NOTE: seq_tempRate (the latest requested rate) is used, because the
              quantized seq_tempRate -> seq_rollRate latch in seq_nextStep()
              does not run while stopped.
   INPUT:  seq_rollMidiHeld, seq_tempRate, seq_tempo, seq_rollVelocity,
           seq_rollNote, systick_ticks.
   OUTPUT: voice triggers through seq_triggerVoice(); updates
           seq_rollStoppedActive, seq_rollStoppedCounter[],
           seq_rollStoppedLastTick, seq_rollStoppedPhase.
   ACCESSORS: seq_tick(), only while !seq_running. Re-armed by
              seq_rollMidiChange(); reset by seq_setRunning().
   AFFILIATES: midiParser_rollKeyOn/Off() (MIDI/MidiParser.c, the source of
               seq_rollMidiHeld), seq_triggerVoice(), seq_calcDeltaT()
               (timing convention), seq_setRollRate() (rate table). */
static void seq_tickStoppedRolls(void)
{
   uint8_t i;
   uint8_t armMask;
   uint8_t subStepDue = 0;

   seq_rollStoppedActive &= seq_rollMidiHeld;
   if(!seq_rollMidiHeld || !seq_tempo)
      return;

   armMask = (uint8_t)(seq_rollMidiHeld & (uint8_t)~seq_rollStoppedActive);
   if(armMask)
   {
      /* Anchor the stopped clock on the first voice armed from idle, so the
         repeat grid starts at the first hit rather than at some stale time. */
      if(!seq_rollStoppedActive)
      {
         seq_rollStoppedLastTick = systick_ticks;
         seq_rollStoppedPhase = 0.f;
      }

      for(i = 0; i < NUM_TRACKS; i++)
      {
         if(armMask & (uint8_t)(1u << i))
         {
            seq_triggerVoice(i, seq_rollVelocity, seq_rollNote);
            seq_rollStoppedCounter[i] = seq_tempRate; /* 0xff parks one-shot */
         }
      }
      seq_rollStoppedActive |= armMask;
   }

   {
      /* Sub-step length in systick units, derived exactly as in
         seq_calcDeltaT() (96ppq tick length, times SEQ_PRESCALER_MASK = 3
         ticks per sequencer sub-step), without shuffle. */
      const float subStepTicks =
         ((1000.f * 60.f) / (float)seq_tempo) / 96.f * 4.f
         * (float)SEQ_PRESCALER_MASK;
      const uint32_t now = systick_ticks;

      seq_rollStoppedPhase += (float)(uint32_t)(now - seq_rollStoppedLastTick);
      seq_rollStoppedLastTick = now;

      if(seq_rollStoppedPhase >= subStepTicks)
      {
         seq_rollStoppedPhase -= subStepTicks;
         /* Never burst to catch up after a main-loop stall. */
         if(seq_rollStoppedPhase >= subStepTicks)
            seq_rollStoppedPhase = 0.f;
         subStepDue = 1;
      }
   }

   if(!subStepDue)
      return;

   for(i = 0; i < NUM_TRACKS; i++)
   {
      if(!(seq_rollStoppedActive & (uint8_t)(1u << i)))
         continue;

      if(seq_rollStoppedCounter[i] == 0xff)
      {
         /* One-shot has fired. If the rate has since been changed to a
            repeating value, resume repeating from here. */
         if(seq_tempRate != 0xff)
            seq_rollStoppedCounter[i] = seq_tempRate;
         continue;
      }

      if(seq_rollStoppedCounter[i] > 0)
         seq_rollStoppedCounter[i]--;

      if(seq_rollStoppedCounter[i] == 0)
      {
         seq_triggerVoice(i, seq_rollVelocity, seq_rollNote);
         seq_rollStoppedCounter[i] = seq_tempRate;
      }
   }
}
```

---

### S5 — lines 1646–1660 — MODIFY `seq_rollMidiChange()`: re-arm the stopped roll clock on a press

**Remove** lines 1646–1660, which are the comment and the body of `seq_rollMidiChange()`.

**Add** in their place:

```c
//-------------------------------------------------------------------------------
/* MIDI ROLL-NOTE SOURCE
   WHAT: sets or clears this voice's MIDI ownership bit and rebuilds the
         aggregate roll request (seq_rollTriggered) from manual and MIDI
         ownership. While the transport is stopped, a press also clears the
         voice's seq_rollStoppedActive bit, so seq_tickStoppedRolls() fires
         it immediately. A new key press therefore re-triggers a voice even
         if another key already holds it.
   WHY:  manual and MIDI sources must not release each other's roll
         (MIDI roll feature). The stopped re-arm implements "MIDI rolls are
         re-triggerable while stopped" (Session 038, D4).
   INPUT:  voice 0..6; onOff non-zero = held by MIDI, zero = released.
   OUTPUT: seq_rollMidiHeld, seq_rollTriggered (via seq_rollApplyAggregate),
           and seq_rollStoppedActive (on a stopped press).
   ACCESSORS: midiParser_rollSyncVoices() only (MIDI/MidiParser.c). Called on
              every press (re-assert), on each voice's final release, and on
              every clear, including sequencer stop.
   AFFILIATES: seq_rollChange() (manual source), seq_rollApplyAggregate(),
               seq_tickStoppedRolls(), seq_setRoll() (running engine entry). */
void seq_rollMidiChange(uint8_t voice, uint8_t onOff)
{
   if(voice >= 7)
      return;

   const uint8_t voiceBit = (uint8_t)(1u << voice);

   if(onOff)
   {
      seq_rollMidiHeld |= voiceBit;
      if(!seq_running)
         seq_rollStoppedActive &= (uint8_t)~voiceBit;
   }
   else
      seq_rollMidiHeld &= (uint8_t)~voiceBit;

   seq_rollApplyAggregate(voice);
}
```

---

### S4 — `seq_setRunning()`, lines 1404–1447 — MODIFY: release MIDI rolls on stop, hand over on start

**S4a — insert after line 1431** (`trigger_allOff();`, inside the `if(!seq_running)` branch):

```c
      /* MIDI ROLL RELEASE ON STOP (Session 038, D3)
         WHAT: releases every MIDI-held roll, resets the stopped roll clock,
               and drops any roll-active or early-roll state that no source
               still requests.
         WHY:  a stop ends all MIDI-owned rolls. Clearing the parser's
               held-key table, and not only seq_rollMidiHeld, is what makes
               each key re-triggerable while stopped: the next press of a
               cleared key is a fresh press, not a duplicate. The sender's
               own note-offs arriving after the stop are then ignored as
               unknown keys. Masking seq_rollState / seq_rollPlayedEarly with
               the surviving request mask means a MIDI roll pressed during
               the stop later restarts through the quantized seq_setRoll()
               entry, not from a stale counter. Manual rolls still requested
               keep their state, exactly as before this change.
               This runs on every stop request, including a repeated stop
               while already stopped, so a stop also works as a MIDI-roll
               panic.
         INPUT:  none (reads seq_rollTriggered after the clear).
         OUTPUT: parser table empty, seq_rollMidiHeld = 0,
                 seq_rollStoppedActive = 0, seq_rollState and
                 seq_rollPlayedEarly reduced to still-requested voices.
         AFFILIATES: midiParser_clearMidiRollHolds() (MIDI/MidiParser.c),
                     seq_rollMidiChange(), seq_tickStoppedRolls(). */
      midiParser_clearMidiRollHolds();
      seq_rollStoppedActive = 0;
      seq_rollState &= seq_rollTriggered;
      seq_rollPlayedEarly &= seq_rollTriggered;
```

**S4b — insert after line 1440** (`trigger_reset(1);`, inside the `else` start branch):

```c
      /* STOPPED -> RUNNING MIDI ROLL HANDOVER (Session 038, D4)
         WHAT: retires the stopped roll clock at transport start.
         WHY:  a MIDI roll still held at start keeps its seq_rollTriggered
               request, and seq_nextStep() takes it over through the normal
               quantized seq_setRoll(voice, 1) entry, as with a manual roll
               held through start. Clearing the stopped state also ensures
               the next stop begins a fresh, immediately-firing stopped roll.
         AFFILIATES: seq_tickStoppedRolls(), seq_nextStep() roll section. */
      seq_rollStoppedActive = 0;
```

The removed lines are none; S4a and S4b are pure insertions.

---

### S3 — `seq_tick()`, line 1308 — MODIFY: service the stopped roll clock

**Insert after line 1309** (the opening `{` of `seq_tick()`, before `if(seq_deltaT == -1)`):

```c
   /* STOPPED MIDI ROLL SERVICE (Session 038, D4)
      WHAT: while the transport is stopped, advances the MIDI-only stopped
            roll clock. It has its own timing state and returns immediately
            when MIDI holds nothing.
      WHY:  seq_nextStep() returns immediately while stopped, so the normal
            roll engine cannot play MIDI rolls pressed during a stop. The
            transport timing below is left untouched so start alignment and
            external sync behave exactly as before.
      AFFILIATES: seq_tickStoppedRolls(). */
   if(!seq_running)
      seq_tickStoppedRolls();

```

---

### S2 — line 192 — ADD a static prototype

**Insert after line 192** (`static void seq_nextStep();`):

```c
/* Stopped-transport MIDI roll clock; defined after seq_rollMidiChange(). */
static void seq_tickStoppedRolls(void);
```

---

### S1 — after line 76 — ADD the stopped roll clock state

**Insert after line 76** (`static uint8_t seq_rollMidiHeld = 0;`):

```c
/* STOPPED-TRANSPORT MIDI ROLL CLOCK STATE (Session 038, D4)
   WHAT: private state of seq_tickStoppedRolls().
     seq_rollStoppedActive    bit v = voice v is rolling on the stopped clock
     seq_rollStoppedCounter[] sub-steps until the voice's next stopped hit;
                              0xff = one-shot already fired (parked)
     seq_rollStoppedLastTick  systick_ticks value at the previous service
     seq_rollStoppedPhase     accumulated systick time toward the next
                              sub-step (float, so tempo division stays exact)
   WHY:  kept separate from seq_rollState / seq_rollCounter / seq_deltaT /
         seq_lastTick, so that stopped-transport rolls can never disturb the
         running roll engine, transport start alignment, or external sync.
   ACCESSORS: seq_tickStoppedRolls() (all four); seq_rollMidiChange() (re-arm
              bit); seq_setRunning() (reset on stop and start).
   AFFILIATES: seq_rollMidiHeld (the only source of stopped rolls). */
static uint8_t  seq_rollStoppedActive = 0;
static uint8_t  seq_rollStoppedCounter[NUM_TRACKS];
static uint32_t seq_rollStoppedLastTick = 0;
static float    seq_rollStoppedPhase = 0.f;
```

`systick_ticks` comes from `globals.h`, which `sequencer.c` already includes (line 38). `midiParser_clearMidiRollHolds()` is declared in `MidiParser.h`, which is already included (line 53). No include changes are needed.

**`seq_rollApplyAggregate()` and `seq_rollChange()` (lines 1610–1644) are unchanged.**

---

## 5. `mainboard/LxrStm32/src/Sequencer/sequencer.h`

Apply the changes bottom-up in this order: SH4, SH3, SH2, SH1.

### SH4 — lines 504–507 — MODIFY the `seq_rollMidiChange()` declaration comment

**Remove** lines 504–506 (the comment only; the declaration on line 507 stays).

**Add** in their place:

```c
/* Record a MIDI-owned roll hold for one voice.
   voice: track index 0..6. onOff: non-zero = held by MIDI, zero = released.
   MIDI ownership is separate from front-panel roll buttons; either source
   keeps the aggregate roll request active until both release. Called only
   by MidiParser's held-key synchroniser: on every roll-key press (re-assert,
   which re-fires one-shot rolls and, while stopped, re-arms an immediate
   hit), on a voice's final release, and on every hold clear, including
   sequencer stop. While the transport is stopped, MIDI-held voices are
   played by a private stopped roll clock serviced from seq_tick().
   See sequencer.c seq_tickStoppedRolls() (Session 038). */
```

### SH3 — lines 480–482 — MODIFY the `seq_setRunning()` declaration comment

**Remove** lines 480–482 (the comment only; the declaration on line 483 stays).

**Add** in their place:

```c
/* Start or stop the sequencer transport.
   isRunning: non-zero starts playback, zero stops playback and resets the
   transport state to its stopped baseline.
   Stop also releases every MIDI-held roll (midiParser_clearMidiRollHolds()),
   and does so on every stop request, so it doubles as a MIDI-roll panic.
   Start hands any MIDI roll pressed during the stop to the normal quantized
   roll engine. Manual roll behaviour is unchanged (Session 038). */
```

### SH2 — lines 363–364 — MODIFY the `seq_tick()` declaration comment

**Remove** line 363 (the comment only).

**Add** in its place:

```c
/* Advance the sequencer one timing quantum and process due playback.
   While stopped, also services the MIDI-only stopped roll clock
   (Session 038); transport timing is not affected by that service. */
```

### SH1 — lines 133–140 — MODIFY the roll playback state comment

**Remove** lines 137–138:

```c
   The requested roll state is the aggregate of manual/front-panel and
   parser-owned MIDI roll-note holds.
```

**Add** in their place:

```c
   The requested roll state is the aggregate of manual/front-panel and
   parser-owned MIDI roll-note holds. MIDI holds are keyed per MIDI key in
   MidiParser.c and are released on note-off (including NOTE_ON velocity 0)
   and on sequencer stop. While stopped, MIDI-held rolls play on a private
   stopped roll clock using seq_rollVelocity / seq_rollNote (Session 038).
```

---

## 6. Documentation Updates

### D-1 — `knowledge_files/comms_spec_reference/MIDI_TABLE.md`, lines 66–71 — MODIFY

**Replace** lines 66–71 with:

```markdown
MIDI roll notes use the saved global roll-note offset. Raw `0` disables the
feature and displays as `off`; raw `1..127` is a positive semitone offset above
the normal trigger note. Shifted roll notes are evaluated only after the normal
global/voice note routes have had a chance to consume the message. Notes that
would shift outside MIDI `0..127` do nothing.

A NOTE_ON with velocity `0` is a note-off (MIDI 1.0) on every note route,
including the roll path (Session 038; the earlier "literal status" rule is
withdrawn). Each held roll key (channel + note) claims the voices it matched at
note-on, and its note-off releases exactly those voices. A voice keeps rolling
while any held key claims it. A repeated note-on for a held key is not counted
twice; unmatched note-offs are ignored; at most 16 roll keys can be held at
once.

All MIDI roll holds are released on: sequencer stop (every stop request),
roll-offset change, MIDI channel change or off, track note-override change,
RX note-filter disable, and parser cache clear. While the sequencer is stopped,
a roll key still starts a roll immediately. It repeats at the current tempo
and roll rate, using roll velocity and roll note. If the key is still held
at start, the roll continues on the normal quantized roll engine.
```

### D-2 — `MIDI_ROLL_TRIGGER.md` — MODIFY (historical design document; mark as superseded, do not rewrite)

- **Line 207** (Settled Decision 4): replace it with
  `4. ~~Note-on and note-off statuses are literal.~~ **Withdrawn in Session 038**: NOTE_ON velocity 0 is note-off. See S038_STUCK_MIDI_ROLL_BUG.md.`
- **Line 94**: append ` **(Superseded by Session 038: NOTE_ON velocity 0 is note-off.)**`
- **Line 178** (verification row): replace it with `- Note-on with velocity 0 releases a held roll key (Session 038).`
- **Line 195** (risk): append ` **Session 038: this risk materialised as the stuck-roll bug; resolved by treating NOTE_ON velocity 0 as note-off.**`
- **Lines 116–121** (hold-count options): append a line after 121:
  `> Session 038: replaced by a per-key (channel + note) held-key table; see S038_STUCK_ROLL_IMPLEMENTATION.md.`

### D-3 — `MIDI_ROLL_TRIGGER_IMPLEMENTATION.md` — MODIFY

- **Line 5**: after "without treating note-on velocity `0` as note-off", add ` (**withdrawn in Session 038**)`.
- **Line 982**: replace it with `- ~~Do not reinterpret NOTE_ON velocity 0 as note-off.~~ Withdrawn in Session 038.`
- **Lines 1110–1114** (proposed MIDI_TABLE text): append the note `Superseded by Session 038; see MIDI_TABLE.md.`
- **Line 1165**: replace it with `- NOTE_ON velocity 0 on a held roll key releases it (Session 038).`
- **Line 1229** (residual risk): append ` **Resolved in Session 038 by the per-key held-key table.**`
- **Append** a new final section, `## Session 038 Correction`, containing a two-line pointer to both S038 documents.

### D-4 — `S038_STUCK_MIDI_ROLL_BUG.md` — MODIFY

- Mark §4.4's first bullet (release on stop) as **decided: yes, plus stopped re-triggering (D3/D4)**.
- Mark §7 Q1 and Q3 as answered.
- Add a `Status` line pointing to this schedule.

### D-5 — `MEMORY.md` — ADD a reminder (after line 424, in the sequencer reminders)

```markdown
- **MIDI roll holds are keyed per MIDI key (channel + note) in `MidiParser.c`**, and each release frees exactly the voices its own note-on claimed. A NOTE_ON with velocity 0 is a note-off on every route, per MIDI 1.0. Never count it as a press; the Session 038 stuck-roll bug came from that. Sequencer stop (`seq_setRunning(0)`) clears all MIDI roll holds. While stopped, MIDI-held rolls play on the private `seq_tickStoppedRolls()` clock (roll velocity/note, `seq_tempRate`), and manual rolls stay silent. (Session 038)
```

Also update the "Current status" block when the session closes.

---

## 7. Verification

### 7.1 Host harness (the Session 037 method: copy code verbatim)

Build this in the session scratchpad, not in the repo. Copy **verbatim** from the patched sources:

- From `MidiParser.c`: the C1 declarations, the C3 helpers, C4, the roll-offset helpers (lines 100–151), and the note block of `midiParser_parseMidiMessage()`.
- From `sequencer.c`: `seq_rollApplyAggregate()`, `seq_rollChange()`, `seq_rollMidiChange()`, `seq_tickStoppedRolls()`, and the S4 fragments.

Stub the following: `channelMidiParser_noteOn/Off` (log only), `seq_triggerVoice` (log voice and time), `systick_ticks` (driven by the test), and the `midi_MidiChannels` / `midi_NoteOverride` / `frontParser_activeTrack` tables.

| # | Stimulus | Expected |
|---|---|---|
| 1 | `9n k 64`, then `9n k 00` | `seq_rollMidiHeld` bit set, then cleared. This reproduces the bug on the old code. |
| 2 | `9n k 64`, then `8n k 40` | set, then cleared |
| 3 | `9n k 64` ×2, then one `9n k 00` | cleared (idempotent press) |
| 4 | keys A and B both map to voice v; release A, then B | v stays held after A and is released after B |
| 5 | global chromatic route: press, change `frontParser_activeTrack`, release | the original voice is released; no other voice is touched |
| 6 | a note-off for a key that was never pressed | no change |
| 7 | 17 distinct keys | the 17th claims nothing; all 16 release correctly |
| 8 | a normal (consumed) note with velocity 0 | goes to `channelMidiParser_noteOff`, not to the roll path |
| 9 | stop while 3 keys are held | all released; later note-offs are ignored |
| 10 | stopped: press | immediate `seq_triggerVoice`, then one hit every `seq_tempRate` sub-steps at the harness tempo |
| 11 | stopped: one-shot rate (0xff), press, press a second key for the same voice | one hit per press |
| 12 | stopped: press, release | no further hits after the release |
| 13 | stopped: press, then start | stopped clock retired; `seq_rollTriggered` still set, so the running engine takes over |
| 14 | manual `seq_rollChange(v,1)` while stopped | no stopped hits (manual behaviour unchanged) |
| 15 | manual held + MIDI press/release | manual request survives the MIDI release |
| 16 | RX note filter disabled while a key is held | released |

### 7.2 Build

```
make -C mainboard/LxrStm32 clean && make -C mainboard/LxrStm32 -j4 stm32
make firmware
```

The only acceptable warnings are the pre-existing ones documented in `MEMORY.md`. Watch in particular for sign-compare warnings on `int8_t slot` against `MIDI_ROLL_MAX_HELD_KEYS`; cast the macro to `int8_t` if the compiler warns.

### 7.3 Hardware (user)

1. **Octatrack**, running: every roll key starts on press and stops on release, on a voice channel and on the global channel.
2. **Renoise or a DAW** (real `0x8n` note-offs): same as 1. Also NRPN 93 (not yet tested) changes the roll rate.
3. Several roll keys on different instruments, released in any order: each instrument stops on its own release. No "brutal voice stealing" with one or two rolls held.
4. Stop the sequencer while rolls are held: all MIDI rolls stop.
5. Sequencer stopped: press a roll key and it rolls immediately at the current tempo and roll rate; release it and the roll stops. Changing `PAR_ROLL` while holding changes the rate.
6. Sequencer stopped, key held, then press play: the roll continues on the grid.
7. Front-panel roll buttons: identical to before, while running and while stopped.
8. Normal (unshifted) MIDI notes: they trigger and record as before, including note-off ghost placement.
9. Changing the MIDI channel, note override or roll offset still releases everything.

---

## 8. Risks and Open Points

- **Automation on stopped hits.** `seq_triggerVoice()` parses the step automation of `seq_stepIndex[voice]` (the start position while stopped), exactly as the existing stopped voice preview (`FRONT_SEQ_SET_ACTIVE_TRACK`) already does. Stopped roll hits therefore inherit that precedent. If the user finds a parameter "sticking" after stopped rolls, add a trigger variant that skips `seq_parseAutomationNodes()`. **Not planned unless it is observed.**
- **MIDI echo feedback (pre-existing).** Roll hits echo a note-on on the voice's MIDI channel, just as running rolls always have. If the external device routes that echo back into the LXR, and the echoed note falls in the shifted roll range, it could start a roll with no release. This is the same exposure as before; the table limit keeps it bounded. It is documented here and not changed.
- **Stop as panic.** A stop request received while already stopped (for example the front-panel stop button, or an MTC timeout) also clears MIDI rolls that were started during the stop. This is intended (D3), but the user should know it.
- **Stopped rolls ignore `seq_rollMode`** (they always use roll velocity and roll note), and running rolls are unchanged. Confirm on hardware that this is the desired stopped sound.
- **Front-panel rolls while stopped remain silent.** Extending the stopped clock to manual rolls is a one-line change (use `seq_rollManualHeld | seq_rollMidiHeld` as the source mask) if the user wants it later. It is out of scope now.
