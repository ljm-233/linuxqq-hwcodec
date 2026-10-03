/*
 * 复现 Electron/Chromium 的进程模型，验证探针的日志在这种进程里不会丢。
 *
 * 收帧进程（--type=ppapi）不是 exec 出来的新进程，而是主进程 **fork** 出来的：
 *   - 构造函数不会再跑（所以每个进程块开头那几行不会出现）
 *   - Chromium 会关掉继承来的、它不认识的文件描述符 —— 探针打开的那个日志 fd 就这样没了
 * 2026-10-03 实测：ppapi 进程里探针明明在（maps 里有），HWPROBE_LOG 也设了，却一行没写。
 *
 * 这个测试做的就是：fork → 关掉所有 fd≥3 → 再去 dlopen 一个"值得记"的库 → 看日志还在不在。
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int close_extra_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int closed = 0;
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        int fd = atoi(e->d_name);
        if (fd > 2 && fd != dirfd(d)) {
            close(fd);
            closed++;
        }
    }
    closedir(d);
    return closed;
}

int main(void)
{
    pid_t pid;

    printf("父 pid=%d（构造函数应已跑过）\n", (int)getpid());
    fflush(stdout);

    pid = fork();
    if (pid == 0) {
        int c = close_extra_fds();
        printf("子 pid=%d：关掉了 %d 个 fd（模拟 Chromium 的 fd 清理）\n", (int)getpid(), c);
        fflush(stdout);
        (void)dlopen("libopenh264.so", RTLD_NOW);   /* 名字命中"值得记"列表 */
        printf("子进程结束（走 exit()，会跑到继承来的 atexit 汇总）\n");
        fflush(stdout);
        exit(0);
    }
    waitpid(pid, NULL, 0);
    printf("子进程 pid=%d 已退出\n", (int)pid);
    return 0;
}
