#!/usr/bin/env python3
"""从 dr_unified_error_map.h 自动生成 docs/ERROR_CODES.md。

同时做穷举性校验：解析 dr_wav.h / dr_mp3.h / dr_flac.h 的 Result Codes 段，
任何一个公开结果码没有出现在映射表中即报错退出（漏一个就算失败）。

用法：python3 tools/gen_error_doc.py   （在仓库根目录运行）
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAP_FILE = ROOT / "dr_unified_error_map.h"
DOC_FILE = ROOT / "docs" / "ERROR_CODES.md"

LIBS = [
    ("dr_wav", "DRWAV", ROOT / "dr_wav.h"),
    ("dr_mp3", "DRMP3", ROOT / "dr_mp3.h"),
    ("dr_flac", "DRFLAC", ROOT / "dr_flac.h"),
]

ENTRY_RE = re.compile(r'X\(\s*([A-Z0-9_]+)\s*,\s*(DRERR_[A-Z0-9_]+)\s*,\s*"([^"]*)"\s*\)')
DEFINE_RE = re.compile(r"#define\s+(DR(?:WAV|MP3|FLAC)_([A-Z0-9_]+))\s+(-?\d+)\s*(?:/\*.*)?$")

DOMAINS = [
    ("DRERR_OK", "OK", "成功", "操作成功。"),
    ("DRERR_FORMAT", "FORMAT", "格式错误", "数据内容非法或已损坏，如非法的块数据、CRC 校验失败。"),
    ("DRERR_TRUNCATED", "TRUNCATED", "数据截断", "流提前结束，数据不完整，如意外 EOF、到达数据末尾。"),
    ("DRERR_UNSUPPORTED", "UNSUPPORTED", "不支持", "格式、协议或特性不被支持，或功能未实现。"),
    ("DRERR_OUT_OF_MEMORY", "OUT_OF_MEMORY", "内存不足", "内存分配失败。"),
    ("DRERR_IO", "IO", "IO 错误", "文件系统、设备、网络等外部环境失败。"),
    ("DRERR_USAGE", "USAGE", "调用方式错误（辅助域）", "参数无效、当前状态不允许该操作等调用方问题。"),
    ("DRERR_UNKNOWN", "UNKNOWN", "未分类错误（辅助域）", "库未进一步分类的通用错误，或超出已知区间的结果码。"),
]


def parse_map():
    """解析映射表，返回 (shared, flac_extra)：[(suffix, domain, desc), ...]，保持书写顺序。"""
    text = MAP_FILE.read_text(encoding="utf-8")

    def block(name):
        m = re.search(name + r"\(X\)\s*\\\n(.*?)(?:\n\s*\n|\Z)", text, re.S)
        if not m:
            sys.exit(f"错误：在 {MAP_FILE.name} 中找不到 {name} 块")
        return ENTRY_RE.findall(m.group(1))

    return block("DRERR_SHARED_MAP"), block("DRERR_FLAC_EXTRA_MAP")


def parse_lib_codes(header):
    """解析库头文件 Result Codes 段，返回 {suffix: value}。"""
    text = header.read_text(encoding="utf-8")
    m = re.search(r"/\* Result Codes \*/(.*?)/\* End Result Codes \*/", text, re.S)
    if not m:
        sys.exit(f"错误：在 {header.name} 中找不到 Result Codes 段")
    codes = {}
    for line in m.group(1).splitlines():
        d = DEFINE_RE.match(line.strip())
        if d:
            codes[d.group(2)] = int(d.group(3))
    return codes


def main():
    shared, flac_extra = parse_map()
    shared_suffixes = [s for s, _, _ in shared]

    # 校验 1：共享表顺序必须等于数值顺序 0, -1, ..., -53（查表数组按下标 -code 寻址）。
    wav_codes = parse_lib_codes(LIBS[0][2])
    expected_order = sorted(wav_codes, key=lambda s: -wav_codes[s])
    if shared_suffixes != expected_order:
        sys.exit(
            "错误：DRERR_SHARED_MAP 条目顺序与结果码数值顺序不一致。\n"
            f"  映射表顺序: {shared_suffixes}\n  应为: {expected_order}"
        )

    # 校验 2：穷举性——三库公开结果码必须全部出现在映射表中，漏一个即失败。
    lib_codes = {}
    total = 0
    for lib_name, prefix, header in LIBS:
        codes = parse_lib_codes(header)
        lib_codes[lib_name] = (prefix, codes)
        allowed = set(shared_suffixes)
        if prefix == "DRFLAC":
            allowed |= {s for s, _, _ in flac_extra}
        missing = sorted(set(codes) - allowed)
        extra = sorted(allowed - set(codes) - ({s for s, _, _ in flac_extra} if prefix != "DRFLAC" else set()))
        if missing:
            sys.exit(f"错误：{header.name} 中的公开结果码未在映射表中覆盖: {missing}")
        if extra:
            sys.exit(f"错误：映射表中存在 {header.name} 没有的结果码后缀: {extra}")
        total += len(codes)

    counts = {name: len(codes) for name, (_, codes) in lib_codes.items()}
    print(f"穷举校验通过：dr_wav {counts['dr_wav']} 个，dr_mp3 {counts['dr_mp3']} 个，"
          f"dr_flac {counts['dr_flac']} 个，共 {total} 个公开结果码全部被映射。")

    shared_by_suffix = {s: (dom, desc) for s, dom, desc in shared}
    extra_by_suffix = {s: (dom, desc) for s, dom, desc in flac_extra}

    def domain_of(lib_name, suffix):
        if suffix in shared_by_suffix:
            return shared_by_suffix[suffix]
        return extra_by_suffix[suffix]

    # 生成文档。
    out = []
    out.append("# 统一错误码说明\n")
    out.append("> 本文档由 `tools/gen_error_doc.py` 从 `dr_unified_error_map.h` 自动生成，请勿手改。")
    out.append("> 修改映射表后运行 `python3 tools/gen_error_doc.py` 重新生成本文档。\n")
    out.append("dr_wav / dr_mp3 / dr_flac 三套原生结果码收敛到一套统一错误域"
               "（`drerr_domain`，见 `dr_unified_error.h`）。上层调用用"
               " `drerr_from_wav()` / `drerr_from_mp3()` / `drerr_from_flac()` "
               "把原生结果码折算成错误域后，只需一套分支。查表为 O(1) 数组下标访问，"
               "单次开销 < 1 微秒（基准见 `tests/dr_unified_error_test.c`）。\n")

    out.append("## 统一错误域\n")
    out.append("| 错误域 | 常量 | 含义 | 说明 |")
    out.append("|---|---|---|---|")
    for const, name, zh, desc in DOMAINS:
        out.append(f"| {name} | `{const}` | {zh} | {desc} |")
    out.append("")

    out.append("## 同一故障 → 同一错误域\n")
    out.append("三个库共享同一组结果码后缀（`SUCCESS` .. `AT_END`，对应 `0` .. `-53`），"
               "映射按后缀定义一次，因此同一故障在三库上必然落入同一错误域。"
               "该性质由 `tests/dr_unified_error_test.c` 中的 12 个构造用例"
               "（4 类故障 × 3 个库）在运行时验证：\n")
    out.append("| 故障 | dr_wav | dr_mp3 | dr_flac | 统一错误域 |")
    out.append("|---|---|---|---|---|")
    faults = [
        ("数据截断", "DRWAV_END_OF_FILE", "DRMP3_AT_END", "DRFLAC_AT_END", "TRUNCATED"),
        ("格式错误", "DRWAV_INVALID_DATA", "DRMP3_INVALID_DATA", "DRFLAC_CRC_MISMATCH", "FORMAT"),
        ("内存不足", "DRWAV_OUT_OF_MEMORY", "DRMP3_OUT_OF_MEMORY", "DRFLAC_OUT_OF_MEMORY", "OUT_OF_MEMORY"),
        ("IO 错误", "DRWAV_DOES_NOT_EXIST", "DRMP3_DOES_NOT_EXIST", "DRFLAC_DOES_NOT_EXIST", "IO"),
    ]
    for fault, w, m, f, dom in faults:
        out.append(f"| {fault} | `{w}` | `{m}` | `{f}` | {dom} |")
    out.append("")

    domain_name = {const: name for const, name, _, _ in DOMAINS}
    for lib_name, prefix, _ in LIBS:
        _, codes = lib_codes[lib_name]
        out.append(f"## {lib_name} 映射表（{len(codes)} 个）\n")
        out.append("| 值 | 原生结果码 | 统一错误域 | 说明 |")
        out.append("|---|---|---|---|")
        for suffix in sorted(codes, key=lambda s: -codes[s]):
            dom, desc = domain_of(lib_name, suffix)
            out.append(f"| {codes[suffix]} | `{prefix}_{suffix}` | {domain_name[dom]} | {desc} |")
        out.append("")

    out.append("## 总计\n")
    out.append(f"- dr_wav：{counts['dr_wav']} 个公开结果码")
    out.append(f"- dr_mp3：{counts['dr_mp3']} 个公开结果码")
    out.append(f"- dr_flac：{counts['dr_flac']} 个公开结果码")
    out.append(f"- **合计 {total} 个，全部被映射表覆盖（生成器已做穷举校验，漏一个即报错）。**")
    out.append("")

    DOC_FILE.parent.mkdir(parents=True, exist_ok=True)
    DOC_FILE.write_text("\n".join(out), encoding="utf-8")
    print(f"已生成 {DOC_FILE.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
