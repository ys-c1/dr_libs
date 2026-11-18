/*
Bad-frame recovery probe for dr_mp3.

Usage:
    dr_mp3_recovery_probe [--memory] [--max-skip N] [--out FILE] <file.mp3>

Decodes the whole stream with dr_mp3, prints a machine-readable report and, with --out,
writes the interleaved s16 samples that were produced. Exit code is 0 when decoding
reached a clean end of stream and 1 when it stopped on a reported decode error.

This is a standalone harness for the recovery tests; the main dr_libs test database is
not affected.
*/
#define DR_MP3_IMPLEMENTATION
#include "../../dr_mp3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
    const char* pFilePath = NULL;
    const char* pOutPath = NULL;
    drmp3_uint32 maxSkip = 0;
    drmp3_bool32 useMemory = DRMP3_FALSE;
    drmp3 mp3;
    drmp3_bool32 opened = DRMP3_FALSE;
    drmp3_int16* pSamples = NULL;
    size_t capacity = 0;
    size_t totalFrames = 0;
    FILE* pOut = NULL;
    FILE* pFileData = NULL;
    unsigned char* pFileBytes = NULL;
    long fileSize;
    int iarg;
    int exitCode = 1;

    for (iarg = 1; iarg < argc; iarg += 1) {
        if (strcmp(argv[iarg], "--memory") == 0) {
            useMemory = DRMP3_TRUE;
        } else if (strcmp(argv[iarg], "--max-skip") == 0) {
            if (iarg + 1 >= argc) {
                fprintf(stderr, "--max-skip requires a value.\n");
                return 2;
            }

            maxSkip = (drmp3_uint32)strtoul(argv[++iarg], NULL, 10);
        } else if (strcmp(argv[iarg], "--out") == 0) {
            if (iarg + 1 >= argc) {
                fprintf(stderr, "--out requires a path.\n");
                return 2;
            }

            pOutPath = argv[++iarg];
        } else if (argv[iarg][0] == '-') {
            fprintf(stderr, "Unknown option: %s.\n", argv[iarg]);
            return 2;
        } else {
            pFilePath = argv[iarg];
        }
    }

    if (pFilePath == NULL) {
        fprintf(stderr, "No input file.\n");
        return 2;
    }

    if (useMemory) {
        pFileData = fopen(pFilePath, "rb");
        if (pFileData == NULL) {
            fprintf(stderr, "Cannot open %s.\n", pFilePath);
            return 2;
        }

        if (fseek(pFileData, 0, SEEK_END) != 0 || (fileSize = ftell(pFileData)) < 0 || fseek(pFileData, 0, SEEK_SET) != 0) {
            fprintf(stderr, "Cannot measure %s.\n", pFilePath);
            goto cleanup;
        }

        pFileBytes = (unsigned char*)malloc((size_t)fileSize);
        if (pFileBytes == NULL) {
            fprintf(stderr, "Out of memory.\n");
            goto cleanup;
        }

        if (fread(pFileBytes, 1, (size_t)fileSize, pFileData) != (size_t)fileSize) {
            fprintf(stderr, "Cannot read %s.\n", pFilePath);
            goto cleanup;
        }

        if (!drmp3_init_memory(&mp3, pFileBytes, (size_t)fileSize, NULL)) {
            printf("open=fail\n");
            goto cleanup;
        }
    } else {
        if (!drmp3_init_file(&mp3, pFilePath, NULL)) {
            printf("open=fail\n");
            goto cleanup;
        }
    }

    opened = DRMP3_TRUE;
    drmp3_set_max_bad_frames_to_skip(&mp3, maxSkip);

    for (;;) {
        drmp3_int16 chunk[1152 * 2];
        drmp3_uint64 framesRead;

        framesRead = drmp3_read_pcm_frames_s16(&mp3, 1152, chunk);
        if (framesRead == 0) {
            break;
        }

        if (totalFrames + (size_t)framesRead > capacity) {
            size_t newCapacity = capacity == 0 ? 4096 : capacity * 2;
            drmp3_int16* pNew;

            while (newCapacity < totalFrames + (size_t)framesRead) {
                newCapacity *= 2;
            }

            pNew = (drmp3_int16*)realloc(pSamples, newCapacity * (size_t)mp3.channels * sizeof(drmp3_int16));
            if (pNew == NULL) {
                fprintf(stderr, "Out of memory.\n");
                goto cleanup;
            }

            pSamples = pNew;
            capacity = newCapacity;
        }

        memcpy(pSamples + totalFrames * (size_t)mp3.channels, chunk,
               (size_t)framesRead * (size_t)mp3.channels * sizeof(drmp3_int16));
        totalFrames += (size_t)framesRead;
    }

    printf("open=ok\n");
    printf("backend=%s\n", useMemory ? "memory" : "file");
    printf("channels=%u\n", mp3.channels);
    printf("sample_rate=%u\n", mp3.sampleRate);
    printf("pcm_frames=%zu\n", totalFrames);
    printf("bad_frames=%llu\n", (unsigned long long)drmp3_get_bad_frame_count(&mp3));
    printf("decode_error=%d\n", (int)drmp3_get_decode_error(&mp3));
    printf("max_skip=%u\n", maxSkip);

    {
        drmp3_bad_frame_info events[DRMP3_MAX_BAD_FRAME_EVENTS];
        drmp3_uint32 count = drmp3_get_bad_frames(&mp3, events, DRMP3_MAX_BAD_FRAME_EVENTS);
        drmp3_uint32 i;

        printf("bad_positions=");
        for (i = 0; i < count; i += 1) {
            printf("%s%llu@%llu+%llu", i == 0 ? "" : ",",
                   (unsigned long long)events[i].mp3FrameIndex,
                   (unsigned long long)events[i].streamCursor,
                   (unsigned long long)events[i].bytesSkipped);
        }
        printf("\n");
    }

    if (pOutPath != NULL) {
        pOut = fopen(pOutPath, "wb");
        if (pOut == NULL) {
            fprintf(stderr, "Cannot write %s.\n", pOutPath);
            goto cleanup;
        }

        if (totalFrames > 0) {
            fwrite(pSamples, sizeof(drmp3_int16), totalFrames * (size_t)mp3.channels, pOut);
        }
    }

    exitCode = drmp3_get_decode_error(&mp3) ? 1 : 0;

cleanup:
    if (pOut != NULL) {
        fclose(pOut);
    }

    if (opened) {
        drmp3_uninit(&mp3);
    }

    free(pSamples);
    free(pFileBytes);

    if (pFileData != NULL) {
        fclose(pFileData);
    }

    return exitCode;
}
