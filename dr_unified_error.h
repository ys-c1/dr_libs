/*
dr_unified_error.h — dr_wav / dr_mp3 / dr_flac 统一错误域映射层

把三个解码器各说各话的原生结果码收敛到一套统一错误域（drerr_domain），
上层调用只需一套分支：

    drwav_result wr = drwav_get_length_in_pcm_frames(pWav, &len);
    if (drerr_from_wav(wr) == DRERR_IO) { ... }   // 与 mp3/flac 同一套判断

映射表定义在 dr_unified_error_map.h（单一事实来源）；人可读文档
docs/ERROR_CODES.md 由 tools/gen_error_doc.py 从映射表自动生成。

查表为 O(1) 数组下标访问，实测单次开销约 1 纳秒量级（见
tests/dr_unified_error_test.c 中的基准，要求 < 1 微秒）。

本头文件为纯头文件库：全部函数为 static，无需定义 IMPLEMENTATION 宏。
兼容 C89。
*/
#ifndef dr_unified_error_h
#define dr_unified_error_h

#include "dr_wav.h"
#include "dr_mp3.h"
#include "dr_flac.h"

#include "dr_unified_error_map.h"

/* static inline：头文件库惯例，未使用的函数不触发告警。 */
#ifndef DRERR_INLINE
    #if defined(_MSC_VER)
        #define DRERR_INLINE __forceinline
    #elif defined(__GNUC__) || defined(__clang__)
        #define DRERR_INLINE __inline__   /* GNU 扩展，C89 模式下亦可用 */
    #elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
        #define DRERR_INLINE inline
    #else
        #define DRERR_INLINE
    #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 统一错误域 */
typedef enum
{
    DRERR_OK = 0,        /* 成功 */
    DRERR_FORMAT,        /* 格式错误：数据内容非法或已损坏 */
    DRERR_TRUNCATED,     /* 数据截断：流提前结束，数据不完整 */
    DRERR_UNSUPPORTED,   /* 不支持：格式、协议或特性不被支持 */
    DRERR_OUT_OF_MEMORY, /* 内存不足：分配失败 */
    DRERR_IO,            /* IO 错误：文件系统、设备、网络等外部失败 */
    DRERR_USAGE,         /* 调用方式错误：参数无效、状态不对等（辅助域） */
    DRERR_UNKNOWN,       /* 未分类错误：库未进一步分类（辅助域） */

    DRERR_DOMAIN_COUNT
} drerr_domain;

/* 共享结果码区间 0 .. -53 的查表数组长度。 */
#define DRERR_TABLE_SIZE 54

/*
查表数组：下标为 -code（0..53）。条目顺序由 dr_unified_error_map.h 的
书写顺序决定，其正确性由 tests/dr_unified_error_test.c 中按名字
拼接的 switch 交叉校验。
*/
static const drerr_domain drerr__shared_table[DRERR_TABLE_SIZE] = {
#define DRERR__TABLE_ENTRY(suffix, domain, desc) domain,
    DRERR_SHARED_MAP(DRERR__TABLE_ENTRY)
#undef DRERR__TABLE_ENTRY
};

static DRERR_INLINE drerr_domain drerr__lookup_shared(drwav_int32 code)
{
    if (code > 0 || -code >= DRERR_TABLE_SIZE) {
        return DRERR_UNKNOWN;   /* 非公开结果码：正数或超出已知区间 */
    }
    return drerr__shared_table[-code];
}

/* drwav_result -> 统一错误域 */
static DRERR_INLINE drerr_domain drerr_from_wav(drwav_result code)
{
    return drerr__lookup_shared(code);
}

/* drmp3_result -> 统一错误域 */
static DRERR_INLINE drerr_domain drerr_from_mp3(drmp3_result code)
{
    return drerr__lookup_shared(code);
}

/* drflac_result -> 统一错误域 */
#define DRERR_FLAC_CRC_MISMATCH_VALUE (-100)   /* DRFLAC_CRC_MISMATCH，见下方说明 */
static DRERR_INLINE drerr_domain drerr_from_flac(drflac_int32 code)
{
    /*
    dr_flac.h 把结果码（含 DRFLAC_CRC_MISMATCH = -100）定义在实现段，
    未定义 DR_FLAC_IMPLEMENTATION 的编译单元看不到该宏，这里直接使用数值。
    数值正确性由 tests/dr_unified_error_test.c（定义 DR_FLAC_IMPLEMENTATION
    后与真实宏比对）和 tools/gen_error_doc.py（与 dr_flac.h 交叉校验）保证。
    */
    if (code == DRERR_FLAC_CRC_MISMATCH_VALUE) {
        return DRERR_FORMAT;
    }
    return drerr__lookup_shared(code);
}

/* 错误域的短名字（用于日志），如 "FORMAT"。 */
static DRERR_INLINE const char* drerr_domain_name(drerr_domain domain)
{
    switch (domain) {
        case DRERR_OK:            return "OK";
        case DRERR_FORMAT:        return "FORMAT";
        case DRERR_TRUNCATED:     return "TRUNCATED";
        case DRERR_UNSUPPORTED:   return "UNSUPPORTED";
        case DRERR_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case DRERR_IO:            return "IO";
        case DRERR_USAGE:         return "USAGE";
        case DRERR_UNKNOWN:       return "UNKNOWN";
        default:                  return "INVALID_DOMAIN";
    }
}

/* 错误域的中文说明。 */
static DRERR_INLINE const char* drerr_domain_description(drerr_domain domain)
{
    switch (domain) {
        case DRERR_OK:            return "成功";
        case DRERR_FORMAT:        return "格式错误：数据内容非法或已损坏";
        case DRERR_TRUNCATED:     return "数据截断：流提前结束，数据不完整";
        case DRERR_UNSUPPORTED:   return "不支持：格式、协议或特性不被支持";
        case DRERR_OUT_OF_MEMORY: return "内存不足：分配失败";
        case DRERR_IO:            return "IO 错误：文件系统、设备、网络等外部失败";
        case DRERR_USAGE:         return "调用方式错误：参数无效、状态不对等";
        case DRERR_UNKNOWN:       return "未分类错误：库未进一步分类";
        default:                  return "无效的错误域";
    }
}

#ifdef __cplusplus
}
#endif

#endif  /* dr_unified_error_h */
