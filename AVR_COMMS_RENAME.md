# AVR Comms Rename Plan

## Goal

Replace the AVR-side naming under `front/LxrAvr/` with the new comms terminology and fold `frontPanelParser.h` into the protocol headers so the receive/send pair owns the full comms interface.

## Current AVR-side files in scope

- `front/LxrAvr/frontPanelReceivingProtocol.c`
- `front/LxrAvr/frontPanelReceivingProtocol.h`
- `front/LxrAvr/frontPanelSendingProtocol.c`
- `front/LxrAvr/frontPanelSendingProtocol.h`
- `front/LxrAvr/frontPanelParser.h`

## Target structure

- Create `front/LxrAvr/avrComms/` as the new home for the AVR comms modules.
- Rename the protocol pair to `avrCommsReceivingProtocol.*` and `avrCommsSendingProtocol.*`.
- Merge the parser declarations from `frontPanelParser.h` into the new protocol headers.
- Apply the naming map consistently:
  - `frontPanel*` -> `avrComms*`
  - `frontParser*` -> `avrCommsParser*`
  - `frontPanelParser*` -> `avrCommsPanelParser*`
- Retire `frontPanelParser.h` after all includes have been updated.
- Rename internal symbols and include paths so `frontPanel*` becomes `avrComms*` anywhere that belongs to the AVR comms layer.

## Planned implementation steps

1. Create the new `front/LxrAvr/avrComms/` folder.
2. Move and rename the receive/send protocol source and header files into that folder.
3. Fold `frontPanelParser.h` into the appropriate renamed headers.
4. Update all include directives across `front/LxrAvr/` to point at the renamed headers.
5. Rename AVR-side identifiers, comments, and documentation references using the agreed naming map.
6. Update the project file(s) so the build still picks up the renamed paths.
7. Remove the old `frontPanelParser.h` file once nothing includes it.

## Scope notes

- Keep the STM-facing protocol names and message constants unchanged unless they are part of the AVR-side naming cleanup.
- Preserve behavior exactly; this is a naming and organization change, not a protocol redesign.
- Treat `frontParser_*` as AVR-side parser state that should become `avrCommsParser_*`.
- Treat `frontPanelParser_*` as the combined panel/parser API namespace that should become `avrCommsPanelParser_*`.

## Validation checklist

- Build the AVR project after the rename.
- Search `front/LxrAvr/` for remaining `frontPanel` references and classify any leftovers.
- Verify that `frontPanelParser.h` is no longer referenced anywhere.
- Confirm the new `avrComms/` folder contains the renamed protocol files and that the project still compiles.

## Open questions to resolve during implementation

- Whether any generated project metadata needs manual editing beyond the source tree and `Makefile`.
