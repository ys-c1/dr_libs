# dr_mp3 bad-frame recovery tests

These tests cover the bad-frame recovery support added to `dr_mp3.h`. A single
undecodable MP3 frame no longer aborts the whole decode: the decoder resynchronizes
to the next frame, keeps going, and reports what it skipped.

## What is tested

`generate_vectors.py` regenerates five deterministic MP3 files (they are build
artifacts and are not committed):

| File             | Content                                                        | Expected result                                            |
|------------------|----------------------------------------------------------------|------------------------------------------------------------|
| `normal.mp3`     | Valid tone/silence/tone stream                                | Decodes fully, zero bad frames.                            |
| `single_bad.mp3` | One frame in the silent section made undecodable              | Recovers, one bad frame reported at the right byte offset. |
| `multi_bad.mp3`  | Three such frames                                              | Recovers, three bad frames with their positions.           |
| `truncated.mp3`  | Stream cut halfway through the final frame                    | All complete frames decode; the partial frame is reported. |
| `invalid_header.mp3` | No valid MP3 header anywhere                              | `drmp3_init*` fails.                                       |

The corrupt frames are taken from the **silent** section on purpose. The IMDCT
overlap-add and polyphase filter bank carry state across frames, so the samples
immediately after a corrupted *audible* frame depend on data that cannot be
recovered. Corrupting a frame that already encodes digital silence means the
decoder's silence-based state reconstruction is exactly equivalent to the real
frame, and every surviving output sample matches the original sample-for-sample.

The MP3 is encoded with the bit reservoir disabled (`-reservoir 0`) so every frame
is self-contained (`main_data_begin == 0`). A bad frame sets `main_data_begin` to
its 9-bit maximum, which makes layer III reservoir restoration fail
deterministically without parsing invalid Huffman data and without poisoning later
frames.

## Running

```
python3 -I run_recovery_tests.py
```

The script compiles `dr_mp3_recovery_probe.c`, regenerates the vectors, and runs the
full acceptance set through both the file and memory backends:

* recovery outcome for all five files;
* bad-frame counts and skip positions checked against `manifest.json`;
* sample-for-sample comparison of every recoverable file against `normal.mp3`;
* the configurable skip cap (`drmp3_set_max_bad_frames_to_skip`) aborting with an
  explicit error.

`ffmpeg` with `libmp3lame` is required to generate the vectors. Nothing in this
directory needs to be built for the normal library; the probe is a standalone
harness for these tests.

## Library API under test

* `drmp3_set_max_bad_frames_to_skip()` - cap the number of bad frames that may be
  skipped; 0 (the default) means unlimited.
* `drmp3_get_bad_frame_count()` - total bad frames skipped for the decoder's
  lifetime.
* `drmp3_get_bad_frames()` - the most recent bad-frame positions (frame ordinal,
  stream byte offset, bytes skipped).
* `drmp3_get_decode_error()` - true when decoding stopped on an error (for example
  the skip cap) rather than a clean end of stream.
