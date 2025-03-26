#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#define NUM_THREADS 5

static struct task_struct *threads[NUM_THREADS];

static int thread_function(void *data)
{
    int id = (int)data;
    printk(KERN_INFO "Thread %d started\n", id);

    // 模拟工作负载
    while (!kthread_should_stop()) {
        printk(KERN_INFO "Thread %d running\n", id);
        msleep(1000); // 每秒打印一次
    }

    printk(KERN_INFO "Thread %d stopped\n", id);
    return 0;
}

static int __init test_sched_init(void)
{
    int i;

    printk(KERN_INFO "Test scheduler module loaded\n");

    for (i = 0; i < NUM_THREADS; i++) {
        threads[i] = kthread_create(thread_function, (void *)i, "test_thread_%d", i);
        if (IS_ERR(threads[i])) {
            printk(KERN_INFO "Failed to create thread %d\n", i);
            return PTR_ERR(threads[i]);
        }
        wake_up_process(threads[i]);
    }

    return 0;
}

static void __exit test_sched_exit(void)
{
    int i;

    printk(KERN_INFO "Test scheduler module unloaded\n");

    for (i = 0; i < NUM_THREADS; i++) {
        if (threads[i]) {
            kthread_stop(threads[i]);
        }
    }
}

module_init(test_sched_init);
module_exit(test_sched_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test Scheduler Module");
MODULE_AUTHOR("Your Name");
