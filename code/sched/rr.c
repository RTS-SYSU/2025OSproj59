#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/deadline.h>
#include <linux/sched/rt.h>
#include <linux/sched/fair.h>
#include <linux/sched/idle.h>
#include <linux/sched/clock.h>
#include <linux/sched/sysctl.h>
#include <linux/sched/stat.h>
#include <linux/sched/nohz.h>
#include <linux/sched/topology.h>
#include <linux/sched/wake_q.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/wait.h>
#include <linux/sched/mm.h>
#include <linux/sched/coredump.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/task_group.h>
#include <linux/sched/task_ravg.h>
#include <linux/sched/task_state.h>
#include <linux/sched/task_io.h>
#include <linux/sched/task_numa.h>
#include <linux/sched/task_power.h>
#include <linux/sched/task_rt.h>
#include <linux/sched/task_wake.h>

// 定义轮转调度类
struct rr_sched_class {
    struct task_struct *curr; // 当前运行的进程
    struct list_head runqueue; // 就绪队列
    int time_slice; // 时间片大小
};

// 初始化轮转调度类
static void rr_init(struct rr_sched_class *rr, int time_slice)
{
    rr->curr = NULL;
    INIT_LIST_HEAD(&rr->runqueue);
    rr->time_slice = time_slice;
}

// 将进程加入就绪队列
static void rr_enqueue_task(struct rr_sched_class *rr, struct task_struct *p)
{
    list_add_tail(&p->run_list, &rr->runqueue);
}

// 从就绪队列中移除进程
static void rr_dequeue_task(struct rr_sched_class *rr, struct task_struct *p)
{
    list_del_init(&p->run_list);
}

// 选择下一个要运行的进程
static struct task_struct *rr_pick_next_task(struct rr_sched_class *rr)
{
    struct task_struct *p = NULL;

    if (!list_empty(&rr->runqueue)) {
        p = list_first_entry(&rr->runqueue, struct task_struct, run_list);
        list_del_init(&p->run_list);
    }

    return p;
}

// 时间片用完后的处理
static void rr_task_tick(struct rr_sched_class *rr, struct task_struct *curr)
{
    if (curr) {
        curr->time_slice--;
        if (curr->time_slice <= 0) {
            // 时间片用完，将当前进程放回就绪队列
            rr_enqueue_task(rr, curr);
            curr = NULL;
        }
    }
}

// 调度器的入口函数
static void rr_schedule(struct rr_sched_class *rr)
{
    struct task_struct *next;

    if (rr->curr) {
        rr_task_tick(rr, rr->curr);
    }

    next = rr_pick_next_task(rr);
    if (next) {
        rr->curr = next;
        next->time_slice = rr->time_slice; // 重置时间片
        // 切换到新进程
        context_switch(rr->curr, next);
    }
}

// 注册调度类
static struct rr_sched_class rr_sched_class;

static int __init rr_init_module(void)
{
    rr_init(&rr_sched_class, 10); // 初始化轮转调度类，时间片大小为 10
    printk(KERN_INFO "RR scheduler initialized\n");
    return 0;
}

static void __exit rr_exit_module(void)
{
    printk(KERN_INFO "RR scheduler unloaded\n");
}

module_init(rr_init_module);
module_exit(rr_exit_module);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple Round-Robin Scheduler");
MODULE_AUTHOR("Your Name");
