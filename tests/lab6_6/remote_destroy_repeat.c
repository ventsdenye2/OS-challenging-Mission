#include <lib.h>

#define MAX_WAIT 30000
#define ROUNDS 4

static void child_loop(void) {
	for (;;) {
		syscall_yield();
	}
}

static void wait_remote(u_int child) {
	const volatile struct Env *child_env = &envs[ENVX(child)];

	for (int i = 0; i < MAX_WAIT; i++) {
		if (child_env->env_status == ENV_RUNNABLE && child_env->env_running &&
		    child_env->env_cpu_id >= 0 && env->env_cpu_id >= 0 &&
		    child_env->env_cpu_id != env->env_cpu_id) {
			return;
		}
		syscall_yield();
	}
	user_panic("round child %08x never became remote-running", child);
}

static void wait_free(u_int child) {
	const volatile struct Env *child_env = &envs[ENVX(child)];

	for (int i = 0; i < MAX_WAIT; i++) {
		if (child_env->env_status == ENV_FREE || child_env->env_id != child) {
			return;
		}
		syscall_yield();
	}
	user_panic("round child %08x was not freed", child);
}

int main(void) {
	for (int round = 0; round < ROUNDS; round++) {
		u_int child = fork();

		if (child == 0) {
			child_loop();
		}

		wait_remote(child);
		debugf("remote_destroy_repeat: round %d destroy %08x from cpu %d target cpu %d\n",
		       round, child, env->env_cpu_id, envs[ENVX(child)].env_cpu_id);
		if (syscall_env_destroy(child) != 0) {
			user_panic("syscall_env_destroy failed in round %d", round);
		}
		wait_free(child);
	}

	debugf("remote_destroy_repeat passed\n");
	return 0;
}
