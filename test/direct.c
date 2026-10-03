/*
 * direct.c -- 直接链接版驱动：**故意不用 dlsym** 去取 DllGetClassObject。
 *
 * 为什么需要它：真实的 libAVSDKPlugin.so 是直接链接 broadcast-core 的（COM 调用），
 * 符号由动态链接器解析 —— 只有 libhwprobe.so 把 DllGetClassObject 导出成**同名动态符号**，
 * LD_PRELOAD 才会介入；driver.c 走的是 dlsym，测不出这一点。
 *
 * 2026-10-03：这个测试就是为那个 bug 加的回归（当时 nm -D 里没有这三个符号，
 * 直接链接的调用方完全绕过探针，日志里 vtable 一直是 0 行）。
 */
#include <stdint.h>
#include <stdio.h>

/* 直接链接：链接时在 libfakecom.so 里找到，运行时由动态链接器解析 —— 可被 LD_PRELOAD 介入 */
extern long DllGetClassObject(const void *, const void *, void **);

typedef long (*create_fn)(void *, void *, const void *, void **);

int main(void)
{
    unsigned char clsid[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    unsigned char iid[16] = { 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 };
    void *fac = NULL, *obj = NULL, **vt;
    create_fn create;
    long hr;

    hr = DllGetClassObject(clsid, iid, &fac);
    printf("DllGetClassObject -> hr=%ld factory=%p\n", hr, fac);
    if (hr != 0 || !fac)
        return 1;

    create = (create_fn)(*(void ***)fac)[3];   /* COM 约定：先取虚表指针 */
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
