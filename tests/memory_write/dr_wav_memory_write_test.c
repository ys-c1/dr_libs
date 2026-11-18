/*
Standalone acceptance tests for the 64-bit memory writer in dr_wav.

Covers the four required cases:
  1. Normal placeholder backfill (RIFF and Wave64), byte-identical to file output.
  2. Sparse stream larger than 4GB through a fixed buffer backed by a sparse mmap (no 4GB allocation).
  3. Buffer exhaustion: fixed-buffer overflow (DRWAV_OUT_OF_RANGE) and growable OOM (DRWAV_OUT_OF_MEMORY).
  4. Repeated drwav_uninit() (idempotent) plus drwav_uninit(NULL).

No third-party libraries are used. The >4GB case is POSIX-only (sparse file + mmap) and is skipped elsewhere.
Build through run_tests.sh.
*/
#if defined(__unix__) || defined(__APPLE__)
    #if defined(__linux__)
        #ifndef _GNU_SOURCE
            #define _GNU_SOURCE
        #endif
    #elif !defined(_POSIX_C_SOURCE)
        #define _POSIX_C_SOURCE 200809L
    #endif
#endif

#define DR_WAV_IMPLEMENTATION
#include "../../dr_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #if defined(__linux__)
        #include <linux/falloc.h>
    #endif
    #define MEMWRITE_TEST_POSIX 1
#endif

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("  FAIL [line %d]: ", __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static drwav_data_format make_format(drwav_container container)
{
    drwav_data_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.container     = container;
    fmt.format        = DR_WAVE_FORMAT_PCM;
    fmt.channels      = 2;
    fmt.sampleRate    = 44100;
    fmt.bitsPerSample = 16;
    return fmt;
}

/* Deterministic, small synthetic sample pattern. */
static void fill_frames(drwav_int16* p, drwav_uint64 frameCount, unsigned int channels, unsigned seed)
{
    drwav_uint64 i;
    for (i = 0; i < frameCount * channels; ++i) {
        p[i] = (drwav_int16)(((i * 7331 + seed * 19937) & 0x7FFF) - 0x4000);
    }
}

static unsigned char* read_whole_file(const char* path, size_t* pSize)
{
    FILE* f = fopen(path, "rb");
    unsigned char* buf;
    long sz;

    if (f == NULL) { return NULL; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }

    buf = (unsigned char*)malloc((size_t)sz);
    if (buf == NULL) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *pSize = (size_t)sz;
    return buf;
}

static drwav_uint32 rd_u32le(const unsigned char* p)
{
    return (drwav_uint32)p[0] | ((drwav_uint32)p[1] << 8) | ((drwav_uint32)p[2] << 16) | ((drwav_uint32)p[3] << 24);
}

static drwav_uint64 rd_u64le(const unsigned char* p)
{
    drwav_uint64 v = 0;
    int i;
    for (i = 7; i >= 0; --i) { v = (v << 8) | p[i]; }
    return v;
}

/* Writes the same frames through a file writer and a growable memory writer and compares every byte. */
static int test_backfill_one_container(drwav_container container, const char* tag, const char* file_path)
{
    const drwav_uint64 frameCount = 1024;
    const unsigned int channels   = 2;
    drwav_data_format fmt = make_format(container);
    drwav_int16 frames[1024 * 2];
    drwav wav_file, wav_mem;
    void* mem = NULL;
    size_t mem_size = 0;
    size_t file_size = 0;
    unsigned char* file_buf = NULL;
    drwav_result r1, r2;
    int ok = 1;

    fill_frames(frames, frameCount, channels, 11);

    /* Reference: file writer. */
    if (!drwav_init_file_write(&wav_file, file_path, &fmt, NULL)) {
        printf("  FAIL: %s file writer init\n", tag);
        return 0;
    }
    CHECK(drwav_write_pcm_frames(&wav_file, frameCount, frames) == frameCount, "%s file frames", tag);
    r1 = drwav_uninit(&wav_file);
    CHECK(r1 == DRWAV_SUCCESS, "%s file uninit (%d)", tag, (int)r1);

    /* Growable memory writer. */
    if (!drwav_init_memory_write(&wav_mem, &mem, &mem_size, &fmt, NULL)) {
        printf("  FAIL: %s memory writer init\n", tag);
        return 0;
    }
    CHECK(drwav_write_pcm_frames(&wav_mem, frameCount, frames) == frameCount, "%s mem frames", tag);
    r2 = drwav_uninit(&wav_mem);
    CHECK(r2 == DRWAV_SUCCESS, "%s mem uninit (%d)", tag, (int)r2);

    file_buf = read_whole_file(file_path, &file_size);
    CHECK(file_buf != NULL, "%s read reference file", tag);

    if (file_buf != NULL) {
        CHECK(mem_size == file_size, "%s size: mem=%zu file=%zu", tag, mem_size, file_size);
        if (mem_size == file_size) {
            CHECK(memcmp(mem, file_buf, file_size) == 0, "%s byte-identical to file output", tag);
        }

        /* Header field-level verification of the backfilled placeholders. */
        if (container == drwav_container_riff) {
            drwav_uint32 riff_size = rd_u32le(file_buf + 4);
            drwav_uint32 data_size = rd_u32le(file_buf + 40);
            drwav_uint32 expect_data = (drwav_uint32)(frameCount * channels * sizeof(drwav_int16));
            CHECK(memcmp(file_buf, "RIFF", 4) == 0, "%s RIFF tag", tag);
            CHECK(riff_size == 36 + expect_data, "%s riff size %u == %u", tag, riff_size, 36u + expect_data);
            CHECK(data_size == expect_data, "%s data size %u == %u", tag, data_size, expect_data);
            CHECK(rd_u32le((const unsigned char*)mem + 4) == riff_size, "%s mem riff backfill", tag);
            CHECK(rd_u32le((const unsigned char*)mem + 40) == data_size, "%s mem data backfill", tag);
        } else {
            drwav_uint64 riff_size = rd_u64le(file_buf + 16);
            /* W64 layout: RIFF hdr 80 bytes; data chunk GUID at 80, its 64-bit size field at 96. */
            drwav_uint64 data_size = rd_u64le(file_buf + 96);
            drwav_uint64 expect_data = frameCount * channels * sizeof(drwav_int16);
            static const unsigned char w64_riff_guid[16] = {
                0x72,0x69,0x66,0x66, 0x2E,0x91, 0xCF,0x11, 0xA5,0xD6, 0x28,0xDB,0x04,0xC1,0x00,0x00
            };
            CHECK(memcmp(file_buf, w64_riff_guid, 16) == 0, "%s W64 RIFF GUID", tag);
            CHECK(riff_size == 80 + 24 + expect_data, "%s w64 riff size %llu", tag, (unsigned long long)riff_size);
            CHECK(data_size == 24 + expect_data, "%s w64 data size %llu", tag, (unsigned long long)data_size);
            CHECK(rd_u64le((const unsigned char*)mem + 16) == riff_size, "%s mem w64 riff backfill", tag);
            CHECK(rd_u64le((const unsigned char*)mem + 96) == data_size, "%s mem w64 data backfill", tag);
        }
    }

    if (g_failures > 0) { ok = 0; }
    free(file_buf);
    drwav_free(mem, NULL);
    remove(file_path);
    return ok;
}

/* A writer with zero audio frames must still produce correct headers via the uninit-time backfill. */
static int test_backfill_zero_frames(const char* file_path)
{
    drwav_data_format fmt = make_format(drwav_container_riff);
    drwav wav_mem, wav_file;
    void* mem = NULL;
    size_t mem_size = 0, file_size = 0;
    unsigned char* file_buf = NULL;
    drwav_result r;
    int ok = 1;

    CHECK(drwav_init_memory_write(&wav_mem, &mem, &mem_size, &fmt, NULL), "zero-frame mem init");
    r = drwav_uninit(&wav_mem);
    CHECK(r == DRWAV_SUCCESS, "zero-frame uninit (%d)", (int)r);
    CHECK(mem_size == 44, "zero-frame mem size %zu == 44", mem_size);
    CHECK(rd_u32le((const unsigned char*)mem + 4) == 36, "zero-frame riff size backfilled to 36");
    CHECK(rd_u32le((const unsigned char*)mem + 40) == 0, "zero-frame data size backfilled to 0");

    CHECK(drwav_init_file_write(&wav_file, file_path, &fmt, NULL), "zero-frame file init");
    CHECK(drwav_uninit(&wav_file) == DRWAV_SUCCESS, "zero-frame file uninit");
    file_buf = read_whole_file(file_path, &file_size);
    CHECK(file_buf != NULL && file_size == mem_size && memcmp(file_buf, mem, mem_size) == 0,
          "zero-frame output identical to file writer");

    if (g_failures > 0) { ok = 0; }
    free(file_buf);
    drwav_free(mem, NULL);
    remove(file_path);
    return ok;
}

/*
Growable-mode sparse positioning: seek past the current end, then write. The realloc gap must be
zero-filled (matching file semantics) even though the jump crosses several capacity doublings.
*/
static int test_growable_sparse_gap(void)
{
    drwav_data_format fmt = make_format(drwav_container_riff);
    drwav wav;
    void* mem = NULL;
    size_t mem_size = 0;
    const drwav_uint64 gap = 4096;                 /* Well past the initial 256-byte capacity. */
    const drwav_uint64 markerOff = 44 + gap;       /* Right after the data chunk header. */
    const unsigned char marker[8] = {0xDE,0xAD,0xBE,0xEF,0x01,0x02,0x03,0x04};
    drwav_uint64 i;
    int gapZero = 1;
    int ok = 1;

    printf("[1b] Growable sparse seek + realloc gap zero-fill\n");

    CHECK(drwav_init_memory_write(&wav, &mem, &mem_size, &fmt, NULL), "growable sparse init");

    /* Jump beyond the current logical end; growable mode allows seeks up to the (unbounded) capacity. */
    CHECK(wav.onSeek(wav.pUserData, (int)markerOff, DRWAV_SEEK_SET), "sparse seek past end");
    CHECK(wav.memoryStreamWrite.currentWritePos == markerOff, "cursor positioned at %llu",
          (unsigned long long)markerOff);

    CHECK(wav.onWrite(wav.pUserData, marker, sizeof(marker)) == sizeof(marker), "sparse marker write");
    CHECK(mem_size == (size_t)(markerOff + sizeof(marker)), "stream size grew to %zu", mem_size);

    /* Every byte in the skipped gap must read back as zero. */
    for (i = 44; i < markerOff; ++i) {
        if (((const unsigned char*)mem)[i] != 0) { gapZero = 0; break; }
    }
    CHECK(gapZero, "realloc gap zero-filled across capacity doublings");
    CHECK(memcmp((const unsigned char*)mem + markerOff, marker, sizeof(marker)) == 0, "marker intact after sparse write");

    /* Seeking before the start must fail and record DRWAV_BAD_SEEK. */
    CHECK(wav.onSeek(wav.pUserData, -1, DRWAV_SEEK_SET) == DRWAV_FALSE, "negative seek rejected");
    CHECK(wav.memoryStreamWrite.error == DRWAV_BAD_SEEK, "negative seek records DRWAV_BAD_SEEK");
    CHECK(drwav_uninit(&wav) == DRWAV_BAD_SEEK, "uninit surfaces DRWAV_BAD_SEEK");

    if (g_failures > 0) { ok = 0; }
    drwav_free(mem, NULL);
    return ok;
}

static int test_normal_backfill(const char* dir)
{
    char path[256];
    int ok = 1;
    printf("[1] Normal placeholder backfill + byte-identical file output\n");

    snprintf(path, sizeof(path), "%s/ref_riff.wav", dir);
    ok &= test_backfill_one_container(drwav_container_riff, "RIFF", path);

    snprintf(path, sizeof(path), "%s/ref_w64.wav", dir);
    ok &= test_backfill_one_container(drwav_container_w64, "W64 ", path);

    snprintf(path, sizeof(path), "%s/ref_zero.wav", dir);
    ok &= test_backfill_zero_frames(path);

    ok &= test_growable_sparse_gap();

    return ok;
}

#if MEMWRITE_TEST_POSIX

/*
Reference stream built on positional pwrite()/an explicit 64-bit cursor. drwav interleaves seeks (for
header backfills) and writes, so relying on the fd's implicit offset would let lseek() and write() fight
over the position. The cursor below is the single source of truth; lseek() on the fd is never used for
data placement, which also keeps every offset 64-bit without any chunked-seek limitation.
*/
typedef struct
{
    int fd;
    drwav_int64 cursor;
    drwav_int64 endPos;   /* Logical end of the stream (highest position written); basis for SEEK_END. */
} ref_stream;

static size_t ref_on_write(void* pUserData, const void* pData, size_t bytesToWrite)
{
    ref_stream* s = (ref_stream*)pUserData;
    ssize_t n = pwrite(s->fd, pData, bytesToWrite, (off_t)s->cursor);
    if (n < 0) {
        return 0;
    }
    s->cursor += n;
    if (s->cursor > s->endPos) {
        s->endPos = s->cursor;
    }
    return (size_t)n;
}

static drwav_bool32 ref_on_seek(void* pUserData, int offset, drwav_seek_origin origin)
{
    ref_stream* s = (ref_stream*)pUserData;
    drwav_int64 base;

    if (origin == DRWAV_SEEK_SET) {
        base = 0;
    } else if (origin == DRWAV_SEEK_CUR) {
        base = s->cursor;
    } else {
        base = s->endPos;   /* DRWAV_SEEK_END */
    }

    if ((drwav_int64)offset < 0 && base < -(drwav_int64)offset) {
        return DRWAV_FALSE;
    }
    s->cursor = base + (drwav_int64)offset;
    return DRWAV_TRUE;
}

/*
Case 2: a Wave64 stream whose audio region crosses 4GB, stored in a fixed buffer backed by a sparse
mmap. After writing, the 4GB zero gap is punched back out as a filesystem hole (FALLOC_FL_PUNCH_HOLE),
so only ~68 MiB of real sample data occupies storage. This proves:
  - the internal cursor is 64-bit and is not truncated at the 2GB/4GB boundaries;
  - W64 64-bit RIFF/data size fields are backfilled correctly for sizes past 4GB;
  - sparse positioning (writing far past the current end) works in a fixed buffer;
  - output matches the stdio file writer byte-for-byte.
*/
#define SPARSE_GAP_FRAMES     ((drwav_uint64)1024*1024*1024)  /* 1 GiB frames * 4 bytes/frame = 4 GiB gap */
#define SPARSE_BLOCK_FRAMES   ((drwav_uint64)1024*1024)       /* 1 Mi frames (4 MiB) per write call */
#define SPARSE_TAIL_BLOCKS    16                              /* 16 blocks = 64 MiB of audio after the gap */

/* Mirrors fill_frames' global-sample-index formula so anchor expectations can be computed independently. */
static drwav_int16 expected_sample(drwav_uint64 globalSampleIndex, unsigned seed)
{
    return (drwav_int16)(((globalSampleIndex * 7331ULL + (drwav_uint64)seed * 19937ULL) & 0x7FFFULL) - 0x4000ULL);
}

static int test_sparse_4gb(const char* dir)
{
    const unsigned int channels = 2;
    const drwav_uint64 bytesPerFrame = channels * (drwav_uint64)sizeof(drwav_int16); /* 4 */
    drwav_data_format fmt = make_format(drwav_container_w64);
    /* Layout: block A (1 block) + 4GiB zero gap (1023 blocks) + tail (16 blocks), all contiguous. */
    drwav_uint64 totalFrames = SPARSE_GAP_FRAMES + (drwav_uint64)SPARSE_TAIL_BLOCKS * SPARSE_BLOCK_FRAMES;
    drwav_uint64 totalAudioBytes = totalFrames * bytesPerFrame;
    drwav_uint64 totalFileSize = 80 + 24 + totalAudioBytes;             /* W64: RIFF hdr 80, data chunk hdr 24 */
    drwav_uint64 cap = totalFileSize;
    drwav_int16* chunk = NULL;
    drwav wav_mem;
    int ref_fd = -1;
    int fd = -1;
    unsigned char* map = (unsigned char*)MAP_FAILED;
    char map_path[256], file_path[256];
    size_t data_size_out = 0;
    drwav_result r;
    struct stat st_map, st_ref;
    int ok = 1;
    drwav_uint64 i;
    /* The zero gap in file coordinates: starts after block A, ends just before the tail. */
    drwav_uint64 gapStart = 104 + SPARSE_BLOCK_FRAMES * bytesPerFrame;
    drwav_uint64 gapBytes = (SPARSE_GAP_FRAMES - SPARSE_BLOCK_FRAMES) * bytesPerFrame;

    printf("[2] Sparse Wave64 stream past 4GB (fixed buffer, sparse mmap)\n");
    printf("    logical size: %.3f GiB; real sample data: %.0f MiB; gap: 4 GiB (punched as a hole)\n",
           (double)totalFileSize / (1024.0*1024.0*1024.0),
           (double)((SPARSE_BLOCK_FRAMES + (drwav_uint64)SPARSE_TAIL_BLOCKS * SPARSE_BLOCK_FRAMES) * bytesPerFrame) / (1024.0*1024.0));

    snprintf(map_path, sizeof(map_path), "%s/sparse_map.w64", dir);
    snprintf(file_path, sizeof(file_path), "%s/sparse_ref.w64", dir);

    /* Sparse backing file for the fixed buffer. */
    fd = open(map_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0, "open sparse backing file");
    if (fd < 0) { return 0; }

    CHECK(ftruncate(fd, (off_t)cap) == 0, "ftruncate map to %llu bytes", (unsigned long long)cap);
    map = (unsigned char*)mmap(NULL, (size_t)cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED, "mmap fixed buffer");
    if (map == MAP_FAILED) { close(fd); remove(map_path); return 0; }

    chunk = (drwav_int16*)malloc((size_t)(SPARSE_BLOCK_FRAMES * bytesPerFrame));
    CHECK(chunk != NULL, "allocate write chunk");
    if (chunk == NULL) { munmap(map, (size_t)cap); close(fd); remove(map_path); return 0; }

    /* Fixed-buffer writer over the sparse mapping (caller-owned, never reallocated). */
    if (!drwav_init_fixed_memory_write(&wav_mem, map, cap, &data_size_out, &fmt, NULL)) {
        printf("  FAIL: fixed memory writer init\n");
        free(chunk); munmap(map, (size_t)cap); close(fd); remove(map_path);
        return 0;
    }

    /*
    Stream layout, all through the public drwav_write_pcm_frames() API, in stream order:
      block A (non-zero, 4 MiB) -> 4 GiB zero gap -> 16 tail blocks (non-zero, 64 MiB).
    */
    fill_frames(chunk, SPARSE_BLOCK_FRAMES, channels, 1);
    CHECK(drwav_write_pcm_frames(&wav_mem, SPARSE_BLOCK_FRAMES, chunk) == SPARSE_BLOCK_FRAMES, "sparse block A write");
    CHECK(wav_mem.memoryStreamWrite.currentWritePos == 104 + SPARSE_BLOCK_FRAMES * bytesPerFrame,
          "64-bit cursor after block A (%llu)", (unsigned long long)wav_mem.memoryStreamWrite.currentWritePos);

    /* Zero gap: 1023 blocks so the running frame total lands exactly on SPARSE_GAP_FRAMES. */
    memset(chunk, 0, (size_t)(SPARSE_BLOCK_FRAMES * bytesPerFrame));
    for (i = 0; i < (SPARSE_GAP_FRAMES / SPARSE_BLOCK_FRAMES) - 1; ++i) {
        drwav_uint64 w = drwav_write_pcm_frames(&wav_mem, SPARSE_BLOCK_FRAMES, chunk);
        CHECK(w == SPARSE_BLOCK_FRAMES, "sparse gap zero write %llu", (unsigned long long)i);
    }
    /* This is the boundary check: the cursor must hold a value above 0x100000000 without truncation. */
    CHECK(wav_mem.memoryStreamWrite.currentWritePos >= (drwav_uint64)0x100000000ULL + 104,
          "cursor crossed 4GB without truncation (pos=%llu)",
          (unsigned long long)wav_mem.memoryStreamWrite.currentWritePos);

    for (i = 0; i < SPARSE_TAIL_BLOCKS; ++i) {
        fill_frames(chunk, SPARSE_BLOCK_FRAMES, channels, (unsigned)(2 + i));
        CHECK(drwav_write_pcm_frames(&wav_mem, SPARSE_BLOCK_FRAMES, chunk) == SPARSE_BLOCK_FRAMES,
              "sparse tail write %llu", (unsigned long long)i);
    }

    r = drwav_uninit(&wav_mem);
    CHECK(r == DRWAV_SUCCESS, "sparse uninit (%d)", (int)r);
    CHECK(data_size_out == (size_t)cap, "sparse pDataSize %zu == %llu", data_size_out, (unsigned long long)cap);

    /* Verify the backfilled W64 64-bit size fields (must not be truncated). */
    CHECK(rd_u64le(map + 16) == totalFileSize, "w64 riff size %llu == %llu",
          (unsigned long long)rd_u64le(map + 16), (unsigned long long)totalFileSize);
    CHECK(rd_u64le(map + 96) == 24 + totalAudioBytes, "w64 data size %llu == %llu",
          (unsigned long long)rd_u64le(map + 96), (unsigned long long)(24 + totalAudioBytes));

    /* Anchors on both sides of the 4GB line hold the expected pattern (global sample indices). */
    {
        const drwav_int16* before = (const drwav_int16*)(map + 104 + (size_t)((SPARSE_BLOCK_FRAMES - 2) * bytesPerFrame));
        const drwav_int16* after  = (const drwav_int16*)(map + 104 + (size_t)(SPARSE_GAP_FRAMES * bytesPerFrame));
        drwav_uint64 idxBefore = (SPARSE_BLOCK_FRAMES - 2) * channels;
        drwav_uint64 idxAfter  = SPARSE_GAP_FRAMES * channels;
        CHECK(before[0] == expected_sample(idxBefore, 1) && before[1] == expected_sample(idxBefore + 1, 1),
              "anchor before 4GB gap matches pattern");
        CHECK(after[0] == expected_sample(idxAfter, 2) && after[1] == expected_sample(idxAfter + 1, 2),
              "anchor after 4GB gap matches pattern");
    }

    /* Punch the zero gap out of the mmap backing file so its physical footprint stays small. */
#if defined(__linux__) && defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
    {
        int pr = fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)gapStart, (off_t)gapBytes);
        CHECK(pr == 0, "punch hole in mmap backing file");
    }
#endif

    /* Reference: same byte sequence through drwav's generic callback writer onto a pre-sized sparse file. */
    ref_fd = open(file_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(ref_fd >= 0, "open reference sparse file");
    if (ref_fd >= 0) {
        drwav ref_wav;
        ref_stream ref;
        ref.fd = ref_fd;
        ref.cursor = 0;
        ref.endPos = 0;
        CHECK(ftruncate(ref_fd, (off_t)cap) == 0, "ftruncate reference file");

        if (drwav_init_write(&ref_wav, &fmt, ref_on_write, ref_on_seek, &ref, NULL)) {
            fill_frames(chunk, SPARSE_BLOCK_FRAMES, channels, 1);
            CHECK(drwav_write_pcm_frames(&ref_wav, SPARSE_BLOCK_FRAMES, chunk) == SPARSE_BLOCK_FRAMES, "ref block A");
            memset(chunk, 0, (size_t)(SPARSE_BLOCK_FRAMES * bytesPerFrame));
            for (i = 0; i < (SPARSE_GAP_FRAMES / SPARSE_BLOCK_FRAMES) - 1; ++i) {
                CHECK(drwav_write_pcm_frames(&ref_wav, SPARSE_BLOCK_FRAMES, chunk) == SPARSE_BLOCK_FRAMES, "ref gap write");
            }
            for (i = 0; i < SPARSE_TAIL_BLOCKS; ++i) {
                fill_frames(chunk, SPARSE_BLOCK_FRAMES, channels, (unsigned)(2 + i));
                CHECK(drwav_write_pcm_frames(&ref_wav, SPARSE_BLOCK_FRAMES, chunk) == SPARSE_BLOCK_FRAMES, "ref tail write");
            }
            CHECK(drwav_uninit(&ref_wav) == DRWAV_SUCCESS, "ref uninit");
            /* uninit leaves the cursor on the backfilled header; the logical end must still be exact. */
            CHECK((drwav_uint64)ref.endPos == cap, "ref stream logical end at exact capacity (%lld)",
                  (long long)ref.endPos);
#if defined(__linux__) && defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
            {
                int pr = fallocate(ref_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)gapStart, (off_t)gapBytes);
                CHECK(pr == 0, "punch hole in reference file");
            }
#endif

            /* Byte-level comparison of the header and every non-gap region (pread keeps the fd offset untouched). */
            {
                unsigned char ref_hdr[104];
                unsigned char blk_a[4096];
                unsigned char blk_b[4096];
                drwav_uint64 tail_off = 104 + SPARSE_GAP_FRAMES * bytesPerFrame;

                CHECK(pread(ref_fd, ref_hdr, sizeof(ref_hdr), 0) == (ssize_t)sizeof(ref_hdr), "read ref header");
                CHECK(memcmp(ref_hdr, map, 104) == 0, "sparse W64 header byte-identical to file");

                CHECK(pread(ref_fd, blk_a, sizeof(blk_a), 104) == (ssize_t)sizeof(blk_a), "read ref block A");
                CHECK(memcmp(blk_a, map + 104, sizeof(blk_a)) == 0, "block A byte-identical");

                CHECK(pread(ref_fd, blk_b, sizeof(blk_b), (off_t)tail_off) == (ssize_t)sizeof(blk_b), "read ref post-4GB block");
                CHECK(memcmp(blk_b, map + (size_t)tail_off, sizeof(blk_b)) == 0, "post-4GB block byte-identical");
            }
        } else {
            CHECK(0, "reference callback writer init");
        }
        close(ref_fd);
    }

    /* The gap must read back as zero from the mmap store (hole semantics after punching). */
    {
        const unsigned char* gap = map + (size_t)gapStart;
        drwav_uint64 k;
        int gap_zero = 1;
        for (k = 0; k < 4096; ++k) { if (gap[k] != 0) { gap_zero = 0; break; } }
        CHECK(gap_zero, "sparse gap reads back as zero");
    }

    /* Both backing files must be sparse: physical allocation far below logical size. */
    if (fstat(fd, &st_map) == 0 && stat(file_path, &st_ref) == 0) {
        double map_mib = (double)st_map.st_blocks * 512.0 / (1024.0 * 1024.0);
        double ref_mib = (double)st_ref.st_blocks * 512.0 / (1024.0 * 1024.0);
        printf("    sparse footprint: mmap=%.1f MiB, file=%.1f MiB (logical %.0f MiB)\n",
               map_mib, ref_mib, (double)cap / (1024.0 * 1024.0));
        CHECK((drwav_uint64)(st_map.st_blocks * 512) < cap / 16, "mmap backing is sparse");
        CHECK((drwav_uint64)(st_ref.st_blocks * 512) < cap / 16, "reference file is sparse");
    }

    if (g_failures > 0) { ok = 0; }
    free(chunk);
    msync(map, (size_t)cap, MS_SYNC);
    munmap(map, (size_t)cap);
    close(fd);
    remove(map_path);
    remove(file_path);
    return ok;
}
#endif /* MEMWRITE_TEST_POSIX */

/* Case 3a: fixed buffer that is too small. The overflow write is rejected, bytes stay intact. */
static int test_fixed_buffer_overflow(void)
{
    const drwav_uint64 frameCount = 256;
    const unsigned int channels = 2;
    drwav_data_format fmt = make_format(drwav_container_riff);
    drwav_int16 frames[256 * 2];
    drwav wav;
    size_t cap = 64;                       /* Only room for part of the 44-byte header plus nothing useful. */
    unsigned char* small = (unsigned char*)malloc(cap);
    unsigned char* guard = (unsigned char*)malloc(cap);
    size_t data_size_out = 999;
    drwav_uint64 w;
    drwav_result r;
    int ok = 1;

    printf("[3a] Fixed buffer overflow -> DRWAV_OUT_OF_RANGE\n");
    CHECK(small != NULL && guard != NULL, "alloc small buffers");
    fill_frames(frames, frameCount, channels, 7);
    memset(small, 0xAB, cap);
    memset(guard, 0xAB, cap);

    CHECK(drwav_init_fixed_memory_write(&wav, small, cap, &data_size_out, &fmt, NULL), "fixed writer init");
    w = drwav_write_pcm_frames(&wav, frameCount, frames);
    CHECK(w == 0, "overflow write rejected (wrote %llu frames)", (unsigned long long)w);
    CHECK(wav.memoryStreamWrite.error == DRWAV_OUT_OF_RANGE, "sticky error is DRWAV_OUT_OF_RANGE (%d)",
          (int)wav.memoryStreamWrite.error);
    r = drwav_uninit(&wav);
    CHECK(r == DRWAV_OUT_OF_RANGE, "uninit returns DRWAV_OUT_OF_RANGE (%d)", (int)r);
    CHECK(data_size_out <= cap, "reported size never exceeds capacity (%zu)", data_size_out);

    /* Nothing may have been written past the capacity; the untouched tail keeps its guard pattern. */
    CHECK(data_size_out == 44, "only the 44-byte header was written (%zu)", data_size_out);
    CHECK(memcmp(small + 44, guard + 44, cap - 44) == 0, "no write beyond fixed capacity");

    /* An exactly-sized buffer for just the header must succeed for a zero-frame writer. */
    {
        drwav wav2;
        size_t exact = 44, out_sz = 0;
        unsigned char* exact_buf = (unsigned char*)malloc(exact);
        drwav_result r2;
        CHECK(exact_buf != NULL, "alloc exact buffer");
        CHECK(drwav_init_fixed_memory_write(&wav2, exact_buf, exact, &out_sz, &fmt, NULL), "exact-size init");
        r2 = drwav_uninit(&wav2);
        CHECK(r2 == DRWAV_SUCCESS, "exact-size zero-frame uninit (%d)", (int)r2);
        CHECK(out_sz == 44, "exact buffer filled to capacity (%zu)", out_sz);
        CHECK(rd_u32le(exact_buf + 4) == 36 && rd_u32le(exact_buf + 40) == 0, "exact buffer header backfilled");
        free(exact_buf);
    }

    if (g_failures > 0) { ok = 0; }
    free(small);
    free(guard);
    return ok;
}

/* Allocation callbacks used to force an out-of-memory condition in the growable writer. */
static void* oom_realloc(void* p, size_t sz, void* user_data)
{
    int* allow = (int*)user_data;
    if (!*allow) { (void)sz; return NULL; }
    return realloc(p, sz);
}
static void oom_free(void* p, void* user_data)
{
    (void)user_data;
    free(p);
}

/* Case 3b: growable memory writer whose realloc fails -> DRWAV_OUT_OF_MEMORY. */
static int test_growable_oom(void)
{
    drwav_data_format fmt = make_format(drwav_container_riff);
    drwav_int16 frames[512 * 2];
    drwav wav;
    void* mem = NULL;
    size_t mem_size = 0;
    drwav_allocation_callbacks cb;
    int allow_allocs = 0;   /* Block every realloc: even the first header write fails. */
    drwav_result r;
    int ok = 1;

    printf("[3b] Growable buffer allocation failure -> DRWAV_OUT_OF_MEMORY\n");
    memset(&cb, 0, sizeof(cb));
    cb.onRealloc = oom_realloc;
    cb.onFree    = oom_free;
    cb.pUserData = &allow_allocs;
    fill_frames(frames, 512, 2, 3);

    CHECK(drwav_init_memory_write(&wav, &mem, &mem_size, &fmt, &cb), "growable writer init (no alloc yet)");
    CHECK(drwav_write_pcm_frames(&wav, 512, frames) == 0, "write fails when realloc is blocked");
    CHECK(wav.memoryStreamWrite.error == DRWAV_OUT_OF_MEMORY, "sticky error is DRWAV_OUT_OF_MEMORY (%d)",
          (int)wav.memoryStreamWrite.error);
    r = drwav_uninit(&wav);
    CHECK(r == DRWAV_OUT_OF_MEMORY, "uninit returns DRWAV_OUT_OF_MEMORY (%d)", (int)r);
    drwav_free(mem, &cb);

    /* Recovery path: same callbacks with allocations enabled works and backfills correctly. */
    allow_allocs = 1;
    {
        drwav wav2;
        void* mem2 = NULL;
        size_t sz2 = 0;
        CHECK(drwav_init_memory_write(&wav2, &mem2, &sz2, &fmt, &cb), "recovered writer init");
        CHECK(drwav_write_pcm_frames(&wav2, 512, frames) == 512, "recovered writer writes frames");
        CHECK(drwav_uninit(&wav2) == DRWAV_SUCCESS, "recovered writer uninit");
        CHECK(rd_u32le((const unsigned char*)mem2 + 40) == 512u * 2u * 2u, "recovered writer data size backfilled");
        drwav_free(mem2, &cb);
    }

    if (g_failures > 0) { ok = 0; }
    return ok;
}

/* Case 4: repeated uninit must be a safe no-op; NULL must return DRWAV_INVALID_ARGS. */
static int test_repeated_uninit(void)
{
    drwav_data_format fmt = make_format(drwav_container_riff);
    drwav wav_grow, wav_fix;
    void* mem = NULL;
    size_t mem_size = 0, fix_size = 0;
    unsigned char fix_buf[512];   /* 44-byte header + 64 frames * 4 bytes = 300 bytes required. */
    drwav_int16 frames[64 * 2];
    drwav_result r;
    int ok = 1;

    printf("[4] Repeated drwav_uninit (idempotent) and NULL handling\n");
    fill_frames(frames, 64, 2, 5);

    CHECK(drwav_uninit(NULL) == DRWAV_INVALID_ARGS, "uninit(NULL) -> DRWAV_INVALID_ARGS");

    CHECK(drwav_init_memory_write(&wav_grow, &mem, &mem_size, &fmt, NULL), "grow init");
    CHECK(drwav_write_pcm_frames(&wav_grow, 64, frames) == 64, "grow write");
    r = drwav_uninit(&wav_grow);
    CHECK(r == DRWAV_SUCCESS, "first grow uninit");
    r = drwav_uninit(&wav_grow);
    CHECK(r == DRWAV_SUCCESS, "second grow uninit is no-op success");
    r = drwav_uninit(&wav_grow);
    CHECK(r == DRWAV_SUCCESS, "third grow uninit is no-op success");

    CHECK(drwav_init_fixed_memory_write(&wav_fix, fix_buf, sizeof(fix_buf), &fix_size, &fmt, NULL), "fixed init");
    CHECK(drwav_write_pcm_frames(&wav_fix, 64, frames) == 64, "fixed write");
    CHECK(drwav_uninit(&wav_fix) == DRWAV_SUCCESS, "first fixed uninit");
    CHECK(drwav_uninit(&wav_fix) == DRWAV_SUCCESS, "second fixed uninit is no-op");

    /* Output of the growable writer remains valid after repeated uninit. */
    CHECK(mem != NULL && mem_size == 44 + 64 * 4, "buffer intact after repeated uninit (%zu)", mem_size);
    CHECK(rd_u32le((const unsigned char*)mem + 40) == 64u * 4u, "backfill intact after repeated uninit");

    if (g_failures > 0) { ok = 0; }
    drwav_free(mem, NULL);
    return ok;
}

int main(int argc, char** argv)
{
    const char* dir = (argc > 1) ? argv[1] : ".";
    int ok = 1;

    printf("dr_wav memory-writer acceptance tests (output dir: %s)\n\n", dir);

    ok &= test_normal_backfill(dir);
#if MEMWRITE_TEST_POSIX
    ok &= test_sparse_4gb(dir);
#else
    printf("[2] Sparse >4GB test skipped (POSIX sparse mmap unavailable on this platform)\n");
#endif
    ok &= test_fixed_buffer_overflow();
    ok &= test_growable_oom();
    ok &= test_repeated_uninit();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (!ok || g_failures != 0) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
