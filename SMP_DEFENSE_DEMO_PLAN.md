# SMP 答辩展示计划与命令清单

本文档面向现场答辩使用，目标是把 `task.md` 的检查项逐条展示出来：至少双核、IPI/核间通信、启动 SHELL、多核调度、多核内存读写与 TLB 同步、实现文档与测试结果。

## 0. 展示总原则

- 所有命令都在仓库根目录运行。
- `make test lab=...` 会触发 `clean-and-all`，会清理并重建镜像；现场不要并行跑多个 `make test`。
- `make run` 使用 QEMU `-nographic`，交互退出为 `Ctrl + A` 后按 `X`。
- 使用 `timeout ... make run` 时，QEMU 被 timeout 杀掉可能返回 124。只要关键输出已经出现，这不代表内核失败。
- 如果构建时出现 `Clock skew detected`，通常是环境中文件时间戳问题；重点看命令退出码和 QEMU 关键输出。
- 当前实现默认 `SMP_USE_MMIO_IPI=0`，即使用共享内存 `ipi_pending[] + ipi_mailbox[][]` 模拟 IPI 协议；代码保留了 MMIO IPI 分支。答辩时建议主动说明这个边界。

## 1. 推荐答辩主线

建议现场按下面顺序展示。前四步是必跑主线，后两步视时间和老师追问选择。

| 顺序 | 展示目的 | 命令 | 关键通过点 |
| --- | --- | --- | --- |
| 1 | 说明任务要求和环境参数 | 静态检索 | `-smp 2`、`24Kc`、`NR_CPUS=2` |
| 2 | 证明双核启动和基础 IPI | 默认镜像运行 | `[1] slave online`、`cpu1 seen = 42 count = 100` |
| 3 | 证明 IPI 协议能双向嵌套调用 | `lab=6_3` | `smp_ipi_roundtrip passed` |
| 4 | 证明进程确实在两个 CPU 上调度 | `lab=6_4` | `cpu0=16 cpu1=16` |
| 5 | 证明 SHELL、FS、用户程序可用 | `lab=6_2` | MOS Shell、`ls`、`cat`、脚本输出 |
| 6 | 证明远端 env 销毁和 TLB shootdown 更稳 | `lab=6_5/6_6/6_7` | `remote_destroy_* passed` |

## 2. 开场：对齐 task.md 要求

先用 30 秒说明 `task.md` 要求：

- 支持至少两个处理器核。
- 支持 IPI 中断/核间通信。
- 支持启动 SHELL。
- 进程在多核上进行调度。
- 支持多核对内存的读写。
- 完成实现文档，课程组会检查实现。

建议打开或检索：

```bash
sed -n '1,80p' task.md
rg -n "QEMU_FLAGS|march=24kc|NR_CPUS|SMP_USE_MMIO_IPI" Makefile include.mk include/smp.h README.md
```

讲解要点：

- `Makefile` 中 QEMU 参数包含 `-smp 2 -cpu 24Kc -M malta`。
- `include.mk` 使用 `-march=24kc`。
- `include/smp.h` 定义 `NR_CPUS 2`。
- README 和实现报告已经记录默认 IPI 模式的限制。

## 3. 演示一：默认双核启动 + 100 次 IPI

命令：

```bash
make clean
make -s all
timeout 12s make run
```

预期关键输出：

```text
[1] slave online
[0] init.c:	mips_init() is called
[1] ipi call on cpu 1 value 42 count 1
[1] ipi call on cpu 1 value 42 count 100
[0] cpu1 seen = 42 count = 100
```

这一步覆盖：

- `-smp 2` 下 CPU1 确实启动。
- CPU0 能通过 `smp_group_function_call()` 让 CPU1 执行函数。
- mailbox 参数传递正确，100 次调用全部完成。

可打开代码：

```bash
rg -n "test_ipi_communication|ipi_test_handler|smp_group_function_call|handle_ipi_irq|slave online" init/init.c kern/smp.c
```

## 4. 演示二：强 IPI 双向嵌套测试

默认测试只能证明 CPU0 到 CPU1 的 IPI。若要更强，可以展示 `lab=6_3`，它会让 CPU0 调 CPU1，CPU1 在 handler 内反向调 CPU0，验证等待过程中不会死锁。

命令：

```bash
make -s test lab=6_3
timeout 12s make run
```

预期关键输出：

```text
[1] slave online
[0] smp_ipi_roundtrip: start
[0] smp_ipi_roundtrip passed: cpu1_calls=128 cpu0_callbacks=128
```

这一步覆盖：

- IPI mailbox 支持双向调用。
- `ipi_wait_done()` 等待目标 CPU 时会主动处理本 CPU pending IPI，避免双方互等。
- handler 校验了回调确实跑在目标 CPU 上。

可打开代码：

```bash
sed -n '1,120p' tests/lab6_3/init.c
sed -n '220,290p' kern/smp.c
```

## 5. 演示三：多核调度真实发生

命令：

```bash
make -s test lab=6_4
timeout 12s make run
```

预期关键输出：

```text
[1] slave online
[0] smp_sched_parallel progress: total=16 cpu0=8 cpu1=8
[1] smp_sched_parallel progress: total=32 cpu0=16 cpu1=16
[1] smp_sched_parallel passed: total=32 cpu0=16 cpu1=16
```

这一步覆盖：

- 用户进程不只是 CPU0 在跑，CPU1 也进入 `env_run()`。
- `pre_env_run` 检查 `env_running == 1` 且 `env_cpu_id == 当前 CPU`，能证明调度器没有把同一个 env 同时交给两个 CPU。
- `env_sched_lock` 保护调度队列，`cpu_data[cpu].sched_count` 是每核时间片。

可打开代码：

```bash
sed -n '1,120p' tests/lab6_4/pre_env_run.c
sed -n '1,130p' kern/sched.c
rg -n "env_running|env_cpu_id|env_pinned_cpu|sched_count" include/env.h include/smp.h kern/sched.c kern/env.c
```

## 6. 演示四：SHELL、文件系统和用户态工作流

命令：

```bash
make -s test lab=6_2
printf 'ls\ncat motd\ncat script\nsh testshell.sh\n' | timeout 20s make run
```

预期关键输出：

```text
FS is running
MOS Shell 2024
$ ls
testshell.sh script lorem ... ls.b cat.b ...
$ cat motd
This is /motd, the message of the day.
Welcome to the MOS kernel, now with a file system!
$ cat script
echo.b This is the end of the script
ls.b | cat.b > aaa.txt
$ sh testshell.sh
Catherine wished dormouse happy birthday yesterday,
which made it very grateful.
Wish her happy everyday.
```

这一步覆盖：

- SHELL 能启动并进入 `$` 提示符。
- FS 服务、文件读取、用户程序 `ls/cat/sh` 可用。
- 脚本、管道/重定向路径能运行。
- 输出中会出现 `[0]`、`[1]` env destroy/free 日志，说明用户程序生命周期在两个 CPU 上都有活动。

可打开代码：

```bash
rg -n "fs_serv|env_pinned_cpu|console_lock|ide_dev_lock|sys_cgetc|schedule\\(1\\)" kern/env.c kern/syscall_all.c kern/printk.c include/printk.h
```

讲解要点：

- `fs_serv` 固定在 CPU0，是为了降低文件系统服务对共享设备状态的并发要求。
- 控制台和 IDE 设备路径有锁。
- `sys_cgetc` 无输入时让出 CPU，避免一个 CPU 忙等拖住系统。

## 7. 可选强测试：远端 env 销毁和生命周期安全

如果老师追问“一个 CPU 销毁另一个 CPU 正在跑的进程怎么办”，展示这些测试。

一次远端销毁：

```bash
make -s test lab=6_5
timeout 12s make run
```

预期：

```text
remote_destroy_once passed
```

多轮远端销毁：

```bash
make -s test lab=6_6
timeout 12s make run
```

预期：

```text
remote_destroy_repeat passed
```

双向远端销毁：

```bash
make -s test lab=6_7
timeout 12s make run
```

预期：

```text
remote_destroy_bidirectional passed
```

可打开代码：

```bash
sed -n '496,560p' kern/env.c
sed -n '1,120p' tests/lab6_5/remote_destroy_once.c
sed -n '1,120p' tests/lab6_7/pre_env_run.c
```

讲解要点：

- 如果目标 env 正在远端 CPU 上运行，发起 CPU 不直接释放，而是设置 `env_kill_pending`。
- 目标 CPU 在调度安全点调用 `env_check_kill_pending()`，由本 CPU 本地释放自己正在使用的 env。
- 真正释放不放在 IPI handler 中，因为 `env_free()` 会触发页表释放和 TLB shootdown，而 TLB shootdown 也依赖 IPI；嵌套 IPI 容易死锁。

## 8. 可选静态展示：多核内存管理与 TLB shootdown

如果老师追问“多核读写内存和 TLB 一致性怎么保证”，展示这里。

命令：

```bash
rg -n "pmap_lock|page_alloc|page_insert|page_remove|page_decref" kern/pmap.c
sed -n '1,70p' kern/tlbex.c
sed -n '1,80p' kern/spinlock.S
```

讲解要点：

- `pmap_lock` 保护 `page_free_list`、引用计数和页表项修改。
- `tlb_invalidate()` 先本地 `tlb_invalidate_local()`，SMP 启动后再通过 `smp_group_function_call()` 让其它 CPU 执行本地失效。
- `tlb_invalidate()` 必须在 `pmap_lock` 外调用，避免持锁等待 IPI 时和远端 CPU 形成死锁。
- `spin_lock`、`atomic_add/sub/cas` 使用 MIPS `ll/sc` 和 `sync`。

## 9. 可选静态展示：启动路径与 per-CPU 状态

命令：

```bash
sed -n '1,70p' init/start.S
sed -n '1,60p' include/stackframe.h
sed -n '1,80p' include/smp.h
```

讲解要点：

- `_start` 读取 CP0 `EBase` 低位区分 CPU0 和 CPU1。
- CPU0 清 `.bss`、调用 `smp_init()`、进入 `mips_init()`。
- CPU1 不清 `.bss`，等待 `smp_boot_ready`，再设置独立内核栈并进入 `smp_secondary_start()`。
- 异常入口 `SAVE_ALL` 在用户态异常进入内核时按 CPU ID 切换到对应内核栈，避免两个 CPU 共用一份 trapframe。
- `cpu_data[NR_CPUS]` 保存每核 `curenv`、`cur_pgdir`、`kernel_stack_top`、`sched_count`。

## 10. 最短现场版本

如果答辩时间很紧，只跑这四组：

```bash
rg -n "QEMU_FLAGS|march=24kc|NR_CPUS" Makefile include.mk include/smp.h

make clean
make -s all
timeout 12s make run

make -s test lab=6_4
timeout 12s make run

make -s test lab=6_2
printf 'ls\ncat motd\nsh testshell.sh\n' | timeout 20s make run
```

这四组已经覆盖：

- 双核启动。
- 基础 IPI。
- 多核调度。
- SHELL 和文件系统。

如果老师追问 IPI 可靠性，再补：

```bash
make -s test lab=6_3
timeout 12s make run
```

如果老师追问远端销毁，再补：

```bash
make -s test lab=6_7
timeout 12s make run
```

## 11. task.md 要求对应表

| `task.md` 要求 | 展示命令 | 判断标准 |
| --- | --- | --- |
| 支持至少两个处理器核 | `timeout 12s make run` | `[1] slave online`，日志有 `[0]` 和 `[1]` |
| 支持 IPI 中断/通信 | 默认镜像、`lab=6_3` | `count = 100`、`cpu1_calls=128 cpu0_callbacks=128` |
| 支持启动 SHELL | `lab=6_2` 自动输入 | 出现 `MOS Shell 2024` 和 `$` |
| 进程在多核上调度 | `lab=6_4` | `cpu0=16 cpu1=16` |
| 支持多核内存读写 | `lab=6_2`、静态 TLB/pmap 展示 | shell/FS 可运行，`pmap_lock` 与 TLB shootdown 存在 |
| 完成实现文档 | 打开项目文档 | `SMP_IMPLEMENTATION_REPORT.md`、`SMP_DESIGN_AND_TEST_SUMMARY.md`、`VALIDATION.md`、本文档 |

## 12. 答辩时可主动说明的限制

- 默认 IPI 是共享内存 pending + mailbox 模拟，不是当前环境下真正依赖 MMIO IPI 控制器的硬中断路径。
- 从核启动不是通过 `IPI_START` 唤醒，而是 QEMU 启动所有 vCPU 后，CPU1 在 `_start` 里等待 `smp_boot_ready`。
- 文件系统服务进程绑定 CPU0，普通用户进程仍可在双核调度。
- 当前目标是满足任务的双核 SMP 演示，不宣称已经扩展到任意核数；`NR_CPUS` 当前为 2。
