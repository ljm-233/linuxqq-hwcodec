#!/bin/bash
# FIXKEY v2 离线单测：从 src/qq-nvenc.c 里按大括号配平抠出真实函数来测（不是抄一份）。
# 覆盖：Annex-B 3/4 字节起始码、P 帧不抠、垃圾输入、参数集前缀拼装、大帧容量增长不越界。
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
# 被测源码：优先 $1，其次 $NVENC_SRC，最后指到构建 worktree。
# 说明：qq-nvenc.c 属于上游 PR，不放在本仓库里，我们只对 worktree 里的副本打补丁。
src="${1:-${NVENC_SRC:-/tmp/pr28-build-bc2/src/qq-nvenc.c}}"
[ -f "$src" ] || { echo "找不到被测源码：$src（用 $0 <qq-nvenc.c 路径> 指定）" >&2; exit 2; }
out="${TMPDIR:-/tmp}/fixkey2-test"
mkdir -p "$out"

python3 - "$src" "$out" <<'PY'
import re, sys
src, out = sys.argv[1], sys.argv[2]
s = open(src).read()

def grab(name):
    i = s.index("static int %s(" % name)
    j = s.index("{", i)
    d = 0
    for k in range(j, len(s)):
        if s[k] == "{":
            d += 1
        elif s[k] == "}":
            d -= 1
            if d == 0:
                return s[i:k + 1]
    raise SystemExit("找不到函数 %s" % name)

funcs = "\n\n".join(grab(n) for n in ("fx_next_nal", "fx_scan_ps", "fx_has_ps", "fx_prefix"))
shim = '''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct wrapped {
    uint8_t *fx_ps;
    uint32_t fx_ps_len;
    uint8_t *fx_pkt;
    uint32_t fx_pkt_cap;
};
#define LOG(...) do { } while (0)
'''
open(out + "/funcs.c", "w").write(shim + funcs + "\n")
print("  抠出 4 个真实函数（%d 字节）" % len(funcs))
PY

cat > "$out/main.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
struct wrapped { uint8_t *fx_ps; uint32_t fx_ps_len; uint8_t *fx_pkt; uint32_t fx_pkt_cap; };
#define LOG(...) do { } while (0)
#include "funcs.c"
static int fails;
#define CHECK(c, ...) do { if (c) { printf("  \xe2\x9c\x93 " __VA_ARGS__); } else { printf("  \xe2\x9c\x97 " __VA_ARGS__); fails++; } printf("\n"); } while (0)

int main(void)
{
    struct wrapped w;
    uint8_t bs4[64], bs3[64], ponly[16], junk[8] = {0xde,0xad,0xbe,0xef,0,0,1,0x61};
    const void *od; uint32_t osz;
    int n, n2;

    memset(&w, 0, sizeof w);
    /* 1) 4 字节起始码：SPS(4+5) + PPS(4+3) + IDR */
    n = 0;
    memcpy(bs4 + n, "\x00\x00\x00\x01\x67\x64\x00\x1f\xac", 9); n += 9;
    memcpy(bs4 + n, "\x00\x00\x00\x01\x68\xee\x3c", 7); n += 7;
    memcpy(bs4 + n, "\x00\x00\x00\x01\x65\x88\x84", 7); n += 7;
    CHECK(fx_scan_ps(&w, bs4, (uint32_t)n) == 1, "4 字节起始码：抠到参数集");
    CHECK(w.fx_ps_len == 16, "  长度 = SPS(9) + PPS(7) = 16（实际 %u）", w.fx_ps_len);
    CHECK(w.fx_ps && memcmp(w.fx_ps, bs4, 16) == 0, "  内容 = 原码流前 16 字节");
    CHECK(fx_has_ps(bs4, (uint32_t)n) == 1, "  首 NAL 是 SPS -> 已带参数集（不重复补）");

    /* 2) 3 字节起始码 */
    n2 = 0;
    memcpy(bs3 + n2, "\x00\x00\x01\x67\x64\x00\x1f\xac", 8); n2 += 8;
    memcpy(bs3 + n2, "\x00\x00\x01\x68\xee\x3c", 6); n2 += 6;
    memcpy(bs3 + n2, "\x00\x00\x01\x65\x88", 5); n2 += 5;
    CHECK(fx_scan_ps(&w, bs3, (uint32_t)n2) == 1, "3 字节起始码：抠到参数集");
    CHECK(w.fx_ps_len == 14, "  长度 = 8 + 6 = 14（实际 %u）", w.fx_ps_len);

    /* 3) 纯 P 帧：不抠、不算已带 */
    memcpy(ponly, "\x00\x00\x00\x01\x61\xe0\x39\x04", 8);
    memset(&w, 0, sizeof w);
    CHECK(fx_scan_ps(&w, ponly, 8) == 0, "纯 P 帧：不抠参数集");
    CHECK(w.fx_ps_len == 0, "  缓存仍为空");
    CHECK(fx_has_ps(ponly, 8) == 0, "  未带参数集（逐帧补前缀的判据）");

    /* 4) 垃圾输入 */
    memset(&w, 0, sizeof w);
    CHECK(fx_scan_ps(&w, junk, 8) == 0, "垃圾输入：安全返回 0");
    CHECK(fx_has_ps(junk, 8) == 0, "垃圾输入：has_ps = 0");

    /* 5) 前缀拼装 */
    memset(&w, 0, sizeof w);
    fx_scan_ps(&w, bs4, (uint32_t)n);
    CHECK(fx_prefix(&w, ponly, 8, &od, &osz) == 1, "前缀拼装：成功");
    CHECK(osz == w.fx_ps_len + 8, "  长度 = 参数集 + 帧（%u）", osz);
    CHECK(memcmp(od, bs4, 16) == 0, "  前缀就是缓存的 SPS/PPS");
    CHECK(memcmp((const uint8_t *)od + 16, ponly, 8) == 0, "  后面原样接原帧");

    /* 6) 容量增长不越界（10 -> 70000 -> 5） */
    {
        uint8_t small[10] = {0}, big[70000], tiny[5] = {0};
        int ok = 1;
        memset(big, 0xAB, sizeof big);
        ok &= fx_prefix(&w, small, 10, &od, &osz) && osz == w.fx_ps_len + 10;
        ok &= fx_prefix(&w, big, 70000, &od, &osz) && osz == w.fx_ps_len + 70000;
        ok &= fx_prefix(&w, tiny, 5, &od, &osz) && osz == w.fx_ps_len + 5;
        CHECK(ok, "大帧容量增长：三次调用长度都对、不越界（cap=%u）", w.fx_pkt_cap);
    }

    free(w.fx_ps);
    free(w.fx_pkt);
    printf(fails ? "\n  == 失败 %d 项 ==\n" : "\n  == 全部通过 ==\n", fails);
    return fails ? 1 : 0;
}
C

gcc -O1 -Wall -Wextra -I"$out" -o "$out/fixkey2-test" "$out/main.c"
"$out/fixkey2-test"
