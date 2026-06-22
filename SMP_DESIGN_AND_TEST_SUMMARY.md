# MIPS MOS SMP 多核改造设计与测试总结

本文档用于答辩复习，重点说明本项目从单核 MOS 改造成支持 2 个 MIPS 24Kc CPU 的 SMP 内核时，多出来的设计、代码路径、同步策略、测试构造方法，以及最近一次远端 `env_destroy` bug 的 debug 过程。

## 1. 改造目标

单核 MOS 的核心假设是：任意时刻只有一个 CPU 在执行内核代码，因此很多全局变量和链表默认不会被并发访问。例如：

- `curenv` 表示当前正在运行的 env。
- `cur_pgdir` 表示当前地址空间。
- `env_sched_list` 只会被一个调度器修改。
- `page_free_list`、页引用计数、页表项只会被一个 CPU 修改。
- TLB 失效只需要在当前 CPU 执行。
- `env_destroy()` 可以直接释放目标 env，因为目标不可能正在另一个 CPU 上运行。

SMP 改造后，上述假设全部失效。两个 CPU 可以同时：

- 进入异常/系统调用。
- 访问同一套物理页管理结构。
- 修改调度队列。
- 运行不同 env，甚至尝试操作同一个 env。
- 修改同一个页表并要求其它 CPU 刷 TLB。

因此改造的核心目标是：

1. CPU0 和 CPU1 都能启动，并拥有独立内核栈。
2. 内核全局状态中与“当前 CPU”相关的部分改成 per-CPU。
3. 共享内核数据结构必须加锁。
4. 调度器必须保证同一个 env 不会同时在两个 CPU 上运行。
5. TLB 失效必须广播到其它 CPU。
6. `env_destroy()` 必须能安全处理“目标 env 正在另一个 CPU 上运行”的情况。
7. 增加多核专项测试，而不是只复用单核测试。

## 2. 单核与 SMP 关键差异总览

| 主题 | 单核实现假设 | SMP 新增设计 |
| --- | --- | --- |
| CPU 启动 | 只有 CPU0 进入 `_start` 和 `mips_init` | `init/start.S` 根据 CP0 `EBase` 分流，CPU0 初始化，CPU1 等待启动栅栏后进入 `smp_secondary_start` |
| 当前进程 | 全局 `curenv` | `cpu_data[cpu].curenv`，通过 `cpu_curenv()` 访问 |
| 当前页目录 | 全局 `cur_pgdir` | `cpu_data[cpu].cur_pgdir`，通过 `cpu_cur_pgdir()` 访问 |
| 内核栈 | 固定 `KSTACKTOP` | `KSTACKTOP_CPU(cpu)`，异常入口按 CPU ID 选择栈 |
| 调度时间片 | `static int count` | `cpu_data[cpu].sched_count`，每核独立时间片 |
| 调度队列 | 无并发修改 | `env_sched_lock` 保护 `env_sched_list` 和运行状态 |
| 运行状态 | env 不会跨 CPU 并发运行 | `env_running/env_cpu_id/env_pinned_cpu/env_kill_pending` |
| IPI | 不需要 | `smp_group_function_call()` + mailbox + `handle_ipi_irq()` |
| TLB 失效 | 本地 `tlb_out` | 本地失效后用 IPI 广播 `tlb_invalidate_local` |
| 物理页管理 | 无锁 | `pmap_lock` 保护空闲链表、引用计数、页表修改 |
| 控制台输出 | 无锁 | `console_lock` 串行化输出，并加 `[cpu]` 前缀 |
| env 销毁 | 直接 `env_free` | 如果目标在远端 CPU 运行，标记 kill request，poke 目标 CPU，在安全点本地释放 |

## 3. 启动路径与每核状态

### 3.1 QEMU 和编译参数

相关文件：

- `Makefile`
- `include.mk`

多核运行参数：

```make
QEMU_FLAGS += -smp 2 -cpu 24Kc -m 64 -nographic -M malta ... -no-reboot
```

含义：

- `-smp 2` 启动两个 vCPU。
- `-cpu 24Kc` 使用当前 QEMU 可用的 MIPS 24Kc CPU。
- `-M malta` 使用 Malta 机器模型。

编译参数中使用：

```make
-march=24kc
```

这与 QEMU CPU 型号一致，避免使用旧的 `4kc` 配置。

### 3.2 CPU0/CPU1 启动分流

相关文件：

- `init/start.S`
- `include/smp.h`
- `kern/smp.c`

`init/start.S` 在 `_start` 入口读取 CP0 `EBase`：

```asm
mfc0    t0, CP0_EBASE
andi    t0, t0, 0x03ff
bnez    t0, secondary_wait
```

设计含义：

- CPU0 的 `EBase & 0x3ff == 0`，进入主启动路径。
- 非 0 CPU 进入 `secondary_wait`。

CPU0 做的事情：

1. 清 `.bss`。
2. 设置 `sp = KSTACKTOP`。
3. 调用 `smp_init()` 初始化 per-CPU 数据、IPI 状态、timer/interrupt。
4. 跳转 `mips_init`，继续原来的内核初始化。

CPU1 做的事情：

1. 不清 `.bss`，避免破坏 CPU0 已经初始化的全局数据。
2. 等待 `smp_boot_ready == SMP_BOOT_READY`。
3. 按 CPU ID 设置自己的内核栈：

```asm
li      sp, KSTACKTOP
sll     t2, t0, KSTACKSHIFT
subu    sp, sp, t2
```

4. 跳转 `smp_secondary_start()`。

### 3.3 Per-CPU 状态

相关文件：

- `include/smp.h`
- `kern/smp.c`

核心结构：

```c
struct cpu_local_data {
	int cpu_id;
	struct Env *curenv;
	Pde *cur_pgdir;
	u_long kernel_stack_top;
	int sched_count;
};
```

全局数组：

```c
struct cpu_local_data cpu_data[NR_CPUS];
```

访问接口：

- `cpu_id()`
- `cpu_curenv()`
- `cpu_cur_pgdir()`
- `cpu_kstack_top()`
- `cpu_trapframe()`

答辩重点：

- 单核时代可以直接读写全局 `curenv` 和 `cur_pgdir`。
- SMP 后，“当前 env”不是系统全局概念，而是“当前 CPU 的 env”。
- 所有 syscall、TLB refill、panic、调度、env 运行路径都应通过 `cpu_curenv()`、`cpu_cur_pgdir()` 等接口访问当前 CPU 状态。

## 4. 异常、中断和 IPI

### 4.1 异常入口的每核内核栈

相关文件：

- `include/stackframe.h`
- `kern/genex.S`

`SAVE_ALL` 在用户态异常进入内核时，根据 CP0 `EBase` 选择对应 CPU 的内核栈：

```asm
mfc0    sp, CP0_EBASE
andi    sp, sp, 0x03ff
sll     sp, sp, KSTACKSHIFT
li      k1, KSTACKTOP
subu    sp, k1, sp
```

设计含义：

- CPU0 使用 `KSTACKTOP`。
- CPU1 使用 `KSTACKTOP - (1 << KSTACKSHIFT)`。
- 避免两个 CPU 进入内核时压同一个 trapframe，造成栈破坏。

### 4.2 中断分流

相关文件：

- `kern/genex.S`
- `kern/smp.c`

`handle_int` 读取 `CP0_CAUSE` 和 `CP0_STATUS`，按中断 mask 分流：

- `STATUS_IM6`：IPI。
- `STATUS_IM7`：timer。

流程：

```text
interrupt
  -> handle_int
    -> IM6: handle_ipi_irq()
    -> IM7: RESET_KCLOCK + handle_timer_irq()
```

`handle_timer_irq()` 中先处理 pending IPI，再按 `timer_schedule_ready` 决定是否调度：

```c
handle_ipi_irq();
if (timer_schedule_ready) {
	schedule(0);
}
```

为什么需要 `timer_schedule_ready`：

- CPU1 很早就 online，但 CPU0 尚未完成 env 初始化、调度队列构建时，CPU1 不能立刻调度。
- CPU0 第一次进入调度路径后，通过 `smp_note_schedule_ready()` 设置该标志，CPU1 才开始正常 timer 调度。

### 4.3 IPI mailbox 机制

相关文件：

- `include/malta.h`
- `include/smp.h`
- `kern/smp.c`

接口：

```c
void smp_group_function_call(void (*fn)(u_int, u_int), u_int arg0, u_int arg1);
```

用途：

- 让当前 CPU 请求其它 CPU 执行一个内核函数。
- 典型场景是 TLB shootdown，也用于远端 env kill poke。

mailbox 数据：

```c
static volatile u_int ipi_mailbox[NR_CPUS][IPI_MBOX_NR];
static volatile int ipi_done[NR_CPUS];
static spinlock_t ipi_mailbox_lock[NR_CPUS];
```

发送流程：

1. 遍历所有 CPU，跳过自己和未 ready 的 CPU。
2. 获取目标 CPU 的 `ipi_mailbox_lock[cpu]`。
3. 写入函数指针和两个参数。
4. `ipi_done[cpu] = 0`。
5. `ipi_send(cpu, IPI_CALL)`。
6. 自旋等待 `ipi_done[cpu]`。
7. 等待期间如果本 CPU 也收到 IPI，主动调用 `handle_ipi_irq()`，避免双向 IPI 死锁。

接收流程：

1. `handle_ipi_irq()` 读取并清除 pending 状态。
2. 从 mailbox 取出函数和参数。
3. 执行函数。
4. 设置 `ipi_done[cpu] = 1`。

当前实现默认：

```c
#define SMP_USE_MMIO_IPI 0
```

也就是说，默认使用共享内存 pending 位模拟 IPI。`include/malta.h` 中保留了 MMIO IPI 寄存器布局，如果平台支持可启用 `SMP_USE_MMIO_IPI=1`。

答辩可讲的限制：

- 默认共享内存 IPI 依赖目标 CPU 在等待循环、timer interrupt 或调度路径中检查 pending。
- 这适合当前 QEMU/Malta + 课程实验环境，但不是完整硬件 IPI。

## 5. 自旋锁与共享资源保护

### 5.1 spinlock 基础

相关文件：

- `include/spinlock.h`
- `kern/spinlock.S`

锁状态：

```c
typedef volatile int spinlock_t;
#define SPINLOCK_INIT 0
```

实现使用 MIPS `ll/sc`：

- `spin_lock`：把 0 原子改成 1。
- `spin_unlock`：`sync` 后写 0。
- `atomic_add/atomic_sub/atomic_cas`：提供原子操作。

答辩重点：

- SMP 下关中断不能阻止另一个 CPU 修改共享数据。
- 因此共享链表、计数、mailbox、console 必须用真正的原子锁。

### 5.2 主要锁

| 锁 | 位置 | 保护对象 |
| --- | --- | --- |
| `console_lock` | `kern/printk.c`, `kern/syscall_all.c` | 串口输出和输入，避免字符交错 |
| `pmap_lock` | `kern/pmap.c` | `page_free_list`、`pp_ref`、页表项修改 |
| `asid_lock` | `kern/env.c` | ASID bitmap |
| `env_sched_lock` | `kern/sched.c` | `env_sched_list`、`env_running`、`env_cpu_id`、部分 IPC 状态 |
| `ipi_mailbox_lock[cpu]` | `kern/smp.c` | 每个目标 CPU 的 IPI mailbox |

## 6. 内存管理和 TLB shootdown

### 6.1 物理页管理锁

相关文件：

- `kern/pmap.c`

单核中 `page_free_list` 和 `pp_ref` 可以直接修改。SMP 中多个 CPU 可能同时：

- `page_alloc`
- `page_decref`
- `page_insert`
- `page_remove`

因此增加 `pmap_lock`。

重要规则：

```c
/* page_insert / page_remove 内部获取锁，在调用 tlb_invalidate 前释放。 */
```

为什么不能持 `pmap_lock` 调 `tlb_invalidate()`：

- `tlb_invalidate()` 会用 IPI 等待其它 CPU 执行远端函数。
- 远端 CPU 处理 IPI 或其它路径时也可能需要 `pmap_lock`。
- 如果本 CPU 持锁等待远端，远端又等同一把锁，就会死锁。

所以设计是：

1. 持 `pmap_lock` 修改页表和引用计数。
2. 释放 `pmap_lock`。
3. 再执行 TLB 失效广播。

### 6.2 TLB 失效广播

相关文件：

- `kern/tlbex.c`

单核只需要：

```c
tlb_out(entry_hi);
```

SMP 中每个 CPU 都有自己的 TLB，因此：

```c
void tlb_invalidate(u_int asid, u_long va) {
	tlb_invalidate_local(asid, va);
	if (smp_boot_ready == SMP_BOOT_READY) {
		smp_group_function_call(tlb_invalidate_local, asid, va);
	}
}
```

设计含义：

- 当前 CPU 立即本地失效。
- SMP 启动完成后，广播给其它 CPU。
- 早期 boot 阶段只有 CPU0 正常执行，跳过广播。

## 7. 调度器 SMP 改造

### 7.1 env 新增字段

相关文件：

- `include/env.h`

新增字段：

```c
int env_cpu_id;
int env_running;
int env_pinned_cpu;
int env_kill_pending;
volatile int env_kill_done;
```

字段含义：

- `env_cpu_id`：当前运行该 env 的 CPU，`-1` 表示未运行。
- `env_running`：是否正在某个 CPU 上运行。
- `env_pinned_cpu`：绑定 CPU，`-1` 表示不绑定。当前 `fs_serv` 被绑定到 CPU0。
- `env_kill_pending`：远端销毁请求，调度器必须跳过。
- `env_kill_done`：远端销毁完成通知。

### 7.2 调度锁和双重运行防护

相关文件：

- `kern/sched.c`

单核调度器只要从 `env_sched_list` 中取一个 runnable env。SMP 调度器必须保证：

- 两个 CPU 不能同时选中同一个 env。
- 修改 `env_sched_list` 时不能和 syscall/IPC/env_free 并发。

因此 `schedule()` 中持 `env_sched_lock`：

```c
spin_lock(&env_sched_lock);
...
if ((e->env_pinned_cpu < 0 || e->env_pinned_cpu == cpu) &&
    !e->env_kill_pending &&
    (e->env_running == 0 || e->env_cpu_id == cpu)) {
	goto found;
}
...
e->env_running = 1;
e->env_cpu_id = cpu;
spin_unlock(&env_sched_lock);
```

关键逻辑：

1. 进入调度前先 `handle_ipi_irq()`，处理 pending IPI。
2. 再 `env_check_kill_pending()`，如果当前 env 被远端要求销毁，则本地释放。
3. 如果当前 env 让出或时间片用完，清除旧 env 的 `env_running/env_cpu_id`。
4. 只选择：
   - 没有被 kill pending 的 env。
   - 没有绑定到其它 CPU 的 env。
   - 没有正在其它 CPU 运行的 env。
5. 选中后在锁内设置 `env_running = 1` 和 `env_cpu_id = cpu`。

### 7.3 每核时间片

单核代码通常用函数内 `static int count` 记录时间片。SMP 中这样会让两个 CPU 共享一个时间片计数，互相干扰。

当前改为：

```c
int count = cpu_data[cpu].sched_count;
...
cpu_data[cpu].sched_count = count;
```

也就是说，每个 CPU 独立维护自己的时间片。

## 8. syscall 和 IPC 的 SMP 改造

相关文件：

- `kern/syscall_all.c`

重要改动方向：

1. 所有“当前 env”相关逻辑使用 `cpu_curenv()`。
2. 所有“当前 CPU trapframe”相关逻辑使用 `cpu_trapframe()`。
3. 修改调度队列时持 `env_sched_lock`。
4. 控制台 syscall 持 `console_lock`。

例子：

- `sys_getenvid()` 返回 `cpu_curenv()->env_id`。
- `sys_exofork()` 从 `cpu_trapframe()` 复制当前 CPU 的 trapframe。
- `sys_set_env_status()` 修改 `env_sched_list` 时持 `env_sched_lock`。
- `sys_ipc_recv()` 阻塞当前 env 时，在锁内设置 `ENV_NOT_RUNNABLE`、清 `env_running/env_cpu_id`，并从调度队列移除。
- `sys_ipc_try_send()` 先完成共享页映射，再在锁内把接收方设回 runnable，避免接收方在页映射完成前被另一个 CPU 抢先运行。

答辩重点：

- 单核中 `sys_ipc_try_send()` 先后顺序不容易出问题。
- SMP 中如果过早把接收方设为 runnable，另一个 CPU 可能立刻运行接收方，看到尚未完成的 IPC page mapping。

## 9. env 生命周期和远端销毁

### 9.1 原始问题

在单核中，`env_destroy(e)` 可以直接 `env_free(e)`。但 SMP 中可能发生：

```text
CPU0: sys_env_destroy(child)
CPU1: child 正在用户态或内核态运行
```

如果 CPU0 直接释放 child 的页表、ASID、调度状态，就会导致 CPU1 继续使用已释放的地址空间和 env 结构。

最初实现为了避免内存破坏，遇到这种情况直接 panic：

```text
env_destroy: env XXXXXXXX running on CPU 1, cannot destroy from CPU 0
```

这是正确暴露问题，但不是最终策略。

### 9.2 当前同步远端销毁设计

相关文件：

- `include/env.h`
- `kern/env.c`
- `kern/sched.c`

核心策略：

1. 发起 CPU 发现目标 env 正在其它 CPU 运行。
2. 在 `env_sched_lock` 内设置：
   - `env_kill_pending = 1`
   - `env_kill_done = 0`
3. 用 `smp_group_function_call()` poke 目标 CPU。
4. 发起 CPU 等待 `env_kill_done == 1`。
5. 目标 CPU 在调度安全点调用 `env_check_kill_pending()`。
6. 目标 CPU 本地执行 `env_free(e)`。
7. `env_free()` 设置 `env_kill_done = 1`，通知发起 CPU。

### 9.3 为什么必须由目标 CPU 本地释放

如果目标 env 正在 CPU1 运行，只有 CPU1 清楚它当前是否还在使用：

- 当前 trapframe。
- 当前地址空间。
- 当前 `cpu_data[1].curenv`。
- 当前 `cpu_data[1].cur_pgdir`。

由 CPU1 自己释放，可以保证：

- 释放前 CPU1 已经回到内核安全点。
- 释放后 CPU1 清空自己的 `curenv`。
- CPU1 不会继续返回到已经释放的用户地址空间。

### 9.4 为什么真正释放放在调度安全点

一次 debug 中发现，如果在 `handle_ipi_irq()` 内直接执行 `env_free()`，会出问题：

```text
CPU0: 等待 remote kill IPI 完成
CPU1: IPI handler 中 env_free()
CPU1: env_free() 中 page_remove/tlb_invalidate()
CPU1: tlb_invalidate() 又向 CPU0 发 IPI
```

这会让 IPI 协议嵌套：

- `ipi_done` 是每 CPU 单个完成标志。
- IPI handler 内再发 IPI，容易覆盖等待状态或形成复杂死锁。

因此当前设计中：

- IPI 只负责 poke 目标 CPU。
- 真正的 `env_free()` 在 `schedule()` 开头的安全点执行。
- `handle_ipi_irq()` 不直接调用 `env_check_kill_pending()`。

### 9.5 等待远端销毁时为什么临时关闭本地中断

双向远端销毁测试中曾出现：

```text
[1] remote_destroy_bidirectional: cpu1 destroys 00002003 on cpu0
[1] remote_destroy_bidirectional: cpu1 destroys 00002003 on cpu0
```

原因是：

- CPU1 正在 `env_wait_remote_destroy()` 等 CPU0 完成销毁。
- timer interrupt 又打断 CPU1。
- CPU1 重新进入调度钩子，导致同一个等待栈里重入远端销毁逻辑。

修复：

```c
static void env_wait_remote_destroy(struct Env *e, u_int envid) {
	u_int status = env_irq_save();

	smp_group_function_call(env_remote_kill_poke, envid, 0);
	while (e->env_id == envid && !e->env_kill_done) {
		handle_ipi_irq();
		__asm__ volatile("nop");
	}
	env_irq_restore(status);
}
```

关键点：

- 临时关闭本 CPU 的全局中断，避免 timer reentry。
- 循环中仍主动 `handle_ipi_irq()`，所以 TLB shootdown 这类 IPI 仍可被处理。
- 等待结束后恢复原 CP0 Status。

### 9.6 为什么本地销毁路径也设置 kill_pending

`env_destroy()` 中，即使目标不是“正在其它 CPU 运行”，也先设置 `env_kill_pending = 1`：

```c
} else {
	e->env_kill_pending = 1;
}
```

目的：

- 如果目标仍在 runnable 队列中，释放锁后可能被另一个 CPU 立刻调度。
- 先打 `kill_pending`，调度器会跳过该 env。
- 然后当前 CPU 再执行本地 `env_free()`，避免“检查时未远端运行，释放前被远端抢走”的 race。

## 10. 文件系统进程绑定策略

相关文件：

- `kern/env.c`

当前在创建 `fs_serv` 时：

```c
if (strcmp(name, "fs_serv") == 0) {
	e->env_pinned_cpu = 0;
}
```

设计原因：

- 文件系统和 IDE 路径在课程代码中本来就有较多共享状态。
- 为了先让 SMP 调度、IPC、TLB 等核心路径稳定，暂时把 `fs_serv` 绑定到 CPU0。
- 这不是最终高性能设计，但能降低多核移植初期的不确定性。

答辩时可以说：

- 这是阶段性保守策略。
- 真正完整的 SMP FS 需要继续梳理 FS buffer、IDE 状态、文件系统请求队列的锁粒度。

## 11. 测试用例构造策略

### 11.1 总体策略

多核测试不能只看“程序跑完”。需要主动制造并发窗口，观察以下性质：

1. 两个 CPU 都实际参与执行。
2. IPI 能双向通信，且嵌套 IPI 不死锁。
3. 调度器不会把同一个 env 同时分配给两个 CPU。
4. 远端销毁能同步完成。
5. env slot 复用不会破坏销毁等待条件。
6. 双向远端销毁不会因为 timer、IPI、TLB shootdown 交错而死锁。

### 11.2 测试构建机制

相关文件：

- `mk/tests.mk`
- `tests/lab6_X/kernel.mk`

测试机制有两类：

1. `init-envs`
   - 通过 `tools/init-gen` 指定初始用户进程。
   - 例如 `init-envs := remote_destroy_once/1`。

2. `pre-env-run`
   - 通过 `MOS_PRE_ENV_RUN` 在 `env_run(e)` 中插入测试钩子。
   - 适合内核侧断言，例如检查调度器是否真的在两个 CPU 上运行。

`mk/tests.mk` 会生成：

```text
include/generated/init_override.h
include/generated/pre_env_run.h
```

这些是构建产物，不应提交。

### 11.3 `lab6_3`: IPI 双向嵌套 roundtrip

文件：

- `tests/lab6_3/init.c`

构造方式：

- CPU0 连续 128 次调用 `smp_group_function_call(cpu1_nested_handler, ...)`。
- CPU1 的 handler 内再反向调用 CPU0 的 `cpu0_callback`。

验证点：

- CPU1 handler 必须运行在 CPU1。
- CPU0 callback 必须运行在 CPU0。
- 参数传递不能错。
- 双向嵌套 IPI 不能死锁。
- 计数必须都等于 128。

通过输出：

```text
smp_ipi_roundtrip passed: cpu1_calls=128 cpu0_callbacks=128
```

### 11.4 `lab6_4`: 多核并行调度

文件：

- `tests/lab6_4/kernel.mk`
- `tests/lab6_4/pre_env_run.c`
- `tests/lab6_4/loop.S`

构造方式：

- 启动 4 个无限循环 env。
- 在 `env_run(e)` 前插入 `pre_env_run(e)`。
- 统计每个 CPU 进入调度的次数。

验证点：

- `cpu_id()` 必须合法。
- 被调度 env 的 `env_running == 1`。
- `env_cpu_id == 当前 CPU`。
- CPU0 和 CPU1 都必须至少调度一次。
- 总运行次数达到 32 后通过。

通过输出：

```text
smp_sched_parallel passed: total=32 cpu0=16 cpu1=16
```

### 11.5 `lab6_5`: 单次远端 env destroy

文件：

- `tests/lab6_5/remote_destroy_once.c`

构造方式：

- 父进程 fork child。
- child 无限 `syscall_yield()`。
- 父进程通过 UENVS 观察 child 的 `env_running/env_cpu_id`。
- 等 child 正在不同 CPU 上运行时，父进程调用 `syscall_env_destroy(child)`。

验证点：

- `syscall_env_destroy` 不 panic。
- child 被远端 CPU 本地释放。
- 父进程等待到 child `ENV_FREE` 或 env slot id 改变。

通过输出：

```text
remote_destroy_once passed
```

### 11.6 `lab6_6`: 多轮远端 destroy 和 env slot 复用

文件：

- `tests/lab6_6/remote_destroy_repeat.c`

构造方式：

- 重复 4 轮：
  1. fork child。
  2. 等 child 在远端 CPU 运行。
  3. destroy child。
  4. 等 child free。

验证点：

- `env_kill_done` 不会因为 env slot 复用被旧等待误判。
- 多轮销毁不会留下调度队列残留。
- ASID、页表、env free list 在多轮释放后仍可继续使用。

通过输出：

```text
remote_destroy_repeat passed
```

### 11.7 `lab6_7`: 双向远端 destroy 竞态

文件：

- `tests/lab6_7/pre_env_run.c`

构造方式：

- 启动 4 个 loop env。
- 用 `pre_env_run(e)` 记录每个 CPU 最近运行的 env。
- 当 CPU0 发现 CPU1 正在运行某个 env，就从 CPU0 destroy 它。
- 当 CPU1 发现 CPU0 正在运行某个 env，就从 CPU1 destroy 它。
- 两个方向都成功后 halt。

验证点：

- 双向远端 destroy 不死锁。
- timer reentry 不会重复销毁同一个 victim。
- `env_wait_remote_destroy()` 等待时仍能处理 TLB shootdown IPI。
- 被销毁 victim 最终 `ENV_FREE`。

通过输出：

```text
remote_destroy_bidirectional passed
```

## 12. 最近一次 debug 过程记录

### 12.1 问题 1：远端运行 env 被 destroy 时 panic

最初发现的问题：

```text
env_destroy: env XXXXXXXX running on CPU 1, cannot destroy from CPU 0
```

原因：

- 单核 `env_destroy()` 直接释放目标 env。
- SMP 中目标可能正在另一个 CPU 运行。
- 直接释放会造成 use-after-free，所以旧实现选择 panic。

最终方案：

- 不再 panic。
- 引入 `env_kill_pending/env_kill_done`。
- 远端运行时，请求目标 CPU 在调度安全点本地释放。

### 12.2 问题 2：第一次实现后，child 被 free 但父进程没有打印 passed

现象来自 `lab6_5` 初次运行：

```text
remote_destroy_once: destroy child 00001001 from cpu 0 while it runs on cpu 1
[0] [00000800] destroying 00001001
[1] [00001001] free env 00001001
```

没有出现：

```text
remote_destroy_once passed
```

分析：

- 当时在 `handle_ipi_irq()` 末尾直接调用 `env_check_kill_pending()`。
- CPU1 在 IPI handler 中执行 `env_free()`。
- `env_free()` 会释放页表并触发 `tlb_invalidate()`。
- `tlb_invalidate()` 又通过 `smp_group_function_call()` 向其它 CPU 发 IPI。
- 这导致 IPI handler 内嵌套 IPI 协议，`ipi_done` 状态容易互相覆盖或死锁。

修复：

- `handle_ipi_irq()` 不再直接释放 env。
- 只让 IPI 唤醒目标 CPU。
- 真正释放放到 `schedule()` 开头的 `env_check_kill_pending()`。

修复后 `lab6_5` 输出：

```text
[1] [00001001] free env 00001001
[1] i am killed remotely ...
remote_destroy_once passed
```

### 12.3 问题 3：双向 destroy 中同一 CPU 重复发起销毁

`lab6_7` 曾出现：

```text
[1] remote_destroy_bidirectional: cpu1 destroys 00002003 on cpu0
[1] remote_destroy_bidirectional: cpu1 destroys 00002003 on cpu0
[0] [00002003] free env 00002003
```

分析：

- CPU1 已经进入 `env_wait_remote_destroy()` 等待 CPU0 释放。
- timer interrupt 打断 CPU1，重新进入调度路径和测试钩子。
- 于是同一 CPU 在同一个等待尚未结束时，再次尝试 destroy 同一 victim。

修复：

- 在 `env_wait_remote_destroy()` 中保存 CP0 Status，并临时清 `STATUS_IE`。
- 等待期间主动 `handle_ipi_irq()`。
- 等待结束恢复 Status。

这样既避免 timer reentry，又保留 IPI 响应能力。

### 12.4 问题 4：非远端运行目标释放前可能被其它 CPU 抢走

潜在 race：

```text
CPU0: env_destroy(e)，检查时 e 没有在远端运行
CPU0: 释放 env_sched_lock
CPU1: schedule() 选中 e 并开始运行
CPU0: env_free(e)
```

修复：

- 在 `env_destroy()` 中，只要目标不是 `ENV_FREE`，就先在锁内打 `env_kill_pending = 1`。
- 调度器选择 env 时跳过 `env_kill_pending`。
- 这样 CPU0 解锁后，CPU1 也不会再选中该 env。

### 12.5 最终验证结果

最终运行过：

```bash
make test lab=6_5
timeout 15s make run

make test lab=6_6
timeout 20s make run

make test lab=6_7
timeout 15s make run
```

观察到：

- `remote_destroy_once passed`
- `remote_destroy_repeat passed`
- `remote_destroy_bidirectional passed`

说明远端销毁、重复销毁、双向竞态都被覆盖。

注意：

- `lab6_5` 和 `lab6_6` 的 QEMU 最后由 `timeout` 结束，是因为用户测试进程返回后系统没有主动 halt。
- 判断通过的依据是测试自己的 `passed` 输出。
- `lab6_7` 会主动 `halt()`，因此 QEMU 正常退出。

## 13. 答辩高频问题准备

### Q1: 为什么不能继续使用全局 `curenv`？

因为两个 CPU 可以同时运行不同 env。全局 `curenv` 只能表达一个当前 env，会互相覆盖。SMP 中必须把当前 env 改成 per-CPU 状态，即 `cpu_data[cpu_id()].curenv`。

### Q2: 为什么调度器需要 `env_running/env_cpu_id`？

`env_status == ENV_RUNNABLE` 只表示 env 可以被调度，不表示它现在是否已经在某个 CPU 上运行。SMP 中如果两个 CPU 同时从 runnable list 选择同一个 env，会造成同一个用户上下文并发运行。因此需要：

- `env_running` 表示已被某 CPU 占用。
- `env_cpu_id` 记录占用者。
- 调度器在锁内检查并设置这两个字段。

### Q3: 为什么 TLB 失效要广播？

每个 CPU 有自己的 TLB。CPU0 修改页表后，CPU1 的 TLB 可能还缓存旧映射。如果不通知 CPU1，CPU1 可能继续使用旧映射，导致权限错误、访问已释放页或数据不一致。因此 `tlb_invalidate()` 要本地失效并 IPI 广播远端失效。

### Q4: 为什么 TLB 广播不能在 `pmap_lock` 内做？

TLB 广播需要等待其它 CPU 执行 IPI handler。如果本 CPU 持有 `pmap_lock` 等远端，而远端处理过程中也需要 `pmap_lock`，就会死锁。因此先释放锁，再广播 TLB 失效。

### Q5: 为什么远端 destroy 要等目标 CPU 自己释放？

因为目标 CPU 可能正在使用该 env 的页表、trapframe 和 `curenv`。如果其它 CPU 直接释放，会产生 use-after-free。让目标 CPU 在调度安全点释放，可以保证它已经回到内核并能安全切换到其它 env。

### Q6: 为什么不在 IPI handler 里直接释放？

因为释放 env 会触发页表释放和 TLB shootdown，而 TLB shootdown 本身也用 IPI。如果在 IPI handler 中嵌套 IPI 协议，当前简单 mailbox/`ipi_done` 设计容易死锁或状态覆盖。当前方案把 IPI handler 保持为轻量函数执行和 ack，复杂释放放到调度安全点。

### Q7: 这套实现还有什么限制？

主要限制：

1. 默认 IPI 是共享内存 pending 模拟，不是真硬件 MMIO IPI。
2. 当前只配置 `NR_CPUS = 2`。
3. `fs_serv` 暂时绑定 CPU0，文件系统还不是完整细粒度 SMP。
4. 调度器没有 idle env，测试进程退出后可能空转，需要 timeout 结束。
5. `env_free()` 释放一个地址空间时可能产生多次 TLB shootdown，性能不是最优，但逻辑正确。

## 14. 代码阅读路线

建议答辩前按下面顺序读代码：

1. `Makefile` 和 `include.mk`
   - 看 QEMU `-smp 2` 和 `-march=24kc`。

2. `include/smp.h`
   - 看 `NR_CPUS`、`cpu_local_data`、SMP 接口。

3. `init/start.S`
   - 看 CPU0/CPU1 启动分流。

4. `include/stackframe.h` 和 `kern/genex.S`
   - 看每核异常栈和中断分流。

5. `kern/smp.c`
   - 看 `cpu_id()`、per-CPU 初始化、IPI mailbox、timer 调度开关。

6. `include/spinlock.h` 和 `kern/spinlock.S`
   - 看 `ll/sc` 自旋锁。

7. `kern/pmap.c` 和 `kern/tlbex.c`
   - 看 `pmap_lock` 和 TLB shootdown。

8. `include/env.h`、`kern/sched.c`、`kern/env.c`
   - 看 env 多核运行状态、调度锁、远端销毁。

9. `kern/syscall_all.c`
   - 看 `cpu_curenv()`、`cpu_trapframe()` 和 IPC 锁。

10. `tests/lab6_3` 到 `tests/lab6_7`
    - 看每个测试如何构造并发场景和通过条件。

## 15. 一句话总结

这次 SMP 改造的核心不是“让两个 CPU 都启动”这么简单，而是系统性消除单核假设：把当前状态 per-CPU 化，把共享结构加锁，把 TLB 操作广播化，把调度器改成不会双重运行 env，并把远端 env 生命周期操作改成由目标 CPU 在安全点同步完成。
