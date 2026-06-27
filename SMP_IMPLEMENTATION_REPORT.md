# MOS SMP 多核移植实现报告

本文依据 `task.md` 中提出的 SMP 移植目标整理当前实现。报告重点说明每一项要求在当前代码中的实现逻辑、与教程建议路径不同的地方，以及测试和调试中发现问题后做出的调整。

## 1. 任务目标与实现概览

`task.md` 要求基于 MIPS R24K/QEMU Malta 将单核 MOS 扩展为 SMP 内核，核心目标包括：

- 支持至少两个处理器核。
- 支持核间通信和 IPI。
- 支持启动 SHELL。
- 支持进程在多个核心上调度。
- 支持多核下的锁、页表管理和 TLB 同步。
- 对文件系统和设备访问做出适合多核环境的约束。

当前实现的整体策略是：

- QEMU 使用 `-smp 2 -cpu 24Kc -M malta` 启动两个 MIPS vCPU。
- 所有 CPU 共享同一个内核镜像和内核地址空间。
- CPU0 执行全局初始化；CPU1 进入等待路径，待 CPU0 设置启动栅栏后上线。
- 每个 CPU 拥有独立内核栈和 per-CPU 状态。
- 共享数据结构通过 `spinlock`、`pmap_lock`、`env_sched_lock`、`console_lock` 等锁保护。
- IPI 默认使用共享内存 `ipi_pending[] + ipi_mailbox[][]` 模拟，保留 MMIO IPI 分支。
- 页表修改后通过 `tlb_invalidate()` 广播 TLB shootdown。
- 普通用户进程可在两个 CPU 上调度，文件系统服务进程固定在 CPU0。

## 2. 与教程建议路线的主要差异

教程中给出了若干建议实现路径，例如使用 `$gp` 保存每核数据指针、通过 `IPI_START` 启动从核、为从核设置 `slave_exc_gen_entry`。当前实现没有完全采用这些路径，主要差异如下。

### 2.1 未使用 `$gp` 保存 per-CPU 指针

教程建议：

```c
#define DECLARE_LOCAL_DATA_PTR register volatile ld_t *ld asm("$28")
```

即用 `$gp` 指向当前 CPU 的 local data。

当前实现没有占用 `$gp`。我们定义了全局 per-CPU 表：

```c
struct cpu_local_data cpu_data[NR_CPUS];
```

并通过 `cpu_id()` 读取 CP0 `EBase` 低位，再访问：

```c
cpu_data[cpu_id()].curenv
cpu_data[cpu_id()].cur_pgdir
cpu_data[cpu_id()].kernel_stack_top
cpu_data[cpu_id()].sched_count
```

这样同样实现了每核独立状态。好处是实现直观，不需要在汇编启动路径和异常入口中维护 `$gp`。当前编译参数使用 `-G 0 -mno-abicalls -fno-pic`，也降低了编译器对传统 `$gp` 小数据区访问的依赖。

### 2.2 从核启动未使用 `IPI_START`

教程建议主核通过 mailbox 填入 `ra/sp/gp/a1` 等启动参数，再发送 `IPI_START` 唤醒从核。

当前本地 QEMU Malta/24Kc 未暴露教程中的 Loongson-style MMIO IPI 控制器，因此从核启动没有依赖 `IPI_START`。当前路径是：

```text
所有 CPU -> _start
CPU0 -> 清 .bss -> smp_init() -> mips_init()
CPU1 -> secondary_wait -> 等 smp_boot_ready -> 设置独立栈 -> smp_secondary_start()
```

也就是说，当前从核不是被主核 IPI 唤醒，而是已经进入 `_start` 后等待 CPU0 完成基础初始化。

### 2.3 没有单独的 `slave_exc_gen_entry`

教程建议为从核设置专门异常入口 `slave_exc_gen_entry`，用于读取 IPI 启动消息。

当前所有 CPU 共用 `exc_gen_entry`。这本身没有问题，因为异常入口会根据当前 CPU ID 切换到对应内核栈，且当前进程、页目录等状态都通过 per-CPU 表访问。当前不需要读取 `IPI_START` 启动 mailbox，因此也不需要单独的从核早期异常入口。

### 2.4 IPI 默认不是硬件中断，而是共享内存模拟

当前 `include/smp.h` 中默认：

```c
#define SMP_USE_MMIO_IPI 0
```

因此默认发送 IPI 时执行：

```c
ipi_pending[cpu] |= signal;
```

如果平台提供 MMIO IPI，可以启用 `SMP_USE_MMIO_IPI=1`，使用 `include/malta.h` 中保留的 `IPI_STATUS/IPI_SET/IPI_CLEAR/IPI_MAILBOX` 宏。当前默认实现适配本地 QEMU 环境，但不是完整硬件 IPI 实现。

## 3. 内核同步与锁机制

### 3.1 自旋锁与原子操作

`include/spinlock.h` 声明：

```c
typedef volatile int spinlock_t;
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
int atomic_add(int *ptr, int value);
int atomic_sub(int *ptr, int value);
int atomic_cas(void *ptr, int old_value, int new_value);
```

这些函数在 `kern/spinlock.S` 中用 MIPS `ll/sc` 和 `sync` 实现：

- `spin_lock` 使用 `ll/sc` 将锁从 0 原子改为 1。
- `spin_unlock` 先 `sync`，再写 0。
- `atomic_add/sub/cas` 为共享计数和状态提供原子操作基础。

SMP 下关中断只能阻止当前 CPU 被打断，不能阻止另一个 CPU 同时修改共享数据。因此调度队列、页表、mailbox、console、设备 MMIO 等都必须使用跨 CPU 可见的锁。

### 3.2 当前主要锁

| 锁 | 位置 | 保护对象 |
| --- | --- | --- |
| `console_lock` | `kern/printk.c`, `kern/syscall_all.c` | 串口输出、控制台输入和 console MMIO |
| `ide_dev_lock` | `kern/syscall_all.c` | IDE MMIO syscall 路径 |
| `env_sched_lock` | `kern/sched.c` | `env_sched_list`、`env_running`、`env_cpu_id`、IPC 阻塞/唤醒 |
| `pmap_lock` | `kern/pmap.c` | `page_free_list`、页引用计数、页表项修改 |
| `asid_lock` | `kern/env.c` | ASID bitmap 分配与释放 |
| `ipi_mailbox_lock[cpu]` | `kern/smp.c` | 单个目标 CPU 的 IPI mailbox |

这些锁都是内核全局变量或静态全局变量，位于所有 CPU 共享可见的内核地址空间中。

## 4. 核间通信与 IPI mailbox

### 4.1 数据结构

当前 IPI mailbox 相关数据位于 `kern/smp.c`：

```c
static volatile int ipi_ready[NR_CPUS];
static volatile int ipi_done[NR_CPUS];
static volatile u_int ipi_pending[NR_CPUS];
static volatile u_int ipi_mailbox[NR_CPUS][IPI_MBOX_NR];
static spinlock_t ipi_mailbox_lock[NR_CPUS];
```

mailbox 槽位为：

```c
IPI_MBOX_FN
IPI_MBOX_ARG0
IPI_MBOX_ARG1
```

即传递一个函数指针和两个参数。

### 4.2 发送流程

`smp_group_function_call(fn, arg0, arg1)` 用于让其它已上线 CPU 执行指定函数。发送流程是：

1. 遍历所有 CPU，跳过当前 CPU 和未 `ipi_ready` 的 CPU。
2. 获取目标 CPU 的 `ipi_mailbox_lock[cpu]`。
3. 清 `ipi_done[cpu]`。
4. 写入函数指针和参数。
5. 调用 `ipi_send(cpu, IPI_CALL)`。
6. 等待目标 CPU 设置 `ipi_done[cpu] = 1`。
7. 清除 mailbox 中的函数指针并释放锁。

等待过程中如果本 CPU 也收到 IPI，会主动调用 `handle_ipi_irq()`。这个设计用于避免双向 IPI 调用时双方互等死锁。

### 4.3 接收流程

`handle_ipi_irq()` 读取当前 CPU 的 IPI 状态：

```c
status = ipi_status_read(cpu);
```

默认共享内存模式下等价于读取 `ipi_pending[cpu]`。随后清除状态，并根据类型处理：

- `IPI_START`：标记 `ipi_ready[cpu] = 1`。
- `IPI_CALL`：从 mailbox 读取函数和参数，执行函数，设置 `ipi_done[cpu] = 1`。

当前真正大量使用的是 `IPI_CALL`，用于 TLB shootdown、远端销毁 poke 和测试中的跨核调用。`IPI_START` 没有用于当前从核启动路径。

## 5. 内核启动与每核状态

### 5.1 QEMU 多核启动参数

`Makefile` 中使用：

```make
QEMU_FLAGS += -smp 2 -cpu 24Kc -m 64 -nographic -M malta -no-reboot
```

`include.mk` 中使用：

```make
-march=24kc
```

这满足任务中至少两个 MIPS 24Kc CPU 的运行要求。

### 5.2 `_start` 主从核分流

`init/start.S` 在入口处先关闭中断：

```asm
mtc0    zero, CP0_STATUS
```

随后读取 CP0 `EBase` 低位作为 CPU ID：

```asm
mfc0    t0, CP0_EBASE
andi    t0, t0, 0x03ff
bnez    t0, secondary_wait
```

CPU0 路径：

- 清 `.bss`。
- 设置 `sp = KSTACKTOP`。
- 保存 boot 参数 `a0-a3`。
- 调用 `smp_init()`。
- 恢复 boot 参数并跳转 `mips_init`。

从核路径：

- 不清 `.bss`，避免破坏 CPU0 已初始化的全局状态。
- 等待 `smp_boot_ready == SMP_BOOT_READY`。
- 按 CPU ID 计算独立内核栈。
- 跳转 `smp_secondary_start()`。

### 5.3 `smp_init()` 和 `smp_secondary_start()`

`smp_init()` 由 CPU0 调用，负责：

- 初始化 `cpu_data[i]`。
- 设置每个 CPU 的 `kernel_stack_top`。
- 初始化 IPI 状态、mailbox 锁和 pending 状态。
- 初始化 CPU0 的 timer/interrupt。
- 设置 `smp_boot_ready = SMP_BOOT_READY`，允许从核继续启动。

`smp_secondary_start()` 由从核执行，负责：

- 获取当前 `cpu_id()`。
- 调用 `setup_cpu_interrupts()` 打开 timer/IPI 相关中断。
- 设置 `ipi_ready[cpu] = 1`。
- 等待 `timer_schedule_ready`。
- 调度系统 ready 后调用 `schedule(0)`，正式参与调度。

### 5.4 每核状态

`include/smp.h` 中定义：

```c
struct cpu_local_data {
	int cpu_id;
	struct Env *curenv;
	Pde *cur_pgdir;
	u_long kernel_stack_top;
	int sched_count;
};
```

`curenv` 和 `cur_pgdir` 从单核全局概念改为每 CPU 状态。内核中通过：

- `cpu_curenv()`
- `cpu_cur_pgdir()`
- `cpu_kstack_top()`
- `cpu_trapframe()`

访问当前 CPU 的运行状态。

## 6. 异常处理、timer 和中断分流

### 6.1 共用异常入口与独立内核栈

当前所有 CPU 共用 `kern/entry.S` 中的 `exc_gen_entry`。关键在 `SAVE_ALL`：当异常从用户态进入时，会读取 CP0 `EBase` 低位并选择当前 CPU 的内核栈：

```asm
mfc0    sp, CP0_EBASE
andi    sp, sp, 0x03ff
sll     sp, sp, KSTACKSHIFT
li      k1, KSTACKTOP
subu    sp, k1, sp
```

因此 CPU0 和 CPU1 即使同时进入异常，也不会压同一个 trapframe。

### 6.2 中断分流

`kern/genex.S` 中的 `handle_int` 读取 `CP0_CAUSE` 和 `CP0_STATUS`：

```asm
mfc0    t0, CP0_CAUSE
mfc0    t2, CP0_STATUS
and     t0, t2
```

然后按中断位分流：

- `STATUS_IM6`：IPI，调用 `handle_ipi_irq()`。
- `STATUS_IM7`：timer，重置时钟后调用 `handle_timer_irq()`。

`setup_cpu_interrupts()` 会设置：

```c
STATUS_IE | STATUS_IM6 | STATUS_IM7
```

即打开全局中断、IPI 中断位和 timer 中断位。

### 6.3 timer 调度

`handle_timer_irq()` 中先处理 pending IPI，再根据 `timer_schedule_ready` 决定是否进入调度：

```c
handle_ipi_irq();
if (timer_schedule_ready) {
	schedule(0);
}
```

`timer_schedule_ready` 用于避免从核过早进入调度。CPU0 首次进入 `schedule()` 后通过 `smp_note_schedule_ready()` 设置该标志，从核之后才正常参与 timer 调度。

## 7. 内存管理与 TLB 同步

### 7.1 页表锁

`kern/pmap.c` 中定义：

```c
static spinlock_t pmap_lock = SPINLOCK_INIT;
```

它保护：

- `page_free_list`
- `pp_ref`
- 页表项和页目录项修改

`page_alloc()`、`page_free()`、`page_decref()`、`page_insert()`、`page_remove()` 均围绕共享页管理状态加锁。

当前采用的是粗粒度全局页表锁，不是教程中提到的 CAS 无锁页表管理。这样性能不是最优，但实现简单可靠，足以保证多核并发下页表和物理页引用计数的一致性。

### 7.2 TLB shootdown

单核 MOS 修改页表后只需清当前 CPU 的 TLB。SMP 下每个 CPU 有独立 TLB，因此修改页表后必须通知其它 CPU。

当前 `kern/tlbex.c` 拆分为：

```c
void tlb_invalidate_local(u_int asid, u_long va);
void tlb_invalidate(u_int asid, u_long va);
```

`tlb_invalidate()` 先本地失效，然后在 SMP 启动完成后广播：

```c
tlb_invalidate_local(asid, va);
if (smp_boot_ready == SMP_BOOT_READY) {
	smp_group_function_call(tlb_invalidate_local, asid, va);
}
```

### 7.3 为什么 TLB 失效放在 `pmap_lock` 外

`tlb_invalidate()` 会通过 `smp_group_function_call()` 等待其它 CPU 执行远端函数。如果持有 `pmap_lock` 时等待 IPI，而远端 CPU 又需要同一把锁，就可能死锁。

因此 `page_insert()` 和 `page_remove()` 的顺序是：

1. 持 `pmap_lock` 修改页表和引用计数。
2. 释放 `pmap_lock`。
3. 调用 `tlb_invalidate()`。

### 7.4 性能说明

`env_free()` 释放地址空间时会逐页调用 `page_remove()`，因此可能产生多次 TLB shootdown。该实现逻辑正确，因为每个删除的 PTE 都会同步失效；但性能不是最优。更高效的实现可以批量删除映射后按 ASID 或地址范围一次性 shootdown。

## 8. 多核进程调度

### 8.1 Env 新增 SMP 字段

`include/env.h` 中增加：

```c
int env_cpu_id;
int env_running;
int env_pinned_cpu;
int env_kill_pending;
volatile int env_kill_done;
```

含义：

- `env_cpu_id`：当前运行该 env 的 CPU。
- `env_running`：该 env 是否正在某个 CPU 上运行。
- `env_pinned_cpu`：绑定 CPU，`-1` 表示不绑定。
- `env_kill_pending`：远端销毁请求。
- `env_kill_done`：远端销毁完成通知。

### 8.2 全局调度队列锁

`kern/sched.c` 中定义：

```c
spinlock_t env_sched_lock = SPINLOCK_INIT;
```

`schedule()` 持锁遍历 `env_sched_list`，只选择满足以下条件的 env：

```c
(e->env_pinned_cpu < 0 || e->env_pinned_cpu == cpu)
!e->env_kill_pending
(e->env_running == 0 || e->env_cpu_id == cpu)
```

选中后在锁内设置：

```c
e->env_running = 1;
e->env_cpu_id = cpu;
```

这样避免两个 CPU 同时运行同一个 env。

### 8.3 每核时间片

单核 MOS 中时间片计数可以是函数静态变量。SMP 中如果共享一个 `count`，两个 CPU 会互相干扰。当前改为：

```c
int count = cpu_data[cpu].sched_count;
...
cpu_data[cpu].sched_count = count;
```

每个 CPU 维护自己的时间片。

### 8.4 IPC 与调度状态一致性

`sys_ipc_recv()` 阻塞当前 env 时，在 `env_sched_lock` 内：

- 设置 `env_ipc_recving`。
- 设置 `ENV_NOT_RUNNABLE`。
- 清除 `env_running/env_cpu_id`。
- 从调度队列移除。

`sys_ipc_try_send()` 先完成可选共享页映射，再写 IPC 字段，最后才把接收方设为 `ENV_RUNNABLE` 并加入调度队列。这样避免 SMP 下另一个 CPU 过早运行接收方，看到尚未完成的 IPC page mapping。

## 9. Env 生命周期与远端销毁

### 9.1 问题

单核中 `env_destroy(e)` 可以直接 `env_free(e)`。SMP 中目标 env 可能正在另一个 CPU 上运行。如果发起 CPU 直接释放其页表、ASID 和 Env 结构，目标 CPU 可能继续使用已释放资源，造成 use-after-free。

### 9.2 当前设计

`env_destroy()` 如果发现目标 env 正在其它 CPU 上运行：

```c
e->env_kill_pending = 1;
e->env_kill_done = 0;
```

然后通过：

```c
smp_group_function_call(env_remote_kill_poke, envid, 0);
```

poke 目标 CPU，并等待 `env_kill_done`。

目标 CPU 在 `schedule()` 开头调用 `env_check_kill_pending()`。如果当前 env 被标记 kill pending，则由目标 CPU 本地执行 `env_free(e)`，清空当前 CPU 的 `curenv`，并重新调度。

### 9.3 为什么不在 IPI handler 中直接释放

调试中发现，如果在 `handle_ipi_irq()` 内直接调用 `env_free()`，会出现复杂嵌套：

```text
IPI handler -> env_free -> page_remove -> tlb_invalidate -> smp_group_function_call
```

这会在 IPI handler 中再次发 IPI，容易覆盖每 CPU 单个 `ipi_done` 状态或形成死锁。因此当前 IPI 只负责 poke，真正释放放在调度安全点。

### 9.4 等待远端销毁时临时关中断

双向 remote destroy 测试中曾出现同一 CPU 重复发起销毁的问题。原因是等待远端销毁时，timer interrupt 可能重入调度测试钩子。

修复方式是 `env_wait_remote_destroy()` 保存 CP0 Status，并临时清 `STATUS_IE`：

```c
u_int status = env_irq_save();
...
while (...) {
	handle_ipi_irq();
	__asm__ volatile("nop");
}
env_irq_restore(status);
```

这样 timer 不会打断等待路径，但循环中仍主动处理 pending IPI，避免 TLB shootdown 或双向 IPI 等待死锁。

### 9.5 本地销毁也设置 `kill_pending`

即使目标 env 没有在远端 CPU 上运行，`env_destroy()` 也会先设置 `env_kill_pending = 1`。这是为了防止释放锁后该 env 被另一个 CPU 立刻调度走。调度器会跳过 kill pending 的 env，从而关闭这个竞态窗口。

## 10. 文件系统、设备与 SHELL

### 10.1 文件系统进程绑定

任务允许为了降低难度让主核负责文件操作。当前实现中，创建 `fs_serv` 时设置：

```c
if (strcmp(name, "fs_serv") == 0) {
	e->env_pinned_cpu = 0;
}
```

调度器会检查 `env_pinned_cpu`，因此 `fs_serv` 只在 CPU0 上运行。普通用户进程仍可在 CPU0/CPU1 上调度，并通过 IPC 请求文件服务。

当前实现不是“禁止 CPU1 上的用户进程发起写文件请求”，而是“所有真实文件系统服务、block cache、open table 和 IDE PIO 操作集中在 CPU0 上的 `fs_serv` 中执行”。

### 10.2 设备访问锁

`fs/ide.c` 中 IDE 读写使用用户态 `ide_lock` 保护完整 IDE 操作序列。内核 `sys_read_dev()` 和 `sys_write_dev()` 通过 `dev_lock_for_pa()` 对 console 和 IDE MMIO 加锁：

- console 地址使用 `console_lock`。
- IDE 地址使用 `ide_dev_lock`。

这样即使有直接设备 syscall 路径，也不会多个 CPU 同时乱序访问 IDE/串口寄存器。

### 10.3 启动 SHELL

启动 shell 的推荐路径是使用现有 `lab6_2` 配置：

```make
init-envs += /user_icode /fs_serv
```

生成的 `mips_init()` 会初始化内存和 env 系统，创建 `user_icode` 与 `fs_serv`，然后进入 `schedule(0)`。

运行链路为：

```text
user_icode -> spawn init.b -> init.b -> open console -> spawn sh.b
```

`sh.b` 支持普通命令、`<`、`>`、`|`、脚本文件和 `#` 注释行。可执行程序来自文件系统镜像，例如 `ls.b`、`cat.b`、`echo.b`、`sh.b`、`halt.b` 等。

默认 `init/init.c` 中 shell 启动相关行被注释，是为了配合不同 lab/test 动态生成启动 env，不让固定 shell 影响专项测试。

## 11. 测试中发现的问题与修复

### 11.1 远端运行 env 被 destroy 时 panic

最初遇到目标 env 正在另一个 CPU 运行时，直接 panic 以避免内存破坏。后续实现 `env_kill_pending/env_kill_done`，改为请求目标 CPU 在安全点本地释放。

### 11.2 IPI handler 中直接释放 env 导致嵌套 IPI

曾尝试在 `handle_ipi_irq()` 中直接执行 `env_check_kill_pending()`，导致 `env_free()` 中的 `tlb_invalidate()` 再次发 IPI，引发 `ipi_done` 覆盖或死锁风险。修复为 IPI 只 poke，释放放在 `schedule()` 开头。

### 11.3 双向 destroy 中 timer reentry

`lab6_7` 双向远端销毁测试中，等待远端释放时可能被 timer 打断并重入调度钩子，导致同一 CPU 重复 destroy 同一 victim。修复为等待远端销毁期间临时关闭本 CPU 全局中断，同时在循环中主动处理 IPI。

### 11.4 目标释放前被其它 CPU 抢走

如果 `env_destroy()` 判断目标未在远端运行后释放锁，目标可能被其它 CPU 立即调度。修复为无论远端还是本地销毁，都先设置 `env_kill_pending`，使调度器跳过该 env。

### 11.5 IPC 唤醒顺序

SMP 下如果在 IPC 页面映射完成前就把接收方设为 runnable，另一个 CPU 可能马上运行接收方并访问尚未完成的映射。当前 `sys_ipc_try_send()` 先完成 `page_insert()` 和 IPC 字段写入，最后才唤醒接收方。

## 12. 验证方法

已有验证记录集中在 `VALIDATION.md` 和 `SMP_DESIGN_AND_TEST_SUMMARY.md`。主要验证方式包括：

- `make -s all` 构建内核和用户程序。
- QEMU 使用 `-smp 2 -cpu 24Kc -M malta` 运行。
- 默认启动日志出现 `[1] slave online`，说明从核上线。
- `lab6_3` 验证双向嵌套 IPI roundtrip。
- `lab6_4` 验证普通进程能在 CPU0/CPU1 上调度。
- `lab6_5`、`lab6_6`、`lab6_7` 验证远端 env destroy、slot 复用和双向 destroy。
- `lab6_2` 验证 shell、文件系统、脚本、管道和重定向。

用于 shell 验证的典型命令：

```sh
make -s test lab=6_2
make run
```

进入 shell 后可运行：

```sh
ls
cat motd
cat script
sh testshell.sh
```

## 13. 当前限制与后续优化方向

当前实现满足任务核心要求，但仍有若干可优化点：

- 默认 IPI 是共享内存模拟，不是本地 QEMU 上的真实 MMIO IPI。
- 从核启动没有使用 `IPI_START` mailbox，而是使用 `smp_boot_ready` 启动栅栏。
- 页表锁是全局粗粒度锁，性能不如细粒度锁或 CAS 管理。
- `env_free()` 逐页 TLB shootdown，释放大地址空间时 IPI 次数较多。
- 文件系统采用 `fs_serv` 固定 CPU0 的保守策略，没有实现多核并行文件系统。
- `halt.b` 在部分 QEMU/Malta 环境中可能进入 panic/halt 路径后停在内核死循环，实际退出 QEMU 更可靠的方法是 monitor `quit` 或 `Ctrl-A x`。

