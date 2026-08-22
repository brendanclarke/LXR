#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rotation_correction.py - rotate sequencer pattern data inside LXR firmware files.

===============================================================================
 WHAT THIS SCRIPT DOES
===============================================================================
The LXR sequencer stores every pattern as a set of tracks, and every track as a
fixed sequence of 128 sub-steps.  Each sub-step is a 7-byte record holding the
note, velocity, probability, the on/off (active) flag, and two automation lanes
(target + amount each).  Separately, every track also stores a 16-bit main-step
gate mask (one bit per main step; there are 16 main steps per track, and each
main step groups 8 sub-steps).

This script cyclically rotates (shifts) that data inside a saved pattern file.
Nothing is added, deleted, overwritten, cropped, or truncated: every value is
moved to a new position in its sequence and wraps around, so the total amount
of data in the file is unchanged.

It understands three on-SD file types, selected automatically from the file
extension (case-insensitive):

    .pat   pattern bank file
    .prf   performance file
    .all   "everything" file (kits + morphs + patterns)

Only the pattern step data and main-step gate masks are touched.  Kit/morph
parameter bytes, the 8-byte name, shuffle, per-track length/scale bytes, and
the pattern chain (next/repeat) are all left byte-for-byte intact.

The input file is modified IN PLACE.  Make a backup copy first if you need one.

===============================================================================
 COMMAND LINE
===============================================================================
    python3 rotation_correction.py <file> step=<N> [options]
    python3 rotation_correction.py <file> substep=<N> [options]

    Example
    python3 rotation_correction.py P000.PRF substep=+2 track="0, 2" data="note, probability"git 

You must supply exactly one positional argument (the file), plus at least one
of "step=" or "substep=".  Supplying both applies both rotations together.

    step=<+/-N>      Rotate by N MAIN steps.  One main step = 8 sub-steps.
                     This shifts the 128 sub-steps by N*8 and rotates the
                     16-bit main-step gate mask by N.
    substep=<+/-N>   Rotate by N SUB-steps.  This shifts the 128 sub-steps by
                     N and leaves the main-step gate mask unchanged.

Positive N rotates forward (data moves toward higher step numbers and wraps
around to the start); negative N rotates backward.  N may be larger than the
sequence length and is reduced modulo 128 (or 16 for the gate mask).

===============================================================================
 OPTIONAL FILTERS
===============================================================================
Each optional filter accepts a comma- and/or whitespace-separated list.  The
values may appear in any order.  Repeated values are ignored (each item is
applied exactly once).

    pattern="0, 1, 2, 3, 4, 5, 6, 7"
        Only rotate the listed pattern numbers (0..7).  Default: all patterns.

    track="0, 1, 2, 3, 4, 5, 6"
        Only rotate the listed track numbers (0..6).  Default: all tracks.

    data="active, volume, probability, note, target1, target2, amount1, amount2"
        Only rotate the listed step data types.  Default: all data types.

The eight step data types map to the on-disk StepData record as follows:

    active       bit 7 of the volume byte (0x80)   - step on/off flag
    volume       bits 0..6 of the volume byte (0x7f)
    probability  probability byte
    note         note byte
    target1      automation lane 1 destination (param1Nr)
    amount1      automation lane 1 value        (param1Val)
    target2      automation lane 2 destination (param2Nr)
    amount2      automation lane 2 value        (param2Val)

When "data=" is given, only those data types are rotated and everything else is
left in place, including the main-step gate mask.  Because "active" and
"volume" share one byte, each is treated as an independent column and they can
be rotated independently without disturbing the other.

===============================================================================
 EXAMPLES
===============================================================================
    # Rotate every pattern/track forward by one main step.
    python3 rotation_correction.py P000.PAT step=+1

    # Rotate pattern 3 only, back by 4 sub-steps.
    python3 rotation_correction.py P001.ALL substep=-4 pattern=3

    # Rotate notes forward 2 sub-steps on tracks 0 and 2, leave everything
    # else (volume, probability, automation, gate mask) where it is.
    python3 rotation_correction.py P000.PRF substep=+2 track="0, 2" data=note

    # Both: one main step plus three sub-steps, on patterns 0,2,4,6.
    python3 rotation_correction.py P000.ALL step=+1 substep=+3 pattern="0,2,4,6"

===============================================================================
 FILE LAYOUT (for reference)
===============================================================================
Step data: 8 patterns x 7 tracks x 128 sub-steps x 7 bytes.
    Linear index i -> track = i // 1024, pattern = (i // 128) % 8, step = i % 128.
    Step record byte order: volume, probability, note, target1, amount1,
    target2, amount2.

Main-step gate mask: 8 patterns x 7 tracks x 2 bytes (little-endian uint16).
    Linear index i -> pattern = i // 7, track = i % 7.  Bit b = main step b.

Offsets per file type (verified against front/LxrAvr/Preset/presetManager.c):

    .pat   step data @ 8,      main-step mask @ 50184
    .prf   step data @ 1098,   main-step mask @ 51274
    .all   step data @ 1097,   main-step mask @ 51273
"""

import os
import sys


NUM_PATTERNS = 8
NUM_TRACKS = 7
STEPS_PER_TRACK = 128
SUBSTEPS_PER_MAIN_STEP = 8
MAIN_STEPS_PER_TRACK = 16
STEP_DATA_BYTES = 7

MAIN_STEP_SIZE = NUM_PATTERNS * NUM_TRACKS * 2

FILE_LAYOUT = {
    ".pat": {"stepdata": 8, "mainstep": 50184},
    ".prf": {"stepdata": 1098, "mainstep": 51274},
    ".all": {"stepdata": 1097, "mainstep": 51273},
}

DATA_TYPES = (
    "active",
    "volume",
    "probability",
    "note",
    "target1",
    "target2",
    "amount1",
    "amount2",
)

VALID_PATTERNS = set(range(NUM_PATTERNS))
VALID_TRACKS = set(range(NUM_TRACKS))
VALID_DATA = set(DATA_TYPES)


def _split_list(value):
    """Split a comma- and/or whitespace-separated value into stripped tokens."""
    result = []
    for chunk in value.split(","):
        for token in chunk.split():
            if token:
                result.append(token)
    return result


def _parse_int(value, label):
    try:
        return int(value)
    except (TypeError, ValueError):
        raise SystemExit(
            "error: %s expects an integer (with optional + or - sign), got %r"
            % (label, value)
        )


def _parse_int_list(value, valid, label):
    result = []
    for token in _split_list(value):
        number = _parse_int(token, label)
        if number not in valid:
            raise SystemExit(
                "error: %s value %d is out of range; allowed: %s"
                % (label, number, sorted(valid))
            )
        if number not in result:
            result.append(number)
    return result


def _parse_str_list(value, valid, label):
    result = []
    for token in _split_list(value):
        if token not in valid:
            raise SystemExit(
                "error: %s value %r is not recognized; allowed: %s"
                % (label, token, sorted(valid))
            )
        if token not in result:
            result.append(token)
    return result


def _parse_arguments(argv):
    """Parse the command line into a normalized options dict."""
    known = {"step", "substep", "pattern", "track", "data"}
    options = {}
    positional = []

    i = 0
    while i < len(argv):
        token = argv[i]
        key = None
        value = None

        if "=" in token:
            lhs, rhs = token.split("=", 1)
            candidate = lhs.lstrip("-").lower()
            if candidate in known:
                key = candidate
                value = rhs
        else:
            candidate = token.lstrip("-").lower()
            if candidate in known:
                key = candidate
                if i + 1 >= len(argv):
                    raise SystemExit("error: option %r requires a value" % token)
                value = argv[i + 1]
                i += 1

        if key is not None:
            options[key] = value
            i += 1
            continue

        positional.append(token)
        i += 1

    if len(positional) != 1:
        raise SystemExit(
            "error: expected exactly one file argument "
            "(usage: rotation_correction.py <file> step=<N>|substep=<N> [options])"
        )

    filename = positional[0]
    ext = os.path.splitext(filename)[1].lower()
    if ext not in FILE_LAYOUT:
        raise SystemExit(
            "error: unsupported file type %r; expected .pat, .prf, or .all" % ext
        )

    if "step" not in options and "substep" not in options:
        raise SystemExit("error: at least one of step=<N> or substep=<N> is required")

    step = _parse_int(options["step"], "step") if "step" in options else 0
    substep = _parse_int(options["substep"], "substep") if "substep" in options else 0

    patterns = list(range(NUM_PATTERNS))
    tracks = list(range(NUM_TRACKS))
    data_types = list(DATA_TYPES)

    if "pattern" in options:
        patterns = _parse_int_list(options["pattern"], VALID_PATTERNS, "pattern")
    if "track" in options:
        tracks = _parse_int_list(options["track"], VALID_TRACKS, "track")
    if "data" in options:
        data_types = _parse_str_list(options["data"], VALID_DATA, "data")

    return {
        "filename": filename,
        "ext": ext,
        "step": step,
        "substep": substep,
        "patterns": patterns,
        "tracks": tracks,
        "data_types": data_types,
    }


def _rotate(seq, amount):
    """Return a copy of seq rotated forward by amount (negative = backward)."""
    if not seq:
        return seq
    shift = amount % len(seq)
    if shift == 0:
        return list(seq)
    return seq[-shift:] + seq[:-shift]


def _step_to_columns(record):
    """Split one 7-byte StepData record into its eight logical columns."""
    volume_byte = record[0]
    return {
        "active": (volume_byte >> 7) & 0x01,
        "volume": volume_byte & 0x7F,
        "probability": record[1],
        "note": record[2],
        "target1": record[3],
        "amount1": record[4],
        "target2": record[5],
        "amount2": record[6],
    }


def _columns_to_step(cols):
    """Reassemble a StepData record from its eight logical columns."""
    volume_byte = ((cols["active"] & 0x01) << 7) | (cols["volume"] & 0x7F)
    return bytes((
        volume_byte,
        cols["probability"] & 0xFF,
        cols["note"] & 0xFF,
        cols["target1"] & 0xFF,
        cols["amount1"] & 0xFF,
        cols["target2"] & 0xFF,
        cols["amount2"] & 0xFF,
    ))


def _main_step_to_bits(word):
    """Expand a 16-bit gate mask into a list of 16 bits (LSB first)."""
    return [(word >> i) & 0x01 for i in range(MAIN_STEPS_PER_TRACK)]


def _bits_to_main_step(bits):
    """Collapse a list of 16 bits back into a 16-bit gate mask (LSB first)."""
    word = 0
    for i, bit in enumerate(bits):
        word |= (bit & 0x01) << i
    return word


def _step_data_offset(ext, track, pattern, step):
    """Byte offset of one sub-step inside the file."""
    base = FILE_LAYOUT[ext]["stepdata"]
    index = track * (NUM_PATTERNS * STEPS_PER_TRACK) + pattern * STEPS_PER_TRACK + step
    return base + index * STEP_DATA_BYTES


def _main_step_offset(ext, pattern, track):
    """Byte offset of one 16-bit main-step gate mask inside the file."""
    base = FILE_LAYOUT[ext]["mainstep"]
    return base + (pattern * NUM_TRACKS + track) * 2


def _rotate_track_step_data(data, ext, pattern, track, substep_shift, data_types):
    """Rotate the selected data-type columns for one track in place."""
    records = []
    for step in range(STEPS_PER_TRACK):
        off = _step_data_offset(ext, track, pattern, step)
        records.append(_step_to_columns(data[off:off + STEP_DATA_BYTES]))

    selected = set(data_types)
    for dtype in DATA_TYPES:
        if dtype not in selected:
            continue
        column = [rec[dtype] for rec in records]
        rotated = _rotate(column, substep_shift)
        for rec, value in zip(records, rotated):
            rec[dtype] = value

    for step, rec in enumerate(records):
        off = _step_data_offset(ext, track, pattern, step)
        data[off:off + STEP_DATA_BYTES] = _columns_to_step(rec)


def _rotate_track_main_step(data, ext, pattern, track, mainstep_shift):
    """Rotate one track's 16-bit gate mask in place."""
    off = _main_step_offset(ext, pattern, track)
    word = data[off] | (data[off + 1] << 8)
    bits = _rotate(_main_step_to_bits(word), mainstep_shift)
    word = _bits_to_main_step(bits)
    data[off] = word & 0xFF
    data[off + 1] = (word >> 8) & 0xFF


def main(argv):
    opts = _parse_arguments(argv)
    filename = opts["filename"]
    ext = opts["ext"]
    step = opts["step"]
    substep = opts["substep"]
    patterns = opts["patterns"]
    tracks = opts["tracks"]
    data_types = opts["data_types"]

    with open(filename, "rb") as fh:
        data = bytearray(fh.read())

    required = FILE_LAYOUT[ext]["mainstep"] + MAIN_STEP_SIZE
    if len(data) < required:
        raise SystemExit(
            "error: %r is only %d bytes; expected at least %d for a %s file"
            % (filename, len(data), required, ext)
        )

    substep_shift = step * SUBSTEPS_PER_MAIN_STEP + substep
    mainstep_shift = step
    rotate_gate_mask = set(data_types) == set(DATA_TYPES)

    for pattern in patterns:
        for track in tracks:
            _rotate_track_step_data(data, ext, pattern, track, substep_shift, data_types)
            if rotate_gate_mask:
                _rotate_track_main_step(data, ext, pattern, track, mainstep_shift)

    with open(filename, "wb") as fh:
        fh.write(data)

    print(
        "rotated %s: step=%+d substep=%+d patterns=%s tracks=%s data=%s"
        % (
            filename,
            step,
            substep,
            ",".join(str(p) for p in patterns),
            ",".join(str(t) for t in tracks),
            ",".join(data_types),
        )
    )


if __name__ == "__main__":
    main(sys.argv[1:])
