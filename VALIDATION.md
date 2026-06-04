# SMP 移植验证手册

本文档记录本仓库按 `task.md` 要求完成后的验证项目和运行方法，方便本地重复检查。

## 0. 准备工作

确认工具链可用：

```bash
mips-linux-gnu-gcc -v
qemu-system-mipsel --version
make --version
```

QEMU 退出方式：

- `make run` 使用 `-nographic`，退出按 `Ctrl + A`，再按 `X`。
- 非交互快速验证可用 `timeout 12s make run`，到时间会自动结束 QEMU。

注意：本仓库跟踪了部分构建产物，例如 `*.b`、`*.x`、`*.b.c`、`target/mos`、`target/fs.img`。运行 `make all` 或 `make test` 后，`git status` 里出现这些产物变化是正常现象。

## 1. 快速完整构建

用途：确认默认 `lab=6` 镜像可以完整构建。

```bash
make clean
make -s all
```

预期结果：

- 命令退出码为 0。
- `target/mos`、`target/fs.img` 生成成功。

## 2. 全量 Lab 构建回归

用途：覆盖内核启动、内存、异常、进程、IPC、fork、pipe、文件系统、shell 等课程测试镜像构建。

可以逐条运行：

```bash
make -s test lab=1_2
make -s test lab=2_1
make -s test lab=2_2
make -s test lab=2_3
make -s test lab=3_1
make -s test lab=3_2
make -s test lab=3_3
make -s test lab=3_4
make -s test lab=4_1
make -s test lab=4_2
make -s test lab=4_3
make -s test lab=4_4
make -s test lab=4_5
make -s test lab=4_6
make -s test lab=4_7
make -s test lab=5_1
make -s test lab=5_2
make -s test lab=5_3
make -s test lab=5_4
make -s test lab=5_5
make -s test lab=6_1
make -s test lab=6_2
```

也可以用循环一次跑完：

```bash
for lab in 1_2 2_1 2_2 2_3 3_1 3_2 3_3 3_4 4_1 4_2 4_3 4_4 4_5 4_6 4_7 5_1 5_2 5_3 5_4 5_5 6_1 6_2; do
    make -s test lab=$lab || exit 1
done
```

预期结果：

- 每条命令退出码为 0。
- lab5/lab6 会输出若干 `writing regular file ... into disk`，表示测试文件被写入文件系统镜像。

## 3. 默认双核启动与 IPI 验证

用途：验证 `task.md` 中至少两个 CPU、IPI 通信、双核启动输出。

先恢复默认 `lab=6` 镜像：

```bash
make clean
make -s all
```

运行：

```bash
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

判定标准：

- 能看到 `[0]` 和 `[1]` 两个 CPU 的日志。
- CPU1 上线。
- 100 次 IPI 回调完成，`count = 100`。

## 4. Shell 与文件系统运行验证

用途：验证 shell 启动、FS 服务、普通命令、脚本和管道/重定向路径。

先构建 `lab=6_2` 测试镜像：

```bash
make -s test lab=6_2
```

启动 QEMU：

```bash
make run
```

等待出现 shell 提示符：

```text
:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
::                     MOS Shell 2024                      ::
:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

$
```

在 shell 中输入：

```sh
ls
cat motd
cat script
sh testshell.sh
```

预期结果：

- `ls` 能列出 `motd`、`script`、`testshell.sh`、`ls.b`、`cat.b`、`sh.b` 等文件。
- `cat motd` 输出 MOS 文件系统欢迎文本。
- `cat script` 输出：

```text
echo.b This is the end of the script
ls.b | cat.b > aaa.txt
```

- `sh testshell.sh` 输出 `lorem` 文件内容：

```text
Catherine wished dormouse happy birthday yesterday,
which made it very grateful.
Wish her happy everyday.
```

退出 QEMU：

```text
Ctrl + A
X
```

## 5. 阶段 7 设备 syscall 与控制台等待验证

用途：覆盖阶段 7 杨璞负责的设备 syscall 串行化与 `sys_cgetc` 等待让出路径。

设备 syscall 验证：

```bash
make -s test lab=5_1
printf 'abcdefghijklmn\r' | timeout 20s make run
```

预期关键输出：

```text
devtst begin
syscall_read_dev is good
end of devtst
dev address is ok
```

判定标准：

- `sys_read_dev`/`sys_write_dev` 的串口路径可以正常读写。
- 无非法设备地址误放行，过程中无 panic。
- `timeout` 到时结束 QEMU 时可能返回 124；只要上述关键输出已出现即可。

shell 控制台等待验证：

```bash
make -s test lab=6_2
printf 'ls\ncat motd\ncat script\nsh testshell.sh\n' | timeout 30s make run
```

判定标准：

- shell 能进入 `$` 提示符并执行输入的命令。
- 能看到 `motd`、`script`、`testshell.sh` 等文件输出。
- `sh testshell.sh` 输出 `lorem` 内容，等待输入期间无单核独占导致的卡死或 panic。

静态补充检查：

```bash
rg -n "console_lock|ide_dev_lock|dev_lock_for_pa|cpu_trapframe\\(\\)->regs\\[2\\] = 0|schedule\\(1\\)" include/printk.h kern/printk.c kern/syscall_all.c
```

预期结果：

- `console_lock` 为共享串口锁，`sys_print_cons`、`sys_putchar`、`sys_cgetc` 和串口 `sys_*_dev` 路径复用该锁。
- IDE 设备地址对应 `ide_dev_lock`。
- `sys_cgetc` 在无输入时先写回返回值 0，再 `schedule(1)` 让出 CPU。

## 6. Task 要求对应检查表

| `task.md` 要求 | 验证方法 | 通过标准 |
| --- | --- | --- |
| 支持至少两个处理器核 | `timeout 12s make run` | 看到 `[0]` 与 `[1]` 日志，且 `[1] slave online` |
| 支持 IPI 中断/通信 | 默认 `make run` 的 IPI 自测 | `cpu1 seen = 42 count = 100` |
| 支持启动 SHELL | `make -s test lab=6_2 && make run` | 出现 `MOS Shell 2024` 与 `$` 提示符 |
| 进程在多核上调度 | shell/测试运行日志 | 用户进程销毁、运行日志中出现 `[0]` 和 `[1]` |
| 支持多核内存读写和 TLB 同步 | lab4/lab6 构建回归，shell 命令运行 | `lab=4_5`、`lab=6_1`、`lab=6_2` 构建通过，shell 命令无 panic |
| 设备 syscall 串行化与控制台等待 | `lab=5_1` 设备验证、`lab=6_2` shell 管线验证、静态补充检查 | 串口/IDE MMIO 有对应锁，`sys_cgetc` 无输入时让出 CPU，运行无 panic |
| 文件系统可用 | shell 中运行 `ls`、`cat motd`、`cat script` | 文件可列出、可读取 |
| 基本脚本/管道/重定向 | shell 中运行 `sh testshell.sh` | 脚本输出 lorem 内容，过程中无 panic |
| 实现文档 | 查看 `SMP_DEVELOPMENT_PLAN.md` 第 8 节 | 包含锁设计、IPI mailbox、调度策略、FS 策略、测试结果 |

## 7. 推荐最终提交前检查

```bash
rg -n "TODO SMP|phase 7|ipc_try_send from|serve req|serve_open from|read_block [0-9].*(alloc|done|ide_read)" include kern fs user init SMP_DEVELOPMENT_PLAN.md task.md README.md
rg -n "console_lock|ide_dev_lock|dev_lock_for_pa|cpu_trapframe\\(\\)->regs\\[2\\] = 0|schedule\\(1\\)" include/printk.h kern/printk.c kern/syscall_all.c
git diff --check
git status --short
```

预期结果：

- 第一条命令不应输出临时调试日志或未完成的 SMP TODO。
- 第二条命令应能定位阶段 7 新增的串口/IDE 锁和 `sys_cgetc` 让出逻辑。
- 第三条命令不应输出空白或格式问题。
- 第四条命令会显示源码/文档变化，以及构建后生成产物变化；构建产物变化属于正常现象。

## 8. 最近一次最终验证记录

验证日期：2026-06-04。

验证环境：

```text
QEMU Malta, 2 CPUs, -smp 2 -cpu 24Kc -m 64 -nographic -M malta -no-reboot
```

已执行并通过：

```bash
make -s all

for lab in 1_2 2_1 2_2 2_3 3_1 3_2 3_3 3_4 4_1 4_2 4_3 4_4 4_5 4_6 4_7 5_1 5_2 5_3 5_4 5_5 6_1 6_2; do
    make -s test lab=$lab || exit 1
done

make clean
make -s all
timeout 12s make run

make -s test lab=5_1
printf 'abcdefghijklmn\r' | timeout 20s make run

make -s test lab=6_2
printf 'ls\ncat motd\ncat script\nsh testshell.sh\n' | timeout 30s make run

rg -n "TODO SMP|phase 7|ipc_try_send from|serve req|serve_open from|read_block [0-9].*(alloc|done|ide_read)" include kern fs user init SMP_DEVELOPMENT_PLAN.md task.md README.md
rg -n "console_lock|ide_dev_lock|dev_lock_for_pa|cpu_trapframe\\(\\)->regs\\[2\\] = 0|schedule\\(1\\)" include/printk.h kern/printk.c kern/syscall_all.c
git diff --check
make clean
```

关键结果：

- 全量 lab 构建回归退出码为 0。
- 默认 `make run` 输出 `[1] slave online`、`ipi call on cpu 1 value 42 count 100`、`cpu1 seen = 42 count = 100`。
- `lab=5_1` 输出 `syscall_read_dev is good`、`end of devtst`、`dev address is ok`。
- `lab=6_2` 输出 shell banner、文件列表、`motd`、`script` 和 `lorem` 内容；运行日志中能看到 `[0]` 与 `[1]` 用户进程输出。
- 临时调试日志检查无输出；阶段 7 锁路径检查能定位 `console_lock`、`ide_dev_lock` 和 `sys_cgetc` 让出逻辑；`git diff --check` 无输出。
