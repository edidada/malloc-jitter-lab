#ifndef MJL_PIN_CPU_H
#define MJL_PIN_CPU_H

/*
 * pin_cpu.h —— CPU 亲和性 / 实时调度工具
 *
 * 知识点：
 * 1. 复现内存抖动必须先把线程绑核（pthread_setaffinity_np），
 *    否则线程在核间迁移本身就会引入数十 us 抖动，污染实验结果。
 * 2. SCHED_FIFO 实时调度 + 不同 prio（1~99）才能构造出真实的
 *    优先级反转场景；默认 SCHED_OTHER 下所有线程同优先级，
 *    CFS 调度器不会表现出"高优先级被低优先级持锁阻塞"。
 * 3. 需要 root（或 CAP_SYS_NICE）才能设置 SCHED_FIFO；
 *    无权限时自动降级为 SCHED_OTHER，实验仍可运行但现象减弱。
 */

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static inline int mjl_pin_to_cpu(pthread_t t, int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(t, sizeof(set), &set);
}

static inline int mjl_pin_self_to_cpu(int cpu)
{
    return mjl_pin_to_cpu(pthread_self(), cpu);
}

/* 设置实时调度策略；失败（无 root）返回 -1 并降级 */
static inline int mjl_set_sched(pthread_t t, int policy, int prio)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    int rc = pthread_setschedparam(t, policy, &sp);
    if (rc != 0) {
        fprintf(stderr, "[sched] set policy=%d prio=%d failed: %s "
                        "(run as root for full effect, degrading to SCHED_OTHER)\n",
                policy, prio, strerror(rc));
        return -1;
    }
    return 0;
}

#endif /* MJL_PIN_CPU_H */
