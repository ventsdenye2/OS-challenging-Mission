#include <mmu.h>
#include <smp.h>

/* 全局 per-cpu 数据表，阶段 1 先只服务单核兼容路径。 */
struct cpu_local_data cpu_data[NR_CPUS];

int cpu_id(void) {
	/* 阶段 1 尚未接入真实 CPU 编号读取，保持旧单核路径。 */
	return 0;
}

struct Env *cpu_curenv(void) {
	return cpu_data[cpu_id()].curenv;
}

Pde *cpu_cur_pgdir(void) {
	return cpu_data[cpu_id()].cur_pgdir;
}

void smp_init(void) {
	int i;

	/* 先为每个 CPU 填入保守默认值，避免后续接口返回野指针。 */
	for (i = 0; i < NR_CPUS; i++) {
		cpu_data[i].cpu_id = i;
		cpu_data[i].curenv = 0;
		cpu_data[i].cur_pgdir = 0;
		cpu_data[i].kernel_stack_top = KSTACKTOP;
	}
}

void smp_secondary_start(void) {
	/* 阶段 2 接入从核启动流程。 */
}

void smp_group_function_call(void (*fn)(u_int, u_int), u_int arg0, u_int arg1) {
	/* 阶段 3 接入 IPI 远程函数调用。 */
	(void)fn;
	(void)arg0;
	(void)arg1;
}

void handle_ipi_irq(void) {
	/* 阶段 3 接入 IPI 中断分发。 */
}
