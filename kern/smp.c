#include <mmu.h>
#include <printk.h>
#include <smp.h>

/* 全局 per-cpu 数据表。 */
struct cpu_local_data cpu_data[NR_CPUS];
u_char smp_kernel_stacks[NR_CPUS][SMP_KSTACK_SIZE] __attribute__((aligned(8)));
volatile int smp_boot_ready = SMP_BOOT_WAIT;

int cpu_id(void) {
	u_int ebase;

	__asm__ volatile("mfc0 %0, $15, 1" : "=r"(ebase));
	ebase &= 0x3ff;
	if (ebase >= NR_CPUS) {
		return 0;
	}
	return ebase;
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
		if (i == 0) {
			cpu_data[i].kernel_stack_top = KSTACKTOP;
		} else {
			cpu_data[i].kernel_stack_top = (u_long)&smp_kernel_stacks[i][SMP_KSTACK_SIZE];
		}
	}
	__asm__ volatile("sync" ::: "memory");
	smp_boot_ready = SMP_BOOT_READY;
	__asm__ volatile("sync" ::: "memory");
}

void smp_secondary_start(void) {
	printk("slave online\n");
	printk("wait for start\n");
	while (1) {
		__asm__ volatile("nop");
	}
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
