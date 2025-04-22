# AJLR 缓存感知调度器：Linux 内核实现指南

## 简介

本文档提供了将 AJLR (Adaptive Job-Level Reclaiming) 缓存感知调度算法实现到 Linux 内核的详细指南。AJLR 是一种旨在通过充分利用缓存亲和性来优化任务分配和调度的算法，它基于任务在不同 CPU 核心上的预期执行时间差异（"加速值"）来做出调度决策。

## 第一阶段：准备工作

### 获取 Linux 内核源码

```bash
git clone https://github.com/torvalds/linux.git
cd linux
git checkout v6.1
```

### 创建开发分支

```bash
git checkout -b ajlr-scheduler
```

### 安装内核开发依赖项

```bash
# 在 Ubuntu/Debian 上
sudo apt-get install build-essential libncurses-dev bison flex libssl-dev libelf-dev
```

## 第二阶段：文件创建与修改

### 创建 AJLR 调度器的配置选项

修改 `kernel/Kconfig`，添加：

```bash
config SCHED_AJLR
    bool "AJLR Cache-Aware Scheduler Class"
    depends on SMP
    default n
    help
      This option enables the AJLR cache-aware scheduler which
      optimizes task placement on CPUs based on cache utilization
      and tasks' cache footprints to minimize cache-related delays.

      If unsure, say N.
```

### 创建 AJLR 调度器的头文件

创建文件 `include/linux/sched/ajlr.h`：

```c
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_AJLR_H
#define _LINUX_SCHED_AJLR_H

#include <linux/sched.h>

#ifdef CONFIG_SCHED_AJLR

/* AJLR 任务的特定数据，嵌入在 task_struct 中 */
struct ajlr_task {
    long wcet;                  /* 最坏情况执行时间 */
    int last_cpu;               /* 上次执行的 CPU */
    struct list_head run_node;  /* 运行队列的链表节点 */
    bool scheduled;             /* 是否已被调度 */
};

/* 每个 CPU 的 AJLR 运行队列 */
struct ajlr_rq {
    raw_spinlock_t lock;        /* 保护运行队列的锁 */
    struct list_head queue;     /* AJLR 任务的运行队列 */
    unsigned int nr_running;    /* 运行队列中的任务数 */
    struct task_struct *curr;   /* 当前运行的 AJLR 任务 */
};

/* AJLR 调度器配置参数 */
#define AJLR_CACHE_BONUS_PCT 10 /* 缓存命中带来的执行时间减少百分比 */

/* 函数声明 */
extern void init_ajlr_rq(struct ajlr_rq *ajlr_rq);
extern long ajlr_compute_speedup(struct task_struct *p, int cpu);
extern long ajlr_get_wcet(struct task_struct *p);
extern int ajlr_set_wcet(struct task_struct *p, long wcet);

#endif /* CONFIG_SCHED_AJLR */
#endif /* _LINUX_SCHED_AJLR_H */
```

### 修改 task_struct 结构体

修改 `include/linux/sched.h`，在 `task_struct` 中添加 AJLR 相关字段：

```c
struct task_struct {
    /* ... 现有字段 ... */

#ifdef CONFIG_SCHED_AJLR
    struct ajlr_task     ajlr;  /* AJLR 调度器特定数据 */
#endif
    
    /* ... 现有字段 ... */
};
```

同时添加新的调度策略常量：

```c
/* 调度策略 */
#define SCHED_NORMAL        0
#define SCHED_FIFO          1
#define SCHED_RR            2
#define SCHED_BATCH         3
/* ... 其他现有策略 ... */
#define SCHED_AJLR          9  /* 新的 AJLR 策略 */
```

### 修改运行队列结构体

修改 `kernel/sched/sched.h`，在 `rq` 结构中添加 AJLR 运行队列：

```c
struct rq {
    /* ... 现有字段 ... */

#ifdef CONFIG_SCHED_AJLR
    struct ajlr_rq        ajlr;  /* AJLR 运行队列 */
#endif
    
    /* ... 现有字段 ... */
};
```

同时添加 AJLR 调度类的前向声明：

```c
#ifdef CONFIG_SCHED_AJLR
extern const struct sched_class ajlr_sched_class;
#endif
```

### 实现 AJLR 调度器核心文件

创建文件 `kernel/sched/ajlr.c`。由于该文件较长，我们分段说明主要实现：

#### 文件头和初始化函数

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * AJLR Cache-Aware Scheduler Class Implementation
 */
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <linux/sched/topology.h>
#include <linux/sched/ajlr.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/profile.h>
#include <linux/interrupt.h>
#include <linux/mempolicy.h>
#include <linux/mutex.h>
#include <linux/kernel.h>

/*
 * 初始化 AJLR 运行队列
 */
void init_ajlr_rq(struct ajlr_rq *ajlr_rq)
{
    raw_spin_lock_init(&ajlr_rq->lock);
    INIT_LIST_HEAD(&ajlr_rq->queue);
    ajlr_rq->nr_running = 0;
    ajlr_rq->curr = NULL;
}
```

#### 核心启发式函数

```c
/*
 * 缓存感知启发式函数：计算任务在特定 CPU 上的 "加速" 值
 * 移植自 ajlrtest.c 中的 compute_et_heuristic 函数
 */
long ajlr_compute_speedup(struct task_struct *p, int cpu)
{
    long wcet = ajlr_get_wcet(p);
    
    /* 如果未设置 WCET，返回 0 （无加速） */
    if (wcet <= 0)
        return 0;
    
    long estimated_et = wcet;
    struct rq *rq = cpu_rq(cpu);
    
    /* 
     * 基础版缓存亲和性逻辑：
     * 如果任务上次在此 CPU 上运行，则预期执行时间减少 
     */
    if (p->ajlr.last_cpu == cpu && p->ajlr.last_cpu != -1) {
        estimated_et -= (wcet * AJLR_CACHE_BONUS_PCT) / 100;
    }
    
    /* 考虑 CPU 当前负载 */
    estimated_et += rq->nr_running * 5; /* 每个排队任务增加 5 单位执行时间 */
    
    /* 计算加速值（WCET - 估计执行时间） */
    long speedup = wcet - estimated_et;
    
    return (speedup > 0) ? speedup : 0;
}

/*
 * 获取任务的 WCET
 */
long ajlr_get_wcet(struct task_struct *p)
{
    return p->ajlr.wcet;
}

/*
 * 设置任务的 WCET
 */
int ajlr_set_wcet(struct task_struct *p, long wcet)
{
    if (wcet < 0)
        return -EINVAL;
    
    p->ajlr.wcet = wcet;
    return 0;
}
```

#### AJLR 调度类回调函数

```c
/*
 * 以下是 AJLR 调度类实现的各个回调函数
 */

/* 将任务入队到 AJLR 运行队列 */
static void enqueue_task_ajlr(struct rq *rq, struct task_struct *p, int flags)
{
    struct ajlr_rq *ajlr_rq = &rq->ajlr;
    
    raw_spin_lock(&ajlr_rq->lock);
    
    /* 如果任务尚未入队 */
    if (list_empty(&p->ajlr.run_node)) {
        list_add_tail(&p->ajlr.run_node, &ajlr_rq->queue);
        p->ajlr.scheduled = true;
        ajlr_rq->nr_running++;
    
        /* 用于调试 */
        trace_printk("AJLR: Enqueued task %d on CPU %d\n", p->pid, cpu_of(rq));
    }
    
    raw_spin_unlock(&ajlr_rq->lock);
}

/* 从 AJLR 运行队列中移除任务 */
static void dequeue_task_ajlr(struct rq *rq, struct task_struct *p, int flags)
{
    struct ajlr_rq *ajlr_rq = &rq->ajlr;
    
    raw_spin_lock(&ajlr_rq->lock);
    
    /* 如果任务在队列中 */
    if (!list_empty(&p->ajlr.run_node) && p->ajlr.scheduled) {
        list_del_init(&p->ajlr.run_node);
        p->ajlr.scheduled = false;
        ajlr_rq->nr_running--;
        
        /* 用于调试 */
        trace_printk("AJLR: Dequeued task %d from CPU %d\n", p->pid, cpu_of(rq));
    }
    
    raw_spin_unlock(&ajlr_rq->lock);
}

/* AJLR 任务让出处理器 */
static void yield_task_ajlr(struct rq *rq)
{
    /* 
     * 实现让出逻辑
     * 当前任务将自己重新添加到队列尾部
     */
    struct task_struct *curr = rq->curr;
    struct ajlr_rq *ajlr_rq = &rq->ajlr;
    
    raw_spin_lock(&ajlr_rq->lock);
    
    if (curr && rq->curr == curr && ajlr_rq->nr_running > 1) {
        /* 从队列中移除 */
        if (!list_empty(&curr->ajlr.run_node)) {
            list_del_init(&curr->ajlr.run_node);
            ajlr_rq->nr_running--;
        }
        
        /* 重新添加到队列末尾 */
        list_add_tail(&curr->ajlr.run_node, &ajlr_rq->queue);
        ajlr_rq->nr_running++;
        
        /* 设置 TIF_NEED_RESCHED 标志 */
        set_tsk_need_resched(curr);
    }
    
    raw_spin_unlock(&ajlr_rq->lock);
}

/* 检查是否应当抢占当前任务 */
static void check_preempt_curr_ajlr(struct rq *rq, struct task_struct *p, int flags)
{
    /* 
     * 实现抢占逻辑
     * 简单版本：AJLR 总是允许抢占
     */
    if (p->prio < rq->curr->prio) {
        resched_curr(rq);
        return;
    }
}

/* 选择下一个要运行的 AJLR 任务 */
static struct task_struct *pick_next_task_ajlr(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
    struct ajlr_rq *ajlr_rq = &rq->ajlr;
    struct task_struct *next = NULL;
    
    if (!ajlr_rq->nr_running)
        return NULL;
    
    raw_spin_lock(&ajlr_rq->lock);
    
    /* 从队列头部取出下一个任务 */
    if (!list_empty(&ajlr_rq->queue)) {
        struct ajlr_task *ajlr_task;
        struct task_struct *task;
        
        task = list_first_entry(&ajlr_rq->queue, struct task_struct, ajlr.run_node);
        next = task;
        ajlr_rq->curr = next;
    }
    
    raw_spin_unlock(&ajlr_rq->lock);
    
    return next;
}

/* 当前任务要放弃 CPU 时调用 */
static void put_prev_task_ajlr(struct rq *rq, struct task_struct *p)
{
    struct ajlr_rq *ajlr_rq = &rq->ajlr;
    
    /* 记录 last_cpu */
    p->ajlr.last_cpu = cpu_of(rq);
    
    /* 如果当前任务是 ajlr_rq->curr，则清除 */
    if (ajlr_rq->curr == p)
        ajlr_rq->curr = NULL;
}

/* 新任务设置为当前任务时调用 */
static void set_curr_task_ajlr(struct rq *rq)
{
    struct task_struct *p = rq->curr;
    
    /* 记录 last_cpu */
    p->ajlr.last_cpu = cpu_of(rq);
}

/* 处理时钟滴答，更新任务的时间片或重新调度 */
static void task_tick_ajlr(struct rq *rq, struct task_struct *curr, int queued)
{
    /* 
     * 实现时钟滴答处理逻辑
     * 简单版本：始终允许任务运行一个完整时间片 (100ms)
     */
    if (--curr->rt.time_slice <= 0) {
        curr->rt.time_slice = HZ / 10; /* 重置为 100ms */
        
        /* 仅当队列中有其他任务时重新调度 */
        if (rq->ajlr.nr_running > 1)
            resched_curr(rq);
    }
}
```

#### 任务分配和 CPU 选择

```c
/* 为 AJLR 任务选择最合适的 CPU - 核心函数 */
static int select_task_rq_ajlr(struct task_struct *p, int prev_cpu, int sd_flag, int wake_flags)
{
    int cpu;
    long max_speedup = LONG_MIN;
    int best_cpu = prev_cpu; /* 默认回退到上一个 CPU */
    
    /* 
     * 实现类似于 allocate_tasks_c 的核心分配逻辑
     * 寻找能为任务提供最大 "加速" 的 CPU
     */
    for_each_online_cpu(cpu) {
        /* 检查 CPU 是否可用于此任务 */
        if (!cpumask_test_cpu(cpu, p->cpus_ptr))
            continue;
        
        /* 调用缓存感知启发式计算该 CPU 的 "加速" 值 */
        long speedup = ajlr_compute_speedup(p, cpu);
        
        if (speedup > max_speedup) {
            max_speedup = speedup;
            best_cpu = cpu;
        }
    }
    
    trace_printk("AJLR: Selected CPU %d for task %d (speedup: %ld)\n", 
                 best_cpu, p->pid, max_speedup);
    
    return best_cpu;
}

/* 当任务优先级变化时的处理 */
static void prio_changed_ajlr(struct rq *rq, struct task_struct *p, int oldprio)
{
    /* 如果优先级提高，可能需要重新调度 */
    if (p->prio < oldprio && rq->curr->prio > p->prio)
        resched_curr(rq);
}

/* 当任务切换到 AJLR 调度类时的处理 */
static void switched_to_ajlr(struct rq *rq, struct task_struct *p)
{
    /* 初始化 AJLR 特定字段 */
    if (p->ajlr.wcet <= 0)
        p->ajlr.wcet = 100; /* 默认值 */
    
    if (p->ajlr.last_cpu == -1)
        p->ajlr.last_cpu = cpu_of(rq);
    
    /* 如果任务可运行且在当前 CPU 上，可能需要重新调度 */
    if (task_on_rq_queued(p) && rq->curr != p) {
        if (p->prio < rq->curr->prio)
            resched_curr(rq);
    }
}
```

#### 调度类定义

```c
/* 定义 AJLR 调度类 */
const struct sched_class ajlr_sched_class = {
    .next                   = &fair_sched_class,
    .enqueue_task           = enqueue_task_ajlr,
    .dequeue_task           = dequeue_task_ajlr,
    .yield_task             = yield_task_ajlr,
    
    .check_preempt_curr     = check_preempt_curr_ajlr,
    
    .pick_next_task         = pick_next_task_ajlr,
    .put_prev_task          = put_prev_task_ajlr,
    
    .set_curr_task          = set_curr_task_ajlr,
    
    .task_tick              = task_tick_ajlr,
    
    .prio_changed           = prio_changed_ajlr,
    .switched_to            = switched_to_ajlr,
    
    .select_task_rq         = select_task_rq_ajlr,
};
```

### 修改调度器层次结构

修改 `kernel/sched/core.c`，将 AJLR 调度类插入到调度类层次结构中：

```c
#ifdef CONFIG_SCHED_AJLR
/* rt_sched_class 之后，fair_sched_class 之前 */
DEFINE_SCHED_CLASS_HIGHEST(stop);
DEFINE_SCHED_CLASS(dl);
DEFINE_SCHED_CLASS(rt);
DEFINE_SCHED_CLASS(ajlr);
DEFINE_SCHED_CLASS(fair);
DEFINE_SCHED_CLASS(idle);
#else
/* 原始层次结构 */
DEFINE_SCHED_CLASS_HIGHEST(stop);
DEFINE_SCHED_CLASS(dl);
DEFINE_SCHED_CLASS(rt);
DEFINE_SCHED_CLASS(fair);
DEFINE_SCHED_CLASS(idle);
#endif
```

并在 `sched_init` 函数中添加 AJLR 运行队列初始化：

```c
void __init sched_init(void)
{
    int i;
    
    /* ... 现有代码 ... */
    
    for_each_possible_cpu(i) {
        struct rq *rq = cpu_rq(i);
        
        /* ... 现有运行队列初始化 ... */
        
#ifdef CONFIG_SCHED_AJLR
        init_ajlr_rq(&rq->ajlr);
#endif
    }
    
    /* ... 现有代码 ... */
}
```

### 更新 Makefile

修改 `kernel/sched/Makefile`，添加：

```makefile
obj-$(CONFIG_SCHED_AJLR) += ajlr.o
```

### 编译和运行测试程序

```bash
gcc -o test_ajlr test_ajlr.c
./test_ajlr
```

## 第四阶段：增强和优化

### 当前实现与 Java 原型代码的差距分析

当前的 AJLR 实现（基于 C 语言原型并增加了 LCIF 机制）已实现了 Java 版本功能的约 80%。以下是实现情况：

#### 已实现的功能（80%）

* **基本数据结构：**
   * C版本定义了`task_struct_simplified`结构体，对应Java中的`Node`类的核心属性
   * 基本的任务属性如ID、WCET和上次执行的CPU均已实现

* **缓存感知启发式：**
   * **compute_et_heuristic**函数实现了基础的缓存感知逻辑
   * 考虑了上次执行CPU的缓存亲和性（减少10%的WCET）

* **核心分配算法：**
   * **allocate_tasks_c**构建了与Java版本相同的"加速表"(speedUpTable)
   * 实现了贪婪算法，为任务找到最大"加速"的CPU
   
* **LCIF机制：**
   * 通过**compute_cache_impact**函数实现了Java版本中的LCIF逻辑
   * 包括识别提供相似"加速"的CPU集合（差异在5%以内）
   * 评估新任务对每个候选CPU上现有任务的影响
   * 选择总影响最小的CPU，最大限度减少缓存冲突

#### 未实现或简化的功能（20%）

* **任务排序：**
   * Java版本使用**Utils.compareNode**方法按优先级排序就绪任务
   * C实现中仍然缺少优先级排序逻辑

* **更复杂的缓存模型：**
   * Java版本使用**computeET**方法考虑多级缓存历史
   * C版本的缓存模型仍然相对简化，没有考虑：
     * 多级缓存历史（`history_level1`, `history_level2`, `history_level3`）
     * 缓存大小限制和更复杂的任务间缓存交互

* **处理器拓扑感知：**
   * C版本尚未实现对处理器拓扑的感知（如共享缓存的核心组）
   * 没有实现Java版本中的集群感知逻辑（如"proc / 4"计算集群ID）

* **批量调度能力：**
   * Java版本可以一次性为多个任务找到全局最优分配方案
   * C版本的实现仍然是一次处理一个任务

### 优先需要改进的方向

为进一步完善AJLR实现，建议按以下优先级实现剩余功能：

* **增强缓存模型：** 实现多级缓存历史记录，参考Java版本的三级历史结构，并考虑缓存大小限制
  
* **添加任务排序：** 实现类似Java版本**compareNode**的任务优先级排序机制
  
* **处理器拓扑感知：** 利用Linux内核的拓扑接口，识别和利用共享缓存的CPU组
  
* **批量调度优化：** 实现全局优化函数，能够一次性为多个任务找到最优分配方案

### 当前缓存模型与 RTNS 论文方法的差距

当前 AJLR 实现中的缓存感知计算采用了简化的启发式方法，与 RTNS 论文中提出的更精细模型存在明显差距：

* **固定缓存命中比例问题：** 当前实现中使用固定比例（10%）来估计缓存命中带来的执行时间减少：
```c
// 假设缓存命中可减少约10%的执行时间
estimated_et -= wcet / 10;
```
而 RTNS 论文中提出的方法采用了基于"缓存相关抢占延迟(CRPD)"的更精确计算，考虑了具体的缓存状态和任务内存访问模式。
  
* **任务干扰计算简化：** 当前实现简单地为每个并发任务增加固定的执行时间：
```c
if (running_tasks && num_running > 0) {
    estimated_et += num_running * 5; // 每个任务增加5单位时间
}
```
而 RTNS 论文强调需要分析任务间的实际缓存冲突集合，评估共享缓存竞争带来的具体性能影响，而不是简单的线性叠加。
  
* **缺少缓存特性感知：** 当前模型没有考虑不同级别缓存（L1/L2/L3）的特性差异，也没有反映缓存容量和替换策略对任务执行的影响。论文中指出，精确的缓存模型需要考虑这些因素。

为使 AJLR 实现更符合 RTNS 论文中的理论基础，应将当前简化的启发式方法替换为更精确的缓存模型：

* 实现缓存状态跟踪机制，记录各任务的内存访问集合
* 分析任务间的缓存冲突域，评估实际干扰程度
* 考虑缓存替换策略和容量限制对执行时间的影响
* 利用 Linux 性能计数器收集实际缓存使用数据，验证模型准确性

这些改进将使我们的缓存感知计算从简单启发式向精确模型转变，更好地反映 RTNS 论文中提出的缓存感知调度理论。

## 关键注意事项

### 内核调试
* 使用 `trace_printk` 进行调试（在最终版本中应移除）
* 使用 `ftrace` 跟踪调度器行为

### 代码优化
* 确保 `ajlr_compute_speedup` 函数高效，因为它在关键路径上
* 适当使用锁保护数据，但避免长时间持有锁

### 测试方法
* 首先测试简单场景：单个任务切换到 AJLR 策略
* 然后测试更复杂场景：多个 AJLR 任务竞争 CPU
* 最后测试与其他调度类的交互

### 渐进式开发
* 从最简单的功能集开始，逐步添加功能
* 每个阶段都进行全面测试

## 总结

通过遵循本指南，可以将 AJLR 缓存感知调度算法实现为 Linux 内核中的一个新调度类。本实现通过识别和利用缓存亲和性，可以提高实时任务的执行效率，减少缓存相关的延迟，从而提高系统整体性能。
