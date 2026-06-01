#ifndef _SMP_H_
#define _SMP_H_

#include <mmu.h>

/* 当前 SMP 移植阶段默认支持的 CPU 数量，后续可扩展。 */
#define NR_CPUS 2

/* 每个 CPU 暂时预留的启动/内核栈大小。 */
#define SMP_KSTACK_SIZE (4 * PAGE_SIZE)
#define SMP_BOOT_WAIT 0x534d5030
#define SMP_BOOT_READY 0x534d5031

#define IPI_START 0x1
#define IPI_CALL 0x2

#ifndef SMP_USE_MMIO_IPI
#define SMP_USE_MMIO_IPI 0
#endif

#ifndef __ASSEMBLER__

#include <types.h>

/* Env 在 env.h 中定义，这里只需要指针类型，避免头文件互相包含。 */
struct Env;

/* 每个 CPU 独有的内核状态，后续替换全局 curenv/cur_pgdir 时使用。 */
struct cpu_local_data {
	int cpu_id;		      /* CPU 编号。 */
	struct Env *curenv;	      /* 当前 CPU 正在运行的进程。 */
	Pde *cur_pgdir;		      /* 当前 CPU 使用的页目录。 */
	u_long kernel_stack_top;      /* 当前 CPU 的内核栈顶。 */
};

/* 所有 CPU 的 per-cpu 状态表。 */
extern struct cpu_local_data cpu_data[NR_CPUS];
extern u_char smp_kernel_stacks[NR_CPUS][SMP_KSTACK_SIZE];
extern volatile int smp_boot_ready;

/* 返回当前 CPU 编号。 */
int cpu_id(void);
/* 返回当前 CPU 的 curenv。 */
struct Env *cpu_curenv(void);
/* 返回当前 CPU 的 cur_pgdir。 */
Pde *cpu_cur_pgdir(void);

/* 初始化 SMP 公共状态；阶段 1 只填充静态 per-cpu 表。 */
void smp_init(void);
/* 从核启动入口；阶段 1 保留为空实现。 */
void smp_secondary_start(void);
/* 让其它 CPU 执行指定函数；阶段 1 只提供接口占位。 */
void smp_group_function_call(void (*fn)(u_int, u_int), u_int arg0, u_int arg1);
/* IPI 中断处理入口；阶段 1 保留为空实现。 */
void handle_ipi_irq(void);

/* TLB 失效操作（阶段 3 提供签名，阶段 5 启用广播）。
 * tlb_invalidate_local: 仅使本地 TLB 条目失效。
 * tlb_invalidate 声明在 mmu.h，阶段 5 将实现为本地失效 + IPI 广播。 */
void tlb_invalidate_local(u_int asid, u_long va);

#endif

#endif
