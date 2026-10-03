/*
 * driver.c -- 测试驱动：像真实调用方那样按 COM 约定走一遍
 *   1) dlopen 目标模块 → dlsym("DllGetClassObject")（这一步会命中探针的 dlsym 包装）
 *   2) 调 DllGetClassObject 拿工厂 → 从工厂虚表第 3 槽调 CreateInstance 拿接口对象
 *   3) 通过接口对象的虚表逐槽调用，检查返回值是否原样穿过转发桩
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

typedef long (*getcls_fn)(const void *, const void *, void **);
typedef long (*create_fn)(void *, void *, const void *, void **);

int main(void)
{
    void *h;
    getcls_fn g;
    create_fn create;
    unsigned char clsid[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    unsigned char iid[16] = { 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 };
    void *fac = NULL, *obj = NULL, **vt;
    long hr;

    h = dlopen("./libfakecom.so", RTLD_NOW);
    if (!h) {
        printf("dlopen 失败: %s\n", dlerror());
        return 1;
    }
    g = (getcls_fn)dlsym(h, "DllGetClassObject");
    if (!g) {
        printf("dlsym 失败: %s\n", dlerror());
        return 1;
    }
    hr = g(clsid, iid, &fac);
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
