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

static int logfd = 2;                 /* 默认 stderr；HWPROBE_LOG 可改 */
static int log_all = 0;               /* HWPROBE_ALL=1 时记录所有 dlsym */
static int in_boot = 0;               /* dlvsym 引导期间的重入保护 */

static unsigned long n_dlopen, n_dlopen_fail, n_dlsym, n_cls, n_nvenc_api, n_nvenc_ver;
static int seen_nvenc, seen_cuda, seen_cuvid, seen_mfx, seen_amf, seen_openh264, seen_vpx, seen_x264, seen_avcodec;

static void *(*real_dlopen)(const char *, int);
static void *(*real_dlmopen)(long, const char *, int);
static void *(*real_dlsym)(void *, const char *);
static char *(*real_dlerror)(void);

/* ---------- 工具 ---------- */

static void plog(const char *fmt, ...)
{
    char buf[600];
    int n;
    struct timespec ts;
    va_list ap;

    if (logfd < 0)
        return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    n = snprintf(buf, sizeof buf, "[hwprobe] %ld.%06ld ", (long)ts.tv_sec, ts.tv_nsec / 1000);
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2)
        n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    if (write(logfd, buf, n) < 0)
        ; /* 日志失败也不该影响宿主进程 */
}

/* 用没有被包装的 dlvsym 取真实函数，避免递归进我们自己的 dlsym */
static void *lookup_real(const char *name)
{
    static const char *vers[] = { "GLIBC_2.2.5", "GLIBC_2.34", "GLIBC_2.17", NULL };
    void *p = NULL;
    int i;

    if (in_boot)
        return NULL;
    in_boot = 1;
    for (i = 0; vers[i] && !p; i++)
        p = dlvsym(RTLD_NEXT, name, vers[i]);
    if (!p)
        p = dlvsym(RTLD_DEFAULT, name, "GLIBC_2.2.5");
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

/* ---------- 被包装的导出符号（先声明，dlsym 里要返回它们） ---------- */

static void *my_DllGetClassObject(const void *clsid, const void *iid, void **out);
static int my_NvEncodeAPICreateInstance(void *functionList);
static int my_NvEncodeAPIGetMaxSupportedVersion(uint32_t *version);

/* NVENC 的真实入口：从我们自己的 handle 取，不动调用方的 handle */
static void *nvenc_handle(void)
{
    static void *h;
    static int tried;

    if (!h && !tried) {
        tried = 1;
        if (real_dlopen)
            h = real_dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (!h && real_dlopen)
            h = real_dlopen("libnvidia-encode.so.1", RTLD_NOW);
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
        real_dlopen = lookup_real("dlopen");
    if (!real_dlerror)
        real_dlerror = lookup_real("dlerror");
    if (!real_dlsym)
        real_dlsym = lookup_real("dlsym");

    wanted = interesting_lib(file);
    n_dlopen++;
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
    if (wanted)
        plog("dlopen(\"%s\", 0x%x) -> %s%s", base(file), flags, h ? "成功" : "失败", err);
    else if (log_all)
        plog("dlopen(\"%s\", 0x%x) -> %s%s", base(file), flags, h ? "成功" : "失败", err);
    return h;
}

void *dlmopen(long nsid, const char *file, int flags)
{
    void *h;

    if (!real_dlmopen)
        real_dlmopen = lookup_real("dlmopen");
    if (!real_dlmopen)
        return NULL;
    h = real_dlmopen(nsid, file, flags);
    note_lib(file, h != NULL);
    if (interesting_lib(file) || log_all)
        plog("dlmopen(%ld, \"%s\", 0x%x) -> %s", nsid, base(file), flags, h ? "成功" : "失败");
    return h;
}

void *dlsym(void *handle, const char *name)
{
    void *r;

    if (!real_dlsym)
        real_dlsym = lookup_real("dlsym");
    n_dlsym++;

    if (name && strcmp(name, "DllGetClassObject") == 0)
        r = (void *)my_DllGetClassObject;
    else if (name && strcmp(name, "NvEncodeAPICreateInstance") == 0)
        r = (void *)my_NvEncodeAPICreateInstance;
    else if (name && strcmp(name, "NvEncodeAPIGetMaxSupportedVersion") == 0)
        r = (void *)my_NvEncodeAPIGetMaxSupportedVersion;
    else
        r = real_dlsym ? real_dlsym(handle, name) : NULL;

    if (interesting_sym(name))
        plog("dlsym(%p, \"%s\") -> %p%s", handle, name ? name : "(null)", r,
             r ? "" : " [没找到]");
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

/* broadcast-core 唯一可 hook 的导出符号：接口创建时机 */
static void *my_DllGetClassObject(const void *clsid, const void *iid, void **out)
{
    void *(*fn)(const void *, const void *, void **);
    void *r;
    char cs[64] = "?", is[64] = "?";

    /* 真实的 DllGetClassObject 从已经加载的 broadcast-core.so 里取 */
    {
        static void *bc;
        static int tried;
        if (!tried) {
            tried = 1;
            if (real_dlopen)
                bc = real_dlopen("broadcast-core.so", RTLD_NOW | RTLD_NOLOAD);
        }
        fn = (bc && real_dlsym) ? real_dlsym(bc, "DllGetClassObject") : NULL;
    }
    n_cls++;
    if (clsid)
        guid_str(clsid, cs, sizeof cs);
    if (iid)
        guid_str(iid, is, sizeof is);
    r = fn ? fn(clsid, iid, out) : NULL;
    plog("DllGetClassObject(clsid=%s) -> %p  [broadcast-core 接口创建 #%lu]", cs, r, n_cls);
    return r;
}

static int my_NvEncodeAPICreateInstance(void *functionList)
{
    int (*fn)(void *) = nvenc_real("NvEncodeAPICreateInstance");
    int r;

    n_nvenc_api++;
    r = fn ? fn(functionList) : -1;
    plog("NvEncodeAPICreateInstance(%p) -> %d  [NVENC 被初始化 #%lu]%s",
         functionList, r, n_nvenc_api, r == 0 ? "" : "  <- 非 0 表示失败（随后必然回退软编）");
    return r;
}

static int my_NvEncodeAPIGetMaxSupportedVersion(uint32_t *version)
{
    int (*fn)(uint32_t *) = nvenc_real("NvEncodeAPIGetMaxSupportedVersion");
    int r;
    uint32_t v = 0;

    n_nvenc_ver++;
    r = fn ? fn(&v) : -1;
    if (version && r == 0)
        *version = v;
    plog("NvEncodeAPIGetMaxSupportedVersion -> %d, 版本 %u.%u  [驱动支持 NVENC]",
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
    plog("--- 进程映射到的编码相关库（%d 个）---", nseen);
    for (i = 0; i < nseen; i++)
        plog("    %s", seen[i]);
}

static void summary(void)
{
    plog("--- 汇总 ---");
    plog("dlopen %lu 次（失败 %lu）；dlsym %lu 次；DllGetClassObject %lu 次",
         n_dlopen, n_dlopen_fail, n_dlsym, n_cls);
    plog("后端加载情况：NVENC=%s CUDA=%s NVDEC=%s OpenH264(软编)=%s IntelQSV=%s AMF=%s vpx=%s x264=%s avcodec=%s",
         seen_nvenc ? "是" : "否", seen_cuda ? "是" : "否", seen_cuvid ? "是" : "否",
         seen_openh264 ? "是" : "否", seen_mfx ? "是" : "否", seen_amf ? "是" : "否",
         seen_vpx ? "是" : "否", seen_x264 ? "是" : "否", seen_avcodec ? "是" : "否");
    plog("NVENC 初始化调用 %lu 次；驱动能力查询 %lu 次", n_nvenc_api, n_nvenc_ver);
    plog("判读：NVENC 初始化成功(返回 0) => 走了硬编；只有 OpenH264 且 NVENC 初始化 0 次 => 软编");
    dump_map();
}

static void on_sig(int sig)
{
    (void)sig;
    summary();
    _exit(0);
}

__attribute__((constructor)) static void hwprobe_init(void)
{
    const char *path = getenv("HWPROBE_LOG");
    const char *p;

    log_all = getenv("HWPROBE_ALL") != NULL;
    if (path && *path) {
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0)
            logfd = fd;
    }
    real_dlopen = lookup_real("dlopen");
    real_dlsym = lookup_real("dlsym");
    real_dlerror = lookup_real("dlerror");
    real_dlmopen = lookup_real("dlmopen");

    plog("=== hwprobe 已注入 pid=%d ===", (int)getpid());
    p = getenv("QQ_WAYLAND_FIX_ANGLE");
    plog("环境：QQ_WAYLAND_FIX_ANGLE=%s", p ? p : "(未设)");
    p = getenv("LD_PRELOAD");
    plog("环境：LD_PRELOAD=%s", p ? p : "(空)");
    atexit(summary);
    signal(SIGUSR1, on_sig);
}
