#include <env.h>
#include <machine.h>
#include <mmu.h>
#include <pmap.h>
#include <printk.h>
#include <sched.h>
#include <smp.h>
#include <trap.h>

#define IPI_ROUNDS 128

static volatile int cpu1_calls;
static volatile int cpu0_callbacks;
static volatile int cpu1_bad_cpu;
static volatile int cpu0_bad_cpu;

static void cpu0_callback(u_int arg0, u_int arg1) {
	if (cpu_id() != 0) {
		cpu0_bad_cpu++;
		return;
	}
	if (arg1 != (arg0 ^ 0x5a5a5a5a)) {
		panic("cpu0_callback got bad args %08x %08x", arg0, arg1);
	}
	cpu0_callbacks++;
}

static void cpu1_nested_handler(u_int arg0, u_int arg1) {
	if (cpu_id() != 1) {
		cpu1_bad_cpu++;
		return;
	}
	if (arg1 != arg0 + 0x1000) {
		panic("cpu1_nested_handler got bad args %08x %08x", arg0, arg1);
	}
	cpu1_calls++;

	/* While CPU0 waits for this IPI call to finish, ask CPU0 to run a callback.
	 * This verifies nested, opposite-direction mailbox delivery does not deadlock. */
	smp_group_function_call(cpu0_callback, arg0, arg0 ^ 0x5a5a5a5a);
}

void mips_init(u_int argc, char **argv, char **penv, u_int ram_low_size) {
	int i;

	(void)argc;
	(void)argv;
	(void)penv;
	(void)ram_low_size;

	printk("smp_ipi_roundtrip: start\n");
	for (i = 0; i < IPI_ROUNDS; i++) {
		smp_group_function_call(cpu1_nested_handler, i, i + 0x1000);
	}

	if (cpu1_bad_cpu || cpu0_bad_cpu) {
		panic("IPI ran on wrong CPU: cpu1_bad=%d cpu0_bad=%d", cpu1_bad_cpu,
		      cpu0_bad_cpu);
	}
	if (cpu1_calls != IPI_ROUNDS || cpu0_callbacks != IPI_ROUNDS) {
		panic("IPI counts mismatch: cpu1=%d cpu0=%d expected=%d", cpu1_calls,
		      cpu0_callbacks, IPI_ROUNDS);
	}

	printk("smp_ipi_roundtrip passed: cpu1_calls=%d cpu0_callbacks=%d\n", cpu1_calls,
	       cpu0_callbacks);
	halt();
}
