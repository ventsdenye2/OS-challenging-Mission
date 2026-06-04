# MIPS SMP 移植两人分工与计划

本文档用于两人并行开发，目标是在尽量减少文件冲突的前提下，把当前单核 MOS 移植为支持至少 2 个 MIPS R24K CPU 的 SMP 内核，并最终在 QEMU Malta 上启动 shell。

## 0. 总体原则

1. 先约定公共接口，再并行实现。所有跨模块共享的数据结构、函数声明、锁类型先集中放到少数头文件，避免两个人反复改同一处。
2. 每个阶段只合并能独立编译的变更。不要提交“接口已改但调用点未改完”的中间状态。
3. 全局共享资源必须有明确锁归属：串口、物理页空闲链表、env 空闲链表、调度队列、ASID bitmap、页表更新、IPI mailbox。
4. 默认只支持 `NR_CPUS = 2`，实现时保留数组维度与循环边界，后续可扩展到更多核。
5. 任何修改 `curenv`、`cur_pgdir`、`KSTACKTOP` 假设的代码，都要同步检查异常、syscall、调度、TLB refill 四条路径。

## 1. 人员分工概览

### 杨璞：启动、每核状态、IPI、异常中断

主要目标：让两个 CPU 都能启动、拥有独立内核栈和每核状态，并能通过 IPI 执行远程函数调用。

主要负责文件：

- `Makefile`
- `include.mk`
- `init/start.S`
- `kern/entry.S`
- `kern/genex.S`
- `kern/machine.c`
- `kern/printk.c`
- 新增 `include/smp.h`
- 新增 `kern/smp.c`
- 新增 `kern/smp_asm.S`
- 新增 `include/spinlock.h`
- 新增 `kern/spinlock.S`
- 必要时新增 `include/atomic.h`

尽量避免改动：

- `kern/env.c`
- `kern/sched.c`
- `kern/pmap.c`
- `kern/tlbex.c`
- `kern/syscall_all.c`

### 李昊泽：内存管理、TLB 同步、调度、文件系统策略

主要目标：把单核全局状态改为多核安全，让进程能在两个 CPU 上调度，并保证页表/TLB/文件系统在多核下可用。

主要负责文件：

- `include/env.h`
- `kern/env.c`
- `kern/sched.c`
- `kern/pmap.c`
- `kern/tlbex.c`
- `kern/tlb_asm.S`
- `kern/syscall_all.c`
- `fs/serv.c`
- `fs/fs.c`
- `fs/ide.c`
- 必要时新增 `include/percpu.h` 或复用 `include/smp.h`

尽量避免改动：

- `init/start.S`
- `kern/entry.S`
- `kern/genex.S`
- `kern/smp.c`
- `kern/smp_asm.S`

### 共同修改区

以下文件不可避免会被两个人都关心，建议由杨璞先建接口，李昊泽只补声明或使用，不重排已有内容：

- `include/smp.h`
- `include/spinlock.h`
- `include/mmu.h`
- `include/trap.h`
- `kern/include.mk` 或 `kern/Makefile`

合并规则：共同修改区每次改动前先同步代码；新增函数声明按模块分组追加，不做格式化大改。

## 2. 阶段计划

### 阶段 1：公共接口与构建骨架

目标：建立 SMP 基本常量、每核数据结构、锁接口，保证仍能单核编译运行。

杨璞负责：

1. 在 `include/smp.h` 定义：
   - `#define NR_CPUS 2`
   - `struct cpu_local_data`
   - `extern struct cpu_local_data cpu_data[NR_CPUS];`
   - `int cpu_id(void);`
   - `struct Env *cpu_curenv(void);`
   - `Pde *cpu_cur_pgdir(void);`
   - `void smp_init(void);`
   - `void smp_secondary_start(void);`
   - `void smp_group_function_call(void (*fn)(u_int, u_int), u_int arg0, u_int arg1);`
   - `void handle_ipi_irq(void);`
2. 在 `include/spinlock.h` 定义：
   - `typedef volatile int spinlock_t;`
   - `#define SPINLOCK_INIT 0`
   - `void spin_lock(spinlock_t *lock);`
   - `void spin_unlock(spinlock_t *lock);`
   - `int atomic_add(int *ptr, int value);`
   - `int atomic_sub(int *ptr, int value);`
   - `int atomic_cas(void *ptr, int old_value, int new_value);`
3. 新增 `kern/spinlock.S`，实现原子操作和自旋锁。
4. 修改 `kern/Makefile`，加入新增 `.c/.S` 文件。
5. 暂时让 `cpu_id()` 返回 0，保证单核旧路径能编译。

李昊泽负责：

1. 在不改变行为的前提下，扫描所有 `curenv`、`cur_pgdir`、`KSTACKTOP - 1` 使用点，列清单。
2. 给后续要改的函数加最小范围 TODO 注释即可，不开始大面积替换。
3. 确认现有 lab 测试仍可构建。

阶段 1 验证：

```bash
make clean
make all
make test lab=2_1
make test lab=3_1
make test lab=4_5
make test lab=5_5
make test lab=6_2
```

通过标准：

- 所有命令能完成编译。
- `make run` 行为与当前单核版本一致。
- `printk` 输出暂时不要求 CPU 前缀。

### 阶段 2：QEMU 多核启动与每核数据

目标：QEMU 启动 2 核；CPU0 进入正常初始化，CPU1 停在等待状态；随后 CPU0 能唤醒 CPU1。

杨璞负责：

1. 修改 `Makefile`：
   - `QEMU_FLAGS` 改为包含 `-smp 2 -cpu 24Kc -m 64 -nographic -M malta -no-reboot`
   - 本地 QEMU 的 CPU 列表中没有 `r24k`，实际可用型号为 `24Kc`。
   - 保留磁盘 `-drive` 逻辑。
2. 修改 `include.mk`：
   - 将 `-march=4kc` 调整为适合 R24K 的参数，如工具链支持则使用 `-march=24kc`；若不支持，先保留并记录。
3. 修改 `init/start.S`：
   - 读取 CP0 `EBase` 低位作为 QEMU/Malta CPU 编号来源。
   - CPU0 执行 `.bss` 清零、设置 CPU0 栈、跳转 `mips_init`。
   - 非 0 CPU 不清 `.bss`，先等待 `smp_boot_ready == SMP_BOOT_READY`，再设置独立静态栈并跳转 `smp_secondary_start`。
4. 在 `kern/smp.c` 初始化：
   - `cpu_data[0].cpu_id = 0`
   - `cpu_data[1].cpu_id = 1`
   - CPU0 的 `kernel_stack_top = KSTACKTOP`
   - CPU1 的 `kernel_stack_top = &smp_kernel_stacks[1][SMP_KSTACK_SIZE]`
   - 每核 `curenv = NULL`
   - 每核 `cur_pgdir = NULL`
   - 使用 `.data` 中的 `smp_boot_ready` 魔数作为 CPU1 等待 CPU0 清完 `.bss` 和初始化 per-cpu 数据的启动栅栏。
5. 修改 `kern/printk.c`：
   - 给串口输出加 `console_lock`。
   - 每条 `printk` 前输出 `[cpu]` 前缀。
   - 注意避免 `printk` 内递归调用 `printk` 造成死锁，前缀建议直接 `printcharc` 或通过底层格式化函数输出。

李昊泽负责：

1. 暂不修改调度，只协助确认所有 C 文件能包含 `smp.h`。
2. 准备多核调度要用的 `Env.cpu_id` 字段补丁，但等阶段 3 再合并。

阶段 2 验证：

```bash
make clean
make all
make run
```

手工观察输出：

- 出现 `[0] mips_init...` 类似主核日志。
- 从核不应清 `.bss`，不应重复初始化页表和 env。
- 加临时日志后，应能看到 `[1] slave online` 或 `[1] wait for start`。
- 串口输出不交错。

建议增加临时测试函数：

```c
void smp_boot_check(void) {
    printk("cpu %d online\n", cpu_id());
}
```

通过标准：

- 2 核 QEMU 不死机。
- CPU0 可以正常走到原来的 `mips_init`。
- CPU1 至少能进入可控等待路径。
- 当前阶段实际输出示例：

```text
[1] slave online
[1] wait for start
[0] init.c:	mips_init() is called
```

### 阶段 3：IPI 与异常中断接入

目标：IPI 能唤醒从核，CPU0 能让 CPU1 执行指定函数；timer interrupt 和 IPI interrupt 能正确分流。

杨璞负责：

1. 在 `include/malta.h` 或 `include/smp.h` 补充 QEMU Malta IPI MMIO 寄存器地址宏：
   - `IPI_BASE`
   - `IPI_STATUS(cpu)`
   - `IPI_ENABLE(cpu)`
   - `IPI_SET(cpu)`
   - `IPI_CLEAR(cpu)`
   - `IPI_MAILBOX(cpu, slot)`
2. 在 `kern/smp.c` 实现：
   - `smp_init()`
   - `ipi_send(cpu, signal)`
   - `smp_secondary_start()`
   - `smp_group_function_call()`
   - `handle_ipi_irq()`
3. 在 `kern/entry.S` / `kern/genex.S` 中改造 `handle_int`：
   - 检查 IPI interrupt pending bit。
   - IPI 调 `handle_ipi_irq`。
   - timer 调 `schedule(0)`。
   - 未识别中断清理或进入 reserved/panic，方便调试。
4. 设置每核 CP0 `Status`：
   - 全局 IE。
   - timer interrupt mask。
   - IPI interrupt mask。
5. 设置每核 CP0 `Compare`，保证 timer 能再次触发。

李昊泽负责：

1. 提供 `tlb_invalidate_local(asid, va)` 的函数签名，供杨璞的 `smp_group_function_call` 后续测试调用。
2. 不在本阶段启用 TLB 广播，只保证链接通过。

阶段 3 验证：

新增内核测试：

```c
static volatile int ipi_seen[NR_CPUS];

static void ipi_test_handler(u_int arg0, u_int arg1) {
    ipi_seen[cpu_id()] = arg0 + arg1;
    printk("ipi call on cpu %d value %d\n", cpu_id(), ipi_seen[cpu_id()]);
}

void test_ipi_communication(void) {
    if (cpu_id() == 0) {
        smp_group_function_call(ipi_test_handler, 20, 22);
        printk("cpu1 seen = %d\n", ipi_seen[1]);
    }
}
```

运行：

```bash
make clean
make all
make run
```

通过标准：

- CPU0 能启动 CPU1。
- CPU1 能打印 IPI 回调日志。
- 连续发送 100 次 IPI 不丢消息、不死锁。
- timer 中断仍能触发调度入口。

### 阶段 4：每核 Trapframe、curenv、cur_pgdir

目标：去掉单核 `KSTACKTOP - 1`、全局 `curenv`、全局 `cur_pgdir` 假设。

杨璞负责：

1. 修改 `include/mmu.h`：
   - 定义 `KSTACKSIZE`，例如 `PAGE_SIZE` 或更大。
   - 定义 `KSTACKTOP_CPU(id)`。
   - 保留 `KSTACKTOP` 作为 CPU0 栈顶兼容旧代码。
2. 修改 `include/stackframe.h`：
   - `SAVE_ALL` 从用户态进入内核时，使用当前 CPU 的内核栈顶，而不是固定 `KSTACKTOP`。
   - 可通过 `$gp` 指向每核数据，或调用/内联 `cpu_id` 后计算栈顶。
3. 保证从核启动入口（当前实现为 `smp_secondary_start`，若后续拆出 `slave_uboot` 汇编入口则同步处理）设置好 `$gp` 或等价的每核指针。

李昊泽负责：

1. 替换以下单核引用：
   - `curenv` 读写改为每核 `curenv`，例如 `cpu_data[cpu_id()].curenv`。
   - `cur_pgdir` 读写改为每核 `cur_pgdir`。
   - `*((struct Trapframe *)KSTACKTOP - 1)` 改为当前 CPU 的 trapframe 地址辅助函数。
2. 重点改动点：
   - `kern/env.c`: `envid2env`、`env_destroy`、`env_run`
   - `kern/syscall_all.c`: `sys_getenvid`、`sys_yield`、`sys_env_destroy`、`sys_exofork`、`sys_set_env_status`、`sys_set_trapframe`、IPC 相关 syscall
   - `kern/tlbex.c`: `_do_tlb_refill`、`do_tlb_mod`
3. 在 `include/env.h` 增加：
   - `int env_cpu_id;`
   - `int env_running;` 或新增状态用于避免同一 env 被两个 CPU 同时运行。

阶段 4 验证：

```bash
make clean
make test lab=3_1
make test lab=4_1
make test lab=4_5
make run
```

新增检查：

- CPU0/CPU1 同时发生 timer interrupt 时，各自保存的 trapframe 地址不同。
- syscall 返回值正常。
- fork/IPC 测试不因 `curenv` 错乱失败。

通过标准：

- 单核 lab 回归仍通过。
- 多核运行时不会出现两个 CPU 修改同一个 trapframe 的现象。
- `printk` 能显示用户进程 syscall 来自不同 CPU。

### 阶段 5：内存管理加锁与 TLB shootdown

目标：页分配、页表修改、ASID 分配在多核下安全；任何页表修改都能广播 TLB 失效。

李昊泽负责：

1. 在 `kern/pmap.c` 增加锁：
   - `page_free_list_lock` 保护 `page_free_list`。
   - `pp_ref_lock` 或统一 `pmap_lock` 保护 `pp_ref` 与页表修改。
   - `asid_lock` 保护 `asid_bitmap`。
2. 修改函数：
   - `page_alloc`
   - `page_free`
   - `page_decref`
   - `pgdir_walk`
   - `page_insert`
   - `page_remove`
   - `passive_alloc`
3. 在 `kern/tlbex.c` 拆分：
   - `void tlb_invalidate_local(u_int asid, u_long va)`
   - `void tlb_invalidate(u_int asid, u_long va)` 调本地失效并通过 `smp_group_function_call` 广播。
4. 避免死锁：
   - 不要在持有 `pmap_lock` 时等待远端 CPU 长时间回调。
   - 如果必须同步等待 IPI 完成，先释放页表锁，或设计短临界区。
5. 注意早期初始化：
   - `smp_init` 之前调用的 `page_insert` 不应广播 IPI。
   - 可用 `smp_started` 标志控制。

杨璞负责：

1. 确保 `smp_group_function_call` 可用于 TLB shootdown。
2. 确保 IPI handler 可重入性足够，至少不会在 printk 或锁竞争中死锁。

阶段 5 验证：

```bash
make clean
make test lab=2_1
make test lab=2_2
make test lab=3_2
make test lab=4_5
make test lab=6_1
make run
```

新增压力测试：

1. 两个 CPU 同时 `sys_mem_alloc`/`sys_mem_unmap`。
2. 一个 CPU unmap，另一个 CPU 立刻访问同一 env 的地址，确认不会命中旧 TLB。
3. fork + COW 测试，重点跑：
   - `user/fktest.c`
   - `user/testptelibrary.c`
   - `user/pingpong.c`

通过标准：

- 无 page_free_list 损坏。
- 无 `pp_ref` 负数或泄漏到明显异常。
- TLB 修改不会只在本核生效。

### 阶段 6：多核调度

目标：多个用户进程能在两个 CPU 上调度，且同一 env 不会同时在两个 CPU 上运行。

李昊泽负责：

1. 在 `kern/sched.c` 增加 `env_sched_lock`。
2. 将 `static int count` 改为每核时间片：
   - `cpu_data[cpu_id()].sched_count`
3. 调度选择逻辑：
   - 持锁遍历 `env_sched_list`。
   - 跳过 `env_running == 1` 且 `env_cpu_id != cpu_id()` 的 env。
   - 选中后设置 `env_running = 1`、`env_cpu_id = cpu_id()`。
   - 当前 env 被换下时，清理其 running 标记或移动到队尾。
4. 修改 `env_run`：
   - 保存当前 CPU 的当前 env trapframe。
   - 更新每核 `curenv`、`cur_pgdir`。
   - 调 `env_pop_tf`。
5. 修改 `env_destroy`/`env_free`：
   - 如果目标 env 正在其他 CPU 运行，先标记不可运行，必要时用 IPI 让目标 CPU 重新调度。
   - 第一版可简化：只允许当前 CPU destroy 当前 env，其他 env destroy 持锁处理。

杨璞负责：

1. 确认每核 timer interrupt 都调用 `schedule(0)`。
2. 确保从核完成初始化后进入 `schedule(0)`，而不是空转。

阶段 6 验证：

新增多核调度测试用户程序或 init 场景：

```c
ENV_CREATE_PRIORITY(user_pingpong, 1);
ENV_CREATE_PRIORITY(user_fktest, 1);
ENV_CREATE_PRIORITY(user_tltest, 1);
ENV_CREATE_PRIORITY(user_idle, 1);
```

运行：

```bash
make clean
make all
make run
```

观察：

- 日志中同时出现 `[0]` 和 `[1]` 的用户态 syscall/调度输出。
- 每个 env 的 `env_runs` 递增。
- 没有同一 env 同时被两个 CPU 打印为 running。

通过标准：

- 多个进程能交替运行。
- CPU1 不只是启动后空闲。
- `yield`、timer preempt、IPC block/unblock 都能继续工作。

### 阶段 7：文件系统策略与 shell

目标：启动 shell，并保证文件系统服务在多核下可控。

李昊泽负责：

1. 决定 FS 策略：
   - 推荐第一版：文件系统服务进程固定 CPU0。
   - 其他用户进程可在 CPU0/CPU1 调度。
2. 在 `struct Env` 增加可选字段：
   - `int env_pinned_cpu;`，`-1` 表示不绑定。
3. 创建 `fs_serv` 时设置 pinned CPU0。
4. 调度器选择 env 时：
   - 如果 `env_pinned_cpu >= 0`，只有对应 CPU 可调度该 env。
5. 给 IDE/FS 共享状态加锁：
   - `fs/ide.c` 中磁盘 PIO 读写加 `ide_lock`。
   - `fs/fs.c` 中 block cache / bitmap / file metadata 修改加锁。
   - 若 FS 仅 CPU0 运行，IDE 锁仍建议保留，防止 syscall/dev 直接访问。

杨璞负责：

1. 确保 `sys_read_dev`/`sys_write_dev` 路径中设备访问不会和串口/IDE 锁冲突。
2. 如果 shell 读控制台卡死，检查 `sys_cgetc` 是否忙等导致单核独占；必要时在等待时 `schedule(1)`。

阶段 7 验证：

```bash
make clean
make test lab=5_5
make test lab=6_1
make test lab=6_2
make all
make run
```

shell 手工测试：

```sh
ls
cat motd
echo hello
cat script
sh testshell.sh
```

通过标准：

- shell 能启动并响应输入。
- 文件读写路径不死锁。
- FS 服务进程只在 CPU0 运行。
- 普通用户进程仍能在两个 CPU 上调度。

## 3. 建议的合并顺序

1. 杨璞合并阶段 1 公共接口和空实现。
2. 李昊泽基于阶段 1 合并 `Env` 字段和辅助函数，但不启用多核调度。
3. 杨璞合并阶段 2 多核 QEMU 启动和 printk 锁。
4. 杨璞合并阶段 3 IPI。
5. 李昊泽合并阶段 4 每核 `curenv/cur_pgdir/trapframe` 替换。
6. 李昊泽合并阶段 5 pmap/TLB。
7. 李昊泽合并阶段 6 调度。
8. 李昊泽合并阶段 7 FS/shell，杨璞协助处理设备/中断问题。

每次合并后至少执行：

```bash
make clean
make all
make run
```

涉及内存/调度/FS 的合并后额外执行：

```bash
make test lab=4_5
make test lab=5_5
make test lab=6_2
```

## 4. 关键改动点清单

### 构建与运行

- `Makefile`
  - `QEMU_FLAGS`: 加 `-smp 2`，CPU 改为 QEMU 支持的 `24Kc`。
  - 保留磁盘镜像参数。
- `include.mk`
  - 使用 `-march=24kc`。

### 启动

- `init/start.S`
  - 用 CP0 `EBase` 低位区分 CPU0 和 CPU1。
  - 只有 CPU0 清 `.bss`。
  - CPU0 调用 `smp_init` 后进入 `mips_init`。
  - 非 0 CPU 等待 `smp_boot_ready`，设置独立静态栈后进入 `smp_secondary_start`。

### SMP/IPI

- `include/smp.h`
  - CPU 数、每核数据、IPI API。
- `kern/smp.c`
  - IPI 地址初始化。
  - mailbox 读写。
  - 启动从核。
  - 远程函数调用。
- `kern/smp_asm.S`
  - 从核启动、读取 mailbox、设置 `sp/gp/ra`。

### 锁和原子操作

- `include/spinlock.h`
  - 锁和原子操作声明。
- `kern/spinlock.S`
  - `ll/sc/sync` 实现。
- 使用点：
  - `kern/printk.c`
  - `kern/pmap.c`
  - `kern/env.c`
  - `kern/sched.c`
  - `fs/ide.c`
  - `fs/fs.c`

### 异常中断

- `include/stackframe.h`
  - 使用每核内核栈保存 trapframe。
- `kern/entry.S`
  - 异常入口保持通用。
- `kern/genex.S`
  - `handle_int` 区分 timer/IPI。
  - timer 调度前重置 Compare。

### 进程与调度

- `include/env.h`
  - 增加 `env_cpu_id`、`env_running`、可选 `env_pinned_cpu`。
- `kern/env.c`
  - per-cpu `curenv`。
  - 保存/恢复当前 CPU trapframe。
  - env 生命周期加锁。
- `kern/sched.c`
  - 调度队列锁。
  - 每核时间片。
  - 跳过其他 CPU 正在运行的 env。

### 内存与 TLB

- `kern/pmap.c`
  - page free list 锁。
  - ASID bitmap 锁。
  - 页表更新锁。
  - `pp_ref` 并发保护。
- `kern/tlbex.c`
  - 拆分本地 TLB invalidate 和广播 TLB invalidate。
  - `_do_tlb_refill` 使用当前 CPU 的 `cur_pgdir`。

### Syscall

- `kern/syscall_all.c`
  - 所有 `curenv` 替换为当前 CPU env。
  - `sys_exofork` 和 `sys_set_trapframe` 使用当前 CPU trapframe。
  - IPC 修改 env 状态时持调度/env 锁。
  - `sys_cgetc` 可考虑等待时让出 CPU。

### 文件系统

- `fs/serv.c`
  - FS 服务进程固定 CPU0。
- `fs/ide.c`
  - IDE 读写锁。
- `fs/fs.c`
  - 文件元数据、bitmap、block cache 修改锁。

## 5. 冲突规避约定

1. 杨璞不改 `kern/sched.c` 的调度算法，只提供 timer 调用入口。
2. 李昊泽不改 `init/start.S` 的启动分支，只使用杨璞暴露的 `cpu_id()` 和每核栈接口。
3. `include/smp.h` 由杨璞维护结构定义；李昊泽只追加调度需要的字段需求，先沟通再改。
4. `include/env.h` 由李昊泽维护；杨璞不在该文件中直接添加 IPI 字段。
5. 所有新锁变量由使用模块自己定义，不集中塞进一个全局文件。
6. 不做全仓格式化，不重排 include，不批量改注释。
7. 每次改公共头文件后，双方都先 `make clean && make all`。

## 6. 最小完成标准

最终交付至少满足：

1. QEMU 使用 2 个 `24Kc` CPU 启动。
2. 两个 CPU 都能进入内核并打印带 CPU id 的日志。
3. IPI 可用，CPU0 能向 CPU1 发送启动和函数调用消息。
4. timer interrupt 在两个 CPU 上可用。
5. 进程能在两个 CPU 上调度，且同一进程不会同时运行在两个 CPU。
6. 页表修改后 TLB 失效同步到所有 CPU。
7. 多核下 fork、IPC、pipe、TLB mod、文件系统基础测试可运行。
8. shell 能启动，基本命令可执行。
9. 文档中说明锁设计、IPI mailbox 格式、调度策略、FS CPU0 绑定策略、测试结果。

## 7. 建议提交记录

建议按阶段拆提交，便于回退和查错：

1. `smp: add common cpu-local and spinlock interfaces`
2. `smp: boot qemu with two 24Kc cpus`
3. `smp: bring up secondary cpu with ipi`
4. `trap: use per-cpu kernel stacks`
5. `env: switch curenv and cur_pgdir to per-cpu state`
6. `pmap: protect page tables and broadcast tlb invalidation`
7. `sched: run environments on multiple cpus`
8. `fs: pin file server and protect device access`
9. `docs: record smp implementation and test results`

## 8. 最终实现记录、测试验证与提交说明

本节作为最终交付记录，和 `VALIDATION.md` 的复现命令配套使用。

### 8.1 实现记录

启动与每核状态：

- `Makefile` 默认 QEMU 参数使用 `-smp 2 -cpu 24Kc -m 64 -nographic -M malta -no-reboot`，满足至少两个 MIPS CPU 的启动要求。
- `init/start.S` 区分 CPU0 和从核路径；CPU0 完成内核初始化，从核进入独立启动等待路径并最终跳转 `smp_secondary_start`。
- `include/smp.h` 和 `kern/smp.c` 提供 `cpu_data[NR_CPUS]`、`cpu_id()`、`cpu_curenv()`、`cpu_cur_pgdir()`、`cpu_kstack_top()`、`cpu_trapframe()` 等每核状态接口。
- `kern/printk.c` 在每条 `printk` 前输出 `[cpu]` 前缀，日志可直接确认当前 CPU。

IPI、异常与 timer：

- `kern/smp.c` 实现 IPI mailbox、`smp_group_function_call()`、`handle_ipi_irq()`、`handle_timer_irq()` 和从核上线流程。
- 默认 QEMU `24Kc` Malta 环境未提供任务文档中的 IPI MMIO 块，因此当前实现使用共享内存 mailbox 模拟同一协议；若环境提供 MMIO IPI，可通过 `SMP_USE_MMIO_IPI=1` 切换到 `include/malta.h` 中的寄存器宏。
- `kern/genex.S` 的 interrupt handler 区分 IPI 与 timer interrupt；timer 路径重置 Compare 后进入调度入口。
- 每核 Trapframe 通过 `cpu_trapframe()` 定位到当前 CPU 的内核栈顶部，syscall、TLB Mod 和 trap 路径不再共享单核栈帧。

内存管理与 TLB 同步：

- `kern/pmap.c` 用 `pmap_lock` 保护 `page_free_list`、页表项与 `pp_ref`，并在释放锁后执行 TLB invalidate，避免持页表锁等待 IPI 造成死锁。
- `kern/env.c` 用 `asid_lock` 保护 ASID bitmap 分配和释放。
- `kern/tlbex.c` 拆分 `tlb_invalidate_local()` 和 `tlb_invalidate()`；后者在 SMP 启动后通过 `smp_group_function_call()` 广播 TLB shootdown。

多核调度与进程状态：

- `include/env.h` 增加 `env_cpu_id`、`env_running`、`env_pinned_cpu`。
- `kern/sched.c` 用每核 `sched_count` 维护时间片，持 `env_sched_lock` 遍历 runnable 队列，跳过正在其他 CPU 上运行或不符合 CPU 绑定的 env。
- IPC 阻塞/唤醒和 env 状态变更在 `kern/syscall_all.c` 中持 `env_sched_lock`，先完成共享页映射再唤醒接收方。

文件系统、设备与 shell：

- `env_create_named("fs_serv", ...)` 将 FS 服务进程固定在 CPU0；普通用户进程不绑定，可在 CPU0/CPU1 调度。
- FS 服务端固定 CPU0 运行，保持 block cache、bitmap、open table 和 IDE PIO 的单服务者语义。
- `console_lock` 保护串口输出、`sys_putchar`、`sys_print_cons`、`sys_cgetc` 和串口 `sys_*_dev` 路径。
- `ide_dev_lock` 保护 IDE MMIO 直接 syscall 路径，防止用户态设备访问与 FS/IDE PIO 并发冲突。
- `sys_cgetc` 在无输入时预置 syscall 返回值 0 后 `schedule(1)`，保持非阻塞 ABI，同时避免 shell 等待输入时单核独占。

### 8.2 锁归属

| 锁 | 位置 | 保护对象 |
| --- | --- | --- |
| `console_lock` | `kern/printk.c` | 串口输出、控制台输入与串口 MMIO syscall |
| `ide_dev_lock` | `kern/syscall_all.c` | IDE MMIO syscall |
| `env_sched_lock` | `kern/sched.c` | 调度队列、`env_running`、`env_cpu_id`、IPC 阻塞/唤醒 |
| `pmap_lock` | `kern/pmap.c` | 物理页空闲链表、页表项、页引用计数 |
| `asid_lock` | `kern/env.c` | ASID bitmap |
| `ipi_mailbox_lock[cpu]` | `kern/smp.c` | 单个目标 CPU 的 IPI mailbox |

### 8.3 测试验证记录

最近一次完整验证日期：2026-06-04。

验证环境：2 核 QEMU Malta (`-smp 2 -cpu 24Kc -m 64 -nographic -M malta -no-reboot`)。

已通过项目：

- `make -s all`
- 全量 lab 构建回归：`lab=1_2`、`2_1`、`2_2`、`2_3`、`3_1`、`3_2`、`3_3`、`3_4`、`4_1`、`4_2`、`4_3`、`4_4`、`4_5`、`4_6`、`4_7`、`5_1`、`5_2`、`5_3`、`5_4`、`5_5`、`6_1`、`6_2`
- 默认双核/IPI 启动：`timeout 12s make run`
- 阶段 7 设备 syscall 验证：`make -s test lab=5_1` 后运行 `printf 'abcdefghijklmn\r' | timeout 20s make run`
- shell/FS/脚本验证：`make -s test lab=6_2` 后运行 `printf 'ls\ncat motd\ncat script\nsh testshell.sh\n' | timeout 30s make run`
- 静态收尾检查：`rg` 临时日志检查、阶段 7 锁路径检查、`git diff --check`

关键通过输出：

- 默认 `make run` 输出 `[1] slave online`，并完成 100 次 IPI 调用测试：`cpu1 seen = 42 count = 100`。
- `lab=5_1` 输出 `syscall_read_dev is good` 和 `dev address is ok`，表示设备 syscall 地址校验与串口读写路径通过。
- `lab=6_2` shell 输出 `MOS Shell 2024`，`ls` 能列出 `testshell.sh`、`script`、`motd` 等文件；`cat motd`、`cat script`、`sh testshell.sh` 输出预期内容。
- shell 运行日志中用户进程销毁/运行输出同时出现 `[0]` 和 `[1]`，说明普通用户进程在两个 CPU 上调度。

说明：

- `timeout` 包住的 QEMU 运行在关键输出出现后可能以 124 退出，这是测试脚本主动结束 QEMU 的预期现象。
- 验证后已执行 `make clean` 清理构建产物；最终提交前只应保留源码和文档改动。

### 8.4 文档整理

- `README.md`：环境配置、工具链检查与文档入口。
- `SMP_DEVELOPMENT_PLAN.md`：分阶段计划、最终实现记录、锁归属、验证结果与提交说明。
- `VALIDATION.md`：可复现验证命令、预期输出、task 要求对应检查表、最近一次最终验证记录。