/*
dr_unified - v0.1 - Unified audio decoding interface on top of dr_wav, dr_flac and dr_mp3.

A single decoder-facing API that sniffs the file header, dispatches to the appropriate
backend (dr_wav / dr_flac / dr_mp3) and exposes one consistent set of functions for
reading PCM frames, seeking and querying metadata, with one consistent set of result
codes.

USAGE

    In exactly one source file:

        #define DR_WAV_IMPLEMENTATION
        #define DR_FLAC_IMPLEMENTATION
        #define DR_MP3_IMPLEMENTATION
        #define DR_UNIFIED_IMPLEMENTATION
        #include "dr_unified.h"

    dr_unified.h includes dr_wav.h, dr_flac.h and dr_mp3.h itself; those headers must be
    includable from the directory containing dr_unified.h (or via the include path).

    Then:

        drunified dec;
        drunified_info info;
        drunified_result result = drunified_init_file(&dec, "song.flac", &info);
        if (result != DRUNIFIED_SUCCESS) { ... drunified_result_string(result) ... }

        drunified_int16 pcm[4096 * 2];
        drunified_uint64 framesRead;
        while ((framesRead = drunified_read_pcm_frames_s16(&dec, 4096, pcm)) > 0) { ... }

        drunified_seek_to_pcm_frame(&dec, 0);
        drunified_uninit(&dec);

FORMAT DETECTION

    Detection looks at the leading bytes only:
      - "RIFF"/"RF64" + "WAVE" at offset 8, or a Wave64 "riff" GUID  -> WAV
      - "fLaC"                                                        -> FLAC
      - "ID3" or an MPEG audio frame sync (0xFF 0xEx)                 -> MP3
    Anything else fails init with DRUNIFIED_UNSUPPORTED_FORMAT.

RESULT CODE MAPPING

    dr_wav and dr_mp3 share the same result code numbering, so one table covers both.
    dr_flac reports init failure as a NULL handle and seek failure as a boolean; those
    are mapped as shown below.

      native (drwav_result / drmp3_result)   unified (drunified_result)
      ------------------------------------   --------------------------
      SUCCESS                (  0)           DRUNIFIED_SUCCESS             (  0)
      ERROR                  ( -1)           DRUNIFIED_ERROR               ( -1)
      INVALID_ARGS           ( -2)           DRUNIFIED_INVALID_ARGS        ( -2)
      INVALID_OPERATION      ( -3)           DRUNIFIED_INVALID_OPERATION   ( -3)
      OUT_OF_MEMORY          ( -4)           DRUNIFIED_OUT_OF_MEMORY       ( -4)
      OUT_OF_RANGE           ( -5)           DRUNIFIED_OUT_OF_RANGE        ( -5)
      ACCESS_DENIED          ( -6)           DRUNIFIED_ACCESS_DENIED       ( -6)
      DOES_NOT_EXIST         ( -7)           DRUNIFIED_DOES_NOT_EXIST      ( -7)
      TOO_MANY_OPEN_FILES    ( -9)           DRUNIFIED_IO_ERROR            ( -8)
      INVALID_FILE           (-10)           DRUNIFIED_DECODE_ERROR        ( -9)
      IO_ERROR               (-20)           DRUNIFIED_IO_ERROR            ( -8)
      BAD_SEEK               (-25)           DRUNIFIED_IO_ERROR            ( -8)
      BAD_PIPE               (-26)           DRUNIFIED_IO_ERROR            ( -8)
      NO_DATA_AVAILABLE      (-32)           DRUNIFIED_DECODE_ERROR        ( -9)
      INVALID_DATA           (-33)           DRUNIFIED_DECODE_ERROR        ( -9)
      any other native code                  DRUNIFIED_ERROR               ( -1)

      drflac_open_*() returns NULL           DRUNIFIED_DECODE_ERROR        ( -9)
                                             (DRUNIFIED_DOES_NOT_EXIST / DRUNIFIED_ACCESS_DENIED
                                              if the file could not be opened at all)
      drflac_seek_to_pcm_frame() == false    DRUNIFIED_ERROR               ( -1)

      dispatch layer itself:
      unrecognised header                    DRUNIFIED_UNSUPPORTED_FORMAT  (-10)
      seek past end of stream                DRUNIFIED_OUT_OF_RANGE        ( -5)

    This library is released into the public domain. See the end of dr_wav.h in this
    repository for the license text that applies to the dr_libs family.
*/

#ifndef dr_unified_h
#define dr_unified_h

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "dr_wav.h"
#include "dr_flac.h"
#include "dr_mp3.h"

#ifndef DR_UNIFIED_API
#define DR_UNIFIED_API
#endif

#define DR_UNIFIED_VERSION_MAJOR    0
#define DR_UNIFIED_VERSION_MINOR    1
#define DR_UNIFIED_VERSION_REVISION 0

typedef uint64_t drunified_uint64;
typedef uint32_t drunified_uint32;
typedef int32_t  drunified_int32;
typedef int16_t  drunified_int16;
typedef uint8_t  drunified_uint8;
typedef int32_t  drunified_bool32;

#define DRUNIFIED_TRUE  1
#define DRUNIFIED_FALSE 0

/* Result codes. */
typedef drunified_int32 drunified_result;
#define DRUNIFIED_SUCCESS               0
#define DRUNIFIED_ERROR                -1   /* A generic error. */
#define DRUNIFIED_INVALID_ARGS         -2
#define DRUNIFIED_INVALID_OPERATION    -3
#define DRUNIFIED_OUT_OF_MEMORY        -4
#define DRUNIFIED_OUT_OF_RANGE         -5
#define DRUNIFIED_ACCESS_DENIED        -6
#define DRUNIFIED_DOES_NOT_EXIST       -7
#define DRUNIFIED_IO_ERROR             -8   /* OS-level open/read/seek failure. */
#define DRUNIFIED_DECODE_ERROR         -9   /* Format recognised, but the backend failed to initialise/decode it. */
#define DRUNIFIED_UNSUPPORTED_FORMAT  -10   /* Header not recognised as WAV, FLAC or MP3. */

typedef enum
{
    drunified_format_unknown = 0,
    drunified_format_wav,
    drunified_format_flac,
    drunified_format_mp3
} drunified_format;

typedef struct
{
    drunified_format format;
    drunified_uint32 channels;
    drunified_uint32 sampleRate;
    drunified_uint64 totalPCMFrameCount;
} drunified_info;

typedef struct
{
    drunified_format format;
    drwav wav;          /* Valid when format == drunified_format_wav. */
    drflac* pFlac;      /* Valid when format == drunified_format_flac. */
    drmp3 mp3;          /* Valid when format == drunified_format_mp3. */
    drunified_uint32 channels;
    drunified_uint32 sampleRate;
    drunified_uint64 totalPCMFrameCount;
    drunified_bool32 hasTotalPCMFrameCount; /* MP3 frame counts are computed lazily. */
} drunified;

/* Sniffs a buffer's leading bytes and reports the detected format. */
DR_UNIFIED_API drunified_format drunified_sniff_format(const void* pData, size_t dataSize);

/* Opens a file, sniffing the format from its header. */
DR_UNIFIED_API drunified_result drunified_init_file(drunified* pDec, const char* pFilePath, drunified_info* pInfoOut);

/* Opens a decoder on a block of memory. The memory must stay alive until drunified_uninit(). */
DR_UNIFIED_API drunified_result drunified_init_memory(drunified* pDec, const void* pData, size_t dataSize, drunified_info* pInfoOut);

/* Retrieves format/channels/sample-rate/total-frame metadata. */
DR_UNIFIED_API drunified_result drunified_get_info(drunified* pDec, drunified_info* pInfoOut);

/* Reads PCM frames as interleaved s16 or f32. Returns the number of frames actually read. */
DR_UNIFIED_API drunified_uint64 drunified_read_pcm_frames_s16(drunified* pDec, drunified_uint64 framesToRead, drunified_int16* pBufferOut);
DR_UNIFIED_API drunified_uint64 drunified_read_pcm_frames_f32(drunified* pDec, drunified_uint64 framesToRead, float* pBufferOut);

/* Seeks to a PCM frame index. */
DR_UNIFIED_API drunified_result drunified_seek_to_pcm_frame(drunified* pDec, drunified_uint64 frameIndex);

/* Closes the decoder and releases backend resources. */
DR_UNIFIED_API drunified_result drunified_uninit(drunified* pDec);

/* Native -> unified result code mapping (see the table at the top of this file). */
DR_UNIFIED_API drunified_result drunified_result_from_wav(drwav_result result);
DR_UNIFIED_API drunified_result drunified_result_from_mp3(drmp3_result result);

/* String helpers for logging/diagnostics. */
DR_UNIFIED_API const char* drunified_result_string(drunified_result result);
DR_UNIFIED_API const char* drunified_format_string(drunified_format format);

#ifdef __cplusplus
}
#endif
#endif  /* dr_unified_h */


/************************************************************************************************************************************************************
 IMPLEMENTATION
 ************************************************************************************************************************************************************/
#ifdef DR_UNIFIED_IMPLEMENTATION
#ifndef dr_unified_c
#define dr_unified_c

#include <stdio.h>  /* fopen/fread for header sniffing in drunified_init_file(). */
#include <errno.h>
#include <string.h>

DR_UNIFIED_API drunified_format drunified_sniff_format(const void* pData, size_t dataSize)
{
    const drunified_uint8* b = (const drunified_uint8*)pData;

    if (b == NULL) {
        return drunified_format_unknown;
    }

    /* WAV: "RIFF"...."WAVE" or "RF64"...."WAVE". */
    if (dataSize >= 12 &&
        ((b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F') ||
         (b[0] == 'R' && b[1] == 'F' && b[2] == '6' && b[3] == '4')) &&
        b[8] == 'W' && b[9] == 'A' && b[10] == 'V' && b[11] == 'E') {
        return drunified_format_wav;
    }

    /* WAV (Wave64): starts with the little-endian "riff" GUID. */
    if (dataSize >= 16 && b[0] == 'r' && b[1] == 'i' && b[2] == 'f' && b[3] == 'f') {
        return drunified_format_wav;
    }

    /* FLAC: "fLaC". */
    if (dataSize >= 4 && b[0] == 'f' && b[1] == 'L' && b[2] == 'a' && b[3] == 'C') {
        return drunified_format_flac;
    }

    /* MP3: ID3v2 tag... */
    if (dataSize >= 3 && b[0] == 'I' && b[1] == 'D' && b[2] == '3') {
        return drunified_format_mp3;
    }

    /* ...or a raw MPEG audio frame sync: 11 sync bits, valid version and layer fields. */
    if (dataSize >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0 &&
        (b[1] & 0x18) != 0x08 &&    /* exclude reserved MPEG version */
        (b[1] & 0x06) != 0x00) {    /* exclude reserved layer */
        return drunified_format_mp3;
    }

    return drunified_format_unknown;
}

/* dr_wav and dr_mp3 share result code numbering; one mapper covers both. */
static drunified_result drunified__result_from_native(drunified_int32 result)
{
    switch (result) {
        case 0:   return DRUNIFIED_SUCCESS;
        case -1:  return DRUNIFIED_ERROR;
        case -2:  return DRUNIFIED_INVALID_ARGS;
        case -3:  return DRUNIFIED_INVALID_OPERATION;
        case -4:  return DRUNIFIED_OUT_OF_MEMORY;
        case -5:  return DRUNIFIED_OUT_OF_RANGE;
        case -6:  return DRUNIFIED_ACCESS_DENIED;
        case -7:  return DRUNIFIED_DOES_NOT_EXIST;
        case -9:  return DRUNIFIED_IO_ERROR;        /* TOO_MANY_OPEN_FILES */
        case -10: return DRUNIFIED_DECODE_ERROR;    /* INVALID_FILE */
        case -20: return DRUNIFIED_IO_ERROR;        /* IO_ERROR */
        case -25: return DRUNIFIED_IO_ERROR;        /* BAD_SEEK */
        case -26: return DRUNIFIED_IO_ERROR;        /* BAD_PIPE */
        case -32: return DRUNIFIED_DECODE_ERROR;    /* NO_DATA_AVAILABLE */
        case -33: return DRUNIFIED_DECODE_ERROR;    /* INVALID_DATA */
        default:  return DRUNIFIED_ERROR;
    }
}

DR_UNIFIED_API drunified_result drunified_result_from_wav(drwav_result result)
{
    return drunified__result_from_native(result);
}

DR_UNIFIED_API drunified_result drunified_result_from_mp3(drmp3_result result)
{
    return drunified__result_from_native(result);
}

static drunified_result drunified__init_backend(drunified* pDec, drunified_format format, const char* pFilePath, const void* pData, size_t dataSize)
{
    pDec->format = format;

    switch (format) {
        case drunified_format_wav:
        {
            drwav_bool32 ok;
            if (pFilePath != NULL) {
                ok = drwav_init_file(&pDec->wav, pFilePath, NULL);
            } else {
                ok = drwav_init_memory(&pDec->wav, pData, dataSize, NULL);
            }
            if (!ok) {
                return DRUNIFIED_DECODE_ERROR;
            }
            pDec->channels = pDec->wav.channels;
            pDec->sampleRate = pDec->wav.sampleRate;
            pDec->totalPCMFrameCount = pDec->wav.totalPCMFrameCount;
            pDec->hasTotalPCMFrameCount = DRUNIFIED_TRUE;
            return DRUNIFIED_SUCCESS;
        }

        case drunified_format_flac:
        {
            if (pFilePath != NULL) {
                pDec->pFlac = drflac_open_file(pFilePath, NULL);
            } else {
                pDec->pFlac = drflac_open_memory(pData, dataSize, NULL);
            }
            if (pDec->pFlac == NULL) {
                return DRUNIFIED_DECODE_ERROR;
            }
            pDec->channels = pDec->pFlac->channels;
            pDec->sampleRate = pDec->pFlac->sampleRate;
            pDec->totalPCMFrameCount = pDec->pFlac->totalPCMFrameCount;
            pDec->hasTotalPCMFrameCount = DRUNIFIED_TRUE;
            return DRUNIFIED_SUCCESS;
        }

        case drunified_format_mp3:
        {
            drmp3_bool32 ok;
            if (pFilePath != NULL) {
                ok = drmp3_init_file(&pDec->mp3, pFilePath, NULL);
            } else {
                ok = drmp3_init_memory(&pDec->mp3, pData, dataSize, NULL);
            }
            if (!ok) {
                return DRUNIFIED_DECODE_ERROR;
            }
            pDec->channels = pDec->mp3.channels;
            pDec->sampleRate = pDec->mp3.sampleRate;
            pDec->totalPCMFrameCount = 0;
            pDec->hasTotalPCMFrameCount = DRUNIFIED_FALSE;  /* Computed lazily; a full stream scan is too expensive to do unconditionally at init. */
            return DRUNIFIED_SUCCESS;
        }

        default:
        {
            return DRUNIFIED_UNSUPPORTED_FORMAT;
        }
    }
}

DR_UNIFIED_API drunified_result drunified_init_file(drunified* pDec, const char* pFilePath, drunified_info* pInfoOut)
{
    drunified_uint8 header[16];
    size_t bytesRead;
    FILE* pFile;
    drunified_format format;
    drunified_result result;

    if (pDec == NULL || pFilePath == NULL) {
        return DRUNIFIED_INVALID_ARGS;
    }

    /* Note: deliberately not zeroing the whole struct; the backend structs are large
       (drmp3 in particular) and each backend initialises its own state. */
    pDec->format = drunified_format_unknown;
    pDec->pFlac = NULL;

    pFile = fopen(pFilePath, "rb");
    if (pFile == NULL) {
        if (errno == ENOENT) return DRUNIFIED_DOES_NOT_EXIST;
        if (errno == EACCES) return DRUNIFIED_ACCESS_DENIED;
        return DRUNIFIED_IO_ERROR;
    }
    bytesRead = fread(header, 1, sizeof(header), pFile);
    fclose(pFile);

    format = drunified_sniff_format(header, bytesRead);
    if (format == drunified_format_unknown) {
        return DRUNIFIED_UNSUPPORTED_FORMAT;
    }

    result = drunified__init_backend(pDec, format, pFilePath, NULL, 0);
    if (result != DRUNIFIED_SUCCESS) {
        return result;
    }

    if (pInfoOut != NULL) {
        return drunified_get_info(pDec, pInfoOut);
    }
    return DRUNIFIED_SUCCESS;
}

DR_UNIFIED_API drunified_result drunified_init_memory(drunified* pDec, const void* pData, size_t dataSize, drunified_info* pInfoOut)
{
    drunified_format format;
    drunified_result result;

    if (pDec == NULL || pData == NULL) {
        return DRUNIFIED_INVALID_ARGS;
    }

    pDec->format = drunified_format_unknown;
    pDec->pFlac = NULL;

    format = drunified_sniff_format(pData, dataSize);
    if (format == drunified_format_unknown) {
        return DRUNIFIED_UNSUPPORTED_FORMAT;
    }

    result = drunified__init_backend(pDec, format, NULL, pData, dataSize);
    if (result != DRUNIFIED_SUCCESS) {
        return result;
    }

    if (pInfoOut != NULL) {
        return drunified_get_info(pDec, pInfoOut);
    }
    return DRUNIFIED_SUCCESS;
}

DR_UNIFIED_API drunified_result drunified_get_info(drunified* pDec, drunified_info* pInfoOut)
{
    if (pDec == NULL || pInfoOut == NULL || pDec->format == drunified_format_unknown) {
        return DRUNIFIED_INVALID_ARGS;
    }

    if (!pDec->hasTotalPCMFrameCount && pDec->format == drunified_format_mp3) {
        pDec->totalPCMFrameCount = drmp3_get_pcm_frame_count(&pDec->mp3);
        pDec->hasTotalPCMFrameCount = DRUNIFIED_TRUE;
    }

    pInfoOut->format = pDec->format;
    pInfoOut->channels = pDec->channels;
    pInfoOut->sampleRate = pDec->sampleRate;
    pInfoOut->totalPCMFrameCount = pDec->totalPCMFrameCount;
    return DRUNIFIED_SUCCESS;
}

DR_UNIFIED_API drunified_uint64 drunified_read_pcm_frames_s16(drunified* pDec, drunified_uint64 framesToRead, drunified_int16* pBufferOut)
{
    if (pDec == NULL || pBufferOut == NULL) {
        return 0;
    }

    switch (pDec->format) {
        case drunified_format_wav:  return drwav_read_pcm_frames_s16(&pDec->wav, framesToRead, pBufferOut);
        case drunified_format_flac: return drflac_read_pcm_frames_s16(pDec->pFlac, framesToRead, pBufferOut);
        case drunified_format_mp3:  return drmp3_read_pcm_frames_s16(&pDec->mp3, framesToRead, pBufferOut);
        default:                    return 0;
    }
}

DR_UNIFIED_API drunified_uint64 drunified_read_pcm_frames_f32(drunified* pDec, drunified_uint64 framesToRead, float* pBufferOut)
{
    if (pDec == NULL || pBufferOut == NULL) {
        return 0;
    }

    switch (pDec->format) {
        case drunified_format_wav:  return drwav_read_pcm_frames_f32(&pDec->wav, framesToRead, pBufferOut);
        case drunified_format_flac: return drflac_read_pcm_frames_f32(pDec->pFlac, framesToRead, pBufferOut);
        case drunified_format_mp3:  return drmp3_read_pcm_frames_f32(&pDec->mp3, framesToRead, pBufferOut);
        default:                    return 0;
    }
}

DR_UNIFIED_API drunified_result drunified_seek_to_pcm_frame(drunified* pDec, drunified_uint64 frameIndex)
{
    drunified_bool32 ok;

    if (pDec == NULL || pDec->format == drunified_format_unknown) {
        return DRUNIFIED_INVALID_ARGS;
    }

    if (pDec->hasTotalPCMFrameCount && frameIndex > pDec->totalPCMFrameCount) {
        return DRUNIFIED_OUT_OF_RANGE;
    }

    switch (pDec->format) {
        case drunified_format_wav:  ok = drwav_seek_to_pcm_frame(&pDec->wav, frameIndex);   break;
        case drunified_format_flac: ok = drflac_seek_to_pcm_frame(pDec->pFlac, frameIndex); break;
        case drunified_format_mp3:  ok = drmp3_seek_to_pcm_frame(&pDec->mp3, frameIndex);   break;
        default:                    return DRUNIFIED_INVALID_OPERATION;
    }

    return ok ? DRUNIFIED_SUCCESS : DRUNIFIED_ERROR;
}

DR_UNIFIED_API drunified_result drunified_uninit(drunified* pDec)
{
    if (pDec == NULL) {
        return DRUNIFIED_INVALID_ARGS;
    }

    switch (pDec->format) {
        case drunified_format_wav:  drwav_uninit(&pDec->wav);    break;
        case drunified_format_flac: drflac_close(pDec->pFlac);   break;
        case drunified_format_mp3:  drmp3_uninit(&pDec->mp3);    break;
        default: break;
    }

    pDec->format = drunified_format_unknown;
    pDec->pFlac = NULL;
    return DRUNIFIED_SUCCESS;
}

DR_UNIFIED_API const char* drunified_result_string(drunified_result result)
{
    switch (result) {
        case DRUNIFIED_SUCCESS:            return "DRUNIFIED_SUCCESS";
        case DRUNIFIED_ERROR:              return "DRUNIFIED_ERROR";
        case DRUNIFIED_INVALID_ARGS:       return "DRUNIFIED_INVALID_ARGS";
        case DRUNIFIED_INVALID_OPERATION:  return "DRUNIFIED_INVALID_OPERATION";
        case DRUNIFIED_OUT_OF_MEMORY:      return "DRUNIFIED_OUT_OF_MEMORY";
        case DRUNIFIED_OUT_OF_RANGE:       return "DRUNIFIED_OUT_OF_RANGE";
        case DRUNIFIED_ACCESS_DENIED:      return "DRUNIFIED_ACCESS_DENIED";
        case DRUNIFIED_DOES_NOT_EXIST:     return "DRUNIFIED_DOES_NOT_EXIST";
        case DRUNIFIED_IO_ERROR:           return "DRUNIFIED_IO_ERROR";
        case DRUNIFIED_DECODE_ERROR:       return "DRUNIFIED_DECODE_ERROR";
        case DRUNIFIED_UNSUPPORTED_FORMAT: return "DRUNIFIED_UNSUPPORTED_FORMAT";
        default:                           return "DRUNIFIED_UNKNOWN_RESULT";
    }
}

DR_UNIFIED_API const char* drunified_format_string(drunified_format format)
{
    switch (format) {
        case drunified_format_wav:  return "wav";
        case drunified_format_flac: return "flac";
        case drunified_format_mp3:  return "mp3";
        default:                    return "unknown";
    }
}

#endif  /* dr_unified_c */
#endif  /* DR_UNIFIED_IMPLEMENTATION */
