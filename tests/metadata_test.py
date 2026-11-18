#!/usr/bin/env python3
"""
End-to-end test for tests/dr_libs_metadata.c.

Generates 5 small FLAC samples (using ffmpeg only as an encoder/muxer, plus
byte-level patching for the cases an encoder cannot produce), runs the C
metadata tool on them, and verifies the output against:
  1. an independent from-spec FLAC metadata parser below (primary reference),
  2. ffprobe/ffmpeg (system reference tool) for tags and cover-art bytes.

Also enforces: parse overhead < 5% of whole-file read time, corrupted comment
blocks skipped and counted, unknown blocks skipped and counted.

Run once:  python3 tests/metadata_test.py
"""

import hashlib
import json
import math
import os
import struct
import subprocess
import sys
import wave
import zlib

ROOT = os.path.dirname(os.path.abspath(__file__))
ART = os.path.normpath(os.path.join(ROOT, "..", "artifacts", "metadata"))
TOOL = os.path.join(ART, "dr_libs_metadata")

# ---------------------------------------------------------------- sample gen

def make_tone_wav(path, seconds=6.0, rate=44100):
    """Mono 16-bit tone with a little deterministic noise (so FLAC has real payload)."""
    n = int(seconds * rate)
    state = 12345
    frames = bytearray()
    for i in range(n):
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        noise = ((state >> 16) & 0x7FFF) / 32768.0 - 0.5
        s = 0.6 * math.sin(2 * math.pi * 440 * i / rate) + 0.05 * noise
        frames += struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))

def make_png(path, w=8, h=8):
    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))
    raw = b""
    for y in range(h):
        raw += b"\x00"  # filter: none
        for x in range(w):
            raw += bytes([(x * 32) & 0xFF, (y * 32) & 0xFF, ((x + y) * 16) & 0xFF])
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(data)

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("command failed: %s\n%s" % (" ".join(cmd), r.stderr))
    return r

# ------------------------------------------------------- flac block helpers

def parse_blocks(data):
    assert data[:4] == b"fLaC", "not a native FLAC file"
    pos = 4
    blocks = []
    while True:
        hdr = data[pos]
        last = bool(hdr & 0x80)
        btype = hdr & 0x7F
        blen = int.from_bytes(data[pos + 1:pos + 4], "big")
        blocks.append({"type": btype, "last": last, "off": pos, "len": blen,
                       "body": data[pos + 4:pos + 4 + blen]})
        pos += 4 + blen
        if last:
            break
    return blocks, pos  # pos = offset of first audio frame

def splice_blocks(data, blocks, audio_off):
    out = bytearray(b"fLaC")
    for i, b in enumerate(blocks):
        hdr = b["type"] | (0x80 if i == len(blocks) - 1 else 0)
        out += bytes([hdr]) + len(b["body"]).to_bytes(3, "big") + b["body"]
    out += data[audio_off:]
    return bytes(out)

def make_vorbis_body(vendor, comments):
    body = struct.pack("<I", len(vendor)) + vendor
    body += struct.pack("<I", len(comments))
    for c in comments:
        body += struct.pack("<I", len(c)) + c
    return body

def set_vorbis_comment(path, vendor, comments, corrupt_count=False, move_to_front=False):
    data = open(path, "rb").read()
    blocks, audio_off = parse_blocks(data)
    body = make_vorbis_body(vendor, comments)
    if corrupt_count:
        # Keep vendor intact, poison the comment count: lengths no longer fit
        # inside the block, so a validating parser must skip the block.
        body = body[:4 + len(vendor)] + struct.pack("<I", 0xFFFFFFFF) + body[8 + len(vendor):]
    idx = next((i for i, b in enumerate(blocks) if b["type"] == 4), None)
    if idx is None:
        blocks.insert(1, {"type": 4, "body": body})
    else:
        blocks[idx]["body"] = body
        if move_to_front:
            # Put the (corrupted) comment block before PICTURE to prove that
            # parsing continues past a skipped corrupt block.
            blocks.insert(1, blocks.pop(idx))
    open(path, "wb").write(splice_blocks(data, blocks, audio_off))

def inject_blocks(path, new_blocks):
    """Insert raw metadata blocks right after STREAMINFO."""
    data = open(path, "rb").read()
    blocks, audio_off = parse_blocks(data)
    for nb in new_blocks:
        blocks.insert(1, nb)
    open(path, "wb").write(splice_blocks(data, blocks, audio_off))

# ------------------------------------------- independent reference parser

def ref_parse(path):
    """From-spec FLAC metadata parser. Independent of the C implementation."""
    data = open(path, "rb").read()
    blocks, _ = parse_blocks(data)
    kvs, pics, unknown, corrupt = [], [], 0, 0
    vendor = None
    for b in blocks:
        t, body = b["type"], b["body"]
        if t == 4:
            ok, v, pairs = ref_vorbis(body)
            if ok:
                vendor = v
                kvs.extend(pairs)
            else:
                corrupt += 1
        elif t == 6:
            pics.append(ref_picture(body))
        elif t in (2, 5) or 7 <= t <= 126:
            unknown += 1
    return {"vendor": vendor, "kvs": kvs, "pics": pics,
            "unknown": unknown, "corrupt": corrupt,
            "blocks": [(b["type"], b["len"]) for b in blocks]}

def ref_vorbis(body):
    if len(body) < 8:
        return False, None, []
    pos = 0
    vlen = struct.unpack_from("<I", body, pos)[0]; pos += 4
    if (len(body) - pos) - 4 < vlen:
        return False, None, []
    vendor = body[pos:pos + vlen]; pos += vlen
    count = struct.unpack_from("<I", body, pos)[0]; pos += 4
    if (len(body) - pos) // 4 < count:
        return False, None, []
    pairs = []
    for _ in range(count):
        if len(body) - pos < 4:
            return False, None, []
        clen = struct.unpack_from("<I", body, pos)[0]; pos += 4
        if len(body) - pos < clen:
            return False, None, []
        c = body[pos:pos + clen]; pos += clen
        s = c.decode("utf-8", "replace")
        k, _, v = s.partition("=")
        pairs.append((k, v))
    return True, vendor.decode("utf-8", "replace"), pairs

def ref_picture(body):
    pos = 0
    def u32():
        nonlocal pos
        v = struct.unpack_from(">I", body, pos)[0]; pos += 4
        return v
    ptype = u32()
    mlen = u32(); mime = body[pos:pos + mlen].decode(); pos += mlen
    dlen = u32(); pos += dlen  # description
    w, h = u32(), u32()
    pos += 8  # depth + indexed colors
    dsize = u32()
    payload = body[pos:pos + dsize]
    assert len(payload) == dsize
    return {"type": ptype, "mime": mime, "w": w, "h": h, "data": payload}

# ------------------------------------------------------------- C tool run

def run_c_tool(flac_path, dump_prefix):
    r = run([TOOL, flac_path, dump_prefix])
    out = {"kvs": [], "pics": [], "blocks": []}
    for line in r.stdout.splitlines():
        if line.startswith("BLOCK "):
            # BLOCK <i> type=<n> name=<s> size=<n>
            parts = line.split()
            out["blocks"].append((int(parts[2].split("=")[1]),
                                  int(parts[4].split("=")[1])))
        elif line.startswith("VENDOR "):
            out["vendor"] = line[7:]
        elif line.startswith("KV "):
            k, _, v = line[3:].partition("=")
            out["kvs"].append((k, v))
        elif line.startswith("PIC "):
            f = dict(p.split("=", 1) for p in line.split()[2:])
            out["pics"].append(f)
        elif line.startswith("PICDUMP "):
            out.setdefault("picdumps", []).append(line.split()[2])
        elif line.startswith("SKIP "):
            out["skip"] = dict(p.split("=") for p in line.split()[1:])
        elif line.startswith("TIME "):
            out["time"] = dict(p.split("=") for p in line.split()[1:])
        elif line.startswith("RESULT "):
            out["result"] = line.split()[1]
    return out

# ------------------------------------------------------------ ffprobe refs

def ffprobe_tags(path):
    r = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format_tags",
                        "-of", "json", path], capture_output=True, text=True)
    if r.returncode != 0:
        return None
    return json.loads(r.stdout).get("format", {}).get("tags", {})

def ffmpeg_extract_pic(path, out_path):
    r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", path,
                        "-map", "0:v:0", "-c", "copy", out_path],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out_path):
        return None
    return open(out_path, "rb").read()

# ------------------------------------------------------------------- main

def main():
    os.makedirs(ART, exist_ok=True)
    failures = []

    def check(cond, msg):
        print(("  PASS " if cond else "  FAIL ") + msg)
        if not cond:
            failures.append(msg)

    print("== build ==")
    run(["gcc", "-O2", "-Wall", "-Wextra", "-o", TOOL,
         os.path.join(ROOT, "dr_libs_metadata.c"), "-lm"])
    print("  gcc -O2 -Wall -Wextra: OK")

    print("== generate samples ==")
    wav = os.path.join(ART, "tone.wav")
    png = os.path.join(ART, "cover.png")
    jpg = os.path.join(ART, "cover.jpg")
    make_tone_wav(wav)
    make_png(png)
    run(["ffmpeg", "-v", "error", "-y", "-i", png, "-frames:v", "1", "-q:v", "5", jpg])

    def encode(dst, cover=None):
        cmd = ["ffmpeg", "-v", "error", "-y", "-i", wav]
        if cover:
            cmd += ["-i", cover, "-map", "0:a", "-map", "1:v",
                    "-c:a", "flac", "-c:v", "copy", "-disposition:v:0", "attached_pic"]
        else:
            cmd += ["-c:a", "flac"]
        cmd += ["-map_metadata", "-1", dst]
        run(cmd)

    V = b"drmeta-test/1.0"
    # f1: common tags + PNG cover
    f1 = os.path.join(ART, "f1_basic_png.flac")
    encode(f1, png)
    set_vorbis_comment(f1, V, [b"TITLE=First Song", b"ARTIST=Alice",
                               b"ALBUM=Demo Album", b"DATE=2026", b"TRACKNUMBER=1"])
    # f2: multi-value key, custom key, '=' in value, JPEG cover
    f2 = os.path.join(ART, "f2_multivalue_jpg.flac")
    encode(f2, jpg)
    set_vorbis_comment(f2, V, [b"TITLE=Second Song", b"ARTIST=Alice", b"ARTIST=Bob",
                               b"LYRICIST=Carol", b"X-CUSTOM-KEY=custom-value",
                               b"DESCRIPTION=has = equals"])
    # f3: no tags, no picture
    f3 = os.path.join(ART, "f3_empty.flac")
    encode(f3)
    set_vorbis_comment(f3, V, [])
    # f4: APPLICATION + reserved block type 100 + PNG cover
    f4 = os.path.join(ART, "f4_unknown_blocks.flac")
    encode(f4, png)
    set_vorbis_comment(f4, V, [b"TITLE=Fourth Song", b"ARTIST=Dave"])
    inject_blocks(f4, [
        {"type": 2, "body": b"TEST" + bytes(range(28))},          # APPLICATION
        {"type": 100, "body": bytes(16)},                          # reserved/unknown
    ])
    # f5: corrupted VORBIS_COMMENT (poisoned comment count), PNG cover kept
    f5 = os.path.join(ART, "f5_corrupt_comment.flac")
    encode(f5, png)
    set_vorbis_comment(f5, V, [b"TITLE=Broken Song", b"ARTIST=Eve"],
                       corrupt_count=True, move_to_front=True)

    files = [f1, f2, f3, f4, f5]
    for f in files:
        print("  %-40s %8d bytes" % (os.path.basename(f), os.path.getsize(f)))

    print("== parse + verify ==")
    for idx, path in enumerate(files, 1):
        name = os.path.basename(path)
        print("-- [%d] %s" % (idx, name))
        got = run_c_tool(path, os.path.join(ART, name))
        ref = ref_parse(path)

        # key/value pairs: exact match with the independent reference parser
        check(got["kvs"] == ref["kvs"],
              "kv pairs match reference (%d pairs)" % len(ref["kvs"]))
        for k, v in got["kvs"]:
            print("       KV %s=%s" % (k, v))

        # pictures: count, mime, byte size, and byte-identical payload
        check(len(got["pics"]) == len(ref["pics"]),
              "picture count %d" % len(ref["pics"]))
        for i, (gp, rp) in enumerate(zip(got["pics"], ref["pics"])):
            dump = got["picdumps"][i]
            dumped = open(dump, "rb").read()
            ok = (gp["mime"] == rp["mime"] and int(gp["bytes"]) == len(rp["data"])
                  and dumped == rp["data"])
            check(ok, "pic%d %s %d bytes sha256=%s" %
                  (i, gp["mime"], len(rp["data"]),
                   hashlib.sha256(rp["data"]).hexdigest()[:16]))
            print("       PIC%d mime=%s type=%s dims=%sx%s bytes=%s sniff=%s"
                  % (i, gp["mime"], gp["pictype"], gp["dims"].split("x")[0],
                     gp["dims"].split("x")[1], gp["bytes"], gp["sniff"]))

        # unknown / corrupt accounting
        check(int(got["skip"]["unknown_blocks"]) == ref["unknown"],
              "unknown blocks skipped: %s" % got["skip"]["unknown_blocks"])
        check(int(got["skip"]["corrupt_comment_blocks"]) == ref["corrupt"],
              "corrupt comment blocks skipped: %s" % got["skip"]["corrupt_comment_blocks"])
        check(int(got["skip"]["vorbis_seen"]) - int(got["skip"]["vorbis_parsed"])
              == ref["corrupt"],
              "corrupt count consistent with dr_flac callback delivery")
        print("       SKIP unknown=%s corrupt_comment=%s (vorbis seen=%s parsed=%s)"
              % (got["skip"]["unknown_blocks"], got["skip"]["corrupt_comment_blocks"],
                 got["skip"]["vorbis_seen"], got["skip"]["vorbis_parsed"]))

        # block inventory sanity
        check(got["blocks"] == ref["blocks"], "block inventory matches")

        # ffprobe cross-check (system reference tool). ffmpeg's demuxer
        # renames a few Vorbis keys to its generic names; map them back.
        FFPROBE_KEY_MAP = {"TRACK": "TRACKNUMBER", "COMMENT": "DESCRIPTION"}
        tags = ffprobe_tags(path)
        if tags is None:
            check(ref["corrupt"] > 0, "ffprobe rejects file only when block is corrupt")
        elif ref["corrupt"] > 0:
            print("       note: ffprobe recovered tags %s from corrupt block (lenient); "
                  "our parser correctly skipped it" % sorted(tags))
        else:
            probe_kvs = {}
            for k, v in tags.items():
                k = FFPROBE_KEY_MAP.get(k.upper(), k.upper())
                # ffmpeg joins multi-value fields with ';' — split them back.
                probe_kvs.setdefault(k, []).extend(v.split(";"))
            # ffmpeg's dict keeps only one value per key, so check both
            # directions: every probe value must be one of ours, and every
            # one of our keys must be present in the probe output.
            my_kvs = {}
            for k, v in got["kvs"]:
                my_kvs.setdefault(k.upper(), []).append(v)
            ok = all(v in my_kvs.get(k, []) for k, vals in probe_kvs.items() for v in vals)
            ok = ok and all(k in probe_kvs for k in my_kvs)
            check(ok, "ffprobe format_tags consistent (%s)" %
                  (", ".join(sorted(probe_kvs)) if probe_kvs else "no tags"))
        if tags is not None and ref["pics"]:
            ext = {"image/png": "png", "image/jpeg": "jpg"}.get(
                ref["pics"][0]["mime"], "bin")
            refpic = ffmpeg_extract_pic(path, os.path.join(ART, name + ".ffprobe." + ext))
            check(refpic is not None and refpic == ref["pics"][0]["data"],
                  "ffmpeg-extracted cover bytes identical (%d bytes)"
                  % len(ref["pics"][0]["data"]))

        # timing: parse overhead < 5% of whole-file read
        t = got["time"]
        pct = float(t["parse_overhead_pct"])
        check(got["result"] == "OK" and pct < 5.0,
              "timing: read=%.1fus parse=%.1fus overhead=%.2f%% (<5%%)"
              % (float(t["file_read_us"]), float(t["parse_us"]), pct))

    print("== summary ==")
    if failures:
        print("FAILED: %d check(s)" % len(failures))
        for f in failures:
            print("  - " + f)
        return 1
    print("ALL CHECKS PASSED")
    return 0

if __name__ == "__main__":
    sys.exit(main())
