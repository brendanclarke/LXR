# Firmware Update

Firmware for both the front AVR and the Cortex-M4 can be updated using the SD card.

## Update Procedure

1. Make sure your SD card is using FAT32. The bootloader can only read FAT32 filesystems.
2. Copy the `FIRMWARE.BIN` file to the root directory of the SD card.
3. Insert the card into the powered-off LXR.
4. Push and hold the encoder, then turn on the synthesizer.

The synth boots in firmware update mode. The display shows:

\oledsingle{Bootloader v1.1}

If a firmware image is found on the SD card, the front AVR firmware is updated first:

\oledsingle{updating... (1/2)}

Followed by the mainboard firmware:

\oledsingle{updating... (2/2)}

During the update, the 16 step LEDs show a binary pattern indicating the synth is busy.
When the update is complete, the display shows:

\oled{success!}{please reboot...}

Power the device off and on again. The new firmware version is shown on the normal boot screen.

## Bootloader Error Codes

If problems occur, the bootloader will show one of the following error messages:

**Firmware Error** — The bootloader could not find the `FIRMWARE.BIN` file. It is either missing
from the SD card or the card is not FAT32.

**Header error** — The `FIRMWARE.BIN` file on the SD card is corrupt or not a valid firmware file.
Try downloading the file again.

**EOF error** — The end of the firmware file was reached before the update finished. The
`FIRMWARE.BIN` file is probably corrupted. Try downloading it again.

**Mainboard error** — The mainboard does not answer. Check that a working mainboard is
connected to the front panel.
