#include <asm/asm.h>
#include <mmu.h>
#include <smp.h>

	.text
EXPORT(_start)
	.set at
	.set reorder
	/* disable interrupts before the primary/secondary split */
	mtc0    zero, CP0_STATUS
	mfc0    t0, CP0_EBASE
	andi    t0, t0, 0x03ff
	bnez    t0, secondary_wait

	/* Lab 1 Key Code "enter-kernel" */
		/* clear .bss segment */
		la      v0, bss_start
	la      v1, bss_end
clear_bss_loop:
	beq     v0, v1, clear_bss_done
	sb      zero, 0(v0)
	addiu   v0, v0, 1
	j       clear_bss_loop
	/* End of Key Code "enter-kernel" */

clear_bss_done:
		/* hint: you can refer to the memory layout in include/mmu.h */
		/* set up the kernel stack */
		/* Exercise 1.3: Your code here. (1/2) */
		li      sp, KSTACKTOP

		/* smp_init must run before printk, but mips_init still needs boot arguments. */
		subu    sp, sp, 16
		sw      a0, 0(sp)
		sw      a1, 4(sp)
		sw      a2, 8(sp)
		sw      a3, 12(sp)
		jal     smp_init
		lw      a0, 0(sp)
		lw      a1, 4(sp)
		lw      a2, 8(sp)
		lw      a3, 12(sp)
		addu    sp, sp, 16

		/* jump to mips_init */
		/* Exercise 1.3: Your code here. (2/2) */
		j       mips_init

secondary_wait:
		la      t1, smp_boot_ready
		li      t3, SMP_BOOT_READY
1:
		lw      t2, 0(t1)
		bne     t2, t3, 1b
		sync
		la      sp, smp_kernel_stacks
		li      t1, SMP_KSTACK_SIZE
		multu   t0, t1
		mflo    t2
		addu    sp, sp, t2
		addu    sp, sp, t1
		j       smp_secondary_start
