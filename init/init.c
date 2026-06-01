#include <asm/asm.h>
#include <env.h>
#include <pmap.h>
#include <printk.h>
#include <sched.h>
#include <smp.h>
#include <trap.h>

/*
 * Note:
 * When build with 'make test lab=?_?', we will replace your 'mips_init' with a generated one from
 * 'tests/lab?_?'.
 */

#ifdef MOS_INIT_OVERRIDDEN
#include <generated/init_override.h>
#else

static volatile int ipi_seen[NR_CPUS];
static volatile int ipi_count[NR_CPUS];

static void ipi_test_handler(u_int arg0, u_int arg1) {
	int cpu = cpu_id();

	ipi_seen[cpu] = arg0 + arg1;
	ipi_count[cpu]++;
	if (ipi_count[cpu] == 1 || ipi_count[cpu] == 100) {
		printk("ipi call on cpu %d value %d count %d\n", cpu, ipi_seen[cpu],
		       ipi_count[cpu]);
	}
}

static void test_ipi_communication(void) {
	int i;

	if (cpu_id() == 0) {
		for (i = 0; i < 100; i++) {
			smp_group_function_call(ipi_test_handler, 20, 22);
		}
		printk("cpu1 seen = %d count = %d\n", ipi_seen[1], ipi_count[1]);
	}
}

/*
 * Note:
 *   When booting the OS, the bootloader passes certain parameters to the OS
 * through registers to provide information regarding the hardware configuration. One
 * of these parameters is ram_low_size, which represents the available physical memory
 * size (in bytes) of the system. Definitions of these parameters are typically agreed upon by
 * the hardware manufacturer and OS developers. YAMON is released by MIPS Inc. as the official
 * bootloader on the malta board, which is possibly the initial standard implementation. These
 * definitions of startup parameters are detailed in MIPS YAMON User’s Manual (Section 5.1,
 * Application Interface - Entry). The manual can be downloaded via the following link:
 * http://os.buaa.edu.cn/public/reference/YAMON_USM-02.19.pdf
 *
 *   In the start.S file, the a0-a3 registers used for passing parameters are not modified, so when
 * calling the mips_init function, these parameters can be directly accessed.
 *
 *   In QEMU emulation environment, a simplified bootloader has been coded to approach YAMON's
 * functionality, particularly in passing the startup parameters. The relevant QEMU code can be
 * viewed at the following link: https://github.com/qemu/qemu/blob/v6.2.0/hw/mips/malta.c#L818-L997
 * U-Boot supports the malta board as well, and we can find the startup parameters it passes to the
 * OS in codes. The relevant U-Boot code can be viewed at the following link:
 * https://github.com/u-boot/u-boot/blob/v2023.10/arch/mips/lib/bootm.c#L274C1-L302C2
 */

void mips_init(u_int argc, char **argv, char **penv, u_int ram_low_size) {
	printk("init.c:\tmips_init() is called\n");
	test_ipi_communication();

	// lab2:
	// mips_detect_memory(ram_low_size);
	// mips_vm_init();
	// page_init();

	// lab3:
	// env_init();

	// lab3:
	// ENV_CREATE_PRIORITY(user_bare_loop, 1);
	// ENV_CREATE_PRIORITY(user_bare_loop, 2);

	// lab4:
	// ENV_CREATE(user_tltest);
	// ENV_CREATE(user_fktest);
	// ENV_CREATE(user_pingpong);

	// lab6:
	// ENV_CREATE(user_icode);  // This must be the first env!

	// lab5:
	// ENV_CREATE(user_fstest);
	// ENV_CREATE(fs_serv);  // This must be the second env!
	// ENV_CREATE(user_devtst);

	// lab3:
	// schedule(0);
	halt();
}

#endif
