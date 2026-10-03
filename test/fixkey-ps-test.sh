#!/bin/bash
# 离线单测：把 qq-nvenc.c 里的 fixkey_grab_ps() 与 fixkey_build() 抠出来，
# 用最小 shim 编译运行，验证"SPS/PPS 抠取 + 每帧前缀拼装"。
#
# 设计要点：测试的是【源码里的真实函数文本】（按大括号配平从源文件里切出来），
# 不是重写一份等价实现 —— 否则测的就不是真正会跑的那份代码了。
#
# 用法：./test/fixkey-ps-test.sh [qq-nvenc.c 路径]
#   默认路径：/tmp/pr28-build/src/qq-nvenc.c（PR #28 的 worktree）

set -uo pipefail
SRC="${1:-/tmp/pr28-build/src/qq-nvenc.c}"
[ -f "$SRC" ] || { echo "找不到源文件: $SRC" >&2; exit 1; }

TMP=$(mktemp -d /tmp/fixkey-ps-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

# ---- 从源码里按大括号配平切出一个函数 ----
extract() {
	local name="$1" out="$2"
	awk -v fn="$name" '
		index($0, "static int " fn "(") == 1 { grab = 1 }
		grab {
			print
			n = gsub(/\{/, "{"); m = gsub(/\}/, "}")
			depth += n - m
			if (started == 0 && n > 0) started = 1
			if (started && depth <= 0) exit
		}
	' "$SRC" >"$out"
	[ -s "$out" ] || { echo "切不出函数 $name" >&2; exit 1; }
}

extract fixkey_grab_ps "$TMP/grab.c"
extract fixkey_build "$TMP/build.c"

{
	cat <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct wrapped {
	unsigned char *sps_pps;
	uint32_t sps_pps_len;
	unsigned char *fixbuf;
	uint32_t fix_cap;
};
#define LOG(...) do { } while (0)
EOF
	cat "$TMP/grab.c" "$TMP/build.c"
	cat <<'EOF'

static int fails, cases;
static void ck(int cond, const char *what)
{
	cases++;
	if (cond) { printf("  ✓ %s\n", what); }
	else { printf("  ✗ %s\n", what); fails++; }
}

/* 构造一段 Annex-B 码流：可选起始码长度、SPS/PPS/slice */
static size_t mk(unsigned char *b, int sc, int with_ps, int with_slice)
{
	size_t n = 0;
	if (with_ps) {
		if (sc == 4) { memset(b + n, 0, 3); b[n + 3] = 1; n += 4; }
		else { b[n] = 0; b[n + 1] = 0; b[n + 2] = 1; n += 3; }
		b[n++] = 0x67; b[n++] = 0x42; b[n++] = 0x00; b[n++] = 0x1e; b[n++] = 0xaa; b[n++] = 0xbb;
		if (sc == 4) { memset(b + n, 0, 3); b[n + 3] = 1; n += 4; }
		else { b[n] = 0; b[n + 1] = 0; b[n + 2] = 1; n += 3; }
		b[n++] = 0x68; b[n++] = 0xce; b[n++] = 0x3c; b[n++] = 0x80;
	}
	if (with_slice) {
		if (sc == 4) { memset(b + n, 0, 3); b[n + 3] = 1; n += 4; }
		else { b[n] = 0; b[n + 1] = 0; b[n + 2] = 1; n += 3; }
		b[n++] = 0x61; b[n++] = 0xe0; b[n++] = 0x11; b[n++] = 0x22;
	}
	return n;
}

int main(void)
{
	unsigned char bs[256];
	size_t n;
	struct wrapped w;
	void *ptr; uint32_t sz;

	printf("fixkey_grab_ps / fixkey_build 单测（源码切出，非重写）\n");

	/* A：4 字节起始码的 IDR（带 SPS/PPS）→ 应抠到 */
	memset(&w, 0, sizeof w);
	n = mk(bs, 4, 1, 1);
	ck(fixkey_grab_ps(&w, bs, (uint32_t)n) == 1 && w.sps_pps_len > 0, "A 4字节起始码：抠到 SPS/PPS");
	/* 期望长度：SPS 的起始码起，到 PPS 之后第一个 NAL 的起始码之前。
	 * mk() 布局 = 4+6(SPS)+4+4(PPS)+4(slice) → 抠出 18 字节。 */
	ck(w.sps_pps_len == 18, "A 抠到的长度 = SPS+PPS 全段（18 字节）");
	{
		int seen_sps = 0, seen_pps = 0;
		uint32_t k;
		for (k = 0; k + 4 <= w.sps_pps_len; k++) {   /* 按起始码扫 NAL 类型，不写死下标 */
			uint32_t sc2 = 0, nal;
			if (w.sps_pps[k] == 0 && w.sps_pps[k + 1] == 0 && w.sps_pps[k + 2] == 1) sc2 = 3;
			else if (w.sps_pps[k] == 0 && w.sps_pps[k + 1] == 0 && w.sps_pps[k + 2] == 0 &&
			         w.sps_pps[k + 3] == 1) sc2 = 4;
			if (!sc2) continue;
			nal = w.sps_pps[k + sc2] & 0x1f;
			if (nal == 7) seen_sps = 1;
			if (nal == 8) seen_pps = 1;
			k += sc2;
		}
		ck(seen_sps && seen_pps, "A 内容含 SPS(7) 与 PPS(8) 两种 NAL");
		ck(w.sps_pps[4] == 0x67, "A 第一个 NAL 是 SPS（0x67）");
	}

	/* B：已有缓存 → 不重复抠 */
	ck(fixkey_grab_ps(&w, bs, (uint32_t)n) == 0, "B 已有缓存：不重复抠取");

	/* C：3 字节起始码 */
	memset(&w, 0, sizeof w);
	n = mk(bs, 3, 1, 1);
	ck(fixkey_grab_ps(&w, bs, (uint32_t)n) == 1, "C 3字节起始码：抠到 SPS/PPS");

	/* D：没有参数集（纯 P 帧）→ 不抠 */
	memset(&w, 0, sizeof w);
	n = mk(bs, 4, 0, 1);
	ck(fixkey_grab_ps(&w, bs, (uint32_t)n) == 0, "D 纯 P 帧：不抠（返回 0）");

	/* E：垃圾输入不崩 */
	memset(&w, 0, sizeof w);
	memset(bs, 0xa5, sizeof bs);
	ck(fixkey_grab_ps(&w, bs, sizeof bs) == 0, "E 垃圾输入：安全返回 0");

	/* F：有缓存 + P 帧 → 拼前缀 */
	memset(&w, 0, sizeof w);
	n = mk(bs, 4, 1, 1);
	(void)fixkey_grab_ps(&w, bs, (uint32_t)n);
	{
		unsigned char pf[64];
		size_t pn = mk(pf, 4, 0, 1);          /* 只有 slice，没参数集 */
		ptr = pf; sz = (uint32_t)pn;
		ck(fixkey_build(&w, pf, (uint32_t)pn, &ptr, &sz) == 0, "F 拼装返回 0");
		ck(sz == pn + w.sps_pps_len, "F 输出长度 = 参数集 + 原码流");
		ck(memcmp(ptr, w.sps_pps, w.sps_pps_len) == 0, "F 输出以缓存的 SPS/PPS 开头");
		ck(memcmp((unsigned char *)ptr + w.sps_pps_len, pf, pn) == 0, "F 原码流原样跟在后面");
	}

	/* G：帧本身已带参数集 → 不重复补 */
	{
		unsigned char full[256];
		size_t fn = mk(full, 4, 1, 1);
		ptr = full; sz = (uint32_t)fn;
		ck(fixkey_build(&w, full, (uint32_t)fn, &ptr, &sz) == 0, "G 已带参数集：拼装返回 0");
		ck(sz == fn, "G 输出长度不变（不重复补）");
	}

	/* H：大帧容量增长不越界 */
	{
		unsigned char big[70000];
		size_t bn = mk(big, 4, 0, 1);
		memset(big + bn, 0x5a, sizeof big - bn);
		bn = sizeof big;
		ptr = big; sz = (uint32_t)bn;
		ck(fixkey_build(&w, big, (uint32_t)bn, &ptr, &sz) == 0 && sz == bn + w.sps_pps_len,
		   "H 大帧（70KB）：容量增长正确、不越界");
	}

	free(w.sps_pps);
	free(w.fixbuf);
	printf("%s：%d 项，失败 %d\n", fails ? "失败" : "全部通过", cases, fails);
	return fails ? 1 : 0;
}
EOF
} >"$TMP/test.c"

gcc -O1 -Wall -Wextra -o "$TMP/test" "$TMP/test.c" 2>&1 | head -20
[ -x "$TMP/test" ] || { echo "编译失败" >&2; exit 1; }
"$TMP/test"
