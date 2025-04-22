#include <stdio.h>
#include <stdlib.h>
#include <limits.h> // For LONG_MIN
#include <stdbool.h> // For bool type

// --- 简化数据结构定义 ---

// 简化表示一个任务
typedef struct {
    int id;         // 任务唯一标识
    long wcet;      // 最坏情况执行时间 (Worst-Case Execution Time)
    int last_cpu;   // 上次执行该任务的 CPU ID (-1 表示从未执行过)
    // 其他需要的任务属性... (例如优先级, 依赖关系等，这里省略)
} task_struct_simplified;

// --- 极其简化的执行时间预测启发式 ---
// !!! 警告：这是一个高度简化的占位符，远非真实缓存模型 !!!
// 目标：模拟将任务放在特定核心上执行的预期时间
// 简化逻辑：如果任务上次就在这个核心运行，则认为有缓存亲和性，执行时间略微减少
long compute_et_heuristic(task_struct_simplified *task, int core_id, task_struct_simplified **running_tasks, int num_running) {
    long wcet = task->wcet;
    long estimated_et = wcet;
    
    // 检查是否为任务曾经使用过的核心（缓存亲和性）
    if (task->last_cpu == core_id && task->last_cpu != -1) {
        // 假设缓存命中可减少约10%的执行时间
        estimated_et -= wcet / 10;
    }
    
    // 考虑处理器当前负载（每个运行中的任务增加一些开销）
    if (running_tasks && num_running > 0) {
        estimated_et += num_running * 5; // 每个任务增加5单位时间
    }
    
    return estimated_et;
}

// LCIF机制：计算新任务对指定核心上现有任务的影响
long compute_cache_impact(task_struct_simplified *new_task, int core_id, 
                          task_struct_simplified **running_tasks, int num_running) {
    long total_impact = 0;
    
    if (!running_tasks || num_running <= 0) {
        return 0; // 没有正在运行的任务，无影响
    }
    
    for (int i = 0; i < num_running; i++) {
        // 跳过空任务
        if (!running_tasks[i]) continue;
        
        // 计算此任务当前的预期执行时间
        long current_et = compute_et_heuristic(running_tasks[i], core_id, NULL, 0);
        
        // 计算新任务到达后的预期执行时间
        // 这里简化为：每个新任务会导致现有任务的执行时间增加其WCET的5%
        long new_et = current_et + (new_task->wcet * 5 / 100);
        
        // 影响是执行时间的增加
        long impact = new_et - current_et;
        total_impact += impact;
    }
    
    return total_impact;
}

// --- 核心分配逻辑 ---
// 输入:
//   ready_tasks: 指向已排序（按优先级）的就绪任务数组的指针
//   num_ready_tasks: 就绪任务的数量
//   available_core_ids: 指向可用核心 ID 数组的指针
//   num_available_cores: 可用核心的数量
//   assignments: 输出数组，assignments[task_id] 将存储分配的核心 ID，需要调用者分配内存并初始化为 -1
//   max_task_id: assignments 数组的大小（应大于等于所有任务的最大 ID）
//
// 输出:
//   填充 assignments 数组，将任务 ID 映射到分配的核心 ID
//   返回成功分配的任务数量
int allocate_tasks_c(task_struct_simplified *ready_tasks, int num_ready_tasks,
                     int *available_core_ids, int num_available_cores,
                     int *assignments, int max_task_id) {

    if (num_ready_tasks == 0 || num_available_cores == 0) {
        return 0; // 没有任务或没有核心可用
    }

    // 确定要考虑分配的任务数量（不超过可用核心数）
    int num_to_consider = num_ready_tasks < num_available_cores ? num_ready_tasks : num_available_cores;

    // --- 计算 "Speedup" 表 ---
    // speedup_table[i][j] 表示将第 i 个就绪任务分配给第 j 个可用核心的 "加速" 值
    // 使用动态分配以适应不同数量的任务和核心
    long **speedup_table = (long **)malloc(num_to_consider * sizeof(long *));
    if (!speedup_table) return -1; // 内存分配失败
    for (int i = 0; i < num_to_consider; i++) {
        speedup_table[i] = (long *)malloc(num_available_cores * sizeof(long));
        if (!speedup_table[i]) {
            // 清理已分配的内存
            for(int k=0; k<i; k++) free(speedup_table[k]);
            free(speedup_table);
            return -1; // 内存分配失败
        }
    }

    // 跟踪哪些任务和核心已被分配
    int *task_assigned = (int *)calloc(num_to_consider, sizeof(int));
    int *core_assigned = (int *)calloc(num_available_cores, sizeof(int));
    if (!task_assigned || !core_assigned) {
        // 清理已分配的内存
        for (int i = 0; i < num_to_consider; i++) free(speedup_table[i]);
        free(speedup_table);
        free(task_assigned);
        free(core_assigned);
        return -1; // 内存分配失败
    }
    
    // 跟踪每个核心上运行的任务
    task_struct_simplified **core_running_tasks[num_available_cores];
    int core_task_counts[num_available_cores];
    
    // 初始化每个核心的运行任务数组
    for (int j = 0; j < num_available_cores; j++) {
        core_running_tasks[j] = (task_struct_simplified **)calloc(num_to_consider, sizeof(task_struct_simplified *));
        core_task_counts[j] = 0;
        if (!core_running_tasks[j]) {
            // 清理已分配的内存
            for (int k = 0; k < j; k++) free(core_running_tasks[k]);
            for (int i = 0; i < num_to_consider; i++) free(speedup_table[i]);
            free(speedup_table); free(task_assigned); free(core_assigned);
            return -1; // 内存分配失败
        }
    }

    // 填充加速表
    for (int i = 0; i < num_to_consider; i++) { // 遍历候选任务
        task_struct_simplified *task = &ready_tasks[i];
        for (int j = 0; j < num_available_cores; j++) { // 遍历可用核心
            int core_id = available_core_ids[j];
            
            // 计算任务在此核心上的预期执行时间
            long estimated_et = compute_et_heuristic(task, core_id, 
                                                   core_running_tasks[j], 
                                                   core_task_counts[j]);
            
            // 计算"加速"值（WCET - 预期执行时间）
            speedup_table[i][j] = task->wcet - estimated_et;
        }
    }

    // 任务分配计数
    int assigned_count = 0;

    // 按贪心策略为任务分配核心
    while (assigned_count < num_to_consider) {
        // 寻找最大加速值
        long max_speedup = LONG_MIN;
        int best_task = -1, best_core = -1;
        
        for (int i = 0; i < num_to_consider; i++) {
            if (task_assigned[i]) continue; // 跳过已分配的任务
            
            for (int j = 0; j < num_available_cores; j++) {
                if (core_assigned[j]) continue; // 跳过已分配的核心
                
                if (speedup_table[i][j] > max_speedup) {
                    max_speedup = speedup_table[i][j];
                    best_task = i;
                    best_core = j;
                }
            }
        }
        
        if (best_task == -1 || best_core == -1) {
            break; // 无法找到更多有效的分配
        }
        
        // 检查是否有多个核心提供相同的最大加速 (LCIF机制)
        // 这里定义"相同"为差异小于等于5%
        int candidate_cores[num_available_cores];
        int num_candidates = 0;
        long threshold = max_speedup - (max_speedup * 5 / 100); // 允许5%的差异
        
        for (int j = 0; j < num_available_cores; j++) {
            if (core_assigned[j]) continue; // 跳过已分配的核心
            
            if (speedup_table[best_task][j] >= threshold) {
                candidate_cores[num_candidates++] = j;
            }
        }
        
        // 如果有多个候选核心，应用LCIF选择影响最小的
        if (num_candidates > 1) {
            long min_impact = LONG_MAX;
            int least_impact_core = best_core;
            
            for (int k = 0; k < num_candidates; k++) {
                int core_idx = candidate_cores[k];
                int core_id = available_core_ids[core_idx];
                
                // 计算此任务对该核心上现有任务的影响
                long impact = compute_cache_impact(&ready_tasks[best_task],
                core_id,
                core_running_tasks[core_idx],
                core_task_counts[core_idx]);
                
                if (impact < min_impact) {
                    min_impact = impact;
                    least_impact_core = core_idx;
                }
            }
            
            // 使用影响最小的核心
            best_core = least_impact_core;
        }
        
        // 进行分配
        int task_id = ready_tasks[best_task].id;
        int core_id = available_core_ids[best_core];
        
        // 更新分配表
        assignments[task_id] = core_id;
        
        // 更新任务的last_cpu (用于下次调度)
        ready_tasks[best_task].last_cpu = core_id;
        
        // 将此任务添加到核心的运行任务列表
        core_running_tasks[best_core][core_task_counts[best_core]++] = &ready_tasks[best_task];
        
        // 标记任务和核心为已分配
        task_assigned[best_task] = 1;
        core_assigned[best_core] = 1;
        
        assigned_count++;
    }

    // 清理内存
    for (int i = 0; i < num_to_consider; i++) {
        free(speedup_table[i]);
    }
    free(speedup_table);
    free(task_assigned);
    free(core_assigned);
    for (int j = 0; j < num_available_cores; j++) {
        free(core_running_tasks[j]);
    }
    
    return assigned_count;
}

// --- 示例用法 ---
int main() {
    // 示例任务数据
    task_struct_simplified tasks[] = {
        { .id = 0, .wcet = 100, .last_cpu = -1 },
        { .id = 1, .wcet = 150, .last_cpu = 0 },
        { .id = 2, .wcet = 80,  .last_cpu = 1 },
        { .id = 3, .wcet = 120, .last_cpu = -1 }
    };
    int num_tasks = sizeof(tasks) / sizeof(tasks[0]);

    // 假设核心 0 和 1 可用
    int available_cores[] = {0, 1};
    int num_cores = sizeof(available_cores) / sizeof(available_cores[0]);

    // 假设任务已按优先级排序 (这里按数组顺序)

    // 分配结果数组 (假设最大任务 ID 是 3)
    int max_id = 4;
    int *task_assignments = (int *)malloc(max_id * sizeof(int));
    if (!task_assignments) return 1;
    for(int i=0; i<max_id; i++) task_assignments[i] = -1; // 初始化为未分配

    printf("Running allocation...\n");
    int count = allocate_tasks_c(tasks, num_tasks, available_cores, 
        num_cores, task_assignments, max_id);

    if (count >= 0) {
        printf("Successfully assigned %d tasks:\n", count);
        for (int i = 0; i < max_id; i++) {
            if (task_assignments[i] != -1) {
                printf("  Task %d -> Core %d\n", i, task_assignments[i]);
            }
        }
    } else {
        printf("Allocation failed due to memory error.\n");
    }


    free(task_assignments);
    return 0;
}
