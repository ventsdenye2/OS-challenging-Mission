#include <env.h>
#include <print.h>
#include <printk.h>
#include <smp.h>

void outputk(void *data, const char *buf, size_t len);

void _panic(const char *file, int line, const char *func, const char *fmt, ...) {
	u_long sp, ra, badva, sr, cause, epc;
	asm("move %0, $29" : "=r"(sp) :);
	asm("move %0, $31" : "=r"(ra) :);
	asm("mfc0 %0, $8" : "=r"(badva) :);
	asm("mfc0 %0, $12" : "=r"(sr) :);
	asm("mfc0 %0, $13" : "=r"(cause) :);
	asm("mfc0 %0, $14" : "=r"(epc) :);

	printk("panic at %s:%d (%s): ", file, line, func);

	va_list ap;
	va_start(ap, fmt);
	vprintfmt(outputk, NULL, fmt, ap);
	va_end(ap);

	printk("\n"
	       "ra:    %08x  sp:  %08x  Status: %08x\n"
	       "Cause: %08x  EPC: %08x  BadVA:  %08x\n",
	       ra, sp, sr, cause, epc, badva);

#if !defined(LAB) || LAB >= 3
	extern struct Env envs[];
	struct Env *cur_env = cpu_curenv();
	Pde *cur_dir = cpu_cur_pgdir();

	if ((u_long)cur_env >= KERNBASE) {
		printk("curenv:    %x (id = 0x%x, off = %d)\n", cur_env, cur_env->env_id,
		       cur_env - envs);
	} else if (cur_env) {
		printk("curenv:    %x (invalid)\n", cur_env);
	} else {
		printk("curenv:    NULL\n");
	}

	if ((u_long)cur_dir >= KERNBASE) {
		printk("cur_pgdir: %x\n", cur_dir);
	} else if (cur_dir) {
		printk("cur_pgdir: %x (invalid)\n", cur_dir);
	} else {
		printk("cur_pgdir: NULL\n", cur_dir);
	}
#endif

#ifdef MOS_HANG_ON_PANIC
	while (1) {
	}
#else
	halt();
#endif
}
