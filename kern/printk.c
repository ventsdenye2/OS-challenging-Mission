#include <print.h>
#include <printk.h>
#include <smp.h>
#include <spinlock.h>
#include <trap.h>

static spinlock_t console_lock = SPINLOCK_INIT;

/* Lab 1 Key Code "outputk" */
void outputk(void *data, const char *buf, size_t len) {
	for (int i = 0; i < len; i++) {
		printcharc(buf[i]);
	}
}
/* End of Key Code "outputk" */

static void output_uint(u_int value) {
	char buf[10];
	int i = 0;

	if (value == 0) {
		printcharc('0');
		return;
	}
	while (value > 0) {
		buf[i++] = '0' + value % 10;
		value /= 10;
	}
	while (i > 0) {
		printcharc(buf[--i]);
	}
}

static void output_cpu_prefix(void) {
	printcharc('[');
	output_uint(cpu_id());
	printcharc(']');
	printcharc(' ');
}

/* Lab 1 Key Code "printk" */
void printk(const char *fmt, ...) {
	va_list ap;

	spin_lock(&console_lock);
	output_cpu_prefix();
	va_start(ap, fmt);
	vprintfmt(outputk, NULL, fmt, ap);
	va_end(ap);
	spin_unlock(&console_lock);
}
/* End of Key Code "printk" */

void print_tf(struct Trapframe *tf) {
	for (int i = 0; i < sizeof(tf->regs) / sizeof(tf->regs[0]); i++) {
		printk("$%2d = %08x\n", i, tf->regs[i]);
	}
	printk("HI  = %08x\n", tf->hi);
	printk("LO  = %08x\n\n", tf->lo);
	printk("CP0.SR    = %08x\n", tf->cp0_status);
	printk("CP0.BadV  = %08x\n", tf->cp0_badvaddr);
	printk("CP0.Cause = %08x\n", tf->cp0_cause);
	printk("CP0.EPC   = %08x\n", tf->cp0_epc);
}
