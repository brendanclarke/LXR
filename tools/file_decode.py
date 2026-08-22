#!/usr/bin/env python3
"""Decode LXR SD-card preset files into annotated text files.

Run from anywhere inside the project (or pass a project directory):

    python3 tools/file_decode.py
    python3 tools/file_decode.py /path/to/project

The decoder is deliberately read-only.  It finds ``SD_CARD`` below the
project directory, and writes one ``.txt`` file per supported input into
``sd_unpacked_decoded`` at the project root.
"""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path
from typing import Iterable


# ---------------------------------------------------------------------------
# FORMAT / MAINTENANCE HEADER
# ---------------------------------------------------------------------------
# All values below describe the current firmware format in
# front/LxrAvr/Preset/presetManager.c and
# mainboard/LxrStm32/src/Sequencer/Pattern/PatternData.h.
#
# Files are little-endian where a multi-byte value is used.  A Step is seven
# bytes: volume (bit 7 = active, bits 0..6 = volume), probability, note,
# automation-1 number/value, automation-2 number/value.
#
# The first 8 bytes of every preset are an 8-character, space-padded name.
# PRF and ALL have one additional version byte at offset 8.  Their fixed
# blocks are 64 bytes of metadata, then a 512-byte kit block and a 512-byte
# morph/end-point block.  ALL's first metadata block contains global values;
# PRF's contains performance values followed by 0xff padding.
#
# If firmware changes these constants, update this section and the field
# tables below.  The decoder always emits a complete raw-byte dump too.
SUPPORTED_EXTENSIONS = {".snd", ".pat", ".prf", ".all", ".cfg"}
NAME_BYTES = 8
NUM_TRACKS = 7
NUM_PATTERNS = 8
STEPS_PER_PATTERN = 128
STEP_BYTES = 7
MAIN_STEP_COUNT = NUM_PATTERNS * NUM_TRACKS       # 56 uint16 values
CHAIN_BYTES = NUM_PATTERNS * 2                    # next/repeat pairs
TRACK_INFO_BYTES = NUM_PATTERNS * NUM_TRACKS      # lengths, then scales
PATTERN_STEP_BYTES = NUM_PATTERNS * NUM_TRACKS * STEPS_PER_PATTERN * STEP_BYTES
FILE_VERSION = 5
PATTERN_OFFSET = {".pat": 8, ".prf": 1098, ".all": 1097}


def clean_c_name(value: str) -> str:
    """Make a C identifier pleasant in the text report."""
    return value.strip().replace("PAR_", "", 1)


def parameter_names(project: Path) -> dict[int, str]:
    """Read enum names from the firmware when available.

    Keeping this small parser here means new parameter names automatically
    appear in reports without duplicating the large firmware enum in Python.
    Numeric indices are still printed when the source is unavailable.
    """
    source = project / "mainboard/LxrStm32/src/Preset/ParameterArray.h"
    if not source.is_file():
        return {}
    text = source.read_text(encoding="utf-8", errors="replace")
    block = text[text.find("enum"):text.find("};", text.find("enum"))]
    names: dict[int, str] = {}
    value = -1
    symbols: dict[str, int] = {}
    for raw in block.splitlines():
        raw = raw.split("//", 1)[0].strip().rstrip(",")
        match = re.match(r"(PAR_[A-Z0-9_]+|END_OF_[A-Z0-9_]+|NUM_PARAMS)\s*(?:=\s*(.+))?$", raw)
        if not match:
            continue
        name, expression = match.groups()
        if expression:
            expression = expression.strip()
            if expression.isdigit():
                value = int(expression)
            elif expression in symbols:
                value = symbols[expression]
            else:
                continue
        else:
            value += 1
        symbols[name] = value
        if name.startswith("PAR_"):
            names.setdefault(value, clean_c_name(name))
    return names


def automation_targets(project: Path) -> dict[int, str]:
    """Build mod-target-index -> voice/parameter descriptions from firmware."""
    source = project / "front/LxrAvr/Menu/Cc2Text.c"
    if not source.is_file():
        return {}
    targets: dict[int, str] = {
        0: "off / no target",
        1: "all voices / VOICE_DECIMATION_ALL",
    }
    voice = "global"
    voice_names = {"1": "drum 1", "2": "drum 2", "3": "drum 3", "4": "snare/clap", "5": "cymbal", "6": "hi-hat"}
    # Entries 0 and 1 are the two non-voice entries above the first voice.
    index = 1
    for raw in source.read_text(encoding="utf-8", errors="replace").splitlines():
        heading = re.search(r"//Voice\s+(\d)\s+-\s*(.*)", raw, re.IGNORECASE)
        if heading:
            voice = voice_names.get(heading.group(1), heading.group(2).strip())
        match = re.search(r"\{\s*TEXT_[A-Z0-9_]+\s*,\s*(PAR_[A-Z0-9_]+)\s*\}", raw)
        if not match:
            continue
        index += 1
        parameter = clean_c_name(match.group(1))
        target_voice = voice if voice != "global" else "all voices"
        targets[index] = f"{target_voice} / {parameter}"
    return targets


def u8(data: bytes, offset: int) -> int:
    return data[offset] if 0 <= offset < len(data) else 0


def name_field(data: bytes) -> str:
    return data[:NAME_BYTES].rstrip(b"\x00 \xff").decode("ascii", errors="replace")


def section(title: str) -> list[str]:
    return ["", f"=== {title} ==="]


def parameter_block(data: bytes, start: int, count: int, labels: dict[int, str], title: str, label_base: int = 0) -> list[str]:
    lines = section(title)
    lines.append(f"offset {start}, {count} unsigned byte parameters")
    for index in range(count):
        offset = start + index
        value = u8(data, offset)
        label = labels.get(label_base + index, f"parameter_{label_base + index:03d}")
        if label == "BEGINNING_OF_GLOBALS":
            label = "BPM"
        lines.append(f"[{index:03d}] {label:<32} = {value:3d} (0x{value:02x}) @ byte {offset}")
    return lines


def decode_step(data: bytes, offset: int) -> tuple[int, int, int, int, int, int, int]:
    values = tuple(u8(data, offset + i) for i in range(STEP_BYTES))
    return values  # type: ignore[return-value]


def format_automation_target(value: int, targets: dict[int, str]) -> str:
    if value == 255:
        return "off / no target"
    return targets.get(value, f"unknown target index {value}")


def pattern_payload(data: bytes, offset: int, targets: dict[int, str]) -> list[str]:
    """Decode every pattern field, including inactive/default steps."""
    lines = section("PATTERN STEP DATA (all records)")
    lines.append("Each record: volume(active bit 7 + level bits 0..6), probability, note, p1 number/value, p2 number/value")
    # The firmware writes/reads track-major data:
    #   track 0: pattern 0 steps, pattern 1 steps, ... pattern 7 steps
    #   track 1: pattern 0 steps, ...
    # Do not change this to pattern-major ordering; that produces plausible
    # looking but incorrect reports for individual pattern/track selections.
    for pattern in range(NUM_PATTERNS):
        for track in range(NUM_TRACKS):
            lines.append(f"-- pattern {pattern}, track {track + 1} --")
            for step in range(STEPS_PER_PATTERN):
                record_index = ((track * NUM_PATTERNS + pattern) * STEPS_PER_PATTERN) + step
                cursor = offset + record_index * STEP_BYTES
                raw = decode_step(data, cursor)
                volume, probability, note, p1n, p1v, p2n, p2v = raw
                active = (volume & 0x80) >> 7
                level = volume & 0x7f
                lines.append(
                    f"pattern={pattern} track={track + 1} step={step + 1:03d} "
                    f"active={active} volume={level:3d} probability={probability:3d} note={note:3d} "
                    f"p1={format_automation_target(p1n, targets)} p1v={p1v:3d} "
                    f"p2={format_automation_target(p2n, targets)} p2v={p2v:3d} "
                    f"p1n={p1n:3d} p2n={p2n:3d} "
                    f"raw={raw!r} @ byte {cursor}"
                )

    lines += section("MAIN STEP MASKS (uint16 little-endian)")
    for index in range(MAIN_STEP_COUNT):
        value = struct.unpack_from("<H", data + b"\x00\x00", cursor)[0] if cursor + 1 < len(data) else 0
        pattern, track = divmod(index, NUM_TRACKS)
        lines.append(f"pattern={pattern} track={track + 1} mask=0x{value:04x} ({value:016b}) @ byte {cursor}")
        cursor += 2

    lines += section("PATTERN CHAIN (next pattern, repeat count)")
    for pattern in range(NUM_PATTERNS):
        next_pattern, repeat = u8(data, cursor), u8(data, cursor + 1)
        lines.append(f"pattern={pattern} next={next_pattern} repeat={repeat} @ byte {cursor}")
        cursor += 2

    shuffle = u8(data, cursor)
    lines += section("PATTERN SETTINGS")
    lines.append(f"shuffle={shuffle} (0x{shuffle:02x}) @ byte {cursor}")
    cursor += 1
    lines.append("track lengths (stored in pattern-major, track order):")
    for index in range(TRACK_INFO_BYTES):
        pattern, track = divmod(index, NUM_TRACKS)
        lines.append(f"pattern={pattern} track={track + 1} length={u8(data, cursor)} @ byte {cursor}")
        cursor += 1
    lines.append("track scales (stored in pattern-major, track order):")
    for index in range(TRACK_INFO_BYTES):
        pattern, track = divmod(index, NUM_TRACKS)
        lines.append(f"pattern={pattern} track={track + 1} scale={u8(data, cursor)} @ byte {cursor}")
        cursor += 1
    return lines


def raw_dump(data: bytes, known_end: int) -> list[str]:
    lines = section("RAW BYTES NOT OTHERWISE CONSUMED")
    if known_end >= len(data):
        lines.append("none")
        return lines
    for offset in range(known_end, len(data), 16):
        chunk = data[offset:offset + 16]
        lines.append(f"{offset:06d}: " + " ".join(f"{byte:02x}" for byte in chunk))
    return lines


def decode_file(path: Path, project: Path, labels: dict[int, str]) -> str:
    data = path.read_bytes()
    ext = path.suffix.lower()
    lines = [
        "LXR SD-CARD FILE DECODE",
        f"source: {path}",
        f"type: {ext}  size: {len(data)} bytes",
        "Note: every field is decoded where the firmware layout is known; raw bytes are retained below.",
    ]
    # PAR_BPM is an enum alias of PAR_BEGINNING_OF_GLOBALS, and the parser
    # keeps the first alias at a shared numeric value.  Accept either spelling.
    global_base = next(
        (index for index, name in labels.items() if name in {"BPM", "BEGINNING_OF_GLOBALS"}),
        0,
    )
    if ext == ".cfg":
        lines += parameter_block(data, 0, len(data), labels, "GLOBAL CONFIGURATION", global_base)
        lines += raw_dump(data, len(data))
        return "\n".join(lines) + "\n"

    lines += section("FILE HEADER")
    lines.append(f"preset name = {name_field(data)!r} (bytes 0..7)")
    cursor = NAME_BYTES
    if ext in {".prf", ".all"}:
        version = u8(data, 8)
        lines.append(f"file version = {version} (expected <= {FILE_VERSION}) @ byte 8")
        cursor = 9
        lines += section("VERSION / METADATA BLOCK")
        lines.append(f"metadata bytes {cursor}..{cursor + 63} (64-byte fixed block)")
        if ext == ".all":
            lines += parameter_block(data, cursor, min(64, len(data) - cursor), labels, "GLOBAL SETTINGS IN ALL FILE", global_base)
        else:
            lines.append("PRF metadata: BPM, bar-reset, pattern-change timing, MIDI channels/notes, then 0xff padding")
            for index in range(min(64, len(data) - cursor)):
                value = u8(data, cursor + index)
                lines.append(f"metadata[{index:02d}] = {value:3d} (0x{value:02x}) @ byte {cursor + index}")
        cursor += 64
        lines += section("KIT AND MORPH BLOCKS")
        lines.append(f"kit block: bytes {cursor}..{cursor + 511} (512-byte allocated block; first parameter bytes are meaningful)")
        lines.append(f"morph/end-point block: bytes {cursor + 512}..{cursor + 1023} (512-byte allocated block)")
        lines += parameter_block(data, cursor, min(243, len(data) - cursor), labels, "KIT PARAMETERS")
        lines += parameter_block(data, cursor + 512, min(243, max(0, len(data) - cursor - 512)), labels, "MORPH / END-POINT PARAMETERS")
    elif ext == ".snd":
        lines += parameter_block(data, 8, max(0, len(data) - 8), labels, "KIT PARAMETERS")

    pattern_offset = PATTERN_OFFSET.get(ext)
    if pattern_offset is not None and pattern_offset < len(data):
        lines += pattern_payload(data, pattern_offset, automation_targets(project))
        lines += raw_dump(data, pattern_offset + PATTERN_STEP_BYTES + MAIN_STEP_COUNT * 2 + CHAIN_BYTES + 1 + TRACK_INFO_BYTES * 2)
    else:
        lines += raw_dump(data, cursor)
    return "\n".join(lines) + "\n"


def input_files(sd_card: Path) -> Iterable[Path]:
    yield from sorted((p for p in sd_card.rglob("*") if p.is_file() and p.suffix.lower() in SUPPORTED_EXTENSIONS), key=lambda p: str(p).lower())


def main() -> int:
    parser = argparse.ArgumentParser(description="Decode LXR SD_CARD preset files into sd_unpacked_decoded/*.txt")
    parser.add_argument("project", nargs="?", type=Path, default=Path(__file__).resolve().parents[1], help="LXR project directory (default: this script's project)")
    args = parser.parse_args()
    project = args.project.resolve()
    sd_card = project / "SD_CARD"
    output = project / "sd_unpacked_decoded"
    if not sd_card.is_dir():
        parser.error(f"SD_CARD directory not found: {sd_card}")
    output.mkdir(exist_ok=True)
    labels = parameter_names(project)
    files = list(input_files(sd_card))
    if not files:
        print(f"No supported files found in {sd_card}")
        return 0
    for source in files:
        # Keep the extension in the report name so P001.SND, P001.PRF and
        # P001.ALL do not overwrite one another as P001.txt.
        destination = output / f"{source.name}.txt"
        destination.write_text(decode_file(source, project, labels), encoding="utf-8")
        print(f"decoded {source.relative_to(project)} -> {destination.relative_to(project)}")
    print(f"Decoded {len(files)} file(s) into {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
