#include <mmu.h>
#include <asm/cp0regdef.h>
#include <kclock.h>
#include <malta.h>
#include <printk.h>
#include <smp.h>
#include <spinlock.h>
#include <trap.h>

#if !defined(LAB) || LAB >= 3
#include <sched.h>
#endif

/* 全局 per-cpu 数据表。 */
struct cpu_local_data cpu_data[NR_CPUS];
volatile int smp_boot_ready = SMP_BOOT_WAIT;

enum {
	IPI_MBOX_FN = 0,
	IPI_MBOX_ARG0,
	IPI_MBOX_ARG1,
	IPI_MBOX_NR,
};

static volatile int ipi_ready[NR_CPUS];
static volatile int ipi_done[NR_CPUS];
static volatile u_int ipi_pending[NR_CPUS];
static volatile u_int ipi_mailbox[NR_CPUS][IPI_MBOX_NR];
static volatile int timer_schedule_ready;
static spinlock_t ipi_call_lock = SPINLOCK_INIT;

#if SMP_USE_MMIO_IPI
static volatile u_int *ipi_status_reg[NR_CPUS];
static volatile u_int *ipi_enable_reg[NR_CPUS];
static volatile u_int *ipi_set_reg[NR_CPUS];
static volatile u_int *ipi_clear_reg[NR_CPUS];
static volatile u_int *ipi_mailbox_reg[NR_CPUS][IPI_MBOX_NR];
#endif

static void smp_sync(void) {
	__asm__ volatile("sync" ::: "memory");
}

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

u_long cpu_kstack_top(void) {
	return cpu_data[cpu_id()].kernel_stack_top;
}

struct Trapframe *cpu_trapframe(void) {
	return (struct Trapframe *)cpu_kstack_top() - 1;
}

static void setup_ipi_mmio(int i) {
#if SMP_USE_MMIO_IPI
	int slot;

	ipi_status_reg[i] = (volatile u_int *)(KSEG1 + IPI_STATUS(i));
	ipi_enable_reg[i] = (volatile u_int *)(KSEG1 + IPI_ENABLE(i));
	ipi_set_reg[i] = (volatile u_int *)(KSEG1 + IPI_SET(i));
	ipi_clear_reg[i] = (volatile u_int *)(KSEG1 + IPI_CLEAR(i));
	for (slot = 0; slot < IPI_MBOX_NR; slot++) {
		ipi_mailbox_reg[i][slot] = (volatile u_int *)(KSEG1 + IPI_MAILBOX(i, slot));
	}
	*ipi_enable_reg[i] = IPI_START | IPI_CALL;
#else
	(void)i;
#endif
}

static void setup_timer_compare(void) {
	u_int interval = TIMER_INTERVAL;

	__asm__ volatile("mtc0 $0, $9");
	__asm__ volatile("mtc0 %0, $11" : : "r"(interval));
}

static void setup_cpu_interrupts(void) {
	u_int status;

	setup_timer_compare();
	__asm__ volatile("mfc0 %0, $12" : "=r"(status));
	status |= STATUS_IE | STATUS_IM6 | STATUS_IM7;
	__asm__ volatile("mtc0 %0, $12" : : "r"(status));
	smp_sync();
}

static void ipi_mailbox_write(int cpu, int slot, u_int value) {
	ipi_mailbox[cpu][slot] = value;
#if SMP_USE_MMIO_IPI
	*ipi_mailbox_reg[cpu][slot] = value;
#endif
}

static u_int ipi_mailbox_read(int cpu, int slot) {
#if SMP_USE_MMIO_IPI
	return *ipi_mailbox_reg[cpu][slot];
#else
	return ipi_mailbox[cpu][slot];
#endif
}

static void ipi_send(u_int cpu, u_int signal) {
	if (cpu >= NR_CPUS) {
		return;
	}

	smp_sync();
#if SMP_USE_MMIO_IPI
	*ipi_set_reg[cpu] = signal;
#else
	ipi_pending[cpu] |= signal;
#endif
	smp_sync();
}

void smp_init(void) {
	int i;

	/* 先为每个 CPU 填入保守默认值，避免后续接口返回野指针。 */
	for (i = 0; i < NR_CPUS; i++) {
		cpu_data[i].cpu_id = i;
		cpu_data[i].curenv = 0;
		cpu_data[i].cur_pgdir = 0;
		cpu_data[i].kernel_stack_top = KSTACKTOP_CPU(i);
		ipi_ready[i] = 0;
		ipi_done[i] = 1;
		ipi_pending[i] = 0;
		setup_ipi_mmio(i);
	}
	ipi_ready[0] = 1;
	setup_cpu_interrupts();
	smp_sync();
	smp_boot_ready = SMP_BOOT_READY;
	smp_sync();
}

void smp_secondary_start(void) {
	int cpu = cpu_id();

	setup_cpu_interrupts();
	ipi_ready[cpu] = 1;
	smp_sync();
	printk("slave online\n");
	while (1) {
		if (ipi_pending[cpu] != 0) {
			handle_ipi_irq();
		}
#if SMP_USE_MMIO_IPI
		__asm__ volatile("wait");
#else
		__asm__ volatile("nop");
#endif
	}
}

void smp_group_function_call(void (*fn)(u_int, u_int), u_int arg0, u_int arg1) {
	int self = cpu_id();
	int cpu;

	if (fn == 0) {
		return;
	}

	spin_lock(&ipi_call_lock);
	for (cpu = 0; cpu < NR_CPUS; cpu++) {
		if (cpu == self || !ipi_ready[cpu]) {
			continue;
		}
		ipi_done[cpu] = 0;
		ipi_mailbox_write(cpu, IPI_MBOX_FN, (u_int)fn);
		ipi_mailbox_write(cpu, IPI_MBOX_ARG0, arg0);
		ipi_mailbox_write(cpu, IPI_MBOX_ARG1, arg1);
		ipi_send(cpu, IPI_CALL);
	}
	for (cpu = 0; cpu < NR_CPUS; cpu++) {
		if (cpu == self || !ipi_ready[cpu]) {
			continue;
		}
		while (!ipi_done[cpu]) {
			__asm__ volatile("nop");
		}
	}
	spin_unlock(&ipi_call_lock);
}

void smp_note_schedule_ready(void) {
	if (cpu_id() == 0) {
		timer_schedule_ready = 1;
	}
}

void handle_timer_irq(void) {
#if !defined(LAB) || LAB >= 3
	if (cpu_id() == 0 && timer_schedule_ready) {
		schedule(0);
	}
#endif
}

void handle_ipi_irq(void) {
	int cpu = cpu_id();
	u_int status;
	void (*fn)(u_int, u_int);
	u_int arg0;
	u_int arg1;

#if SMP_USE_MMIO_IPI
	status = *ipi_status_reg[cpu];
#else
	status = ipi_pending[cpu];
#endif
	if (status == 0) {
		return;
	}

#if SMP_USE_MMIO_IPI
	*ipi_clear_reg[cpu] = status;
#else
	ipi_pending[cpu] &= ~status;
#endif
	smp_sync();

	if (status & IPI_START) {
		ipi_ready[cpu] = 1;
	}
	if (status & IPI_CALL) {
		fn = (void (*)(u_int, u_int))ipi_mailbox_read(cpu, IPI_MBOX_FN);
		arg0 = ipi_mailbox_read(cpu, IPI_MBOX_ARG0);
		arg1 = ipi_mailbox_read(cpu, IPI_MBOX_ARG1);
		if (fn != 0) {
			fn(arg0, arg1);
		}
		ipi_done[cpu] = 1;
		smp_sync();
	}
}
