#define _GNU_SOURCE   /* 需要 dlvsym 的声明 */
/*
 * dlvdriver.c -- 复现 QQ 真实的取符号方式：dlopen + dlvsym(带版本号)
 *
 * 为什么单独一个驱动：真实调用方（libAVSDKPlugin）不是用 dlsym 拿 DllGetClassObject，
 * 而是 dlvsym(handle, "DllGetClassObject", "VERS_1.0") —— 只包 dlsym 会整条漏掉。
 *
 * 顺带做一条**反向断言**：探针绝不能弄坏 glibc 自己的版本化查找。第一版包 dlvsym 时
 * "解析不到真实实现就返回 NULL"，导致 dlvsym(RTLD_DEFAULT,"dlopen","GLIBC_2.34") 变 nil，
 * 动态加载链整体退化。这里把这条守住。
 */
#include <dlfcn.h>
#include <stdio.h>

typedef long (*getcls_fn)(const void *, const void *, void **);
typedef long (*create_fn)(void *, void *, const void *, void **);

int main(void)
{
    void *h, *fac = NULL, *obj = NULL, **vt;
    getcls_fn g;
    create_fn create;
    unsigned char clsid[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    unsigned char iid[16] = { 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 };
    long hr;

    /* 反向断言：glibc 自己的版本化查找必须仍然可用 */
    {
        void *dl = dlvsym(RTLD_DEFAULT, "dlopen", "GLIBC_2.34");
        printf("glibc_dlvsym_dlopen=%d   期望 1（探针不能弄坏 glibc 的版本探测）\n", dl != NULL);
        if (!dl)
            return 2;
    }

    h = dlopen("./libfakecom_ver.so", RTLD_NOW);
    if (!h) {
        printf("dlopen 失败: %s\n", dlerror());
        return 1;
    }
    g = (getcls_fn)dlvsym(h, "DllGetClassObject", "VERS_1.0");
    printf("dlvsym_DllGetClassObject=%d   期望 1\n", g != NULL);
    if (!g) {
        printf("dlvsym 失败: %s\n", dlerror());
        return 1;
    }
    hr = g(clsid, iid, &fac);
    printf("DllGetClassObject -> hr=%ld factory=%p\n", hr, fac);
    if (hr != 0 || !fac)
        return 1;

    create = (create_fn)(*(void ***)fac)[3];
    hr = create(fac, NULL, iid, &obj);
    printf("CreateInstance -> hr=%ld obj=%p\n", hr, obj);
    if (hr != 0 || !obj)
        return 1;

    vt = *(void ***)obj;
    printf("m0(5)   = %ld   期望 6\n", ((long (*)(void *, long))vt[0])(obj, 5));
    printf("m1(5)   = %ld   期望 10\n", ((long (*)(void *, long))vt[1])(obj, 5));
    printf("m2()    = %ld   期望 42\n", ((long (*)(void *))vt[2])(obj));
    printf("m3()    = %ld   期望 0（假扮『硬件不受支持』）\n", ((long (*)(void *))vt[3])(obj));
    printf("m5(9,4) = %ld   期望 5\n", ((long (*)(void *, long, long))vt[5])(obj, 9, 4));
    printf("m8(1..7)= %ld   期望 1234567 ← 7 个整数参数，第 7 个走栈\n",
           ((long (*)(void *, long, long, long, long, long, long, long))vt[8])(obj, 1, 2, 3, 4, 5, 6, 7));
    return 0;
}
