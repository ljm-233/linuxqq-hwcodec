/*
 * 长命测试宿主：每秒 dlopen/dlclose 一次（用来触发惰性周期摘要），并 fork 一个子进程
 * 做同样的事。用途是回归测试「探针不得因为周期汇总而杀死宿主」。
 *
 * 历史 bug（2026-10-03）：周期汇总用 signal(SIGALRM)+alarm() 实现，本库被注入到每一个
 * 子进程里，于是每个进程 20 秒后被 SIGALRM 的默认动作终止 —— 启动器被当场打死、QQ 起不来。
 * 现在改成惰性打印（不使用任何信号与定时器），本测试就是钉住这一点：
 * 宿主与子进程都必须活到自然结束，退出码 0（旧实现下这里会是 142 = 128+14/SIGALRM）。
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static void loop(const char *tag, int secs)
{
    int i;
    for (i = 0; i < secs; i++) {
        void *h = dlopen("libm.so.6", RTLD_NOW);
        if (h)
            dlclose(h);
        sleep(1);
    }
    printf("%s完成\n", tag);
    fflush(stdout);
}

int main(void)
{
    pid_t p = fork();
    int st = 0;

    if (p == 0) {
        loop("子进程", 9);
        _exit(0);
    }
    loop("宿主", 9);
    if (p < 0) {
        fprintf(stderr, "fork 失败\n");
        return 3;
    }
    if (waitpid(p, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "子进程异常退出：st=%d（被信号杀死？）\n", st);
        return 2;
    }
    return 0;
}
