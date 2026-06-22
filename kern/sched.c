#include <env.h>
#include <pmap.h>
#include <printk.h>
#include <smp.h>
#include <spinlock.h>

/* SMP 阶段 6: 调度队列锁，保护 env_sched_list 和 env_running/env_cpu_id 字段。 */
spinlock_t env_sched_lock = SPINLOCK_INIT;

/* Overview:
 *   Implement a round-robin scheduling to select a runnable env and schedule it using 'env_run'.
 *
 *   SMP 阶段 6 变更:
 *   - 用 cpu_data[cpu_id()].sched_count 替代 static int count。
 *   - 遍历 env_sched_list 时持 env_sched_lock。
 *   - 跳过 env_running == 1 且 env_cpu_id != cpu_id() 的 env。
 *   - 选中后设置 env_running = 1、env_cpu_id = cpu_id()。
 *   - 换下当前 env 时清除其 env_running 标记。
 *
 * Post-Condition:
 *   If 'yield' is set (non-zero), 'curenv' should not be scheduled again unless it is the only
 *   runnable env.
 *
 * Hints:
 *   1. The variable 'count' used for counting slices should be defined as 'static'.
 *   2. Use variable 'env_sched_list', which contains and only contains all runnable envs.
 *   3. You shouldn't use any 'return' statement because this function is 'noreturn'.
 */
void schedule(int yield) {
	int cpu = cpu_id();
	int count = cpu_data[cpu].sched_count;
	struct Env *e = cpu_curenv();

	handle_ipi_irq();
	env_check_kill_pending();
	smp_note_schedule_ready();

	spin_lock(&env_sched_lock);

	/* We always decrease the 'count' by 1.
	 *
	 * If 'yield' is set, or 'count' has been decreased to 0, or 'e' (previous 'curenv') is
	 * 'NULL', or 'e' is not runnable, then we pick up a new env from 'env_sched_list' (list of
	 * all runnable envs), set 'count' to its priority, and schedule it with 'env_run'. **Panic
	 * if that list is empty**.
	 *
	 * (Note that if 'e' is still a runnable env, we should move it to the tail of
	 * 'env_sched_list' before picking up another env from its head, or we will schedule the
	 * head env repeatedly.)
	 *
	 * Otherwise, we simply schedule 'e' again.
	 *
	 * SMP 阶段 6: 所有 env_sched_list 的遍历和修改都在 env_sched_lock 保护下进行。
	 * 如果当前 CPU 找不到可调度的 env，则自旋等待直到有可用 env。
	 */
	if (yield || count == 0 || e == NULL || e->env_status != ENV_RUNNABLE) {
		if (e != NULL) {
			/* Clear the old env's running state before picking a new one. */
			e->env_running = 0;
			e->env_cpu_id = -1;

			/* If still runnable, move to tail of sched list for fairness. */
			if (e->env_status == ENV_RUNNABLE) {
				TAILQ_REMOVE(&env_sched_list, e, env_sched_link);
				TAILQ_INSERT_TAIL(&env_sched_list, e, env_sched_link);
			}
		}

		/* Pick a new env from the head of env_sched_list.
		 * Skip envs that are already running on another CPU. */
		while (1) {
			e = TAILQ_FIRST(&env_sched_list);
			while (e != NULL) {
				if ((e->env_pinned_cpu < 0 || e->env_pinned_cpu == cpu) &&
				    !e->env_kill_pending &&
				    (e->env_running == 0 || e->env_cpu_id == cpu)) {
					goto found;
				}
				e = TAILQ_NEXT(e, env_sched_link);
			}
			/* No runnable env available for this CPU.
			 * Release lock briefly to let other CPUs make progress. */
			spin_unlock(&env_sched_lock);
			handle_ipi_irq();
			for (int i = 0; i < 100; i++) {
				__asm__ volatile("nop");
			}
			spin_lock(&env_sched_lock);
		}
	found:
		/* Mark the newly selected env as running on this CPU. */
		e->env_running = 1;
		e->env_cpu_id = cpu;
		count = e->env_pri;
	}

	count--;
	cpu_data[cpu].sched_count = count;

	spin_unlock(&env_sched_lock);
	env_run(e);
}
