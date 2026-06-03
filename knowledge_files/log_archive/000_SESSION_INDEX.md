# LXR -bc- Enhanced Firmware — Session Index

**Project**: Fork of LXR drum machine firmware  
**Repo**: `/Users/bc/LXR01/LXR-current/LXR-custom-develop-patload-envmod` | **Log format**: `00x_SESSION_HANDOFF_LOG.md`

---

## Quick Reference — What's In Each Log

| # | Date | Source at end | Topic |
|---|------|----------------|-------|
| 001 | 2026-05-21 | local branch `custom-develop-patload-envmod` (commit `3698612`) | build-break triage, source compatibility fixes (A1/A2/A3/B1/B2/B3), full firmware build verified |
| 002 | 2026-05-29 to 2026-06-01 | local branch `custom-develop-patload-envmod` (merged temp-pattern WIP) | temp-pattern parameter isolation, symmetric kit states, endpoint/automation storage, normal-only file loads, rate-limited endpoint restore, morph-to-STM plan |
| 003 | 2026-06-03 | local branch `custom-develop-patload-envmod` (STM morph WIP, dirty tree) | morph computation moved fully to STM for standard operation; normal/temp exchange broken and queued for session 004 |

---

## Session Summaries

### 001 — Build Recovery + Session Memory Baseline (2026-05-21)
Session 001 established project context docs, audited build failures, applied the requested source-level compatibility fixes, and verified a successful end-to-end `make clean && make firmware` build in the active repository. Session closeout added the first formal handoff log and a full MEMORY baseline for subsequent sessions.
- **Find here**: build failure root cause, A/B fix groups, toolchain requirements draft, repository-path mismatch lesson, successful firmware image output

### 002 — Temp Pattern Parameter Isolation + Morph Move Prep (2026-05-29 to 2026-06-01)
Session 002 completed encoder work, restored the `.ALL` / `.PRF` load and parameter-pushback baseline, then advanced the temporary-pattern model: symmetric normal/temp `SeqKitState` storage, kit/front and morph parameter endpoint capture, three resolved automation-target images per kit, normal-only file-load routing, lazy temp initialization, per-track endpoint sync, low-CC offset fixes, endpoint/menu restore chirp isolation, queued/rate-limited endpoint restore, and the audit plan for moving morph computation fully onto STM.
- **Find here**: encoder completion, comms/load checkpoint, restore handshake, +1/-1 parameter offset, SEQ16 temp pattern, symmetric `SeqKitState`, endpoint dump/copy-to-temp, automation target image storage, normal-only file loads, temp edit isolation, per-track temp/normal endpoint sync, endpoint restore chirp, rate-limited restore, AVR morph-state audit, STM morph move plan

### 003 — STM-Owned Morph Move + Post-Morph Temp Cache Handoff (2026-06-03)
Session 003 moved standard morph computation fully onto STM: STM now owns global/per-voice morph state, one-parameter-per-main-loop interpolation, live DSP morph application, LFO/velocity modulation of voice morph, raw endpoint-index storage, automation-target image refresh, and the live-apply cache needed for zero-valued file parameters to land in DSP. The session also removed per-parameter valid arrays from `SeqKitState`; sound parameter arrays are always-defined from zero init, file loads write normal endpoint arrays only, and the morph worker is the only intended writer of interpolation arrays. Standard morph was hardware-confirmed, but the normal/temporary pattern and parameter exchange is broken after the move and is the recommended Session 004 goal.
- **Find here**: STM-owned morph, per-voice morph, global morph command, raw endpoint indexing, automation target sidebands, LFO target to voice morph, valid-array removal, file-load endpoint-to-DSP flow, live-apply cache, zero-valued parameter apply bug, waveform Drum 2 fix, temp exchange broken, SEQ16 temp cache observation bodge, post-morph Session 004 audit plan


---

## Key Cross-Session Facts (quick lookup)

| Topic | Canonical session |
|-------|------------------|
| Header/global-definition and inline-linkage compatibility fixes for modern GCC toolchains | 001 |
| Session memory baseline and directory map bootstrapped | 001 |
| Encoder work completed successfully | 002 |
| `.ALL` / `.PRF` load checkpoint and normal-only file-load routing for temp playback | 002 |
| Comms flow-control checkpoint uses load sessions, quiet mode, and credit-metered globals/voice/meta bursts; old callback waits still need timeouts | 002 |
| SEQ16 temp pattern selector/copy/play, symmetric STM parameter images, and endpoint/menu restore behavior | 002 |
| Morph computation is STM-owned for standard operation; AVR keeps global morph/menu/file endpoint responsibilities only | 003 |
| `SeqKitState` no longer has per-parameter valid arrays; parameter arrays are always-defined sound state from zero init | 003 |
| Endpoint storage uses raw AVR/menu parameter indices; low `+1` only applies when sending ordinary live low CC to STM MIDI/DSP apply | 003 |
| Session 004 should fix normal/temp cache loading from the post-morph audits | 003 |


---

## Append Template

```
| 0NN | YYYY-MM-DD | working repository status/path | One-line topic |
```

```
### 0NN — Title (YYYY-MM-DD)
One paragraph summary.
- **Find here**: comma-separated topics
```

Add any new cross-session facts to the Key Cross-Session Facts table.
