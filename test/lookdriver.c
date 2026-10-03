/*
 * G 段测试宿主：制造几次 dlsym / dlvsym 查找，其中
 *   - 无关名字（sin / cos / pow）→ 只应进"查找名"统计，不逐条打日志
 *   - 目标名字（NvEncodeAPICreateInstance）→ 应另有一条逐次记录
 * 用来验证"那些 dlvsym 到底在找什么"这个问题现在能从汇总里读出来。
 *
 * 背景（2026-10-03）：日志里只有 `dlvsym 77 次 / DllGetClassObject 0 次`，没有名字，
 * 无法判断那 77 次是否与编码器有关；全量记录（HWPROBE_ALL=1）能看名字但会刷屏。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>

int main(void)
{
    void *h = dlopen("libm.so.6", RTLD_NOW);
    if (!h) {
        printf("libm 打不开\n");
        return 1;
    }
    (void)dlsym(h, "sin");
    (void)dlsym(h, "cos");
    (void)dlsym(h, "cos");                 /* 重复 → 计数应为 2 */
    (void)dlvsym(h, "pow", "GLIBC_2.29");  /* 走 dlvsym 路径，也要计入 */
    (void)dlsym(RTLD_DEFAULT, "NvEncodeAPICreateInstance"); /* 目标名 */
    dlclose(h);
    printf("lookdriver done\n");
    return 0;
}
