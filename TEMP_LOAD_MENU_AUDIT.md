# TEMP_LOAD_MENU_AUDIT.md

## Session 023 — Background Load Menu Plan

**Goal**: Replace the current file-load toggle with a 5-state background-load selector in the AVR menu. This pass covers the menu parameter, the menu text wiring, and the AVR-side send symbol name only. The actual background-load decision logic is deferred to a later implementation pass, and STM-side behavior must not change in this pass.

**Naming rule for this session**: use `loadBackground` or `BACKGROUND` terminology everywhere in the AVR-facing menu code. Do not carry any old file-load shorthand forward into the plan, code, or documentation.

**New states (0–4)**:
| Value | Display | Meaning |
|-------|---------|---------|
| 0 | `off` | No background load |
| 1 | `pat` | Only patterns may background-load |
| 2 | `prf` | Only `.PRF` kits may background-load |
| 3 | `all` | Only `.ALL` files may background-load |
| 4 | `tot` | All supported file types may background-load |

**Compatibility rule**: keep the existing persisted parameter slot and wire byte values unchanged. The stored byte is still a single global setting, so old saved values remain readable, but the symbol names and user-visible labels must switch to background terminology.

**Protocol rule**: the AVR continues to send the same raw byte value on the same wire opcode number. STM-side receive handling and runtime behavior are intentionally untouched in this pass.

**Current implementation status**:
- AVR menu parameter rename is in progress/completed in `front/LxrAvr/Parameters.h`, `front/LxrAvr/Menu/menu.h`, `front/LxrAvr/Menu/MenuText.h`, `front/LxrAvr/Menu/menuPages.h`, `front/LxrAvr/Menu/menu.c`, and `front/LxrAvr/buttonHandler.c`.
- The AVR send opcode alias has been renamed to `SEQ_LOAD_BACKGROUND` in `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h` with the same numeric value.
- STM-side source files were intentionally left unchanged so no STM operation changes in this pass.
- `.cfg` persistence remains generic because the global settings file already stores the raw byte range directly.
- Verified with `make -C front/LxrAvr avr -j4`, `make -C mainboard/LxrStm32 -j4 stm32`, and `make firmware`.
- The background-load selector uses the free `MENU_* = 0` slot because the packed menu ID lives in the upper nibble and cannot grow past 15.

---

## What This Pass Should Rename

| Current concept | Target name |
|----------------|-------------|
| file-load menu parameter | `PAR_FILE_LOAD_BACKGROUND` |
| background-load menu text group | `MENU_FILE_LOAD_BACKGROUND` |
| background-load short/long labels | `TEXT_FILE_LOAD_BACKGROUND`, `LONG_FILE_LOAD_BACKGROUND` |
| menu text table | `backgroundLoadNames` |
| AVR send opcode | `SEQ_LOAD_BACKGROUND` |

The exact short label can follow the existing 3-character menu naming style, but it must be background-oriented and must not preserve the old wording.

---

## Files To Modify

### 1. `front/LxrAvr/Parameters.h`

Rename the existing file-load parameter enum entry to `PAR_FILE_LOAD_BACKGROUND` while keeping the same enum slot and stored value semantics.

The comment should also change from a boolean-style note to a background-load note, because the value range is no longer a simple on/off flag.

---

### 2. `front/LxrAvr/Menu/menu.h`

Rename the text enum entry to `TEXT_FILE_LOAD_BACKGROUND`.

Add a new menu-text family ID for the background-load text table:

```c
#define MENU_FILE_LOAD_BACKGROUND 0
```

This ID is the lookup key for the max-entry and label functions. It uses the
free `0` slot because the packed menu-ID field only has four bits.

---

### 3. `front/LxrAvr/Menu/MenuText.h`

Add a 5-entry table for the background-load display strings, following the same count-plus-values pattern used by the other menu text tables.

Suggested structure:

```c
const char backgroundLoadNames[][4] PROGMEM =
{
    {5},
    {"off"},
    {"pat"},
    {"prf"},
    {"all"},
    {"tot"},
};
```

Update the short and long menu text entries so the visible label uses background terminology. The important part is the user-facing copy, not the exact abbreviation, as long as it stays consistent with the rest of the menu system.

---

### 4. `front/LxrAvr/Menu/menu.c`

Make the background-load parameter a menu-style parameter instead of a boolean-style parameter:

```c
/*PAR_FILE_LOAD_BACKGROUND*/  DTYPE_MENU | (MENU_FILE_LOAD_BACKGROUND<<4),
```

Update `getMaxEntriesForMenu()` and `getMenuItemNameForValue()` to use the new menu ID and background-load text table.

The parameter-setting branch should continue to store the raw byte and send the same raw byte to STM, but through the renamed opcode:

```c
case PAR_FILE_LOAD_BACKGROUND:
   parameter_values[PAR_FILE_LOAD_BACKGROUND] = value;
   avrComms_sendData(SEQ_CC, SEQ_LOAD_BACKGROUND, value);
   break;
```

No extra packing or translation is needed in this pass.

---

### 5. `front/LxrAvr/Menu/menuPages.h`

Update the load-page mapping so the page points at `TEXT_FILE_LOAD_BACKGROUND` and `PAR_FILE_LOAD_BACKGROUND`.

This is the point where the load-page UI stops referring to the old wording in its page definition.

---

### 6. `front/LxrAvr/buttonHandler.c`

Rename the parameter references to `PAR_FILE_LOAD_BACKGROUND`.

Keep the current load-page branching behavior unless later testing shows the UI needs to distinguish the non-zero background modes from each other. The current evidence says the existing truthy check is enough for this pass, because the new enum values 1–4 are still all enabled states from the UI's point of view.

---

### 7. `front/LxrAvr/avrComms/avrCommsReceivingProtocol.h`

Rename the send opcode constant to `SEQ_LOAD_BACKGROUND`, keeping the existing value:

```c
#define SEQ_LOAD_BACKGROUND  0x50
```

The byte value must remain stable so the link protocol does not change.

---

## Implementation Notes

- The menu parameter is the only user-facing control in this pass, so the code should focus on symbol and label alignment rather than new loading behavior.
- The current `buttonHandler.c` branches already treat the value as enabled/disabled; that remains acceptable because the new enum values 1–4 are all enabled states.
- The later behavior pass can interpret the 0–4 mode byte to decide which file kinds may auto-load in the background.
- The wire byte, stored parameter slot, and opcode number should all remain unchanged so compatibility is preserved.
- STM-side receive handling remains as-is in this pass, so only AVR menu-side names and the AVR send-side symbol are expected to move.

---

## Follow-Up Documentation

After the rename lands, refresh the project docs so they stop using the old file-load wording:

- `knowledge_files/comms_spec_reference/COMMS_FLOW_SPEC.md`
- `knowledge_files/comms_spec_reference/TEMPORARY_PAT_PARAM_LOAD_SPEC.md`
- `MEMORY.md`
- `knowledge_files/log_archive/000_SESSION_INDEX.md`

Those docs should describe the background-load model with the new naming, while keeping the existing behavioral history intact.

---

## Verification

After the rename pass, verify with:

```bash
make -C front/LxrAvr clean
make -C front/LxrAvr avr -j4
make -C mainboard/LxrStm32 -j4 stm32
make firmware
```

The AVR build should still pass cleanly, and the STM32 build should remain unaffected because the wire values do not change.
