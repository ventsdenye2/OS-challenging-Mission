#include <lib.h>

#define MAX_WAIT 20000

static void child_loop(void) {
	for (;;) {
		syscall_yield();
	}
}

static void wait_child_on_remote_cpu(u_int child) {
	const volatile struct Env *child_env = &envs[ENVX(child)];

	for (int i = 0; i < MAX_WAIT; i++) {
		if (child_env->env_status == ENV_RUNNABLE && child_env->env_running &&
		    child_env->env_cpu_id >= 0 && env->env_cpu_id >= 0 &&
		    child_env->env_cpu_id != env->env_cpu_id) {
			return;
		}
		syscall_yield();
	}
	user_panic("child %08x did not run on a remote CPU", child);
}

static void wait_child_free(u_int child) {
	const volatile struct Env *child_env = &envs[ENVX(child)];

	for (int i = 0; i < MAX_WAIT; i++) {
		if (child_env->env_status == ENV_FREE || child_env->env_id != child) {
			return;
		}
		syscall_yield();
	}
	user_panic("child %08x was not freed after remote destroy", child);
}

int main(void) {
	u_int child = fork();

	if (child == 0) {
		child_loop();
	}

	wait_child_on_remote_cpu(child);
	debugf("remote_destroy_once: destroy child %08x from cpu %d while it runs on cpu %d\n",
	       child, env->env_cpu_id, envs[ENVX(child)].env_cpu_id);
	if (syscall_env_destroy(child) != 0) {
		user_panic("syscall_env_destroy failed");
	}
	wait_child_free(child);
	debugf("remote_destroy_once passed\n");
	return 0;
}
