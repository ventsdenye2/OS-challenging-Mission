#include <machine.h>

static inline void pre_env_run(struct Env *e) {
	static spinlock_t lock = SPINLOCK_INIT;
	static int total_runs;
	static int cpu_runs[NR_CPUS];
	int cpu = cpu_id();

	spin_lock(&lock);
	if (cpu < 0 || cpu >= NR_CPUS) {
		panic("smp_sched_parallel: bad cpu id %d", cpu);
	}
	if (e->env_running != 1 || e->env_cpu_id != cpu) {
		panic("smp_sched_parallel: env %08x running=%d owner=%d on cpu %d", e->env_id,
		      e->env_running, e->env_cpu_id, cpu);
	}

	total_runs++;
	cpu_runs[cpu]++;
	if ((total_runs % 16) == 0) {
		printk("smp_sched_parallel progress: total=%d cpu0=%d cpu1=%d\n", total_runs,
		       cpu_runs[0], cpu_runs[1]);
	}
	if (cpu_runs[0] > 0 && cpu_runs[1] > 0 && total_runs >= 32) {
		printk("smp_sched_parallel passed: total=%d cpu0=%d cpu1=%d\n", total_runs,
		       cpu_runs[0], cpu_runs[1]);
		halt();
	}
	if (total_runs > 512) {
		panic("smp_sched_parallel: cpu1 did not schedule; cpu0=%d cpu1=%d", cpu_runs[0],
		      cpu_runs[1]);
	}
	spin_unlock(&lock);
}
