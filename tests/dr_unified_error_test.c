/*
dr_unified_error_test.c — 统一错误域映射的验证与基准

验证内容：
  1. 查表数组与按名字拼接的 switch 交叉一致（防止映射表书写顺序错位）；
  2. 共享区间 0..-53 内同一数值在三库上映射到同一错误域；
  3. 12 个构造用例：4 类故障 × 3 个库，同一故障必须落入同一错误域；
  4. 真实 API 调用冒烟：drwav_get_cursor_in_pcm_frames(NULL, NULL)
     返回 DRWAV_INVALID_ARGS，映射为 DRERR_USAGE；
  5. 基准：163 个原生码随机查表，单次平均开销必须 < 1 微秒。

构建（仓库根目录）：
    cc -O2 -I. tests/dr_unified_error_test.c -o /tmp/dr_unified_error_test
*/
#define _POSIX_C_SOURCE 199309L   /* clock_gettime（严格 C89 模式下需要） */
#define DR_WAV_IMPLEMENTATION   /* 第 4 项真实调用需要 dr_wav 实现 */
#define DR_FLAC_IMPLEMENTATION  /* dr_flac 的结果码定义在实现段，测试需要看到 DRFLAC_* 宏 */
#include "../dr_unified_error.h"

#include <stdio.h>
#include <time.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);        \
            g_failures += 1;                                              \
        }                                                                 \
    } while (0)

/* 用映射表按名字拼接出 switch，与查表数组交叉校验。 */
static drerr_domain wav_switch(drwav_result code)
{
    switch (code) {
#define DRERR__CASE(suffix, domain, desc) case DRWAV_##suffix: return domain;
        DRERR_SHARED_MAP(DRERR__CASE)
#undef DRERR__CASE
        default: return DRERR_UNKNOWN;
    }
}

static drerr_domain mp3_switch(drmp3_result code)
{
    switch (code) {
#define DRERR__CASE(suffix, domain, desc) case DRMP3_##suffix: return domain;
        DRERR_SHARED_MAP(DRERR__CASE)
#undef DRERR__CASE
        default: return DRERR_UNKNOWN;
    }
}

static drerr_domain flac_switch(drflac_result code)
{
    switch (code) {
#define DRERR__CASE(suffix, domain, desc) case DRFLAC_##suffix: return domain;
        DRERR_SHARED_MAP(DRERR__CASE)
#undef DRERR__CASE
#define DRERR__CASE_EXTRA(suffix, domain, desc) case DRFLAC_##suffix: return domain;
        DRERR_FLAC_EXTRA_MAP(DRERR__CASE_EXTRA)
#undef DRERR__CASE_EXTRA
        default: return DRERR_UNKNOWN;
    }
}

/* 12 个构造用例：4 类故障 × 3 个库。 */
typedef struct {
    const char* fault;    /* 故障类别 */
    const char* lib;      /* 库名 */
    drwav_int32 code;     /* 该故障在该库上的原生结果码 */
    drerr_domain expect;  /* 期望的统一错误域 */
} fault_case;

#define FAULT_CASES(X)                                                    \
    /* 故障 1：数据截断（流提前结束） */                                    \
    X("数据截断", "dr_wav",  DRWAV_END_OF_FILE,      DRERR_TRUNCATED)      \
    X("数据截断", "dr_mp3",  DRMP3_AT_END,           DRERR_TRUNCATED)      \
    X("数据截断", "dr_flac", DRFLAC_AT_END,          DRERR_TRUNCATED)      \
    /* 故障 2：格式错误（数据内容损坏） */                                   \
    X("格式错误", "dr_wav",  DRWAV_INVALID_DATA,     DRERR_FORMAT)         \
    X("格式错误", "dr_mp3",  DRMP3_INVALID_DATA,     DRERR_FORMAT)         \
    X("格式错误", "dr_flac", DRFLAC_CRC_MISMATCH,    DRERR_FORMAT)         \
    /* 故障 3：内存不足 */                                                  \
    X("内存不足", "dr_wav",  DRWAV_OUT_OF_MEMORY,    DRERR_OUT_OF_MEMORY)  \
    X("内存不足", "dr_mp3",  DRMP3_OUT_OF_MEMORY,    DRERR_OUT_OF_MEMORY)  \
    X("内存不足", "dr_flac", DRFLAC_OUT_OF_MEMORY,   DRERR_OUT_OF_MEMORY)  \
    /* 故障 4：IO 错误（文件不存在） */                                     \
    X("IO 错误",  "dr_wav",  DRWAV_DOES_NOT_EXIST,   DRERR_IO)             \
    X("IO 错误",  "dr_mp3",  DRMP3_DOES_NOT_EXIST,   DRERR_IO)             \
    X("IO 错误",  "dr_flac", DRFLAC_DOES_NOT_EXIST,  DRERR_IO)

static drerr_domain map_by_lib(const char* lib, drwav_int32 code)
{
    if (lib[3] == 'w') return drerr_from_wav(code);    /* "dr_wav" */
    if (lib[3] == 'm') return drerr_from_mp3(code);    /* "dr_mp3" */
    return drerr_from_flac(code);                      /* "dr_flac" */
}

static void test_table_vs_switch(void)
{
    int i;
    for (i = 0; i < DRERR_TABLE_SIZE; i++) {
        drwav_int32 code = (drwav_int32)(-i);
        CHECK(drerr_from_wav(code) == wav_switch(code),  "wav table != switch");
        CHECK(drerr_from_mp3(code) == mp3_switch(code),  "mp3 table != switch");
        CHECK(drerr_from_flac(code) == flac_switch(code), "flac table != switch");
    }
    CHECK(drerr_from_flac(DRFLAC_CRC_MISMATCH) == flac_switch(DRFLAC_CRC_MISMATCH),
          "flac CRC_MISMATCH table != switch");
    /* dr_unified_error.h 在纯声明场景下用数值 -100 特判 CRC_MISMATCH，这里与真实宏比对。 */
    CHECK(DRFLAC_CRC_MISMATCH == DRERR_FLAC_CRC_MISMATCH_VALUE,
          "DRERR_FLAC_CRC_MISMATCH_VALUE drifted from DRFLAC_CRC_MISMATCH");
}

static void test_cross_lib_consistency(void)
{
    int i;
    for (i = 0; i < DRERR_TABLE_SIZE; i++) {
        drwav_int32 code = (drwav_int32)(-i);
        drerr_domain w = drerr_from_wav(code);
        drerr_domain m = drerr_from_mp3(code);
        drerr_domain f = drerr_from_flac(code);
        CHECK(w == m && m == f, "same code mapped to different domains across libs");
        CHECK(w >= DRERR_OK && w < DRERR_DOMAIN_COUNT, "domain out of range");
    }
}

static void test_12_fault_cases(void)
{
    static const fault_case cases[] = {
#define DRERR__CASE_ROW(fault, lib, code, expect) { fault, lib, code, expect },
        FAULT_CASES(DRERR__CASE_ROW)
#undef DRERR__CASE_ROW
    };
    const size_t count = sizeof(cases) / sizeof(cases[0]);
    size_t i;

    CHECK(count == 12, "constructed case count must be 12");
    printf("12 个构造用例：\n");
    for (i = 0; i < count; i++) {
        drerr_domain got = map_by_lib(cases[i].lib, cases[i].code);
        int ok = (got == cases[i].expect);
        printf("  [%s] %-7s code=%-4d -> %-13s %s\n",
               cases[i].fault, cases[i].lib, (int)cases[i].code,
               drerr_domain_name(got), ok ? "PASS" : "FAIL");
        CHECK(ok, "fault case mapped to wrong domain");
    }

    /* 同一故障在三库上的映射必须两两相等。 */
    for (i = 0; i + 2 < count; i += 3) {
        drerr_domain a = map_by_lib(cases[i].lib, cases[i].code);
        drerr_domain b = map_by_lib(cases[i + 1].lib, cases[i + 1].code);
        drerr_domain c = map_by_lib(cases[i + 2].lib, cases[i + 2].code);
        CHECK(a == b && b == c, "same fault mapped to different domains across libs");
    }
}

static void test_real_api_call(void)
{
    /* 真实调用：NULL 参数 -> DRWAV_INVALID_ARGS -> DRERR_USAGE。 */
    drwav_result r = drwav_get_cursor_in_pcm_frames(NULL, NULL);
    CHECK(r == DRWAV_INVALID_ARGS, "drwav_get_cursor_in_pcm_frames(NULL,NULL) != DRWAV_INVALID_ARGS");
    CHECK(drerr_from_wav(r) == DRERR_USAGE, "DRWAV_INVALID_ARGS should map to DRERR_USAGE");
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void test_benchmark(void)
{
    /* 覆盖全部 163 个公开结果码的查表基准。 */
    enum { ITERATIONS = 10000000 };
    volatile int sink = 0;
    double t0, elapsed, ns_per_lookup;
    int i;

    t0 = now_seconds();
    for (i = 0; i < ITERATIONS; i++) {
        drwav_int32 code = (drwav_int32)(-(i % DRERR_TABLE_SIZE));
        sink ^= drerr_from_wav(code);
        sink ^= drerr_from_mp3(code);
        sink ^= drerr_from_flac((i % 7 == 0) ? DRFLAC_CRC_MISMATCH : code);
    }
    elapsed = now_seconds() - t0;
    ns_per_lookup = elapsed * 1e9 / (3.0 * ITERATIONS);

    printf("基准：%d 次迭代 × 3 库，平均单次查表 %.2f ns（要求 < 1000 ns）%s\n",
           ITERATIONS, ns_per_lookup, ns_per_lookup < 1000.0 ? "PASS" : "FAIL");
    CHECK(ns_per_lookup < 1000.0, "lookup slower than 1 microsecond");
    (void)sink;
}

int main(void)
{
    test_table_vs_switch();
    test_cross_lib_consistency();
    test_12_fault_cases();
    test_real_api_call();
    test_benchmark();

    if (g_failures == 0) {
        printf("全部通过。\n");
        return 0;
    }
    printf("%d 项失败。\n", g_failures);
    return 1;
}
