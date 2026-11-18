#!/usr/bin/env python3
"""Acceptance harness for the dr_mp3 bad-frame recovery work.

It regenerates the five vectors, drives the C recovery probe through both the file and
memory backends, and checks every requirement from the task:

  * normal / single_bad / multi_bad / truncated open and report the expected recovery;
  * invalid_header fails to open;
  * bad-frame counts and skip positions match manifest.json;
  * every PCM sample produced for a recoverable vector matches normal.mp3 sample-for-
    sample after the excised bad frames are accounted for;
  * the configurable skip cap aborts decoding with an explicit error.

The probe binary and all vectors are build/test artifacts. Nothing here needs to be
checked in except this script, the generator and the probe source.
"""

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
PROBE = os.path.join(HERE, "bin", "probe")
SAMPLES_PER_FRAME = 1152

failures = []


def check(condition, message):
    if condition:
        print("  PASS: %s" % message)
    else:
        print("  FAIL: %s" % message)
        failures.append(message)


def run_probe(vector, backend, max_skip=0, out_path=None):
    args = [PROBE]
    if backend == "memory":
        args.append("--memory")
    if max_skip:
        args += ["--max-skip", str(max_skip)]
    if out_path:
        args += ["--out", out_path]
    args.append(vector)
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    report = {}
    for line in result.stdout.decode("utf-8", "replace").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            report[key] = value
    return result.returncode, report


def parse_positions(text):
    if not text:
        return []
    events = []
    for token in text.split(","):
        frame_index, rest = token.split("@")
        stream_cursor, bytes_skipped = rest.split("+")
        events.append((int(frame_index), int(stream_cursor), int(bytes_skipped)))
    return events


def read_s16(path):
    import array
    samples = array.array("h")
    with open(path, "rb") as handle:
        samples.frombytes(handle.read())
    return samples


def excise(samples, frame_indices):
    """Return samples with the given 1152-frame indices removed."""
    import array
    out = array.array("h")
    pos = 0
    for index in frame_indices:
        out += samples[pos:index * SAMPLES_PER_FRAME]
        pos = (index + 1) * SAMPLES_PER_FRAME
    out += samples[pos:]
    return out


def main():
    vectors_dir = os.path.join(HERE, "vectors")
    os.makedirs(os.path.join(HERE, "bin"), exist_ok=True)

    print("Building probe...")
    probe_source = os.path.join(HERE, "dr_mp3_recovery_probe.c")
    compile_cmd = [
        os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Wpedantic", "-std=c99",
        "-I", REPO, "-o", PROBE, probe_source,
    ]
    subprocess.run(compile_cmd, check=True)

    print("Generating vectors...")
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "generate_vectors.py")], check=True)

    with open(os.path.join(vectors_dir, "manifest.json"), encoding="utf-8") as handle:
        manifest = json.load(handle)

    files = manifest["files"]
    frame_count = manifest["frame_count"]

    # Reference output from the untouched normal vector.
    with tempfile.TemporaryDirectory(prefix="dr_mp3_recovery_out_") as out_dir:
        normal_path = os.path.join(out_dir, "normal.s16")
        code, normal_report = run_probe(os.path.join(vectors_dir, "normal.mp3"), "file", out_path=normal_path)
        normal = read_s16(normal_path)

        for backend in ("file", "memory"):
            print("\n== backend: %s ==" % backend)

            # 1. Normal: clean decode, no bad frames, no error.
            code, report = run_probe(os.path.join(vectors_dir, "normal.mp3"), backend)
            check(code == 0, "normal decodes with exit code 0")
            check(int(report["bad_frames"]) == 0, "normal reports zero bad frames")
            check(int(report["decode_error"]) == 0, "normal reports no decode error")
            check(int(report["pcm_frames"]) == frame_count * SAMPLES_PER_FRAME,
                  "normal decodes every PCM frame")

            # 2/3. Single and multiple bad frames: recovery, counts, positions, exact samples.
            for name, expected_count in (("single_bad.mp3", 1), ("multi_bad.mp3", 3)):
                entry = files[name]
                out_path = os.path.join(out_dir, name + ".s16")
                code, report = run_probe(os.path.join(vectors_dir, name), backend, out_path=out_path)
                events = parse_positions(report.get("bad_positions", ""))
                expected_offsets = [item["byte_offset"] for item in entry["bad_frames"]]
                expected_skips = [item["frame_bytes"] for item in entry["bad_frames"]]

                check(code == 0, "%s recovers with exit code 0" % name)
                check(int(report["decode_error"]) == 0, "%s reports no decode error" % name)
                check(int(report["bad_frames"]) == expected_count,
                      "%s reports %d bad frame(s)" % (name, expected_count))
                check(len(events) == expected_count, "%s reports %d position event(s)" % (name, expected_count))
                check([event[1] for event in events] == expected_offsets,
                      "%s skip positions match manifest byte offsets" % name)
                check([event[2] for event in events] == expected_skips,
                      "%s skipped byte counts match manifest frame sizes" % name)

                original_indices = [item["frame_index"] for item in entry["bad_frames"]]
                got = read_s16(out_path)
                expected = excise(normal, original_indices)
                check(got == expected,
                      "%s output is sample-for-sample identical to normal with the bad frame(s) removed" % name)
                check(len(got) == (frame_count - expected_count) * SAMPLES_PER_FRAME,
                      "%s output length equals the surviving frames" % name)

            # 4. Truncated: all complete frames recovered, the partial frame counted as bad.
            out_path = os.path.join(out_dir, "truncated.s16")
            code, report = run_probe(os.path.join(vectors_dir, "truncated.mp3"), backend, out_path=out_path)
            events = parse_positions(report.get("bad_positions", ""))
            recoverable = files["truncated.mp3"]["recoverable_frame_count"]
            check(code == 0, "truncated decodes the complete frames with exit code 0")
            check(int(report["bad_frames"]) == 1, "truncated counts the partial final frame as bad")
            check(events and events[0][1] == files["truncated.mp3"]["bad_frames"][0]["byte_offset"],
                  "truncated reports the partial frame's byte offset")
            got = read_s16(out_path)
            check(got == normal[:recoverable * SAMPLES_PER_FRAME],
                  "truncated output matches normal for every complete frame")

            # 5. Invalid header: opening must fail before any decode.
            code, report = run_probe(os.path.join(vectors_dir, "invalid_header.mp3"), backend)
            check(code != 0 and report.get("open") == "fail",
                  "invalid_header fails to open")

        # Skip cap: multi_bad has three bad frames. Boundary-check the limit on both backends.
        print("\n== skip cap ==")
        for backend in ("file", "memory"):
            for cap in (1, 2):
                code, report = run_probe(os.path.join(vectors_dir, "multi_bad.mp3"), backend, max_skip=cap)
                check(code != 0 and int(report["decode_error"]) == 1,
                      "%s backend aborts with an explicit error when cap %d is reached" % (backend, cap))
                check(int(report["bad_frames"]) == cap,
                      "%s backend stops after exactly %d skipped frame(s)" % (backend, cap))

            code, report = run_probe(os.path.join(vectors_dir, "multi_bad.mp3"), backend, max_skip=3)
            check(code == 0 and int(report["decode_error"]) == 0 and int(report["bad_frames"]) == 3,
                  "%s backend succeeds when the cap equals the number of bad frames" % backend)

    print("\n==== RESULT ====")
    if failures:
        print("%d check(s) failed:" % len(failures))
        for message in failures:
            print("  - %s" % message)
        return 1

    print("All recovery checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
