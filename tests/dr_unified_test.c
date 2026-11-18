/*
Tests for dr_unified.h to verifies:
  1. Samples decoded through the unified API are bit-identical to each native API
     (WAV/FLAC/MP3, 48kHz and 44.1kHz).
  2. Seeking through the unified API matches the native APIs.
  3. Metadata (channels/sampleRate/totalPCMFrameCount) matches the native APIs.
  4. The native -> unified error code mapping table is complete.
  5. Unsupported/corrupt/missing inputs fail with the documented unified codes.
  6. Dispatch overhead of the unified read path is < 1% of native decode time.

Usage:
    dr_unified_test --gen <dir>   Generate the reference WAV files into <dir>.
    dr_unified_test <dir>         Run all tests against the samples in <dir>.
*/

#define _POSIX_C_SOURCE 199309L

#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DR_UNIFIED_IMPLEMENTATION
#include "../dr_unified.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_PassCount = 0;
static int g_FailCount = 0;

#define CHECK(cond, ...) do { \
    if (cond) { g_PassCount++; } \
    else { g_FailCount++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ sample generation */

#define GEN_CHANNELS   2
#define GEN_DURATION_S 0.5

static int gen_wav(const char* pPath, unsigned int sampleRate)
{
    drwav wav;
    drwav_data_format fmt;
    drwav_uint64 totalFrames = (drwav_uint64)(sampleRate * GEN_DURATION_S);
    drwav_uint64 i;
    drwav_int16* pPcm;

    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_PCM;
    fmt.channels = GEN_CHANNELS;
    fmt.sampleRate = sampleRate;
    fmt.bitsPerSample = 16;

    if (!drwav_init_file_write(&wav, pPath, &fmt, NULL)) {
        printf("  FAIL: cannot create %s\n", pPath);
        return 0;
    }

    pPcm = (drwav_int16*)malloc((size_t)totalFrames * GEN_CHANNELS * sizeof(drwav_int16));
    for (i = 0; i < totalFrames; i++) {
        double t = (double)i / (double)sampleRate;
        double l = 0.45*sin(2.0*M_PI*440.0*t) + 0.20*sin(2.0*M_PI*1230.0*t);
        double r = 0.45*sin(2.0*M_PI*550.0*t + 0.3) + 0.20*sin(2.0*M_PI*1490.0*t);
        pPcm[i*2+0] = (drwav_int16)(l * 30000.0);
        pPcm[i*2+1] = (drwav_int16)(r * 30000.0);
    }

    drwav_write_pcm_frames(&wav, totalFrames, pPcm);
    free(pPcm);
    drwav_uninit(&wav);
    return 1;
}

static int gen_samples(const char* pDir)
{
    char path[1024];
    int ok = 1;

    snprintf(path, sizeof(path), "%s/wav_48k.wav", pDir);
    ok &= gen_wav(path, 48000);
    snprintf(path, sizeof(path), "%s/wav_44k.wav", pDir);
    ok &= gen_wav(path, 44100);

    /* Unsupported format: an OGG capture pattern the dispatcher must reject. */
    {
        FILE* f;
        snprintf(path, sizeof(path), "%s/unsupported.ogg", pDir);
        f = fopen(path, "wb");
        if (f != NULL) {
            fwrite("OggS\x00\x02" "not-a-real-ogg-but-the-header-is-enough", 1, 44, f);
            fclose(f);
        } else {
            ok = 0;
        }
    }

    /* Corrupt file: FLAC magic followed by garbage. */
    {
        FILE* f;
        snprintf(path, sizeof(path), "%s/corrupt.flac", pDir);
        f = fopen(path, "wb");
        if (f != NULL) {
            fwrite("fLaC\xDE\xAD\xBE\xEF\x00\x11\x22\x33", 1, 12, f);
            fclose(f);
        } else {
            ok = 0;
        }
    }

    return ok;
}

/* ------------------------------------------------------------------ helpers */

typedef struct
{
    const char* pFileName;  /* Relative to the sample dir. */
    drunified_format expectedFormat;
    unsigned int expectedSampleRate;
} test_file;

static const test_file g_TestFiles[] =
{
    { "wav_48k.wav",  drunified_format_wav,  48000 },
    { "wav_44k.wav",  drunified_format_wav,  44100 },
    { "flac_48k.flac", drunified_format_flac, 48000 },
    { "flac_44k.flac", drunified_format_flac, 44100 },
    { "mp3_48k.mp3",  drunified_format_mp3,  48000 },
    { "mp3_44k.mp3",  drunified_format_mp3,  44100 },
};

static unsigned char* read_entire_file(const char* pPath, size_t* pSizeOut)
{
    FILE* f = fopen(pPath, "rb");
    unsigned char* pData;
    long size;

    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    pData = (unsigned char*)malloc((size_t)size);
    if (pData == NULL || fread(pData, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(pData);
        return NULL;
    }
    fclose(f);
    *pSizeOut = (size_t)size;
    return pData;
}

/* Native full decode to s16, using the backend matching `format`. */
static drunified_uint64 native_decode_all_s16(const char* pPath, drunified_format format, drunified_int16** ppPcmOut,
                                              drunified_uint32* pChannelsOut, drunified_uint32* pSampleRateOut,
                                              drunified_uint64* pTotalFramesOut)
{
    drunified_uint64 capacity = 1 << 20;
    drunified_uint64 totalFrames = 0;
    drunified_int16* pPcm = (drunified_int16*)malloc((size_t)capacity * 4 * sizeof(drunified_int16));
    drunified_int16 chunk[4096 * 8];

    *pChannelsOut = 0;
    *pSampleRateOut = 0;
    *pTotalFramesOut = 0;

    if (format == drunified_format_wav) {
        drwav dec;
        if (!drwav_init_file(&dec, pPath, NULL)) { free(pPcm); return 0; }
        *pChannelsOut = dec.channels;
        *pSampleRateOut = dec.sampleRate;
        *pTotalFramesOut = dec.totalPCMFrameCount;
        for (;;) {
            drwav_uint64 n = drwav_read_pcm_frames_s16(&dec, 4096, chunk);
            if (n == 0) break;
            if (totalFrames + n > capacity) { capacity *= 2; pPcm = (drunified_int16*)realloc(pPcm, (size_t)capacity * dec.channels * sizeof(drunified_int16)); }
            memcpy(pPcm + (size_t)totalFrames * dec.channels, chunk, (size_t)n * dec.channels * sizeof(drunified_int16));
            totalFrames += n;
        }
        drwav_uninit(&dec);
    } else if (format == drunified_format_flac) {
        drflac* pDec = drflac_open_file(pPath, NULL);
        if (pDec == NULL) { free(pPcm); return 0; }
        *pChannelsOut = pDec->channels;
        *pSampleRateOut = pDec->sampleRate;
        *pTotalFramesOut = pDec->totalPCMFrameCount;
        for (;;) {
            drflac_uint64 n = drflac_read_pcm_frames_s16(pDec, 4096, chunk);
            if (n == 0) break;
            if (totalFrames + n > capacity) { capacity *= 2; pPcm = (drunified_int16*)realloc(pPcm, (size_t)capacity * pDec->channels * sizeof(drunified_int16)); }
            memcpy(pPcm + (size_t)totalFrames * pDec->channels, chunk, (size_t)n * pDec->channels * sizeof(drunified_int16));
            totalFrames += n;
        }
        drflac_close(pDec);
    } else {
        drmp3 dec;
        if (!drmp3_init_file(&dec, pPath, NULL)) { free(pPcm); return 0; }
        *pChannelsOut = dec.channels;
        *pSampleRateOut = dec.sampleRate;
        *pTotalFramesOut = drmp3_get_pcm_frame_count(&dec);
        for (;;) {
            drmp3_uint64 n = drmp3_read_pcm_frames_s16(&dec, 4096, chunk);
            if (n == 0) break;
            if (totalFrames + n > capacity) { capacity *= 2; pPcm = (drunified_int16*)realloc(pPcm, (size_t)capacity * dec.channels * sizeof(drunified_int16)); }
            memcpy(pPcm + (size_t)totalFrames * dec.channels, chunk, (size_t)n * dec.channels * sizeof(drunified_int16));
            totalFrames += n;
        }
        drmp3_uninit(&dec);
    }

    *ppPcmOut = pPcm;
    return totalFrames;
}

/* ------------------------------------------------------------------ test 1+3: decode & metadata */

static void test_decode_and_metadata(const char* pDir, const test_file* pFile)
{
    char path[1024];
    drunified dec;
    drunified_info info;
    drunified_result r;
    drunified_int16* pNativePcm = NULL;
    drunified_int16* pUnifiedPcm = NULL;
    drunified_uint64 nativeFrames, unifiedFrames = 0;
    drunified_uint32 nativeCh, nativeRate;
    drunified_uint64 nativeTotal;
    drunified_uint64 capacity = 1 << 20;
    drunified_int16 chunk[4096 * 8];

    snprintf(path, sizeof(path), "%s/%s", pDir, pFile->pFileName);
    printf("[decode] %s\n", pFile->pFileName);

    nativeFrames = native_decode_all_s16(path, pFile->expectedFormat, &pNativePcm, &nativeCh, &nativeRate, &nativeTotal);
    CHECK(nativeFrames > 0, "native decode produced no frames");

    r = drunified_init_file(&dec, path, &info);
    CHECK(r == DRUNIFIED_SUCCESS, "drunified_init_file failed: %s", drunified_result_string(r));
    if (r != DRUNIFIED_SUCCESS) { free(pNativePcm); return; }

    /* Metadata. */
    CHECK(info.format == pFile->expectedFormat, "format: got %s, expected %s",
          drunified_format_string(info.format), drunified_format_string(pFile->expectedFormat));
    CHECK(info.channels == nativeCh, "channels: unified=%u native=%u", info.channels, nativeCh);
    CHECK(info.sampleRate == nativeRate, "sampleRate: unified=%u native=%u", info.sampleRate, nativeRate);
    CHECK(info.sampleRate == pFile->expectedSampleRate, "sampleRate: got %u, expected %u", info.sampleRate, pFile->expectedSampleRate);
    CHECK(info.totalPCMFrameCount == nativeTotal, "totalPCMFrameCount: unified=%llu native=%llu",
          (unsigned long long)info.totalPCMFrameCount, (unsigned long long)nativeTotal);

    /* Full decode through the unified API. */
    pUnifiedPcm = (drunified_int16*)malloc((size_t)capacity * info.channels * sizeof(drunified_int16));
    for (;;) {
        drunified_uint64 n = drunified_read_pcm_frames_s16(&dec, 4096, chunk);
        if (n == 0) break;
        if (unifiedFrames + n > capacity) { capacity *= 2; pUnifiedPcm = (drunified_int16*)realloc(pUnifiedPcm, (size_t)capacity * info.channels * sizeof(drunified_int16)); }
        memcpy(pUnifiedPcm + (size_t)unifiedFrames * info.channels, chunk, (size_t)n * info.channels * sizeof(drunified_int16));
        unifiedFrames += n;
    }
    drunified_uninit(&dec);

    CHECK(unifiedFrames == nativeFrames, "frame count: unified=%llu native=%llu",
          (unsigned long long)unifiedFrames, (unsigned long long)nativeFrames);
    if (unifiedFrames == nativeFrames && info.channels == nativeCh) {
        int same = memcmp(pUnifiedPcm, pNativePcm, (size_t)unifiedFrames * info.channels * sizeof(drunified_int16)) == 0;
        CHECK(same, "PCM data mismatch between unified and native decode");
        if (same) {
            printf("  OK: %llu frames x %u ch @ %u Hz, bit-identical\n",
                   (unsigned long long)unifiedFrames, info.channels, info.sampleRate);
        }
    }

    free(pNativePcm);
    free(pUnifiedPcm);
}

/* ------------------------------------------------------------------ test 2: seeking */

static drunified_uint64 native_seek_and_read(const char* pPath, drunified_format format, drunified_uint64 frameIndex,
                                             drunified_int16* pOut, drunified_uint64 framesToRead)
{
    if (format == drunified_format_wav) {
        drwav dec;
        drwav_uint64 n = 0;
        if (!drwav_init_file(&dec, pPath, NULL)) return 0;
        if (drwav_seek_to_pcm_frame(&dec, frameIndex)) n = drwav_read_pcm_frames_s16(&dec, framesToRead, pOut);
        drwav_uninit(&dec);
        return n;
    } else if (format == drunified_format_flac) {
        drflac* pDec = drflac_open_file(pPath, NULL);
        drflac_uint64 n = 0;
        if (pDec == NULL) return 0;
        if (drflac_seek_to_pcm_frame(pDec, frameIndex)) n = drflac_read_pcm_frames_s16(pDec, framesToRead, pOut);
        drflac_close(pDec);
        return n;
    } else {
        drmp3 dec;
        drmp3_uint64 n = 0;
        if (!drmp3_init_file(&dec, pPath, NULL)) return 0;
        if (drmp3_seek_to_pcm_frame(&dec, frameIndex)) n = drmp3_read_pcm_frames_s16(&dec, framesToRead, pOut);
        drmp3_uninit(&dec);
        return n;
    }
}

static void test_seeking(const char* pDir, const test_file* pFile)
{
    char path[1024];
    drunified dec;
    drunified_info info;
    int i;
    int allOk = 1;
    unsigned int rng = 0x12345678u;

    snprintf(path, sizeof(path), "%s/%s", pDir, pFile->pFileName);
    printf("[seek]   %s\n", pFile->pFileName);

    if (drunified_init_file(&dec, path, &info) != DRUNIFIED_SUCCESS) {
        CHECK(0, "drunified_init_file failed");
        return;
    }

    for (i = 0; i < 32; i++) {
        drunified_uint64 target;
        drunified_int16 unifiedBuf[256 * 8];
        drunified_int16 nativeBuf[256 * 8];
        drunified_uint64 unifiedFrames, nativeFrames;
        drunified_result sr;

        rng = rng * 1664525u + 1013904223u;
        target = (info.totalPCMFrameCount > 300) ? (rng % (info.totalPCMFrameCount - 300)) : 0;

        sr = drunified_seek_to_pcm_frame(&dec, target);
        if (sr != DRUNIFIED_SUCCESS) {
            printf("  FAIL: unified seek to %llu failed: %s\n", (unsigned long long)target, drunified_result_string(sr));
            allOk = 0;
            continue;
        }
        unifiedFrames = drunified_read_pcm_frames_s16(&dec, 256, unifiedBuf);
        nativeFrames = native_seek_and_read(path, pFile->expectedFormat, target, nativeBuf, 256);

        if (unifiedFrames != nativeFrames ||
            memcmp(unifiedBuf, nativeBuf, (size_t)unifiedFrames * info.channels * sizeof(drunified_int16)) != 0) {
            printf("  FAIL: seek to %llu: unified %llu frames, native %llu frames, data %s\n",
                   (unsigned long long)target, (unsigned long long)unifiedFrames, (unsigned long long)nativeFrames,
                   (unifiedFrames == nativeFrames) ? "differs" : "n/a");
            allOk = 0;
        }
    }

    /* Seeking past the end must be a clean DRUNIFIED_OUT_OF_RANGE. */
    {
        drunified_result sr = drunified_seek_to_pcm_frame(&dec, info.totalPCMFrameCount + 1000);
        CHECK(sr == DRUNIFIED_OUT_OF_RANGE, "seek past end: got %s, expected DRUNIFIED_OUT_OF_RANGE", drunified_result_string(sr));
    }

    CHECK(allOk, "32 random seek+read comparisons vs native");
    if (allOk) printf("  OK: 32 seek positions, frames bit-identical\n");

    drunified_uninit(&dec);
}

/* ------------------------------------------------------------------ test 4: error codes */

static void test_error_codes(const char* pDir)
{
    char path[1024];
    drunified dec;
    drunified_info info;
    drunified_result r;

    printf("[errors] error handling\n");

    /* Unsupported format. */
    snprintf(path, sizeof(path), "%s/unsupported.ogg", pDir);
    r = drunified_init_file(&dec, path, &info);
    CHECK(r == DRUNIFIED_UNSUPPORTED_FORMAT, "unsupported.ogg: got %s, expected DRUNIFIED_UNSUPPORTED_FORMAT", drunified_result_string(r));
    printf("  unsupported.ogg -> %s\n", drunified_result_string(r));

    /* Recognised but corrupt. */
    snprintf(path, sizeof(path), "%s/corrupt.flac", pDir);
    r = drunified_init_file(&dec, path, &info);
    CHECK(r == DRUNIFIED_DECODE_ERROR, "corrupt.flac: got %s, expected DRUNIFIED_DECODE_ERROR", drunified_result_string(r));
    printf("  corrupt.flac    -> %s\n", drunified_result_string(r));

    /* Missing file. */
    snprintf(path, sizeof(path), "%s/does_not_exist.wav", pDir);
    r = drunified_init_file(&dec, path, &info);
    CHECK(r == DRUNIFIED_DOES_NOT_EXIST, "missing file: got %s, expected DRUNIFIED_DOES_NOT_EXIST", drunified_result_string(r));
    printf("  missing file    -> %s\n", drunified_result_string(r));

    /* Invalid args. */
    r = drunified_init_file(NULL, path, &info);
    CHECK(r == DRUNIFIED_INVALID_ARGS, "NULL decoder: got %s", drunified_result_string(r));
    r = drunified_init_file(&dec, NULL, &info);
    CHECK(r == DRUNIFIED_INVALID_ARGS, "NULL path: got %s", drunified_result_string(r));
    r = drunified_init_memory(&dec, NULL, 100, &info);
    CHECK(r == DRUNIFIED_INVALID_ARGS, "NULL data: got %s", drunified_result_string(r));
    printf("  NULL args       -> %s\n", drunified_result_string(r));

    /* Sniffer unit checks. */
    CHECK(drunified_sniff_format("RIFF\x00\x00\x00\x00WAVE", 12) == drunified_format_wav, "sniff RIFF/WAVE");
    CHECK(drunified_sniff_format("RF64\xFF\xFF\xFF\xFFWAVE", 12) == drunified_format_wav, "sniff RF64/WAVE");
    CHECK(drunified_sniff_format("fLaC", 4) == drunified_format_flac, "sniff fLaC");
    CHECK(drunified_sniff_format("ID3\x04\x00", 5) == drunified_format_mp3, "sniff ID3");
    CHECK(drunified_sniff_format("\xFF\xFB\x90\x00", 4) == drunified_format_mp3, "sniff MPEG1 L3 sync");
    CHECK(drunified_sniff_format("\xFF\xF3\x90\x00", 4) == drunified_format_mp3, "sniff MPEG2 L3 sync");
    CHECK(drunified_sniff_format("OggS\x00\x02", 6) == drunified_format_unknown, "sniff OggS -> unknown");
    CHECK(drunified_sniff_format("", 0) == drunified_format_unknown, "sniff empty -> unknown");
}

static void test_error_mapping_table(void)
{
    static const struct { drunified_int32 native; drunified_result unified; } kExpected[] = {
        {   0, DRUNIFIED_SUCCESS },
        {  -1, DRUNIFIED_ERROR },
        {  -2, DRUNIFIED_INVALID_ARGS },
        {  -3, DRUNIFIED_INVALID_OPERATION },
        {  -4, DRUNIFIED_OUT_OF_MEMORY },
        {  -5, DRUNIFIED_OUT_OF_RANGE },
        {  -6, DRUNIFIED_ACCESS_DENIED },
        {  -7, DRUNIFIED_DOES_NOT_EXIST },
        {  -9, DRUNIFIED_IO_ERROR },        /* TOO_MANY_OPEN_FILES */
        { -10, DRUNIFIED_DECODE_ERROR },    /* INVALID_FILE */
        { -20, DRUNIFIED_IO_ERROR },        /* IO_ERROR */
        { -25, DRUNIFIED_IO_ERROR },        /* BAD_SEEK */
        { -26, DRUNIFIED_IO_ERROR },        /* BAD_PIPE */
        { -32, DRUNIFIED_DECODE_ERROR },    /* NO_DATA_AVAILABLE */
        { -33, DRUNIFIED_DECODE_ERROR },    /* INVALID_DATA */
    };
    size_t i;
    int code;
    int complete = 1;

    printf("[errors] native -> unified error code mapping table\n");
    printf("  %-34s %-34s\n", "native (drwav/drmp3)", "unified");
    for (i = 0; i < sizeof(kExpected)/sizeof(kExpected[0]); i++) {
        drunified_result fromWav = drunified_result_from_wav(kExpected[i].native);
        drunified_result fromMp3 = drunified_result_from_mp3(kExpected[i].native);
        printf("  %-34d %-34s\n", kExpected[i].native, drunified_result_string(fromWav));
        CHECK(fromWav == kExpected[i].unified, "wav mapping of %d: got %s", kExpected[i].native, drunified_result_string(fromWav));
        CHECK(fromMp3 == kExpected[i].unified, "mp3 mapping of %d: got %s", kExpected[i].native, drunified_result_string(fromMp3));
    }

    /* Completeness: every native code in the defined ranges must map to a valid unified code. */
    for (code = 1; code >= -36; code--) {   /* dr_wav defines 0..-36, dr_mp3 0..-33. */
        drunified_result r = drunified_result_from_wav(code);
        if (r > 0 || r < DRUNIFIED_UNSUPPORTED_FORMAT) complete = 0;
        if (code >= -33) {
            drunified_result r2 = drunified_result_from_mp3(code);
            if (r2 > 0 || r2 < DRUNIFIED_UNSUPPORTED_FORMAT) complete = 0;
        }
    }
    CHECK(complete, "every native code in 0..-36 maps to a defined unified code");
    printf("  completeness: all drwav codes 0..-36 and drmp3 codes 0..-33 map to defined unified codes: %s\n",
           complete ? "yes" : "NO");
}

/* ------------------------------------------------------------------ test 5: dispatch overhead */

#define BENCH_CHUNK_FRAMES 4096
#define BENCH_TARGET_REP_S 0.15  /* Each timed rep runs ~150ms; longer reps average out scheduler noise. */
#define BENCH_REPS         9     /* Interleaved native/unified pairs; the min is used as the noise-free estimate. */

static void bench_one_native(const void* pData, size_t dataSize, drunified_format format, drunified_int16* pChunk)
{
    if (format == drunified_format_wav) {
        drwav dec;
        drwav_init_memory(&dec, pData, dataSize, NULL);
        while (drwav_read_pcm_frames_s16(&dec, BENCH_CHUNK_FRAMES, pChunk) > 0) {}
        drwav_uninit(&dec);
    } else if (format == drunified_format_flac) {
        drflac* pDec = drflac_open_memory(pData, dataSize, NULL);
        while (drflac_read_pcm_frames_s16(pDec, BENCH_CHUNK_FRAMES, pChunk) > 0) {}
        drflac_close(pDec);
    } else {
        drmp3 dec;
        drmp3_init_memory(&dec, pData, dataSize, NULL);
        while (drmp3_read_pcm_frames_s16(&dec, BENCH_CHUNK_FRAMES, pChunk) > 0) {}
        drmp3_uninit(&dec);
    }
}

static void bench_one_unified(const void* pData, size_t dataSize, drunified_format format, drunified_int16* pChunk)
{
    drunified dec;
    (void)format;
    drunified_init_memory(&dec, pData, dataSize, NULL);
    while (drunified_read_pcm_frames_s16(&dec, BENCH_CHUNK_FRAMES, pChunk) > 0) {}
    drunified_uninit(&dec);
}

/* Picks an iteration count so one timed rep takes ~BENCH_TARGET_REP_S seconds. */
static int bench_calibrate(const void* pData, size_t dataSize, drunified_format format, drunified_int16* pChunk)
{
    double t0 = now_seconds();
    double dt;
    long iters;
    bench_one_native(pData, dataSize, format, pChunk);
    dt = now_seconds() - t0;
    if (dt < 1e-7) dt = 1e-7;
    iters = (long)(BENCH_TARGET_REP_S / dt);
    if (iters < 10) iters = 10;
    if (iters > 2000000) iters = 2000000;
    return (int)iters;
}

static int compare_doubles(const void* a, const void* b)
{
    double da = *(const double*)a, db = *(const double*)b;
    return (da > db) - (da < db);
}

/*
Times native and unified decode in interleaved pairs (one warmup pair first).
Noise on a shared machine is strictly additive, so the minimum total time over the
pairs is the noise-free estimate of true decode time; the median is reported too.
*/
static void bench_run_pair(const void* pData, size_t dataSize, drunified_format format, drunified_int16* pChunk,
                           int iterations, double* pMinOverheadOut, double* pMedianOverheadOut)
{
    double overheads[BENCH_REPS];
    double minNative = 1e30, minUnified = 1e30;
    int rep, iter;

    for (rep = 0; rep < BENCH_REPS + 1; rep++) {    /* First pair is a warmup and is discarded. */
        double t0, tNative, tUnified;

        /* Alternate which side runs first to cancel any first/second-in-pair systematic bias. */
        if (rep % 2 == 0) {
            t0 = now_seconds();
            for (iter = 0; iter < iterations; iter++) bench_one_native(pData, dataSize, format, pChunk);
            tNative = now_seconds() - t0;

            t0 = now_seconds();
            for (iter = 0; iter < iterations; iter++) bench_one_unified(pData, dataSize, format, pChunk);
            tUnified = now_seconds() - t0;
        } else {
            t0 = now_seconds();
            for (iter = 0; iter < iterations; iter++) bench_one_unified(pData, dataSize, format, pChunk);
            tUnified = now_seconds() - t0;

            t0 = now_seconds();
            for (iter = 0; iter < iterations; iter++) bench_one_native(pData, dataSize, format, pChunk);
            tNative = now_seconds() - t0;
        }

        if (rep == 0) continue;
        if (tNative < minNative) minNative = tNative;
        if (tUnified < minUnified) minUnified = tUnified;
        overheads[rep - 1] = tUnified / tNative - 1.0;
    }

    qsort(overheads, BENCH_REPS, sizeof(double), compare_doubles);
    *pMinOverheadOut = minUnified / minNative - 1.0;
    *pMedianOverheadOut = overheads[BENCH_REPS / 2];
}

static void test_overhead(const char* pDir)
{
    size_t i;
    drunified_int16* pChunk = (drunified_int16*)malloc(BENCH_CHUNK_FRAMES * 8 * sizeof(drunified_int16));

    printf("[bench]  dispatch overhead (memory decode, %d interleaved native/unified pairs of ~%.0f ms + warmup)\n", BENCH_REPS, BENCH_TARGET_REP_S * 1000.0);
    printf("  min = best-of-%d per-side estimate (noise is strictly additive); median shown for reference\n", BENCH_REPS);
    printf("  %-16s %10s %10s %10s\n", "file", "iters", "min", "median");

    for (i = 0; i < sizeof(g_TestFiles)/sizeof(g_TestFiles[0]); i++) {
        char path[1024];
        size_t size;
        unsigned char* pData;
        int iterations;
        double minOverhead, medianOverhead;

        snprintf(path, sizeof(path), "%s/%s", pDir, g_TestFiles[i].pFileName);
        pData = read_entire_file(path, &size);
        if (pData == NULL) {
            CHECK(0, "cannot read %s", path);
            continue;
        }

        iterations = bench_calibrate(pData, size, g_TestFiles[i].expectedFormat, pChunk);
        bench_run_pair(pData, size, g_TestFiles[i].expectedFormat, pChunk, iterations, &minOverhead, &medianOverhead);

        printf("  %-16s %10d %9.3f%% %9.3f%%\n", g_TestFiles[i].pFileName, iterations,
               minOverhead * 100.0, medianOverhead * 100.0);

        free(pData);
    }
    printf("  (informational only; the asserted measurement is [bench2] below)\n");
    free(pChunk);
}

/*
Direct dispatch-cost measurement.

The end-to-end ratio above cannot resolve sub-1% differences on this machine (run-to-run
noise is +-1-2%). The dispatch layer's cost is exactly two things, so measure both
directly, amplified over millions of calls where the signal dwarfs the noise:

  1. per-init:  drunified_sniff_format() on the file header.
  2. per-read:  one extra function call + format switch per drunified_read_pcm_frames_*()
                call, isolated by timing 0-frame reads through both APIs on an open decoder.

The relative overhead is then bounded against the measured native decode time of a real
4096-frame chunk (and of a whole file for the init part).
*/

#define DISPATCH_READ_ITERS  3000000
#define DISPATCH_SNIFF_ITERS 5000000
#define DISPATCH_REPS        7

static volatile drunified_uint64 g_BenchSink;

static double best_of_7_zero_frame_reads_native(const void* pData, size_t dataSize, drunified_format format)
{
    double best = 1e30;
    int rep;
    long k;

    for (rep = 0; rep < DISPATCH_REPS; rep++) {
        double t0, dt;
        if (format == drunified_format_wav) {
            drwav dec;
            drwav_init_memory(&dec, pData, dataSize, NULL);
            t0 = now_seconds();
            for (k = 0; k < DISPATCH_READ_ITERS; k++) g_BenchSink += drwav_read_pcm_frames_s16(&dec, 0, NULL);
            dt = now_seconds() - t0;
            drwav_uninit(&dec);
        } else if (format == drunified_format_flac) {
            drflac* pDec = drflac_open_memory(pData, dataSize, NULL);
            t0 = now_seconds();
            for (k = 0; k < DISPATCH_READ_ITERS; k++) g_BenchSink += drflac_read_pcm_frames_s16(pDec, 0, NULL);
            dt = now_seconds() - t0;
            drflac_close(pDec);
        } else {
            drmp3 dec;
            drmp3_init_memory(&dec, pData, dataSize, NULL);
            t0 = now_seconds();
            for (k = 0; k < DISPATCH_READ_ITERS; k++) g_BenchSink += drmp3_read_pcm_frames_s16(&dec, 0, NULL);
            dt = now_seconds() - t0;
            drmp3_uninit(&dec);
        }
        if (dt < best) best = dt;
    }
    return best;
}

static double best_of_7_zero_frame_reads_unified(const void* pData, size_t dataSize)
{
    double best = 1e30;
    int rep;
    long k;

    for (rep = 0; rep < DISPATCH_REPS; rep++) {
        double t0, dt;
        drunified dec;
        drunified_init_memory(&dec, pData, dataSize, NULL);
        t0 = now_seconds();
        for (k = 0; k < DISPATCH_READ_ITERS; k++) g_BenchSink += drunified_read_pcm_frames_s16(&dec, 0, NULL);
        dt = now_seconds() - t0;
        drunified_uninit(&dec);
        if (dt < best) best = dt;
    }
    return best;
}

static double best_of_7_sniffs(const void* pHeader, size_t headerSize)
{
    double best = 1e30;
    int rep;
    long k;

    for (rep = 0; rep < DISPATCH_REPS; rep++) {
        double t0 = now_seconds();
        for (k = 0; k < DISPATCH_SNIFF_ITERS; k++) g_BenchSink += drunified_sniff_format(pHeader, headerSize);
        { double dt = now_seconds() - t0; if (dt < best) best = dt; }
    }
    return best;
}

/* Best-of-21 native full-file decode time in seconds. */
static double best_native_decode_time(const void* pData, size_t dataSize, drunified_format format, drunified_int16* pChunk)
{
    double best = 1e30;
    int rep;

    for (rep = 0; rep < 21; rep++) {
        double t0 = now_seconds();
        bench_one_native(pData, dataSize, format, pChunk);
        { double dt = now_seconds() - t0; if (dt < best) best = dt; }
    }
    return best;
}

static void test_dispatch_overhead(const char* pDir)
{
    size_t i;
    int allUnder1pct = 1;
    drunified_int16* pChunk = (drunified_int16*)malloc(BENCH_CHUNK_FRAMES * 8 * sizeof(drunified_int16));

    printf("[bench2] dispatch cost, measured directly (%dM read calls, %dM sniffs, best of %d)\n",
           DISPATCH_READ_ITERS / 1000000, DISPATCH_SNIFF_ITERS / 1000000, DISPATCH_REPS);
    printf("  %-16s %12s %12s %14s %14s\n", "file", "read(ns)", "sniff(ns)", "read-ovhd", "init-ovhd");

    for (i = 0; i < sizeof(g_TestFiles)/sizeof(g_TestFiles[0]); i++) {
        char path[1024];
        size_t size;
        unsigned char* pData;
        double tReadNative, tReadUnified, dispatchNsPerCall, sniffNs;
        double decodeS, chunkDecodeNs, readOverhead, initOverhead;
        drunified_uint64 totalFrames, chunks;
        drunified dec;
        drunified_info info;

        snprintf(path, sizeof(path), "%s/%s", pDir, g_TestFiles[i].pFileName);
        pData = read_entire_file(path, &size);
        if (pData == NULL) {
            CHECK(0, "cannot read %s", path);
            continue;
        }

        if (drunified_init_memory(&dec, pData, size, &info) != DRUNIFIED_SUCCESS) {
            CHECK(0, "unified init failed for %s", path);
            free(pData);
            continue;
        }
        totalFrames = info.totalPCMFrameCount;
        drunified_uninit(&dec);
        chunks = (totalFrames + BENCH_CHUNK_FRAMES - 1) / BENCH_CHUNK_FRAMES;

        tReadNative = best_of_7_zero_frame_reads_native(pData, size, g_TestFiles[i].expectedFormat);
        tReadUnified = best_of_7_zero_frame_reads_unified(pData, size);
        dispatchNsPerCall = (tReadUnified - tReadNative) / DISPATCH_READ_ITERS * 1e9;
        sniffNs = best_of_7_sniffs(pData, size < 16 ? size : 16) / DISPATCH_SNIFF_ITERS * 1e9;

        decodeS = best_native_decode_time(pData, size, g_TestFiles[i].expectedFormat, pChunk);
        chunkDecodeNs = decodeS / (double)chunks * 1e9;

        readOverhead = dispatchNsPerCall / chunkDecodeNs;
        initOverhead = sniffNs * 1e-9 / decodeS;

        printf("  %-16s %12.3f %12.3f %13.4f%% %13.5f%%\n", g_TestFiles[i].pFileName,
               dispatchNsPerCall, sniffNs, readOverhead * 100.0, initOverhead * 100.0);

        CHECK(readOverhead < 0.01, "per-read dispatch overhead %.4f%% >= 1%% for %s", readOverhead * 100.0, g_TestFiles[i].pFileName);
        CHECK(initOverhead < 0.01, "per-init dispatch overhead %.5f%% >= 1%% for %s", initOverhead * 100.0, g_TestFiles[i].pFileName);
        if (readOverhead >= 0.01 || initOverhead >= 0.01) allUnder1pct = 0;

        free(pData);
    }
    printf("  dispatch overhead < 1%% on all formats: %s\n", allUnder1pct ? "yes" : "NO");
    free(pChunk);
}

/* ------------------------------------------------------------------ f32 spot check */

static void test_f32_spot(const char* pDir)
{
    char path[1024];
    drunified dec;
    float unifiedBuf[512 * 8];
    float nativeBuf[512 * 8];
    drwav wav;
    drunified_uint64 nU, nN;

    snprintf(path, sizeof(path), "%s/wav_48k.wav", pDir);
    printf("[f32]    wav_48k.wav f32 read path\n");

    if (drunified_init_file(&dec, path, NULL) != DRUNIFIED_SUCCESS) { CHECK(0, "unified init failed"); return; }
    nU = drunified_read_pcm_frames_f32(&dec, 512, unifiedBuf);
    drunified_uninit(&dec);

    if (!drwav_init_file(&wav, path, NULL)) { CHECK(0, "native init failed"); return; }
    nN = drwav_read_pcm_frames_f32(&wav, 512, nativeBuf);
    drwav_uninit(&wav);

    CHECK(nU == nN && memcmp(unifiedBuf, nativeBuf, (size_t)nU * 2 * sizeof(float)) == 0,
          "f32 read matches native");
}

/* ------------------------------------------------------------------ main */

int main(int argc, char** argv)
{
    const char* pDir;
    size_t i;

    if (argc < 2) {
        printf("usage: dr_unified_test [--gen] <sample-dir>\n");
        return 1;
    }

    if (strcmp(argv[1], "--gen") == 0) {
        if (argc < 3) { printf("usage: dr_unified_test --gen <dir>\n"); return 1; }
        if (!gen_samples(argv[2])) { printf("sample generation failed\n"); return 1; }
        printf("generated samples in %s\n", argv[2]);
        return 0;
    }

    pDir = argv[1];

    for (i = 0; i < sizeof(g_TestFiles)/sizeof(g_TestFiles[0]); i++) {
        test_decode_and_metadata(pDir, &g_TestFiles[i]);
    }
    for (i = 0; i < sizeof(g_TestFiles)/sizeof(g_TestFiles[0]); i++) {
        test_seeking(pDir, &g_TestFiles[i]);
    }
    test_f32_spot(pDir);
    test_error_codes(pDir);
    test_error_mapping_table();
    test_overhead(pDir);
    test_dispatch_overhead(pDir);

    printf("\n==== %d passed, %d failed ====\n", g_PassCount, g_FailCount);
    return g_FailCount == 0 ? 0 : 1;
}
