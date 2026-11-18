/*
Unified FLAC metadata query interface built on top of dr_flac's metadata
callback (drflac_open_memory_with_metadata), plus a self-contained test and
benchmark driver.

What this adds on top of dr_flac:
  - Vorbis comment parsing into a key/value list (multi-value keys supported,
    case-insensitive lookup as per the Vorbis comment spec).
  - Picture (cover art) extraction: mime type, picture type, dimensions and
    the raw image bytes.
  - A unified query interface: drmeta_get() for key/value pairs and
    drmeta_get_picture() for binary blocks.
  - Robustness accounting: metadata blocks dr_flac cannot parse (e.g. a
    corrupted VORBIS_COMMENT block) are skipped and counted, and unknown /
    application-specific blocks are skipped and counted.

Build the test driver:
    gcc -O2 -Wall -Wextra -o dr_libs_metadata tests/dr_libs_metadata.c -lm

No third-party dependencies; only dr_flac.h from this repository.
*/

#define DR_FLAC_IMPLEMENTATION
#include "../dr_flac.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================ unified metadata interface ============================ */

typedef struct
{
    char* key;      /* ASCII, as stored in the file (case preserved). */
    char* value;    /* UTF-8. */
} drmeta_kv;

typedef struct
{
    drflac_uint32 type;         /* DRFLAC_PICTURE_TYPE_* */
    char          mime[64];     /* e.g. "image/png" */
    drflac_uint32 width;
    drflac_uint32 height;
    drflac_uint8* data;         /* Owned. */
    drflac_uint32 size;
} drmeta_picture;

#define DRMETA_MAX_BLOCKS 64

typedef struct
{
    /* Key/value pairs from the VORBIS_COMMENT block. */
    drmeta_kv* kvs;
    size_t     kvCount;
    size_t     kvCap;

    /* Binary blocks (cover art) from PICTURE blocks. */
    drmeta_picture* pics;
    size_t          picCount;
    size_t          picCap;

    char* vendor;   /* Vorbis vendor string (owned). */

    /* Robustness accounting. */
    unsigned int unknownBlocksSkipped;      /* APPLICATION / CUESHEET / reserved block types. */
    unsigned int corruptCommentBlocks;      /* VORBIS_COMMENT blocks that failed structural validation and were skipped. */
    unsigned int vorbisCommentBlocksSeen;   /* VORBIS_COMMENT blocks found by the header scan. */
    unsigned int vorbisCommentBlocksParsed; /* VORBIS_COMMENT blocks successfully delivered by dr_flac. */

    /* Block inventory (for reporting). */
    drflac_uint32 blockTypes[DRMETA_MAX_BLOCKS];
    drflac_uint32 blockSizes[DRMETA_MAX_BLOCKS];
    size_t        blockCount;
} drmeta;

static char* drmeta__strndup(const char* src, size_t len)
{
    char* dst = (char*)malloc(len + 1);
    if (dst == NULL) return NULL;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

static drflac_bool32 drmeta__ascii_equal_nocase(const char* a, const char* b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return DRFLAC_FALSE;
        a += 1; b += 1;
    }
    return (*a == '\0' && *b == '\0') ? DRFLAC_TRUE : DRFLAC_FALSE;
}

static void drmeta__push_kv(drmeta* m, const char* key, size_t keyLen, const char* value, size_t valueLen)
{
    drmeta_kv* kv;
    if (m->kvCount == m->kvCap) {
        size_t newCap = (m->kvCap == 0) ? 16 : m->kvCap * 2;
        drmeta_kv* newKVs = (drmeta_kv*)realloc(m->kvs, newCap * sizeof(*newKVs));
        if (newKVs == NULL) return;
        m->kvs = newKVs;
        m->kvCap = newCap;
    }
    kv = &m->kvs[m->kvCount];
    kv->key   = drmeta__strndup(key, keyLen);
    kv->value = drmeta__strndup(value, valueLen);
    if (kv->key == NULL || kv->value == NULL) { free(kv->key); free(kv->value); return; }
    m->kvCount += 1;
}

static void drmeta__push_picture(drmeta* m, const drflac_metadata* pMetadata)
{
    drmeta_picture* pic;
    if (pMetadata->data.picture.pPictureData == NULL) return;   /* Nothing to copy. */
    if (m->picCount == m->picCap) {
        size_t newCap = (m->picCap == 0) ? 4 : m->picCap * 2;
        drmeta_picture* newPics = (drmeta_picture*)realloc(m->pics, newCap * sizeof(*newPics));
        if (newPics == NULL) return;
        m->pics = newPics;
        m->picCap = newCap;
    }
    pic = &m->pics[m->picCount];
    pic->type   = pMetadata->data.picture.type;
    pic->width  = pMetadata->data.picture.width;
    pic->height = pMetadata->data.picture.height;
    pic->size   = pMetadata->data.picture.pictureDataSize;
    {
        size_t mimeLen = pMetadata->data.picture.mimeLength;
        if (mimeLen >= sizeof(pic->mime)) mimeLen = sizeof(pic->mime) - 1;
        memcpy(pic->mime, pMetadata->data.picture.mime, mimeLen);
        pic->mime[mimeLen] = '\0';
    }
    pic->data = (drflac_uint8*)malloc(pic->size);
    if (pic->data == NULL) return;
    memcpy(pic->data, pMetadata->data.picture.pPictureData, pic->size);
    m->picCount += 1;
}

static void drmeta__on_meta(void* pUserData, drflac_metadata* pMetadata)
{
    drmeta* m = (drmeta*)pUserData;

    switch (pMetadata->type)
    {
        case DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT:
        {
            drflac_vorbis_comment_iterator iter;
            const char* pComment;
            drflac_uint32 commentLen;

            m->vorbisCommentBlocksParsed += 1;

            free(m->vendor);
            m->vendor = drmeta__strndup(pMetadata->data.vorbis_comment.vendor,
                                        pMetadata->data.vorbis_comment.vendorLength);

            drflac_init_vorbis_comment_iterator(&iter,
                                                pMetadata->data.vorbis_comment.commentCount,
                                                pMetadata->data.vorbis_comment.pComments);
            while ((pComment = drflac_next_vorbis_comment(&iter, &commentLen)) != NULL) {
                /* Format is "KEY=VALUE". Split at the first '='. */
                drflac_uint32 i;
                for (i = 0; i < commentLen; ++i) {
                    if (pComment[i] == '=') break;
                }
                drmeta__push_kv(m, pComment, i,
                                (i < commentLen) ? pComment + i + 1 : "",
                                (i < commentLen) ? commentLen - i - 1 : 0);
            }
        } break;

        case DRFLAC_METADATA_BLOCK_TYPE_PICTURE:
        {
            drmeta__push_picture(m, pMetadata);
        } break;

        case DRFLAC_METADATA_BLOCK_TYPE_STREAMINFO:
        case DRFLAC_METADATA_BLOCK_TYPE_PADDING:
        case DRFLAC_METADATA_BLOCK_TYPE_SEEKTABLE:
        {
            /* Known structural blocks: not surfaced through the query interface. */
        } break;

        case DRFLAC_METADATA_BLOCK_TYPE_APPLICATION:
        case DRFLAC_METADATA_BLOCK_TYPE_CUESHEET:
        default:    /* Reserved block types. */
        {
            m->unknownBlocksSkipped += 1;
        } break;
    }
}

static drflac_uint32 drmeta__le32(const drflac_uint8* p)
{
    return (drflac_uint32)p[0] | ((drflac_uint32)p[1] << 8) | ((drflac_uint32)p[2] << 16) | ((drflac_uint32)p[3] << 24);
}

/* Structural validation of a VORBIS_COMMENT block body, mirroring dr_flac's checks. */
static drflac_bool32 drmeta__vorbis_block_is_valid(const drflac_uint8* p, drflac_uint32 size)
{
    drflac_uint32 pos = 0;
    drflac_uint32 vendorLen, commentCount, i;

    if (size < 8) return DRFLAC_FALSE;

    vendorLen = drmeta__le32(p + pos); pos += 4;
    if ((drflac_int64)(size - pos) - 4 < (drflac_int64)vendorLen) return DRFLAC_FALSE;
    pos += vendorLen;

    commentCount = drmeta__le32(p + pos); pos += 4;
    if ((size - pos) / 4 < commentCount) return DRFLAC_FALSE;

    for (i = 0; i < commentCount; ++i) {
        drflac_uint32 commentLen;
        if (size - pos < 4) return DRFLAC_FALSE;
        commentLen = drmeta__le32(p + pos); pos += 4;
        if ((drflac_int64)(size - pos) < (drflac_int64)commentLen) return DRFLAC_FALSE;
        pos += commentLen;
    }

    return DRFLAC_TRUE;
}

/*
Walks the FLAC metadata block headers directly. dr_flac silently drops a
VORBIS_COMMENT block that fails validation (its callback simply never fires),
so this scan is what lets us count corrupted comment blocks. It also records
the block inventory for reporting.
*/
static void drmeta__scan_blocks(const drflac_uint8* data, size_t size, drmeta* m)
{
    size_t pos;

    if (size < 4 || memcmp(data, "fLaC", 4) != 0) return;
    pos = 4;

    for (;;) {
        drflac_uint8  type, isLast;
        drflac_uint32 blockSize;

        if (pos + 4 > size) break;
        type      = data[pos] & 0x7F;
        isLast    = (drflac_uint8)(data[pos] >> 7);
        blockSize = ((drflac_uint32)data[pos + 1] << 16) | ((drflac_uint32)data[pos + 2] << 8) | (drflac_uint32)data[pos + 3];
        pos += 4;
        if (pos + blockSize > size) break;

        if (m->blockCount < DRMETA_MAX_BLOCKS) {
            m->blockTypes[m->blockCount] = type;
            m->blockSizes[m->blockCount] = blockSize;
            m->blockCount += 1;
        }

        if (type == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
            m->vorbisCommentBlocksSeen += 1;
            if (!drmeta__vorbis_block_is_valid(data + pos, blockSize)) {
                m->corruptCommentBlocks += 1;
            }
        }

        pos += blockSize;
        if (isLast) break;
    }
}

void drmeta_free(drmeta* m);

drmeta* drmeta_open_memory(const void* data, size_t size)
{
    drmeta* m;
    drflac* pFlac;

    m = (drmeta*)calloc(1, sizeof(*m));
    if (m == NULL) return NULL;

    drmeta__scan_blocks((const drflac_uint8*)data, size, m);

    pFlac = drflac_open_memory_with_metadata(data, size, drmeta__on_meta, m, NULL);
    if (pFlac == NULL) {
        drmeta_free(m);
        return NULL;
    }
    drflac_close(pFlac);
    return m;
}

drmeta* drmeta_open_file(const char* pFilePath)
{
    FILE* f;
    long size;
    drflac_uint8* data;
    drmeta* m;

    f = fopen(pFilePath, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (drflac_uint8*)malloc((size_t)size);
    if (data == NULL) { fclose(f); return NULL; }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) { free(data); fclose(f); return NULL; }
    fclose(f);

    m = drmeta_open_memory(data, (size_t)size);
    free(data);
    return m;
}

/* Returns the first value for `key` (case-insensitive), or NULL. */
const char* drmeta_get(const drmeta* m, const char* key)
{
    size_t i;
    if (m == NULL || key == NULL) return NULL;
    for (i = 0; i < m->kvCount; ++i) {
        if (drmeta__ascii_equal_nocase(m->kvs[i].key, key)) return m->kvs[i].value;
    }
    return NULL;
}

/* Fills `valuesOut` with all values for `key` (multi-value keys). Returns the count. */
size_t drmeta_get_values(const drmeta* m, const char* key, const char** valuesOut, size_t valuesCap)
{
    size_t i, count = 0;
    if (m == NULL || key == NULL) return 0;
    for (i = 0; i < m->kvCount; ++i) {
        if (drmeta__ascii_equal_nocase(m->kvs[i].key, key)) {
            if (valuesOut != NULL && count < valuesCap) valuesOut[count] = m->kvs[i].value;
            count += 1;
        }
    }
    return count;
}

size_t drmeta_get_picture_count(const drmeta* m)
{
    return (m != NULL) ? m->picCount : 0;
}

const drmeta_picture* drmeta_get_picture(const drmeta* m, size_t index)
{
    if (m == NULL || index >= m->picCount) return NULL;
    return &m->pics[index];
}

void drmeta_free(drmeta* m)
{
    size_t i;
    if (m == NULL) return;
    for (i = 0; i < m->kvCount; ++i) { free(m->kvs[i].key); free(m->kvs[i].value); }
    free(m->kvs);
    for (i = 0; i < m->picCount; ++i) { free(m->pics[i].data); }
    free(m->pics);
    free(m->vendor);
    free(m);
}

/* ============================ test / benchmark driver ============================ */

static const char* block_type_name(drflac_uint32 type)
{
    switch (type) {
        case DRFLAC_METADATA_BLOCK_TYPE_STREAMINFO:     return "STREAMINFO";
        case DRFLAC_METADATA_BLOCK_TYPE_PADDING:        return "PADDING";
        case DRFLAC_METADATA_BLOCK_TYPE_APPLICATION:    return "APPLICATION";
        case DRFLAC_METADATA_BLOCK_TYPE_SEEKTABLE:      return "SEEKTABLE";
        case DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT: return "VORBIS_COMMENT";
        case DRFLAC_METADATA_BLOCK_TYPE_CUESHEET:       return "CUESHEET";
        case DRFLAC_METADATA_BLOCK_TYPE_PICTURE:        return "PICTURE";
        case DRFLAC_METADATA_BLOCK_TYPE_INVALID:        return "INVALID";
        default:                                        return "RESERVED/UNKNOWN";
    }
}

static const char* sniff_image_format(const drflac_uint8* p, drflac_uint32 n)
{
    if (n >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) return "png";
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return "jpeg";
    if (n >= 6 && (memcmp(p, "GIF87a", 6) == 0 || memcmp(p, "GIF89a", 6) == 0)) return "gif";
    if (n >= 2 && memcmp(p, "BM", 2) == 0) return "bmp";
    return "unknown";
}

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1.0e6 + (double)ts.tv_nsec / 1.0e3;
}

#define BENCH_ITERS 500

int main(int argc, char** argv)
{
    const char* pFilePath;
    const char* pPicDumpPrefix = NULL;
    FILE* f;
    long fileSize;
    drflac_uint8* data;
    drmeta* m;
    size_t i;
    double tRead, tParse, t0, ratio;
    int timingOK;

    if (argc < 2) {
        printf("usage: dr_libs_metadata <file.flac> [pic-dump-prefix]\n");
        return 1;
    }
    pFilePath = argv[1];
    if (argc >= 3) pPicDumpPrefix = argv[2];

    /* Read the whole file into memory. */
    f = fopen(pFilePath, "rb");
    if (f == NULL) { printf("ERROR cannot open %s\n", pFilePath); return 1; }
    fseek(f, 0, SEEK_END);
    fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (drflac_uint8*)malloc((size_t)fileSize);
    if (data == NULL || fread(data, 1, (size_t)fileSize, f) != (size_t)fileSize) {
        printf("ERROR cannot read %s\n", pFilePath);
        fclose(f);
        return 1;
    }
    fclose(f);

    m = drmeta_open_memory(data, (size_t)fileSize);
    if (m == NULL) { printf("ERROR drmeta_open_memory failed for %s\n", pFilePath); free(data); return 1; }

    printf("FILE %s\n", pFilePath);
    printf("SIZE %ld\n", fileSize);
    for (i = 0; i < m->blockCount; ++i) {
        printf("BLOCK %zu type=%u name=%s size=%u\n", i, m->blockTypes[i],
               block_type_name(m->blockTypes[i]), m->blockSizes[i]);
    }
    printf("VENDOR %s\n", (m->vendor != NULL) ? m->vendor : "(none)");
    for (i = 0; i < m->kvCount; ++i) {
        printf("KV %s=%s\n", m->kvs[i].key, m->kvs[i].value);
    }
    for (i = 0; i < m->picCount; ++i) {
        const drmeta_picture* pic = &m->pics[i];
        printf("PIC %zu mime=%s pictype=%u dims=%ux%u bytes=%u sniff=%s\n",
               i, pic->mime, pic->type, pic->width, pic->height, pic->size,
               sniff_image_format(pic->data, pic->size));
        if (pPicDumpPrefix != NULL) {
            char dumpPath[1024];
            snprintf(dumpPath, sizeof(dumpPath), "%s.pic%zu.%s", pPicDumpPrefix, i,
                     sniff_image_format(pic->data, pic->size));
            f = fopen(dumpPath, "wb");
            if (f != NULL) { fwrite(pic->data, 1, pic->size, f); fclose(f); }
            printf("PICDUMP %zu %s\n", i, dumpPath);
        }
    }
    printf("SKIP unknown_blocks=%u corrupt_comment_blocks=%u vorbis_seen=%u vorbis_parsed=%u\n",
           m->unknownBlocksSkipped, m->corruptCommentBlocks,
           m->vorbisCommentBlocksSeen, m->vorbisCommentBlocksParsed);

    /* Benchmark: whole-file read vs metadata parse. */
    t0 = now_us();
    for (i = 0; i < BENCH_ITERS; ++i) {
        FILE* bf = fopen(pFilePath, "rb");
        if (bf == NULL) break;
        if (fread(data, 1, (size_t)fileSize, bf) != (size_t)fileSize) { fclose(bf); break; }
        fclose(bf);
    }
    tRead = (now_us() - t0) / BENCH_ITERS;

    t0 = now_us();
    for (i = 0; i < BENCH_ITERS; ++i) {
        drmeta* tmp = drmeta_open_memory(data, (size_t)fileSize);
        drmeta_free(tmp);
    }
    tParse = (now_us() - t0) / BENCH_ITERS;

    ratio = (tRead > 0.0) ? (tParse / tRead) : 1.0;
    timingOK = (ratio < 0.05);
    printf("TIME file_read_us=%.1f parse_us=%.1f parse_overhead_pct=%.2f\n", tRead, tParse, ratio * 100.0);
    printf("RESULT %s\n", timingOK ? "OK" : "FAIL_TIMING");

    drmeta_free(m);
    free(data);
    return timingOK ? 0 : 1;
}
