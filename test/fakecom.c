/*
 * fakecom.c -- 测试用的最小 COM 风格模块
 *
 * 为什么需要它：真的 broadcast-core.so 只有等 QQ 开共享才会创建接口，无法用来
 * 回归验证探针。这里造一个同样形状的东西：DllGetClassObject 返回一个 5 槽工厂，
 * 工厂的 CreateInstance 返回一个 9 槽接口对象。
 *
 * 其中 m8 故意要 7 个整数参数（第 7 个走栈）—— 用来验证转发桩**没有挪动 rsp**：
 * 如果实现里把参数压栈再调用真实函数，栈上的那个参数就会错位，m8 的返回值立刻不对。
 */
#include <stddef.h>

/* ---- 接口对象：第一个字段必须是 vtable 指针（COM 约定） ---- */
struct fake_obj {
    void **vt;
    long tag;
};

static void *obj_vt[9];
static void *obj_storage[1];      /* 让对象地址看起来像真的分配出来的 */

static long m0(void *self, long a) { (void)self; return a + 1; }
static long m1(void *self, long a) { (void)self; return a * 2; }
static long m2(void *self) { (void)self; return 42; }
static long m3(void *self) { (void)self; return 0; }        /* 假扮「硬件不受支持」 */
static long m4(void *self, void *p) { (void)self; return p ? 1 : 0; }
static long m5(void *self, long a, long b) { (void)self; return a - b; }
static long m6(void *self) { (void)self; return 7; }
static long m7(void *self) { (void)self; return 8; }
static long m8(void *self, long a, long b, long c, long d, long e, long f, long g)
{
    (void)self;
    return a * 1000000 + b * 100000 + c * 10000 + d * 1000 + e * 100 + f * 10 + g;
}

static struct fake_obj *get_obj(void)
{
    struct fake_obj *o = (struct fake_obj *)obj_storage;

    if (!obj_vt[0]) {
        obj_vt[0] = (void *)m0;
        obj_vt[1] = (void *)m1;
        obj_vt[2] = (void *)m2;
        obj_vt[3] = (void *)m3;
        obj_vt[4] = (void *)m4;
        obj_vt[5] = (void *)m5;
        obj_vt[6] = (void *)m6;
        obj_vt[7] = (void *)m7;
        obj_vt[8] = (void *)m8;
        o->tag = 0x5eed;
    }
    o->vt = obj_vt;
    return o;
}

/* ---- 类工厂：5 个槽（IUnknown 3 个 + CreateInstance + LockServer） ---- */
static void *fac_vt[5];
static void *fac_storage[1];
static void *g_fac_ptr;           /* 指向 fac_vt 的「工厂对象」 */

static long f_qi(void *self, const void *riid, void **ppv)
{
    (void)riid;
    if (ppv)
        *ppv = self;
    return 0;
}
static unsigned long f_addref(void *self) { (void)self; return 2; }
static unsigned long f_release(void *self) { (void)self; return 1; }
static long f_create(void *self, void *outer, const void *riid, void **ppv)
{
    (void)self;
    (void)outer;
    (void)riid;
    if (!ppv)
        return -1;
    *ppv = get_obj();
    return 0;
}
static long f_lock(void *self, int lock)
{
    (void)self;
    return lock;
}

long DllGetClassObject(const void *clsid, const void *iid, void **out)
{
    (void)clsid;
    (void)iid;
    if (!out)
        return -1;
    if (!fac_vt[0]) {
        fac_vt[0] = (void *)f_qi;
        fac_vt[1] = (void *)f_addref;
        fac_vt[2] = (void *)f_release;
        fac_vt[3] = (void *)f_create;
        fac_vt[4] = (void *)f_lock;
        g_fac_ptr = (void *)fac_vt;
        fac_storage[0] = g_fac_ptr;
    }
    *out = &fac_storage[0];
    return 0;
}
