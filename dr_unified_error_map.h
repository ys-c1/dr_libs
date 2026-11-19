/*
dr_unified_error_map.h — 统一错误域映射表（单一事实来源 / single source of truth）

本文件是 dr_wav / dr_mp3 / dr_flac 三套原生结果码到统一错误域（drerr_domain）
的唯一映射定义。dr_unified_error.h 用它生成 O(1) 查表数组；
docs/ERROR_CODES.md 由 tools/gen_error_doc.py 从本文件自动生成。

三个库共享同一组结果码后缀（SUCCESS .. AT_END，对应 0 .. -53），因此映射按
“后缀”定义一次，从结构上保证同一故障在三库上必然落入同一错误域。
dr_flac 独有的结果码在 DRERR_FLAC_EXTRA_MAP 中单独列出。

修改规则：
  1. 条目顺序必须与结果码数值顺序一致：0, -1, -2, ..., -53（查表数组按下标 -code 寻址，
     顺序错误会被 tests/dr_unified_error_test.c 的 switch 交叉校验捕获）。
  2. 修改本文件后运行：python3 tools/gen_error_doc.py
     重新生成 docs/ERROR_CODES.md。生成器会校验三库公开结果码被穷举覆盖，
     漏一个即报错退出。
*/

/* 三库共享的结果码映射：X(后缀, 统一错误域, 中文说明) */
#define DRERR_SHARED_MAP(X)                                                      \
    X(SUCCESS,                        DRERR_OK,            "操作成功")            \
    X(ERROR,                          DRERR_UNKNOWN,       "通用错误（库未进一步分类）") \
    X(INVALID_ARGS,                   DRERR_USAGE,         "参数无效")            \
    X(INVALID_OPERATION,              DRERR_USAGE,         "当前状态下不允许该操作") \
    X(OUT_OF_MEMORY,                  DRERR_OUT_OF_MEMORY, "内存分配失败")        \
    X(OUT_OF_RANGE,                   DRERR_USAGE,         "索引或范围越界")       \
    X(ACCESS_DENIED,                  DRERR_IO,            "权限不足，访问被拒绝")  \
    X(DOES_NOT_EXIST,                 DRERR_IO,            "文件或路径不存在")     \
    X(ALREADY_EXISTS,                 DRERR_IO,            "文件已存在")          \
    X(TOO_MANY_OPEN_FILES,            DRERR_IO,            "打开文件数超限")       \
    X(INVALID_FILE,                   DRERR_IO,            "无效的文件或句柄")     \
    X(TOO_BIG,                        DRERR_IO,            "文件超出可处理大小")   \
    X(PATH_TOO_LONG,                  DRERR_IO,            "路径过长")            \
    X(NAME_TOO_LONG,                  DRERR_IO,            "文件名过长")          \
    X(NOT_DIRECTORY,                  DRERR_IO,            "路径不是目录")         \
    X(IS_DIRECTORY,                   DRERR_IO,            "路径是目录而非文件")   \
    X(DIRECTORY_NOT_EMPTY,            DRERR_IO,            "目录非空")            \
    X(END_OF_FILE,                    DRERR_TRUNCATED,     "意外到达文件末尾（数据被截断）") \
    X(NO_SPACE,                       DRERR_IO,            "存储空间不足")         \
    X(BUSY,                           DRERR_IO,            "设备或资源被占用")     \
    X(IO_ERROR,                       DRERR_IO,            "底层 I/O 错误")       \
    X(INTERRUPT,                      DRERR_IO,            "操作被中断")          \
    X(UNAVAILABLE,                    DRERR_IO,            "资源暂不可用")         \
    X(ALREADY_IN_USE,                 DRERR_IO,            "资源已被占用")         \
    X(BAD_ADDRESS,                    DRERR_USAGE,         "无效内存地址")         \
    X(BAD_SEEK,                       DRERR_IO,            "非法的文件定位")       \
    X(BAD_PIPE,                       DRERR_IO,            "管道错误")            \
    X(DEADLOCK,                       DRERR_IO,            "资源死锁")            \
    X(TOO_MANY_LINKS,                 DRERR_IO,            "链接数超限")          \
    X(NOT_IMPLEMENTED,                DRERR_UNSUPPORTED,   "功能未实现")          \
    X(NO_MESSAGE,                     DRERR_IO,            "无可用消息")          \
    X(BAD_MESSAGE,                    DRERR_IO,            "消息损坏")            \
    X(NO_DATA_AVAILABLE,              DRERR_TRUNCATED,     "无可用数据（流提前结束）") \
    X(INVALID_DATA,                   DRERR_FORMAT,        "数据内容非法或已损坏")  \
    X(TIMEOUT,                        DRERR_IO,            "操作超时")            \
    X(NO_NETWORK,                     DRERR_IO,            "网络不可用")          \
    X(NOT_UNIQUE,                     DRERR_IO,            "名称不唯一")          \
    X(NOT_SOCKET,                     DRERR_IO,            "不是套接字")          \
    X(NO_ADDRESS,                     DRERR_IO,            "地址不可用")          \
    X(BAD_PROTOCOL,                   DRERR_FORMAT,        "协议数据无效")         \
    X(PROTOCOL_UNAVAILABLE,           DRERR_UNSUPPORTED,   "协议不可用")          \
    X(PROTOCOL_NOT_SUPPORTED,         DRERR_UNSUPPORTED,   "协议不支持")          \
    X(PROTOCOL_FAMILY_NOT_SUPPORTED,  DRERR_UNSUPPORTED,   "协议族不支持")        \
    X(ADDRESS_FAMILY_NOT_SUPPORTED,   DRERR_UNSUPPORTED,   "地址族不支持")        \
    X(SOCKET_NOT_SUPPORTED,           DRERR_UNSUPPORTED,   "套接字类型不支持")     \
    X(CONNECTION_RESET,               DRERR_IO,            "连接被重置")          \
    X(ALREADY_CONNECTED,              DRERR_USAGE,         "连接已建立（重复连接）") \
    X(NOT_CONNECTED,                  DRERR_USAGE,         "连接未建立")          \
    X(CONNECTION_REFUSED,             DRERR_IO,            "连接被拒绝")          \
    X(NO_HOST,                        DRERR_IO,            "主机不可达")          \
    X(IN_PROGRESS,                    DRERR_IO,            "操作进行中")          \
    X(CANCELLED,                      DRERR_IO,            "操作已取消")          \
    X(MEMORY_ALREADY_MAPPED,          DRERR_USAGE,         "内存已映射")          \
    X(AT_END,                         DRERR_TRUNCATED,     "已到达数据末尾")

/* dr_flac 独有的结果码映射：X(后缀, 统一错误域, 中文说明) */
#define DRERR_FLAC_EXTRA_MAP(X)                                                  \
    X(CRC_MISMATCH,                   DRERR_FORMAT,        "CRC 校验失败，帧数据已损坏")
