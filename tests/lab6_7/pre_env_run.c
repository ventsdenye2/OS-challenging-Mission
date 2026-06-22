#include <machine.h>

static inline void pre_env_run(struct Env *e) {
	static spinlock_t lock = SPINLOCK_INIT;
	static struct Env *last_on_cpu[NR_CPUS];
	static int killed_cpu0;
	static int killed_cpu1;
	int cpu = cpu_id();

	spin_lock(&lock);
	last_on_cpu[cpu] = e;

	if (!killed_cpu1 && cpu == 0 && last_on_cpu[1] &&
	    last_on_cpu[1]->env_status == ENV_RUNNABLE && last_on_cpu[1]->env_running &&
	    last_on_cpu[1]->env_cpu_id == 1) {
		struct Env *victim = last_on_cpu[1];
		printk("remote_destroy_bidirectional: cpu0 destroys %08x on cpu1\n",
		       victim->env_id);
		spin_unlock(&lock);
		env_destroy(victim);
		spin_lock(&lock);
		if (victim->env_status != ENV_FREE) {
			panic("cpu1 victim %08x was not freed", victim->env_id);
		}
		killed_cpu1 = 1;
	}

	if (!killed_cpu0 && cpu == 1 && last_on_cpu[0] &&
	    last_on_cpu[0]->env_status == ENV_RUNNABLE && last_on_cpu[0]->env_running &&
	    last_on_cpu[0]->env_cpu_id == 0) {
		struct Env *victim = last_on_cpu[0];
		printk("remote_destroy_bidirectional: cpu1 destroys %08x on cpu0\n",
		       victim->env_id);
		spin_unlock(&lock);
		env_destroy(victim);
		spin_lock(&lock);
		if (victim->env_status != ENV_FREE) {
			panic("cpu0 victim %08x was not freed", victim->env_id);
		}
		killed_cpu0 = 1;
	}

	if (killed_cpu0 && killed_cpu1) {
		printk("remote_destroy_bidirectional passed\n");
		halt();
	}
	spin_unlock(&lock);
}
