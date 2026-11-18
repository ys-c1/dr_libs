#!/usr/bin/env python3
"""Generate deterministic MP3 recovery test vectors.

Produces five files in the given output directory (default: tests/recovery/vectors):

    normal.mp3          A valid tone/silence/tone MP3, untouched.
    single_bad.mp3      One MP3 frame inside the silent section made undecodable.
    multi_bad.mp3       Three MP3 frames inside the silent section made undecodable.
    truncated.mp3       The valid stream cut partway through its final frame.
    invalid_header.mp3  No valid MP3 frame header anywhere (open must fail).

Every corrupted frame is taken from the silent section on purpose. The frames encode
digital silence, so losing one costs no audible samples and lets the decoder reproduce
every surviving frame sample-for-sample.

The MP3 is encoded with libmp3lame and the bit reservoir disabled, which makes every
frame self-contained (main_data_begin == 0). A corrupted frame sets main_data_begin to
its 9-bit maximum so layer III restore-reservoir deterministically fails without
parsing any (possibly invalid) Huffman data and without poisoning later frames.

A manifest.json next to the vectors records the ground-truth corruption points. No
binary vectors should be committed; this script regenerates them.
"""

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile

# MPEG1 Layer III bitrate table, index 1..14, kbit/s.
BITRATES_MPEG1_L3 = [None, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, None]
SAMPLE_RATES = {0: 44100, 1: 48000, 2: 32000}
SAMPLES_PER_FRAME = 1152
MAIN_DATA_BEGIN_BITS = 9
MAIN_DATA_BEGIN_MAX = (1 << MAIN_DATA_BEGIN_BITS) - 1  # 511 for MPEG1


def frame_size(header):
    """Frame length in bytes for an MPEG1 Layer III header."""
    if not (header[0] == 0xFF and (header[1] & 0xE0) == 0xE0):
        raise ValueError("not an MP3 frame sync")
    bitrate_index = (header[2] >> 4) & 0x0F
    sample_rate_index = (header[2] >> 2) & 0x03
    padding = (header[2] >> 1) & 0x01
    bitrate = BITRATES_MPEG1_L3[bitrate_index]
    sample_rate = SAMPLE_RATES[sample_rate_index]
    return (144 * bitrate * 1000) // sample_rate + padding


def parse_frames(data):
    """Return a list of absolute byte offsets, one per MPEG1 Layer III frame."""
    offsets = []
    pos = 0
    while pos + 4 <= len(data):
        header = data[pos:pos + 4]
        size = frame_size(header)
        offsets.append(pos)
        pos += size
    if pos != len(data):
        raise ValueError("stream does not end on a frame boundary: %d != %d" % (pos, len(data)))
    return offsets


def main_data_begin(frame):
    """Read the MPEG1 Layer III main_data_begin field (top 9 bits after the 4-byte header)."""
    return ((frame[4] << 8) | frame[5]) >> 7


def corrupt_frame(buf, offset):
    """Force main_data_begin to its maximum so the frame's reservoir restore fails."""
    old = (buf[offset + 4] << 8) | buf[offset + 5]
    new = (old & 0x007F) | (MAIN_DATA_BEGIN_MAX << 7)
    buf[offset + 4] = (new >> 8) & 0xFF
    buf[offset + 5] = new & 0xFF


def run_ffmpeg(args):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        sys.stderr.write(result.stderr.decode("utf-8", "replace"))
        raise RuntimeError("ffmpeg failed: %s" % " ".join(args))


def encode_source(work_dir):
    """Create a 3.6 s mono 44.1 kHz tone/silence/tone WAV and a self-contained MP3."""
    wav_path = os.path.join(work_dir, "mixed.wav")
    mp3_path = os.path.join(work_dir, "mixed.mp3")

    # 440 Hz, a 0.6 s silent gap from 1.5 s to 2.1 s, then the gain envelope brings it back.
    run_ffmpeg([
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=3.6",
        "-af",
        "aformat=sample_fmts=s16:channel_layouts=mono,"
        "volume='if(lt(t,1.5),1,if(lt(t,2.1),0,1))':eval=frame",
        "-ac", "1", wav_path,
    ])

    run_ffmpeg([
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-i", wav_path, "-ac", "1",
        "-c:a", "libmp3lame", "-b:a", "128k",
        "-reservoir", "0",            # Self-contained frames.
        "-id3v2_version", "0",        # No leading ID3v2 tag.
        "-write_xing", "0",           # No Xing/Info frame.
        "-write_id3v1", "0",          # No trailing ID3v1 tag.
        mp3_path,
    ])

    with open(mp3_path, "rb") as handle:
        data = handle.read()
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "vectors"),
                        help="Output directory for the generated vectors.")
    args = parser.parse_args()

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="dr_mp3_recovery_") as work_dir:
        clean = encode_source(work_dir)

    offsets = parse_frames(clean)

    # Sanity: reservoir disabled means every frame must be self-contained.
    for index, offset in enumerate(offsets):
        frame = clean[offset:offset + frame_size(clean[offset:offset + 4])]
        if main_data_begin(frame) != 0:
            raise RuntimeError("frame %d is not self-contained; re-check the encoder options" % index)

    frame_count = len(offsets)

    # Corruption targets sit well inside the silent gap and are never adjacent.
    single_target = frame_count // 2
    multi_targets = [frame_count // 2 - 7, frame_count // 2, frame_count // 2 + 7]

    def write(name, data):
        with open(os.path.join(out_dir, name), "wb") as handle:
            handle.write(data)

    manifest = {
        "sample_rate": 44100,
        "channels": 1,
        "samples_per_frame": SAMPLES_PER_FRAME,
        "frame_count": frame_count,
        "files": {},
    }

    # 1. Normal.
    write("normal.mp3", clean)
    manifest["files"]["normal.mp3"] = {"kind": "normal", "bad_frames": []}

    # 2. Single bad frame.
    single = bytearray(clean)
    corrupt_frame(single, offsets[single_target])
    write("single_bad.mp3", single)
    manifest["files"]["single_bad.mp3"] = {
        "kind": "single_bad",
        "bad_frames": [{
            "frame_index": single_target,
            "byte_offset": offsets[single_target],
            "frame_bytes": frame_size(clean[offsets[single_target]:offsets[single_target] + 4]),
        }],
    }

    # 3. Multiple bad frames.
    multi = bytearray(clean)
    multi_entries = []
    for target in multi_targets:
        corrupt_frame(multi, offsets[target])
        multi_entries.append({
            "frame_index": target,
            "byte_offset": offsets[target],
            "frame_bytes": frame_size(clean[offsets[target]:offsets[target] + 4]),
        })
    write("multi_bad.mp3", multi)
    manifest["files"]["multi_bad.mp3"] = {"kind": "multi_bad", "bad_frames": multi_entries}

    # 4. Truncated: keep every full frame except the last, plus a partial slice of that last frame.
    last_offset = offsets[-1]
    truncated_len = last_offset + frame_size(clean[last_offset:last_offset + 4]) // 2
    write("truncated.mp3", clean[:truncated_len])
    manifest["files"]["truncated.mp3"] = {
        "kind": "truncated",
        "bad_frames": [{
            "frame_index": frame_count - 1,
            "byte_offset": last_offset,
            "frame_bytes": 0,  # The partial frame cannot be resynchronized.
        }],
        "truncated_at": truncated_len,
        "recoverable_frame_count": frame_count - 1,
    }

    # 5. Invalid header: deterministic non-sync bytes with no valid header anywhere.
    invalid = bytes(((index * 73 + 17) & 0xFF for index in range(512)))
    if any(invalid[i] == 0xFF and (invalid[i + 1] & 0xE0) == 0xE0
           for i in range(len(invalid) - 1)):
        raise RuntimeError("internal error: invalid-header vector accidentally contains a sync")
    write("invalid_header.mp3", invalid)
    manifest["files"]["invalid_header.mp3"] = {"kind": "invalid_header", "bad_frames": []}

    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")

    print("Wrote %d vectors and manifest to %s" % (len(manifest["files"]), out_dir))
    for name in sorted(manifest["files"]):
        print("  %s" % name)


if __name__ == "__main__":
    main()
