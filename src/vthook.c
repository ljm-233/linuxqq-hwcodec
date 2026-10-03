/*
 * vthook.c -- Phase 2：观测 COM 接口的虚表调用，找出"编码器选择点"
 *
 * 为什么是这个做法（Phase 1 结论）：
 *   broadcast-core.so 只导出 DllGetClassObject / DllCanUnloadNow / RegisterLog，
 *   BroadcastCore_EnumEncoderDevice / IsSupportedHardware / SwitchEncoderDevice 这些名字
 *   只在字符串表里 —— 它们是 COM 接口的**方法名**，调用走虚表，按名字 hook 不到。
 *   所以只能：① 在 DllGetClassObject 返回的 IClassFactory 上放代理（5 个槽，签名已知，最安全）
 *            ② 在 CreateInstance 返回的对象上换掉它的虚表指针，每个槽指向我们生成的桩
 *            ③ 桩只做两件事：记录"谁被调用了"，然后**原样转发**给真实实现
 *
 * 为什么不用"搬栈"的常见写法（把参数寄存器压在栈上再调真实函数）：
 *   那样会把 rsp 挪到调用者的栈参数之下 —— 一旦方法有 7 个以上整数参数（参数走栈），
 *   真实实现读到的就是错位的数据，轻则返回值错、重则崩。QQ 崩了用户只会怪我们。
 *   这里的做法是：
 *     - 参数寄存器与 xmm0-7 全部存进 **TLS**（线程局部，不在栈上，被调函数碰不到）
 *     - 记完日志后把寄存器原样恢复
 *     - 调用真实方法时 rsp 恢复成"调用者直接 call 它"时的值（R+8），
 *       所以**栈参数位置与直接调用完全一致**；调用者的返回地址先存进 TLS 再还原
 *     - 返回值（rax/rdx/xmm0/xmm1）也经 TLS 取回并原样返回给调用者
 *   代价是这段汇编麻烦一点，换来的是"无论方法有多少个参数都不会错位"。
 *
 * 安全阀：默认**不启用**（要 HWPROBE_VTABLE=1）。任何一步判断不了（对象内存不可写、
 * 内存不足、表用尽）就原样返回真实对象，只在日志里写一行跳过原因，绝不崩。
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "hwprobe.h"

#if !defined(__x86_64__)
#error "vthook.c 目前只实现了 x86_64 的转发桩（本机目标平台）"
#endif

#define VT_SLOTS      512                 /* 虚表槽位上限：真实接口不可能有这么多方法 */
#define VT_STUB_BYTES 32                  /* 每个桩占 32 字节（19 有效，便于对齐） */
#define VT_REAL_IDX   VT_SLOTS            /* 表尾一格存"真实虚表指针" */
#define VT_MAX_TABLES 64

/* ---------------- TLS：寄存器暂存区（偏移与下面汇编里的数字一一对应） ---------------- */

struct vt_tls {
    uint64_t slot;                  /* 0   本次调用的槽位序号 */
    uint64_t rdi, rsi, rdx;         /* 8, 16, 24 */
    uint64_t rcx, r8, r9;           /* 32, 40, 48 */
    uint64_t rax;                   /* 56  原始 rax（vtable 方法不用它，但保住更稳） */
    uint64_t ret_rax, ret_rdx;      /* 64, 72  真实方法的返回值 */
    unsigned char xmm[8][16];       /* 80 .. 207   xmm0-7 */
    unsigned char ret_xmm[2][16];   /* 208, 224    xmm0/xmm1 返回值 */
    uint64_t retaddr;               /* 240  调用者的返回地址 */
} __attribute__((aligned(16)));

static __thread struct vt_tls vt_tls __attribute__((tls_model("initial-exec"), used));

_Static_assert(offsetof(struct vt_tls, slot) == 0, "TLS 布局变了，汇编里的偏移要跟着改");
_Static_assert(offsetof(struct vt_tls, rdi) == 8, "TLS 布局变了");
_Static_assert(offsetof(struct vt_tls, rsi) == 16, "TLS 布局变了");
_Static_assert(offsetof(struct vt_tls, rdx) == 24, "TLS 布局变了");
_Static_assert(offsetof(struct vt_tls, rcx) == 32, "TLS 布局变了");
_Static_assert(offsetof(struct vt_tls, r8) == 40, "TLS 布局变了");
_Static_assert(offsetof(struct vt_tls, r9) == 48, "TLS 布局变了");
_Static_assert(offsetof(struct vt_tls, rax) == 56, "TLS 布局变了");
_Static_assert(offsetof(struct vt_tls, ret_rax) == 64, "TLS 布局变了");
_Static_assert(offsetof(struct vt_tls, ret_rdx) == 72, "TLS 布局变了");
_Static_assert(offsetof(struct vt_tls, xmm) == 80, "TLS 布局变了");
_Static_assert(offsetof(struct vt_tls, ret_xmm) == 208, "TLS 布局变了");
_Static_assert(offsetof(struct vt_tls, retaddr) == 240, "TLS 布局变了");

/* ---------------- 热路径日志：不用 stdio、不用 malloc，一次 write ---------------- */

static int vt_on;                     /* HWPROBE_VTABLE=1 才启用 */
static long vt_log_left = 2000;       /* HWPROBE_VTABLE_MAXLOG 可改；用完只计数不打印 */
static uint64_t slot_calls[VT_SLOTS];
static unsigned long n_entered, n_leave_calls, n_hooked, n_skipped;

static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static char *put_hex(char *p, uint64_t v)
{
    static const char d[] = "0123456789abcdef";
    int i;

    *p++ = '0';
    *p++ = 'x';
    for (i = 15; i >= 0; i--)
        *p++ = d[(v >> (i * 4)) & 0xf];
    return p;
}

static char *put_dec(char *p, uint64_t v)
{
    char tmp[24];
    int n = 0;

    if (!v) {
        *p++ = '0';
        return p;
    }
    while (v && n < (int)sizeof tmp) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n)
        *p++ = tmp[--n];
    return p;
}

static char *put_time(char *p)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    p = put_dec(p, (uint64_t)ts.tv_sec);
    *p++ = '.';
    p = put_dec(p, (uint64_t)(ts.tv_nsec / 1000));
    return p;
}

static void vt_emit(const char *tag, uint32_t slot, uint64_t a, uint64_t b, int nargs)
{
    char buf[256];
    char *p = buf;
    int fd = hwprobe_logfd();

    if (fd < 0)
        return;
    p = put_str(p, "[hwprobe] ");
    p = put_time(p);
    p = put_str(p, " vtable[");
    p = put_dec(p, slot);
    p = put_str(p, "] ");
    p = put_str(p, tag);
    p = put_str(p, " this=");
    p = put_hex(p, a);
    if (nargs > 1) {
        p = put_str(p, " a2=");
        p = put_hex(p, b);
    }
    *p++ = '\n';
    if (write(fd, buf, (size_t)(p - buf)) < 0) {
        /* 日志失败不影响宿主 */
    }
}

/* 进入：记录槽位与 this（以及第二个参数，能安全判断就是整数/指针） */
void hwprobe_vt_enter(uint32_t slot, void *obj, void *a2, void *a3)
{
    if (slot < VT_SLOTS)
        __atomic_fetch_add(&slot_calls[slot], 1, __ATOMIC_RELAXED);
    n_entered++;
    if (vt_log_left <= 0)
        return;
    vt_log_left--;
    vt_emit("call", slot, (uint64_t)(uintptr_t)obj, (uint64_t)(uintptr_t)a2, 2);
    (void)a3;
}

/* 返回：记录 rax/rdx（bool / HRESULT / int 都在 rax 里） */
void hwprobe_vt_leave(uint32_t slot, uint64_t ret, uint64_t ret2)
{
    n_leave_calls++;
    if (vt_log_left <= 0)
        return;
    vt_log_left--;
    vt_emit("ret ", slot, ret, ret2, 2);
}

/* ---------------- 桩（运行时生成机器码） ----------------
 * 每个桩 19 字节有效（补到 32）：
 *   41 ba <slot imm32>              mov $slot, %r10d        （r10 易失，不承载参数）
 *   49 bb <vt_dispatch imm64>       movabs $vt_dispatch, %r11
 *   41 ff e3                        jmp *%r11
 * 用 movabs 而不是 jmp rel32：桩页是 mmap 出来的，和 .so 可能隔着 2GB 以上，
 * rel32 会溢出跳飞（这正是第一版测试里段错误的原因之一）。
 */
extern void vt_dispatch(void);

static unsigned char *stub_page;

static void *stub_addr(int i)
{
    return stub_page + (size_t)i * VT_STUB_BYTES;
}

static int emit_stubs(void)
{
    long pg = sysconf(_SC_PAGESIZE);
    size_t need = (size_t)VT_SLOTS * VT_STUB_BYTES;
    size_t sz;
    unsigned char *mem;
    int i;

    if (pg <= 0)
        pg = 4096;
    sz = ((need + (size_t)pg - 1) / (size_t)pg) * (size_t)pg;
    mem = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return 0;
    memset(mem, 0xcc, sz); /* 万一跳飞，int3 比乱跑好定位 */
    for (i = 0; i < VT_SLOTS; i++) {
        unsigned char *s = mem + (size_t)i * VT_STUB_BYTES;
        uint64_t target = (uint64_t)(uintptr_t)&vt_dispatch;

        s[0] = 0x41;                 /* REX.B */
        s[1] = 0xba;                 /* mov r10d, imm32 */
        memcpy(s + 2, &i, 4);
        s[6] = 0x49;                 /* REX.WB */
        s[7] = 0xbb;                 /* movabs imm64, %r11 */
        memcpy(s + 8, &target, 8);
        s[16] = 0x41;                /* REX.B */
        s[17] = 0xff;                /* jmp *%r11 */
        s[18] = 0xe3;
    }
    __builtin___clear_cache((char *)mem, (char *)mem + sz);
    if (mprotect(mem, sz, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, sz);
        return 0;
    }
    stub_page = mem;
    return 1;
}

/*
 * 转发桩的统一入口。
 * 进入时：r10 = 槽位，rsp = R（调用者 call 桩时压了返回地址，故 R%16 == 8），
 *         参数在 rdi/rsi/rdx/rcx/r8/r9 + xmm0-7，栈参数在 [R+8] 往上（属于调用者）。
 * 调用真实方法时让 rsp = R+8（= 调用者直接 call 时的 rsp），于是栈参数位置与直接调用一致。
 */
__attribute__((naked, noinline, used)) void vt_dispatch(void)
{
    __asm__ __volatile__(
        /* 先保存原始 rax 与调用者的返回地址（这两个动作只碰 r11/rax） */
        "movq vt_tls@gottpoff(%rip), %r11\n\t"
        "movq %rax, %fs:56(%r11)\n\t"
        "movq (%rsp), %rax\n\t"
        "movq %rax, %fs:240(%r11)\n\t"
        /* 槽位与参数寄存器进 TLS（不在栈上，被调函数碰不到） */
        "movq %r10, %fs:0(%r11)\n\t"
        "movq %rdi, %fs:8(%r11)\n\t"
        "movq %rsi, %fs:16(%r11)\n\t"
        "movq %rdx, %fs:24(%r11)\n\t"
        "movq %rcx, %fs:32(%r11)\n\t"
        "movq %r8,  %fs:40(%r11)\n\t"
        "movq %r9,  %fs:48(%r11)\n\t"
        "movups %xmm0, %fs:80(%r11)\n\t"
        "movups %xmm1, %fs:96(%r11)\n\t"
        "movups %xmm2, %fs:112(%r11)\n\t"
        "movups %xmm3, %fs:128(%r11)\n\t"
        "movups %xmm4, %fs:144(%r11)\n\t"
        "movups %xmm5, %fs:160(%r11)\n\t"
        "movups %xmm6, %fs:176(%r11)\n\t"
        "movups %xmm7, %fs:192(%r11)\n\t"
        /* 记录"进入了哪个槽" */
        "subq $8, %rsp\n\t"
        "movq %fs:0(%r11), %rdi\n\t"
        "movq %fs:8(%r11), %rsi\n\t"
        "movq %fs:16(%r11), %rdx\n\t"
        "movq %fs:24(%r11), %rcx\n\t"
        "call hwprobe_vt_enter\n\t"
        "addq $8, %rsp\n\t"
        /* 恢复参数寄存器 */
        "movq vt_tls@gottpoff(%rip), %r11\n\t"
        "movq %fs:8(%r11), %rdi\n\t"
        "movq %fs:16(%r11), %rsi\n\t"
        "movq %fs:24(%r11), %rdx\n\t"
        "movq %fs:32(%r11), %rcx\n\t"
        "movq %fs:40(%r11), %r8\n\t"
        "movq %fs:48(%r11), %r9\n\t"
        "movups %fs:80(%r11), %xmm0\n\t"
        "movups %fs:96(%r11), %xmm1\n\t"
        "movups %fs:112(%r11), %xmm2\n\t"
        "movups %fs:128(%r11), %xmm3\n\t"
        "movups %fs:144(%r11), %xmm4\n\t"
        "movups %fs:160(%r11), %xmm5\n\t"
        "movups %fs:176(%r11), %xmm6\n\t"
        "movups %fs:192(%r11), %xmm7\n\t"
        /* 真实函数 = ((void**)this)[VT_SLOTS][slot] */
        "movq (%rdi), %rax\n\t"
        "movq 4096(%rax), %rax\n\t"
        "movq %fs:0(%r11), %r10\n\t"
        "movq (%rax,%r10,8), %rax\n\t"
        /* rsp 回到"调用者直接 call"时的值，栈参数位置才正确 */
        "addq $8, %rsp\n\t"
        "call *%rax\n\t"
        "subq $8, %rsp\n\t"
        /* 保存返回值 */
        "movq vt_tls@gottpoff(%rip), %r11\n\t"
        "movq %rax, %fs:64(%r11)\n\t"
        "movq %rdx, %fs:72(%r11)\n\t"
        "movups %xmm0, %fs:208(%r11)\n\t"
        "movups %xmm1, %fs:224(%r11)\n\t"
        /* 记录返回值 */
        "subq $8, %rsp\n\t"
        "movq %fs:0(%r11), %rdi\n\t"
        "movq %fs:64(%r11), %rsi\n\t"
        "movq %fs:72(%r11), %rdx\n\t"
        "call hwprobe_vt_leave\n\t"
        "addq $8, %rsp\n\t"
        /* 还原调用者的返回地址与返回值，交还给调用者 */
        "movq vt_tls@gottpoff(%rip), %r11\n\t"
        "movq %fs:240(%r11), %rax\n\t"
        "movq %rax, (%rsp)\n\t"
        "movq %fs:64(%r11), %rax\n\t"
        "movq %fs:72(%r11), %rdx\n\t"
        "movups %fs:208(%r11), %xmm0\n\t"
        "movups %fs:224(%r11), %xmm1\n\t"
        "ret\n\t");
}

/* ---------------- 对象内存可写性：不能往只读页写虚表指针 ---------------- */

static int range_writable(uintptr_t a, size_t len)
{
    FILE *f = fopen("/proc/self/maps", "re");
    char line[512];
    int ok = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perms[8];

        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3)
            continue;
        if (a >= lo && a + len <= hi) {
            ok = (perms[1] == 'w');
            break;
        }
    }
    fclose(f);
    return ok;
}

/* ---------------- 安装/统计 ---------------- */

static void *tables[VT_MAX_TABLES];
static int n_tables;
static int vt_lock;

void vthook_install(void *obj, const char *why)
{
    void **real_vt, **t;
    int i;

    if (!vt_on || !obj)
        return;
    if (!stub_page && !emit_stubs()) {
        n_skipped++;
        hwprobe_plog("vtable hook 跳过：桩页分配失败");
        return;
    }
    if (!range_writable((uintptr_t)obj, sizeof(void *))) {
        n_skipped++;
        hwprobe_plog("vtable hook 跳过：对象 %p 所在内存不可写（只读页）", obj);
        return;
    }
    real_vt = *(void ***)obj;
    if (!real_vt) {
        n_skipped++;
        hwprobe_plog("vtable hook 跳过：对象 %p 的第一个字段是空指针", obj);
        return;
    }
    for (i = 0; i < n_tables; i++)
        if (tables[i] == real_vt)
            return; /* 同一个接口对象已经挂过 */

    while (__atomic_exchange_n(&vt_lock, 1, __ATOMIC_ACQUIRE))
        ; /* 极短的临界区 */
    if (n_tables >= VT_MAX_TABLES) {
        __atomic_store_n(&vt_lock, 0, __ATOMIC_RELEASE);
        n_skipped++;
        hwprobe_plog("vtable hook 跳过：已挂 %d 个接口，表用尽", n_tables);
        return;
    }
    t = malloc((VT_SLOTS + 1) * sizeof(void *));
    if (!t) {
        __atomic_store_n(&vt_lock, 0, __ATOMIC_RELEASE);
        n_skipped++;
        hwprobe_plog("vtable hook 跳过：内存不足");
        return;
    }
    for (i = 0; i < VT_SLOTS; i++)
        t[i] = stub_addr(i);
    t[VT_REAL_IDX] = real_vt;
    *(void ***)obj = t;
    tables[n_tables++] = real_vt;
    __atomic_store_n(&vt_lock, 0, __ATOMIC_RELEASE);

    n_hooked++;
    hwprobe_plog("vtable 已挂钩：obj=%p 原 vtable=%p（%s）— 之后每次方法调用都会记一行 vtable[槽位]", obj,
                 (void *)real_vt, why);
}

/* ---------------- IClassFactory 代理：5 个槽，签名已知，纯 C ---------------- */

typedef struct {
    void *vt;
    void *real;
} fac_proxy;

static fac_proxy fac_pool[8];
static int fac_used;

#define FAC_QI(self, riid, ppv)      ((long (*)(void *, const void *, void **))(*(void ***)((fac_proxy *)(self))->real)[0])(((fac_proxy *)(self))->real, (riid), (ppv))
#define FAC_ADDREF(self)             ((unsigned long (*)(void *))(*(void ***)((fac_proxy *)(self))->real)[1])(((fac_proxy *)(self))->real)
#define FAC_RELEASE(self)            ((unsigned long (*)(void *))(*(void ***)((fac_proxy *)(self))->real)[2])(((fac_proxy *)(self))->real)

static long fac_qi(void *self, const void *riid, void **ppv)
{
    long r = FAC_QI(self, riid, ppv);
    char gs[64] = "?";

    if (riid)
        hwprobe_vt_guid(riid, gs, sizeof gs);
    hwprobe_plog("IClassFactory::QueryInterface(iid=%s) -> 0x%lx, iface=%p", gs, r,
                 (ppv && r == 0) ? *ppv : NULL);
    if (ppv && r == 0 && *ppv && *ppv != ((fac_proxy *)self)->real)
        hwprobe_plog("  （返回的是别的接口指针，它不在代理里，所以它的方法调用看不到）");
    return r;
}

static unsigned long fac_addref(void *self)
{
    return FAC_ADDREF(self);
}

static unsigned long fac_release(void *self)
{
    return FAC_RELEASE(self);
}

static long fac_create(void *self, void *outer, const void *riid, void **ppv)
{
    long r = ((long (*)(void *, void *, const void *, void **))(*(void ***)((fac_proxy *)self)->real)[3])(
        ((fac_proxy *)self)->real, outer, riid, ppv);
    char gs[64] = "?";

    if (riid)
        hwprobe_vt_guid(riid, gs, sizeof gs);
    hwprobe_plog("IClassFactory::CreateInstance(iid=%s) -> 0x%lx, obj=%p", gs, r,
                 (ppv && r == 0) ? *ppv : NULL);
    if (r == 0 && ppv && *ppv)
        vthook_install(*ppv, "CreateInstance 返回的接口");
    return r;
}

static long fac_lock(void *self, int lock)
{
    return ((long (*)(void *, int))(*(void ***)((fac_proxy *)self)->real)[4])(((fac_proxy *)self)->real, lock);
}

static void *fac_vtbl[5];

void vthook_wrap_factory(void **ppv_factory)
{
    fac_proxy *p;

    if (!vt_on || !ppv_factory || !*ppv_factory)
        return;
    if (fac_used >= (int)(sizeof fac_pool / sizeof fac_pool[0])) {
        hwprobe_plog("工厂代理池已满（%d 个），这个工厂不代理（功能不受影响）", fac_used);
        return;
    }
    p = &fac_pool[fac_used++];
    p->vt = fac_vtbl;
    p->real = *ppv_factory;
    *ppv_factory = p;
    hwprobe_plog("已代理 IClassFactory：真实 %p -> 代理 %p（CreateInstance 会被观测）", p->real, (void *)p);
}

/* ---------------- 汇总 ---------------- */

void vthook_summary(void)
{
    uint64_t counts[VT_SLOTS];
    int i, k;

    if (!vt_on) {
        hwprobe_plog("COM 虚表观测：未启用（HWPROBE_VTABLE=1 可开）");
        return;
    }
    hwprobe_plog("--- COM 虚表观测 ---");
    hwprobe_plog("已挂钩接口 %lu 个；跳过 %lu 个；方法调用进入 %lu 次，记录返回值 %lu 次",
                 n_hooked, n_skipped, n_entered, n_leave_calls);
    memcpy(counts, slot_calls, sizeof counts);
    hwprobe_plog("vtable 方法调用次数最多的前 5 个槽位：");
    for (k = 0; k < 5; k++) {
        int best = -1;
        uint64_t bestv = 0;

        for (i = 0; i < VT_SLOTS; i++) {
            if (counts[i] > bestv) {
                bestv = counts[i];
                best = i;
            }
        }
        if (best < 0 || bestv == 0)
            break;
        hwprobe_plog("    vtable[%d]  调用 %llu 次", best, (unsigned long long)bestv);
        counts[best] = 0;
    }
    hwprobe_plog("判读：找那种只被调用一两次、返回值是 0x0 / 0x1 的槽位 —— 它很可能就是"
                 "『硬件编码是否受支持』的判断点，返回值决定后面走 NVENC 还是 OpenH264。");
}

/* ---------------- 初始化 ---------------- */

void vthook_init(void)
{
    const char *e = getenv("HWPROBE_VTABLE");
    const char *m;

    if (e && *e && strcmp(e, "0") != 0)
        vt_on = 1;
    m = getenv("HWPROBE_VTABLE_MAXLOG");
    if (m && *m) {
        long v = strtol(m, NULL, 10);
        if (v >= 0)
            vt_log_left = v;
    }
    fac_vtbl[0] = (void *)fac_qi;
    fac_vtbl[1] = (void *)fac_addref;
    fac_vtbl[2] = (void *)fac_release;
    fac_vtbl[3] = (void *)fac_create;
    fac_vtbl[4] = (void *)fac_lock;

    hwprobe_plog("COM 虚表观测：%s（HWPROBE_VTABLE=%s，日志上限 %ld 行）",
                 vt_on ? "已启用" : "未启用（默认）", e ? e : "(未设)", vt_log_left);
}

/* guid -> 字符串（给 hwprobe.c 的 DllGetClassObject 日志复用） */
void hwprobe_vt_guid(const void *g, char *out, size_t n)
{
    const unsigned char *b = g;

    if (!g) {
        snprintf(out, n, "?");
        return;
    }
    snprintf(out, n, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13],
             b[14], b[15]);
}
