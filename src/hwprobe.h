/*
 * hwprobe.h -- hwprobe.c 与 vthook.c 之间共享的最小接口
 */
#ifndef HWPROBE_H
#define HWPROBE_H

#include <stddef.h>
#include <stdint.h>

/* 日志（hwprobe.c 里实现；不用 stdio 之外的额外缓冲） */
void hwprobe_plog(const char *fmt, ...);
int hwprobe_logfd(void);

/* 未被包装的真实函数 */
void *hwprobe_lookup_real(const char *name);
void *hwprobe_real_dlopen(const char *file, int flags);
void *hwprobe_real_dlsym(void *handle, const char *name);

/* vthook.c：COM 层观测 */
void vthook_init(void);
void vthook_wrap_factory(void **ppv_factory);
void vthook_summary(void);
void hwprobe_vt_guid(const void *g, char *out, size_t n);

#endif /* HWPROBE_H */
