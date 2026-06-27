# MOS SMP 多核移植项目

本仓库是基于北航 MOS 教学内核的 MIPS SMP 多核移植实现。当前目标平台为 QEMU Malta / MIPS 24Kc，默认以 2 个 vCPU 运行，支持多核启动、核间通信、TLB shootdown、多核调度、远端 env 销毁、文件系统服务和 shell 启动。

## 项目状态

当前实现覆盖 `task.md` 中的主要要求：

- 支持至少两个处理器核：QEMU 参数使用 `-smp 2 -cpu 24Kc -M malta`。
- 支持核间通信：实现 `smp_group_function_call()`、IPI mailbox、`handle_ipi_irq()`。
- 支持启动 shell：通过 `lab=6_2` 创建 `/user_icode` 和 `/fs_serv`，最终进入 `sh.b`。
- 支持多核调度：普通用户进程可在 CPU0/CPU1 上运行，调度器避免同一 env 被两个 CPU 同时运行。
- 支持多核内存管理：页表和物理页管理由 `pmap_lock` 保护，TLB 修改通过跨核 shootdown 同步。
- 支持文件系统保守策略：`fs_serv` 固定在 CPU0，普通用户进程仍可在多核间调度并通过 IPC 使用文件服务。

需要注意：当前默认 QEMU Malta/24Kc 环境没有教程中假设的 MMIO IPI 控制器，因此默认使用共享内存 `ipi_pending[] + ipi_mailbox[][]` 模拟 IPI 协议。代码中保留了 `SMP_USE_MMIO_IPI=1` 的 MMIO IPI 分支。

## 重要文档

- `task.md`：挑战性任务原始要求。
- `SMP_IMPLEMENTATION_REPORT.md`：按照 `task.md` 梳理的当前实现报告，适合作业提交和答辩说明。
- `SMP_DESIGN_AND_TEST_SUMMARY.md`：设计细节、测试构造和 debug 过程总结。
- `SMP_DEVELOPMENT_PLAN.md`：分阶段开发计划和实现记录。
- `VALIDATION.md`：验证命令、预期输出和检查表。

## 环境依赖

推荐在 Linux 或 WSL2 Ubuntu 环境中构建。Debian/Ubuntu 可安装：

```bash
sudo apt update
sudo apt install -y build-essential gcc-mips-linux-gnu \
    binutils-mips-linux-gnu gdb-multiarch \
    qemu-system-mips curl git
```

本仓库默认使用小端 MIPS：

```make
QEMU := qemu-system-mipsel
CROSS_COMPILE ?= mips-linux-gnu-
```

可检查本地 QEMU 是否支持当前 CPU 型号：

```bash
qemu-system-mipsel -cpu help
```

应能看到 `24Kc`。

## 构建

完整构建：

```bash
make -s all
```

清理构建产物：

```bash
make clean
```

本仓库会生成大量 `.b`、`.x`、`target/fs.img`、`target/mos` 等构建产物。如果只提交源码和文档，注意不要误提交这些临时产物。

## 运行

默认运行：

```bash
make run
```

默认 QEMU 参数来自 `Makefile`：

```make
-smp 2 -cpu 24Kc -m 64 -nographic -M malta -no-reboot
```

退出 QEMU：

```text
Ctrl-A
然后按 x
```

如果快捷键被终端吞掉，可在另一个终端执行：

```bash
pkill -f qemu-system-mipsel
```

## 启动 Shell

推荐使用现有 `lab6_2` 配置启动 shell：

```bash
make -s test lab=6_2
make run
```

`tests/lab6_2/kernel.mk` 会生成启动配置：

```make
init-envs += /user_icode /fs_serv
```

启动链路为：

```text
CPU0 mips_init()
  -> 创建 user_icode 和 fs_serv
  -> schedule()
  -> user_icode 读取 /motd 并 spawn init.b
  -> init.b 打开 console 并循环 spawn sh.b
  -> 进入 MOS Shell
```

进入 shell 后可尝试：

```sh
ls
cat motd
cat script
sh testshell.sh
echo hello
```

shell 支持普通命令、参数、输入重定向 `<`、输出重定向 `>`、管道 `|`、脚本文件和 `#` 注释行。它没有完整 Unix shell 的所有功能，例如没有 `cd`、环境变量、后台任务和通配符展开。

## 常用验证

### 双核启动与默认运行

```bash
timeout 12s make run
```

可观察到类似输出：

```text
[1] slave online
cpu1 seen = 42 count = 100
```

### Shell 和文件系统

```bash
make -s test lab=6_2
printf 'ls\ncat motd\ncat script\nsh testshell.sh\n' | timeout 30s make run
```

预期能看到 shell banner、文件列表、`motd`、`script` 和 `lorem` 内容。

### 多核专项测试

可参考 `VALIDATION.md` 中的完整命令。重点测试包括：

- `lab6_3`：双向嵌套 IPI roundtrip。
- `lab6_4`：普通进程在两个 CPU 上调度。
- `lab6_5`：单次远端 env destroy。
- `lab6_6`：多轮远端 destroy 和 env slot 复用。
- `lab6_7`：双向远端 destroy 竞态。

## 当前实现要点

### 启动路径

所有 CPU 从 `init/start.S` 的 `_start` 进入。入口读取 CP0 `EBase` 低位区分 CPU：

- CPU0 清 `.bss`、设置初始栈、调用 `smp_init()`、进入 `mips_init()`。
- 非 0 CPU 进入 `secondary_wait`，等待 `smp_boot_ready` 后设置独立内核栈并跳转 `smp_secondary_start()`。

当前没有按教程使用 `IPI_START` 唤醒从核。

### Per-CPU 状态

当前没有使用 `$gp` 保存每核数据指针，而是使用：

```c
struct cpu_local_data cpu_data[NR_CPUS];
```

并通过 `cpu_data[cpu_id()]` 访问当前 CPU 的 `curenv`、`cur_pgdir`、内核栈顶和时间片计数。

### 异常与中断

所有 CPU 共用 `exc_gen_entry`。异常入口根据 CPU ID 选择独立内核栈，避免多个 CPU 同时进入内核时覆盖 trapframe。

`handle_int` 将中断分流为：

- `STATUS_IM6`：IPI。
- `STATUS_IM7`：timer。

### 锁与共享数据

主要锁包括：

- `console_lock`：串口输入输出。
- `pmap_lock`：物理页空闲链表、引用计数、页表修改。
- `env_sched_lock`：调度队列和 env 运行状态。
- `asid_lock`：ASID bitmap。
- `ipi_mailbox_lock[cpu]`：目标 CPU mailbox。
- `ide_dev_lock`：IDE MMIO syscall 路径。

### TLB 同步

`tlb_invalidate()` 先执行本地失效，SMP 启动后再通过 `smp_group_function_call()` 让其它 CPU 执行 `tlb_invalidate_local()`。

页表修改时先持 `pmap_lock` 修改 PTE/PDE 和引用计数，释放锁后再 TLB shootdown，避免持页表锁等待 IPI 导致死锁。

### 调度

调度器持 `env_sched_lock` 遍历 `env_sched_list`，跳过：

- 绑定到其它 CPU 的 env。
- `env_kill_pending` 的 env。
- 已经在其它 CPU 上运行的 env。

选中 env 后设置 `env_running` 和 `env_cpu_id`，防止双重调度。

### 远端 env 销毁

如果 CPU0 要销毁正在 CPU1 上运行的 env，CPU0 不直接释放它，而是：

1. 设置 `env_kill_pending` 和 `env_kill_done`。
2. 通过 IPI poke 目标 CPU。
3. 等待目标 CPU 在调度安全点本地释放。

真正的 `env_free()` 不在 IPI handler 中执行，避免在中断上下文里嵌套 TLB shootdown 和锁操作。

### 文件系统

`fs_serv` 创建时被绑定到 CPU0。普通进程仍可运行在 CPU0 或 CPU1 上，并通过 IPC 请求文件系统服务。这样将 block cache、open table 和 IDE PIO 操作集中到 CPU0，降低多核文件系统并发复杂度。

## 已知限制

- 默认 IPI 是共享内存模拟，不是当前 QEMU 上的真实 MMIO IPI。
- 从核启动使用 `smp_boot_ready` 栅栏，而不是 `IPI_START`。
- 页表管理使用全局粗粒度 `pmap_lock`，性能不是最优。
- `env_free()` 释放大地址空间时可能产生多次 TLB shootdown。
- 文件系统采用 CPU0 单服务者策略，未实现真正多核并行 FS。
- `halt.b` 在部分 QEMU/Malta 环境中可能进入内核 halt 死循环；退出 QEMU 更可靠的方法是 `Ctrl-A x` 或 `pkill`。
