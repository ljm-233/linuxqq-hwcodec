/*
 * hwprobe -- LD_PRELOAD 探针：看清 LinuxQQ 屏幕共享到底用哪条编码路径
 *
 * 为什么是这几个 hook 点（本机静态调查结论，2026-10-03）：
 *   - broadcast-core.so 只导出 3 个符号（DllGetClassObject / DllCanUnloadNow / RegisterLog），
 *     它是个 COM 风格的插件；NvEncoder_* / OpenH264Encoder_* / AmfEncoder_* 这些名字
 *     只在字符串表里（内部 vtable），**不是动态符号，无法按名字 hook**
 *   - 所以能做到的观测面是：
 *       1) dlopen：它明确会 dlopen libnvidia-encode.so / libcuda.so / libnvcuvid.so /
 *          libmfx.so.1 / libamfrt64.so.1 / libopenh264.so —— 加载成功与否直接区分后端
 *       2) dlsym：看它有没有去取 NVENC 的入口（NvEncodeAPICreateInstance 等）
 *       3) DllGetClassObject：broadcast-core 接口何时被创建（唯一可 hook 的导出符号）
 *       4) NvEncodeAPICreateInstance / NvEncodeAPIGetMaxSupportedVersion：NVENC 是否真的被初始化
 *       5) 退出时 dump 自己映射了哪些编码库（这是"走了哪条路"的直接答案）
 *
 * 递归安全：真实函数一律用**未被包装**的 dlvsym(RTLD_NEXT, ...) 取，日志用 write()，
 * 不用 stdio、不在 hook 里 malloc。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <link.h>      /* dl_iterate_phdr：dlvsym 引导用（不依赖任何被包装的函数） */
#include <elf.h>

#include "hwprobe.h"

static int logfd = 2;                 /* 默认 stderr；HWPROBE_LOG 可改 */
static int log_all = 0;               /* HWPROBE_ALL=1 时记录所有 dlsym */
static int period_secs = 0;            /* HWPROBE_PERIOD：秒；0 = 不周期汇总 */
static long last_period_mono = 0;      /* 上次惰性周期摘要的单调时钟秒数 */
static void maybe_periodic_summary(void);            /* HWPROBE_PERIOD：秒；0 = 不周期汇总 */

/* 本进程 dlopen 过的库名（去重，环形）。2026-10-03：主进程是长命的，只在退出时
   才写汇总 —— 而"broadcast-core 到底在哪个进程、什么时候被加载"必须能当场看见。 */
#define HWPROBE_RING 48
static char dl_names[HWPROBE_RING][64];
static int dl_names_n;
static int in_boot = 0;               /* dlvsym 引导期间的重入保护 */

static unsigned long n_dlopen, n_dlopen_fail, n_dlsym, n_cls, n_nvenc_api, n_nvenc_ver;
/* 经 dlsym 把我们的实现交出去的次数：用来分辨"调用方是 dlsym 拿的"还是"动态链接器直接解析到我们" */
static unsigned long n_gave_cls, n_gave_nvenc_api, n_gave_nvenc_ver;
static int path_cls_logged, path_api_logged, path_ver_logged;
static int seen_nvenc, seen_cuda, seen_cuvid, seen_mfx, seen_amf, seen_openh264, seen_vpx, seen_x264, seen_avcodec;

static void *(*real_dlopen)(const char *, int);
static void *(*real_dlmopen)(long, const char *, int);
static void *(*real_dlsym)(void *, const char *);
static char *(*real_dlerror)(void);
static void *(*real_dlvsym)(void *, const char *, const char *);
static int in_dlvsym;                 /* dlvsym 包装的重入保护 */
/* 我们**自己**解析符号时置位：这类调用绝不能走 route_special。
   2026-10-03：包装 dlvsym 后，hwprobe_lookup_real/next_definition 里的 dlvsym(...) 会
   落进我们自己的包装 —— 目标名字恰好是 DllGetClassObject 时，route_special 会把它当成
   "调用方来取符号"，把 cls_handle 覆盖成 RTLD_NEXT，真实实现再也找不到（测试 A/B/C 全挂）。 */
static int internal_lookup;
static int dlvsym_fallback_logged;    /* "真实 dlvsym 取不到，退化成 dlsym"只记一次 */
static unsigned long n_dlvsym;        /* 走 dlvsym 路径的调用次数 */
static int path_dlvsym_logged;        /* 目标符号经 dlvsym 被取走，只记一次 */

/* 给 vthook.c 的访问器（日志 fd 与"未被包装"的真实函数） */
int hwprobe_logfd(void)
{
    return logfd;
}

void *hwprobe_real_dlopen(const char *file, int flags)
{
    return real_dlopen ? real_dlopen(file, flags) : NULL;
}

void *hwprobe_real_dlsym(void *handle, const char *name)
{
    return real_dlsym ? real_dlsym(handle, name) : NULL;
}

static void *cls_handle;   /* dlsym 请求 DllGetClassObject 时用的那个 handle */

static char log_path[512];    /* HWPROBE_LOG 的副本：fd 被关掉时可以重开 */
static int log_reopened = 0;  /* 每个进程只说明一次 */

/* ---------- 工具 ---------- */

/*
 * 另写一份到 stderr（fd 2）。
 * 修复版启动器把 QQ 的 stderr 重定向到它自己的日志（/run/user/1000/linuxqq-wayland-fix.log），
 * 这条通道不受 Chromium 关 fd 的影响，用来回答"构造函数到底跑没跑"。
 */
static void plog_stderr(const char *fmt, ...)
{
    char buf[400];
    int n;
    va_list ap;

    n = snprintf(buf, sizeof buf, "[hwprobe/err] ");
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2)
        n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    (void)!write(2, buf, n);
}

void hwprobe_plog(const char *fmt, ...)
{
    char buf[600];
    int n;
    struct timespec ts;
    va_list ap;

    if (logfd < 0)
        return;
    /*
     * 收帧进程（--type=ppapi）是 Electron 从主进程 **fork** 出来的：构造函数不会再跑，
     * 而 Chromium 会关掉它不认识的文件描述符 —— 继承来的 logfd 就这么没了，日志全部丢失。
     * （2026-10-03 实测：ppapi 进程里探针在、HWPROBE_LOG 也设了，却一行没写，
     *   fd 表里也找不到那个日志文件。）所以每次写之前确认 fd 还活着，不活就重开。
     */
    if (fcntl(logfd, F_GETFD) < 0) {
        int fd = log_path[0] ? open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
        if (fd < 0)
            return;
        logfd = fd;
        if (!log_reopened) {
            log_reopened = 1;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            n = snprintf(buf, sizeof buf,
                         "[hwprobe] %ld.%06ld 日志 fd 被继承后关闭，已重新打开（本进程 pid=%d "
                         "多半是从主进程 fork 出来的）\n",
                         (long)ts.tv_sec, ts.tv_nsec / 1000, (int)getpid());
            (void)!write(logfd, buf, n);
            plog_stderr("日志 fd 已重开 pid=%d（fork 出来的子进程）", (int)getpid());
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    n = snprintf(buf, sizeof buf, "[hwprobe] %ld.%06ld ", (long)ts.tv_sec, ts.tv_nsec / 1000);
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2)
        n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    if (write(logfd, buf, n) < 0) {
        /* 日志失败也不该影响宿主进程 */
    }
}

/* 我们自己的符号解析：置 internal_lookup，避免被自己的 dlvsym 包装误当成"调用方取符号" */
static void *internal_dlvsym(void *handle, const char *name, const char *version)
{
    void *p;

    internal_lookup = 1;
    p = real_dlvsym ? real_dlvsym(handle, name, version) : dlvsym(handle, name, version);
    internal_lookup = 0;
    return p;
}

/* 用没有被包装的 dlvsym 取真实函数，避免递归进我们自己的 dlsym */
void *hwprobe_lookup_real(const char *name)
{
    /*
     * 版本列表必须够全：dlmopen 的标签是 GLIBC_2.3.4，之前不在列表里 —— 于是真实
     * dlmopen 取不到、我们的包装对每次调用都返回 NULL，把用 dlmopen 隔离命名空间的
     * 程序（实测：eglinfo 的 EGL 探测整段失败）直接搞坏。最后再用"不指定版本"兜底。
     */
    static const char *vers[] = { "GLIBC_2.2.5", "GLIBC_2.34", "GLIBC_2.17", "GLIBC_2.3.4",
                                  "GLIBC_2.35", NULL };
    void *p = NULL;
    int i;

    if (in_boot)
        return NULL;
    in_boot = 1;
    for (i = 0; vers[i] && !p; i++) {
        /* real_dlvsym 已解析出来就直接用它 —— 否则裸写 dlvsym(...) 会绑到我们自己的
         * 包装上（我们自己导出了它），多绕一圈（虽然包装里也有引导兜底）。 */
        p = internal_dlvsym(RTLD_NEXT, name, vers[i]);
    }
    /* 下面这两步以前写成 dlvsym(..., NULL)：glibc 把 version 标成 nonnull，
     * 传 NULL 是未定义行为 —— 实测在无版本符号（假 COM 模块）上直接段错误。
     * 改成用真正的 dlsym；real_dlsym 还没解析出来时（正是本函数在干的事）就跳过。 */
    if (!p && real_dlsym)
        p = real_dlsym(RTLD_NEXT, name);
    if (!p && real_dlsym)
        p = real_dlsym(RTLD_DEFAULT, name);
    in_boot = 0;
    return p;
}

static const char *base(const char *path)
{
    const char *s;

    if (!path)
        return "(null)";
    s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* 记录我们关心的后端库是否被加载过 */
static void note_dl_name(const char *file)
{
    const char *b;
    int i;

    if (!file || !*file)
        return;
    b = base(file);
    for (i = 0; i < dl_names_n; i++)
        if (strcmp(dl_names[i], b) == 0)
            return;
    if (dl_names_n < HWPROBE_RING)
        snprintf(dl_names[dl_names_n++], sizeof dl_names[0], "%s", b);
}

static void note_lib(const char *file, int ok)
{
    const char *b = base(file);

    if (!ok)
        return;
    if (strstr(b, "libnvidia-encode")) seen_nvenc = 1;
    else if (strstr(b, "libcuda")) seen_cuda = 1;
    else if (strstr(b, "libnvcuvid")) seen_cuvid = 1;
    else if (strstr(b, "libmfx")) seen_mfx = 1;
    else if (strstr(b, "libamfrt64")) seen_amf = 1;
    else if (strstr(b, "libopenh264")) seen_openh264 = 1;
    else if (strstr(b, "libvpx")) seen_vpx = 1;
    else if (strstr(b, "libx264")) seen_x264 = 1;
    else if (strstr(b, "libavcodec")) seen_avcodec = 1;
}

/* 关心的库名 / 符号名 */
static int interesting_lib(const char *f)
{
    static const char *keys[] = { "nvidia-encode", "libcuda", "nvcuvid", "libmfx", "amfrt64",
                                  "openh264", "libvpx", "libx264", "avcodec", NULL };
    int i;

    if (!f)
        return 0;
    for (i = 0; keys[i]; i++)
        if (strstr(f, keys[i]))
            return 1;
    return 0;
}

static int interesting_sym(const char *n)
{
    static const char *pre[] = { "Nv", "nvEnc", "cuvid", "BroadcastCore", "Encoder", "encoder",
                                 "DllGetClassObject", "mfx", "AMF", "amf_", NULL };
    int i;

    if (!n)
        return 0;
    if (log_all)
        return 1;
    for (i = 0; pre[i]; i++)
        if (strncmp(n, pre[i], strlen(pre[i])) == 0)
            return 1;
    return 0;
}

/* ---------- 被包装的导出符号 ----------
 *
 * 这三个必须是**真正的动态符号**（同名、非 static、default 可见性）：
 * LD_PRELOAD 的介入靠的是动态链接器按名字解析 —— 只要调用方是"直接链接"到
 * broadcast-core / libnvidia-encode（例如 libAVSDKPlugin.so 里
 * _ZN27QRTCServiceInterfaceWrapper17InitBroadcastCoreEv 走 COM 那条路），
 * 就不会有任何 dlsym 调用，static 包装 + dlsym 转发那条路**根本不参与**。
 * （2026-10-03 实测：nm -D 里没有这三个名字，于是接口创建、NVENC 初始化全都观测不到。）
 * dlsym 转发那条路继续保留，作为调用方主动 dlsym 时的兜底。
 */
#define HWPROBE_EXPORT __attribute__((visibility("default")))

HWPROBE_EXPORT void *DllGetClassObject(const void *clsid, const void *iid, void **out);
HWPROBE_EXPORT int NvEncodeAPICreateInstance(void *functionList);
HWPROBE_EXPORT int NvEncodeAPIGetMaxSupportedVersion(uint32_t *version);

/* 取"查找顺序里我们后面那个同名定义"。**绝不能用 RTLD_DEFAULT 兜底**：
 * 我们自己就导出了这个名字，RTLD_DEFAULT 会解析回我们 → 无限递归 → 栈溢出
 * （2026-10-03 实测：直接链接的测试里 rc=139 段错误就是这么来的）。
 * 这里只走 RTLD_NEXT，并且再挡一道 self 判断。 */
static void *next_definition(const char *name, void *self)
{
    static const char *vers[] = { "GLIBC_2.2.5", "GLIBC_2.34", "GLIBC_2.17", "GLIBC_2.3.4",
                                  "GLIBC_2.35", NULL };
    void *p = NULL;
    int i;

    for (i = 0; vers[i] && !p; i++)
        p = internal_dlvsym(RTLD_NEXT, name, vers[i]);
    /* 无版本符号（例如测试用的假 COM 模块）只能用 dlsym；
     * 绝不能写 dlvsym(..., NULL) —— glibc 标了 nonnull，实测直接段错误。 */
    if (!p && real_dlsym)
        p = real_dlsym(RTLD_NEXT, name);
    if (p == self)
        p = NULL;
    return p;
}

/* 重入保护：万一真实实现没取到而调用方又转回来，宁可报错也不要递归到爆栈 */
static int in_cls, in_api, in_ver;

/* 调用方所在的模块：介入的符号是通用名字（DllGetClassObject），不能一律转发给
 * broadcast-core —— 别的 COM 插件也导出这个名字。用 dladdr 认准"谁在调我们"。 */
static void *caller_module(void *retaddr)
{
    Dl_info info;
    void *h = NULL;

    if (!retaddr || !real_dlopen || !dladdr(retaddr, &info) || !info.dli_fname)
        return NULL;
    h = real_dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    return h;
}

/* NVENC 的真实入口：从我们自己的 handle 取，不动调用方的 handle */
static void *nvenc_handle(void)
{
    static void *h;
    static int tried;

    if (!h && !tried) {
        tried = 1;
        if (!real_dlopen)
            return NULL;
        /* 先 NOLOAD：qq 自己已经加载过就别重复加载；再退回正式加载 */
        h = real_dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (!h)
            h = real_dlopen("libnvidia-encode.so", RTLD_NOW | RTLD_NOLOAD);
        if (!h)
            h = real_dlopen("libnvidia-encode.so.1", RTLD_NOW);
        if (!h)
            h = real_dlopen("libnvidia-encode.so", RTLD_NOW);
    }
    return h;
}

static void *nvenc_real(const char *name)
{
    void *h = nvenc_handle();

    return (h && real_dlsym) ? real_dlsym(h, name) : NULL;
}

/* ---------- dlopen / dlmopen / dlsym 包装 ---------- */

void *dlopen(const char *file, int flags)
{
    void *h;
    char err[200] = "";
    int wanted;

    if (!real_dlopen)
        real_dlopen = hwprobe_lookup_real("dlopen");
    if (!real_dlerror)
        real_dlerror = hwprobe_lookup_real("dlerror");
    if (!real_dlsym)
        real_dlsym = hwprobe_lookup_real("dlsym");

    wanted = interesting_lib(file);
    n_dlopen++;
    maybe_periodic_summary();
    h = real_dlopen ? real_dlopen(file, flags) : NULL;
    if (!h) {
        n_dlopen_fail++;
        if (real_dlerror) {
            const char *e = real_dlerror();
            if (e)
                snprintf(err, sizeof err, " (%s)", e);
        }
    }
    note_lib(file, h != NULL);
    note_dl_name(file);
    if (file && (strstr(file, "broadcast") || strstr(file, "AVSDK")))
        hwprobe_plog("dlopen 命中目标：\"%s\" -> %s%s", base(file), h ? "成功" : "失败", err);
    if (wanted)
        hwprobe_plog("dlopen(\"%s\", 0x%x) -> %s%s", base(file), flags, h ? "成功" : "失败", err);
    else if (log_all)
        hwprobe_plog("dlopen(\"%s\", 0x%x) -> %s%s", base(file), flags, h ? "成功" : "失败", err);
    return h;
}

void *dlmopen(long nsid, const char *file, int flags)
{
    void *h;

    if (!real_dlmopen)
        real_dlmopen = hwprobe_lookup_real("dlmopen");
    if (!real_dlmopen)
        return NULL;
    h = real_dlmopen(nsid, file, flags);
    note_lib(file, h != NULL);
    note_dl_name(file);
    if (interesting_lib(file) || log_all)
        hwprobe_plog("dlmopen(%ld, \"%s\", 0x%x) -> %s", nsid, base(file), flags, h ? "成功" : "失败");
    return h;
}

/*
 * 三个目标符号的路由：dlsym 与 dlvsym 都要走这里（返回 1 = 已处理）。
 * 2026-10-03：日志里 dlsym 一直是 0 次，而 Chromium 在 glibc 上也会用 dlvsym 取
 * 版本化符号 —— 只包 dlsym 会整条漏掉。
 */
static int route_special(void *handle, const char *name, void **out)
{
    if (!name)
        return 0;
    if (strcmp(name, "DllGetClassObject") == 0) {
        cls_handle = handle;
        n_gave_cls++;
        *out = (void *)DllGetClassObject;
        if (!path_cls_logged) {
            path_cls_logged = 1;
            hwprobe_plog("符号介入路径：DllGetClassObject 被调用方取走（交给我们的实现）");
        }
        return 1;
    }
    if (strcmp(name, "NvEncodeAPICreateInstance") == 0) {
        n_gave_nvenc_api++;
        *out = (void *)NvEncodeAPICreateInstance;
        if (!path_api_logged) {
            path_api_logged = 1;
            hwprobe_plog("符号介入路径：NvEncodeAPICreateInstance 被调用方取走");
        }
        return 1;
    }
    if (strcmp(name, "NvEncodeAPIGetMaxSupportedVersion") == 0) {
        n_gave_nvenc_ver++;
        *out = (void *)NvEncodeAPIGetMaxSupportedVersion;
        if (!path_ver_logged) {
            path_ver_logged = 1;
            hwprobe_plog("符号介入路径：NvEncodeAPIGetMaxSupportedVersion 被调用方取走");
        }
        return 1;
    }
    return 0;
}

void *dlsym(void *handle, const char *name)
{
    void *r;

    if (!real_dlsym)
        real_dlsym = hwprobe_lookup_real("dlsym");
    n_dlsym++;
    maybe_periodic_summary();

    if (!route_special(handle, name, &r))
        r = real_dlsym ? real_dlsym(handle, name) : NULL;

    if (interesting_sym(name))
        hwprobe_plog("dlsym(%p, \"%s\") -> %p%s", handle, name ? name : "(null)", r,
             r ? "" : " [没找到]");
    return r;
}

/*
 * 关于 dlvsym：**必须包，但只能纯透传**。
 *
 * 为什么必须包：QQ 的调用方是 `dlopen("broadcast-core.so")` 之后用
 * `dlvsym(handle, "DllGetClassObject", "VERS_1.0")` 取入口的（符号本身带版本），
 * 所以 dlsym 计数一直是 0 —— 只包 dlsym 会把整条路漏掉。
 *
 * 第一版为什么翻车：实现里"解析不到真实实现就返回 NULL"，而 glibc 自己会用
 * dlvsym 做版本探测（实测 `dlvsym(RTLD_DEFAULT, "dlopen", "GLIBC_2.34")` 返回 nil），
 * 于是动态加载链整体退化（run-test.sh 段 D 当场挂）。现在的规矩是：
 *   1) 只对三个目标名字做拦截，其余**原样透传**（含 version == NULL 的调用）
 *   2) 真实实现优先用 real_dlsym(RTLD_NEXT, "dlvsym") 取；取不到就扫 ELF 动态符号表
 *   3) 再取不到才退化成不带版本的 dlsym —— **绝不无缘无故返回 NULL**
 *
 * 引导为什么要扫 ELF：我们一旦导出 dlvsym，连自己文件里的 `dlvsym(...)` 调用也会
 * 被动态链接器解析回我们自己，而"真实的 dlvsym"没法再用 dlsym/dlvsym 去取（鸡生蛋）。
 * dl_iterate_phdr 不经过任何 PLT，正好用来打破这个循环。
 */
struct boot_scan {
    const char *want;
    void *found;
};

static int boot_scan_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    struct boot_scan *s = data;
    const ElfW(Dyn) *dyn = NULL;
    const ElfW(Sym) *symtab = NULL;
    const char *strtab = NULL;
    const uint32_t *gnu_hash = NULL;
    size_t nsym = 0, k;
    unsigned i;

    (void)size;
    if (s->found)
        return 1;
    /* 只认 libc：dlvsym 在那里；也顺带避开我们自己（我们也导出同名符号） */
    if (!info->dlpi_name || !strstr(info->dlpi_name, "libc.so"))
        return 0;
    for (i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = (const ElfW(Dyn) *)(uintptr_t)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    if (!dyn)
        return 0;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
        case DT_SYMTAB: symtab = (const ElfW(Sym) *)(uintptr_t)dyn->d_un.d_ptr; break;
        case DT_STRTAB: strtab = (const char *)(uintptr_t)dyn->d_un.d_ptr; break;
        case DT_HASH:   nsym = ((const uint32_t *)(uintptr_t)dyn->d_un.d_ptr)[1]; break;
        case DT_GNU_HASH: gnu_hash = (const uint32_t *)(uintptr_t)dyn->d_un.d_ptr; break;
        default: break;
        }
    }
    /* 现代 glibc 的 libc.so.6 只有 DT_GNU_HASH，没有 DT_HASH（实测 nchain 取不到）——
       靠它算符号总数：遍历每个桶，走到链尾（最低位为 1）就得到该桶最后一个符号的下标。 */
    if (!nsym && gnu_hash) {
        const uint32_t nbuckets = gnu_hash[0];
        const uint32_t symoffset = gnu_hash[1];
        const uint32_t bloom_size = gnu_hash[2];
        const uint32_t *buckets = gnu_hash + 4 + bloom_size * 2; /* bloom 是 64 位，占 2 个 u32 */
        const uint32_t *chain = buckets + nbuckets;
        uint32_t b, maxidx = 0;

        for (b = 0; b < nbuckets; b++) {
            uint32_t idx = buckets[b];
            uint32_t j, last;

            if (idx < symoffset)
                continue;
            last = idx;
            j = idx - symoffset;
            while (!(chain[j] & 1u)) {
                j++;
                last++;
            }
            if (last + 1 > maxidx)
                maxidx = last + 1;
        }
        nsym = maxidx;
    }
    if (!symtab || !strtab || !nsym)
        return 0;
    for (k = 0; k < nsym; k++) {
        if (symtab[k].st_shndx == SHN_UNDEF || symtab[k].st_name == 0)
            continue;
        if (strcmp(strtab + symtab[k].st_name, s->want) == 0) {
            s->found = (void *)(uintptr_t)(info->dlpi_addr + symtab[k].st_value);
            return 1;
        }
    }
    return 0;
}

static void *elf_bootstrap_lookup(const char *want)
{
    struct boot_scan s;

    s.want = want;
    s.found = NULL;
    dl_iterate_phdr(boot_scan_cb, &s);
    return s.found;
}

HWPROBE_EXPORT void *dlvsym(void *handle, const char *name, const char *version)
{
    void *r;

    /* 引导：优先问真实 dlsym，其次扫 ELF（两条路都不经过被包装的 dlvsym） */
    if (!real_dlvsym) {
        if (real_dlsym)
            real_dlvsym = real_dlsym(RTLD_NEXT, "dlvsym");
        if (!real_dlvsym)
            real_dlvsym = elf_bootstrap_lookup("dlvsym");
    }
    /* 内部解析：纯转发，不计数、不路由、不记日志 */
    if (internal_lookup)
        return real_dlvsym ? real_dlvsym(handle, name, version)
                           : (real_dlsym ? real_dlsym(handle, name) : NULL);

    n_dlvsym++;
    maybe_periodic_summary();

    if (in_dlvsym) {
        /* 重入（真实 dlvsym 内部又调到我们）：直接用真实实现，避免自锁 */
        return real_dlvsym ? real_dlvsym(handle, name, version) : NULL;
    }
    in_dlvsym = 1;
    if (route_special(handle, name, &r)) {
        if (!path_dlvsym_logged && name && strcmp(name, "DllGetClassObject") == 0) {
            path_dlvsym_logged = 1;
            hwprobe_plog("符号介入路径：DllGetClassObject 是调用方用 "
                         "dlvsym(handle, \"%s\", \"%s\") 取走的",
                         name, version ? version : "(null)");
        }
    } else if (real_dlvsym) {
        r = real_dlvsym(handle, name, version);
    } else if (real_dlsym) {
        /* 彻底取不到真实 dlvsym 时，退化到"不解释版本"的 dlsym —— 绝不返回 nil */
        r = real_dlsym(handle, name);
        if (!dlvsym_fallback_logged) {
            dlvsym_fallback_logged = 1;
            hwprobe_plog("dlvsym 真实实现取不到，已退化为不带版本的 dlsym（不影响调用方）");
        }
    } else {
        r = NULL;
    }
    in_dlvsym = 0;

    if (interesting_sym(name))
        hwprobe_plog("dlvsym(%p, \"%s\", \"%s\") -> %p%s", handle, name ? name : "(null)",
                     version ? version : "(null)", r, r ? "" : " [没找到]");
    return r;
}

/* ---------- 被包装的目标符号 ---------- */

static void guid_str(const unsigned char *g, char *out, size_t n)
{
    snprintf(out, n,
             "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             *(const uint32_t *)g, *(const uint16_t *)(g + 4), *(const uint16_t *)(g + 6),
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* broadcast-core 唯一可 hook 的导出符号：接口创建时机。
 * 这个函数有两条到达路径：调用方直接链接（动态链接器解析到我们）或先 dlsym 拿指针。 */
HWPROBE_EXPORT void *DllGetClassObject(const void *clsid, const void *iid, void **out)
{
    void *(*fn)(const void *, const void *, void **);
    void *r;
    char cs[64] = "?", is[64] = "?";

    if (!real_dlopen)
        real_dlopen = hwprobe_lookup_real("dlopen");
    if (!real_dlsym)
        real_dlsym = hwprobe_lookup_real("dlsym");

    if (!path_cls_logged) {
        path_cls_logged = 1;
        if (n_gave_cls)
            hwprobe_plog("符号介入路径：DllGetClassObject 被调用（调用方先前用 dlsym 取过它）");
        else
            hwprobe_plog("符号介入路径：DllGetClassObject 被调用 —— 导出符号介入生效"
                         "（动态链接器直接解析到我们，全程没有 dlsym）");
    }

    if (in_cls) {
        hwprobe_plog("DllGetClassObject 重入且真实实现没取到 —— 返回失败，避免无限递归");
        return NULL;
    }
    in_cls = 1;
    /* 真实实现按可靠性依次尝试：
     *   1) RTLD_NEXT —— 直接链接时唯一正确的做法：取"查找顺序里我们后面那个定义"
     *      （libfakecom / broadcast-core 都在这条路上）；我们被 preload 在它前面。
     *   2) dlsym 那次记下来的 handle —— 目标库是 RTLD_LOCAL 加载时 RTLD_NEXT 看不见它
     *   3) 调用方自己的模块（dladdr 认人）—— 库调库、且那个库自己就实现该符号时
     *   4) broadcast-core.so（NOLOAD 优先，取不到再正式加载）
     * 通用名字必须这样一层层退，否则别的 COM 插件的同名符号会被错送到 broadcast-core。
     */
    {
        void *p = next_definition("DllGetClassObject", (void *)DllGetClassObject);

        fn = p ? (void *(*)(const void *, const void *, void **))p : NULL;
    }
    if (!fn && cls_handle && real_dlsym)
        fn = real_dlsym(cls_handle, "DllGetClassObject");
    if (!fn) {
        void *h = caller_module(__builtin_return_address(0));

        if (h && real_dlsym)
            fn = real_dlsym(h, "DllGetClassObject");
    }
    if (!fn) {
        static void *bc;
        static int tried;

        if (!tried) {
            tried = 1;
            if (real_dlopen) {
                bc = real_dlopen("broadcast-core.so", RTLD_NOW | RTLD_NOLOAD);
                if (!bc)
                    bc = real_dlopen("broadcast-core.so", RTLD_NOW);
            }
        }
        fn = (bc && real_dlsym) ? real_dlsym(bc, "DllGetClassObject") : NULL;
    }
    n_cls++;
    maybe_periodic_summary();
    if (clsid)
        guid_str(clsid, cs, sizeof cs);
    if (iid)
        guid_str(iid, is, sizeof is);
    /* 记下真实实现来自哪个模块：解析错库时（同名符号是通用名字）这条一眼看得出来 */
    if (fn) {
        Dl_info di;

        if (dladdr((void *)fn, &di) && di.dli_fname)
            hwprobe_plog("DllGetClassObject 真实实现 = %s+0x%lx", base(di.dli_fname),
                         (unsigned long)((const char *)fn - (const char *)di.dli_fbase));
        else
            hwprobe_plog("DllGetClassObject 真实实现 = %p（dladdr 认不出模块）", (void *)fn);
    }
    r = fn ? fn(clsid, iid, out) : NULL;
    in_cls = 0;
    hwprobe_plog("DllGetClassObject(clsid=%s, iid=%s) -> hr=0x%lx, factory=%p  [接口创建 #%lu]", cs, is,
                 (unsigned long)(intptr_t)r, (r == 0 && out) ? *out : NULL, n_cls);
    if (r == 0 && out && *out)
        vthook_wrap_factory(out);   /* 之后 CreateInstance 与对象方法调用都会被观测 */
    return r;
}

HWPROBE_EXPORT int NvEncodeAPICreateInstance(void *functionList)
{
    int (*fn)(void *);
    int r;

    if (!real_dlopen)
        real_dlopen = hwprobe_lookup_real("dlopen");
    if (!real_dlsym)
        real_dlsym = hwprobe_lookup_real("dlsym");
    if (!path_api_logged) {
        path_api_logged = 1;
        if (n_gave_nvenc_api)
            hwprobe_plog("符号介入路径：NvEncodeAPICreateInstance 被调用（dlsym 取走的）");
        else
            hwprobe_plog("符号介入路径：NvEncodeAPICreateInstance 被调用 —— 导出符号介入生效");
    }
    if (in_api) {
        hwprobe_plog("NvEncodeAPICreateInstance 重入 —— 返回失败，避免无限递归");
        return -1;
    }
    in_api = 1;
    fn = nvenc_real("NvEncodeAPICreateInstance");
    n_nvenc_api++;
    maybe_periodic_summary();
    r = fn ? fn(functionList) : -1;
    in_api = 0;
    hwprobe_plog("NvEncodeAPICreateInstance(%p) -> %d  [NVENC 被初始化 #%lu]%s",
         functionList, r, n_nvenc_api, r == 0 ? "" : "  <- 非 0 表示失败（随后必然回退软编）");
    return r;
}

HWPROBE_EXPORT int NvEncodeAPIGetMaxSupportedVersion(uint32_t *version)
{
    int (*fn)(uint32_t *);
    int r;
    uint32_t v = 0;

    if (!real_dlopen)
        real_dlopen = hwprobe_lookup_real("dlopen");
    if (!real_dlsym)
        real_dlsym = hwprobe_lookup_real("dlsym");
    if (!path_ver_logged) {
        path_ver_logged = 1;
        if (n_gave_nvenc_ver)
            hwprobe_plog("符号介入路径：NvEncodeAPIGetMaxSupportedVersion 被调用（dlsym 取走的）");
        else
            hwprobe_plog("符号介入路径：NvEncodeAPIGetMaxSupportedVersion 被调用 —— 导出符号介入生效");
    }
    if (in_ver) {
        hwprobe_plog("NvEncodeAPIGetMaxSupportedVersion 重入 —— 返回失败，避免无限递归");
        return -1;
    }
    in_ver = 1;
    fn = nvenc_real("NvEncodeAPIGetMaxSupportedVersion");
    n_nvenc_ver++;
    maybe_periodic_summary();
    r = fn ? fn(&v) : -1;
    in_ver = 0;
    if (version && r == 0)
        *version = v;
    hwprobe_plog("NvEncodeAPIGetMaxSupportedVersion -> %d, 版本 %u.%u  [驱动支持 NVENC]",
         r, v >> 4, v & 0xf);
    return r;
}

/* ---------- 收尾：映射了哪些库 + 计数 ---------- */

static void dump_map(void)
{
    FILE *f;
    char line[1024];
    char seen[32][128];
    int nseen = 0, i, j;

    f = fopen("/proc/self/maps", "re");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *p = strchr(line, '/');
        int dup = 0;

        if (!p)
            continue;
        p[strcspn(p, "\n")] = 0;
        if (!interesting_lib(p))
            continue;
        for (j = 0; j < nseen; j++)
            if (strcmp(seen[j], p) == 0)
                dup = 1;
        if (dup || nseen >= 32)
            continue;
        snprintf(seen[nseen], sizeof seen[0], "%s", p);
        nseen++;
    }
    fclose(f);
    hwprobe_plog("--- 进程映射到的编码相关库（%d 个）---", nseen);
    for (i = 0; i < nseen; i++)
        hwprobe_plog("    %s", seen[i]);
}

static void summary(void)
{
    hwprobe_plog("--- 汇总 ---");
    hwprobe_plog("dlopen %lu 次（失败 %lu）；dlsym %lu 次；dlvsym %lu 次；DllGetClassObject %lu 次",
         n_dlopen, n_dlopen_fail, n_dlsym, n_dlvsym, n_cls);
    hwprobe_plog("后端加载情况：NVENC=%s CUDA=%s NVDEC=%s OpenH264(软编)=%s IntelQSV=%s AMF=%s vpx=%s x264=%s avcodec=%s",
         seen_nvenc ? "是" : "否", seen_cuda ? "是" : "否", seen_cuvid ? "是" : "否",
         seen_openh264 ? "是" : "否", seen_mfx ? "是" : "否", seen_amf ? "是" : "否",
         seen_vpx ? "是" : "否", seen_x264 ? "是" : "否", seen_avcodec ? "是" : "否");
    hwprobe_plog("NVENC 初始化调用 %lu 次；驱动能力查询 %lu 次", n_nvenc_api, n_nvenc_ver);
    hwprobe_plog("判读：NVENC 初始化成功(返回 0) => 走了硬编；只有 OpenH264 且 NVENC 初始化 0 次 => 软编");
    {
        int i;
        hwprobe_plog("--- 本进程 dlopen 过的库（%d 个%s）---", dl_names_n,
                     dl_names_n >= HWPROBE_RING ? "，已达上限" : "");
        for (i = 0; i < dl_names_n; i++)
            hwprobe_plog("    %s", dl_names[i]);
    }
    dump_map();
    vthook_summary();
}

/*
 * 周期汇总（惰性）：主进程（/opt/QQ/qq）是长命的，只在退出时才写汇总 —— 于是"它到底
 * 有没有 dlopen broadcast-core / 有没有走 DllGetClassObject"在日志里永远看不到。
 *
 * 实现方式：**不使用任何信号，也不使用任何定时器**。
 * 早期版本用 signal(SIGALRM)+alarm() 做这件事，而本库被注入到每一个子进程里，于是
 * 每个进程 20 秒后都会收到 SIGALRM —— 默认动作是终止进程，等于把宿主（QQ、乃至启动器
 * 自己）打死。2026-10-03 用户实测：启动器被 SIGALRM 杀掉、QQ 根本没起来。
 * 现在改为在本来就会执行的地方（dlopen / dlsym / DllGetClassObject / NVENC 入口）
 * 顺手判断"距上次摘要是否够久"。没有这些调用就不打印 —— 这完全够用，因为要观测的
 * 正是这些调用。
 */
static long mono_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long)ts.tv_sec;
}

static void maybe_periodic_summary(void)
{
    char b[320];
    int n, i, nlibs;
    long now;
    const char *bc = "否";

    if (period_secs <= 0)
        return;
    now = mono_now();
    if (now == 0)
        return;
    if (last_period_mono != 0 && now - last_period_mono < period_secs)
        return;
    last_period_mono = now;

    for (i = 0; i < dl_names_n; i++)
        if (strstr(dl_names[i], "broadcast"))
            bc = "是";
    nlibs = (seen_nvenc ? 1 : 0) + (seen_cuda ? 1 : 0) + (seen_cuvid ? 1 : 0) +
            (seen_openh264 ? 1 : 0) + (seen_mfx ? 1 : 0) + (seen_amf ? 1 : 0) +
            (seen_vpx ? 1 : 0) + (seen_x264 ? 1 : 0) + (seen_avcodec ? 1 : 0);
    n = snprintf(b, sizeof b,
        "[hwprobe] 周期汇总 pid=%d dlopen=%lu(失败 %lu) dlsym=%lu dlvsym=%lu "
        "DllGetClassObject=%lu nvenc_api=%lu nvenc_ver=%lu broadcast-core=%s "
        "dlopen库数=%d 编码库=%d\n",
        (int)getpid(), n_dlopen, n_dlopen_fail, n_dlsym, n_dlvsym, n_cls,
        n_nvenc_api, n_nvenc_ver, bc, dl_names_n, nlibs);
    if (n > 0)
        (void)!write(hwprobe_logfd(), b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

/* 只有 cmdline 里带 qq / ppapi 的进程才值得输出那一整套汇总块 */
static int cmdline_relevant(const char *cbuf)
{
    return cbuf && (strstr(cbuf, "qq") || strstr(cbuf, "QQ") || strstr(cbuf, "ppapi"));
}

__attribute__((constructor)) static void hwprobe_init(void)
{
    const char *path = getenv("HWPROBE_LOG");
    const char *p;
    char cbuf[512];

    log_all = getenv("HWPROBE_ALL") != NULL;
    if (path && *path) {
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0)
            logfd = fd;
        /* 记下路径：fork 出来的子进程里 fd 会被 Chromium 关掉，那时按它重开 */
        snprintf(log_path, sizeof log_path, "%s", path);
    }
    real_dlopen = hwprobe_lookup_real("dlopen");
    real_dlsym = hwprobe_lookup_real("dlsym");
    real_dlerror = hwprobe_lookup_real("dlerror");
    real_dlmopen = hwprobe_lookup_real("dlmopen");

    /*
     * 同一份日志里有几十个进程，只记 pid 分不清哪个块是哪个进程
     * （2026-10-03：Phase 2 的日志因此无法判断收帧进程有没有被覆盖）。
     */
    cbuf[0] = '\0';
    {
        int cfd = open("/proc/self/cmdline", O_RDONLY);
        ssize_t cn = cfd >= 0 ? read(cfd, cbuf, sizeof(cbuf) - 1) : -1;
        ssize_t ci;
        if (cfd >= 0) close(cfd);
        if (cn > 0) {
            cbuf[cn] = '\0';
            for (ci = 0; ci < cn - 1; ci++)
                if (cbuf[ci] == '\0') cbuf[ci] = ' ';
            if (cn > 260) cbuf[260] = '\0';
        }
    }

    /*
     * 非 QQ 进程（date / pgrep / grep / head 这些辅助命令也会继承 LD_PRELOAD）
     * 只留一行说明就退出：2026-10-03 实测，不加这个闸门时这些辅助命令各自写一份
     * 十几行的汇总块，一份日志被灌到 3768 行 / 232 个进程块，有效数据为零。
     */
    if (!log_all && !cmdline_relevant(cbuf)) {
        hwprobe_plog("hwprobe 已注入 pid=%d cmdline=%s（非 QQ 进程，只记这一行）",
                     (int)getpid(), cbuf[0] ? cbuf : "?");
        return;
    }

    vthook_init();
    hwprobe_plog("=== hwprobe 已注入 pid=%d ===", (int)getpid());
    /* stderr 那条通道用来确认"构造函数到底跑没跑"（启动器会把 QQ 的 stderr 收进自己的日志） */
    plog_stderr("已注入 pid=%d cmdline=%s", (int)getpid(), cbuf[0] ? cbuf : "?");
    if (cbuf[0])
        hwprobe_plog("命令行：%s", cbuf);
    hwprobe_plog("父进程：%d", (int)getppid());
    p = getenv("QQ_WAYLAND_FIX_ANGLE");
    hwprobe_plog("环境：QQ_WAYLAND_FIX_ANGLE=%s", p ? p : "(未设)");
    p = getenv("LD_PRELOAD");
    hwprobe_plog("环境：LD_PRELOAD=%s", p ? p : "(空)");
    atexit(summary);
    {
        const char *ps = getenv("HWPROBE_PERIOD");
        period_secs = (ps && *ps) ? atoi(ps) : 20;   /* 默认 20 秒一条；<=0 关闭 */
        /* 记下起点：第一次周期摘要在 period 秒之后，而不是立刻 */
        last_period_mono = mono_now();
    }
}
