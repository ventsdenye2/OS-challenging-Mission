# MIPS MOS SMP 答辩高频问题题库

本文档整理 `SMP_DESIGN_AND_TEST_SUMMARY.md` 第 13 节已有问题、题目文档中的参考问题，并从助教和老师检查实现的角度补充追问。每个能通过测试点、命令或静态检索佐证的问题，都附了“可直接运行”的命令，便于答辩前快速复现和定位代码。

## 1. 环境与工具链相关

### Q1: QEMU 启动参数里写了 `-cpu 24Kc`，但本机 QEMU 报找不到这个 CPU 型号，怎么办？

答题要点：

- 先用 `qemu-system-mipsel -cpu help` 或 `qemu-system-mips -cpu help` 查看当前 QEMU 支持的 CPU 名称。
- 不同 QEMU 版本可能支持 `24Kc`、`4Kc`、`mips32r6-generic` 等不同名字，需要选择本机实际支持且与编译参数兼容的型号。
- 当前项目使用 `-march=24kc` 编译，是为了和 `24Kc` 模拟 CPU 对齐；如果换 CPU 型号，需要确认指令集级别仍支持当前代码用到的 CP0、`ll/sc`、`sync` 等指令。

可直接运行：

```bash
qemu-system-mipsel -cpu help
rg -n "QEMU_FLAGS|march=24kc|QEMU :=" Makefile include.mk
```

### Q2: 多核环境下如何用 GDB 调试从核 Core 1？如何切换当前查看的 CPU？

答题要点：

- QEMU GDB stub 通常把每个 vCPU 暴露成不同 thread。
- 启动 QEMU 时加 `-S -s`，GDB 连接 `target remote :1234`。
- 在 GDB 中用 `info threads` 查看 vCPU，对应 thread 后用 `thread <id>` 切换。
- 可在从核入口如 `secondary_wait`、`smp_secondary_start`、`handle_ipi_irq` 处下断点，配合 `thread apply all bt` 或查看 CP0 `EBase` 低位确认当前 CPU。

可直接运行：

```bash
make -s all
make dbg
```

进入 GDB 后可输入：

```gdb
info threads
thread 2
b smp_secondary_start
b handle_ipi_irq
c
```

### Q3: 为什么 QEMU 要使用 `-smp 2 -M malta -nographic`？

答题要点：

- `-smp 2` 是本任务至少两个 CPU 的基础。
- `-M malta` 使用 QEMU 的 MIPS Malta 板级模型，串口、IDE、内存布局等和项目代码匹配。
- `-nographic` 将串口输出重定向到终端，便于观察 `printk`、shell 和测试输出。
- 这些参数只是运行环境，不自动保证内核支持 SMP；内核仍然要完成 per-CPU 栈、IPI、调度和锁。

可直接运行：

```bash
rg -n "QEMU_FLAGS|MALTA|SERIAL|IDE" Makefile include/malta.h
timeout 12s make run
```


### Q5: 怎么证明不是“只有 CPU0 在跑”，而是两个 CPU 都真正参与了？

答题要点：

- 不能只看 `-smp 2`，要看内核日志、调度统计和测试。
- 当前项目通过 `[cpu]` 前缀、`slave online`、IPI roundtrip、调度计数、远端 destroy 测试证明 CPU1 参与执行。
- `tests/lab6_4` 统计 CPU0/CPU1 的 `env_run` 次数，可以作为“多核调度真实发生”的证据。

可直接运行：

```bash
make -s test lab=6_4
timeout 20s make run
```

预期看到：

```text
smp_sched_parallel passed: total=... cpu0=... cpu1=...
```


## 2. 启动、寄存器与 Per-CPU 状态

### Q7: 为什么不能继续使用全局 `curenv`？

答题要点：

- 两个 CPU 可以同时运行不同 env，全局 `curenv` 只能表示一个“当前进程”，会被互相覆盖。
- SMP 中“当前进程”必须是 per-CPU 概念，即 `cpu_data[cpu_id()].curenv`。
- 当前项目通过 `cpu_curenv()` 统一访问当前 CPU 的 env。

可直接运行：

```bash
rg -n "cpu_curenv\\(|cpu_data\\[.*\\]\\.curenv|struct cpu_local_data|curenv" include/smp.h kern include
```

### Q8: 为什么不能继续使用全局 `cur_pgdir`？

答题要点：

- 每个 CPU 当前运行的进程可能不同，地址空间也可能不同。
- TLB refill、page fault、syscall 都要使用当前 CPU 的页目录，而不是系统全局页目录。
- 当前项目通过 `cpu_cur_pgdir()` 访问每核页目录。

可直接运行：

```bash
rg -n "cpu_cur_pgdir\\(|cpu_data\\[.*\\]\\.cur_pgdir|cur_pgdir" include/smp.h kern include
```

### Q9: 为什么要使用 `$gp` 保存本地数据指针？能不能用 `ld_t local_datas[NR_CPUS]` 加 CPU ID 索引？

答题要点：

- 使用 `$gp` 的优点是访问 per-CPU 数据快速，且不必每次读取 CP0 获取 CPU ID。
- 但它不是唯一方案。也可以使用全局 `cpu_data[NR_CPUS]`，每次通过 `cpu_id()` 索引。
- 当前项目采用全局 `cpu_data` 数组和 `cpu_id()` 接口，重点是保证当前状态 per-CPU 化，而不是必须绑定 `$gp`。
- 如果使用 `$gp`，要注意 MIPS ABI 原本把 `$gp` 用作全局指针，编译选项和汇编保存恢复逻辑必须统一。

可直接运行：

```bash
rg -n "register.*\\$28|asm\\(\"\\$28\"\\)|cpu_data|cpu_id\\(" include kern init
```

### Q10: 启动阶段主核和从核的栈在哪里分配？是否需要在 `.bss` 里单独预留从核栈？

答题要点：

- 当前项目按 `KSTACKTOP_CPU(cpu)` 或 `KSTACKTOP - cpu * KSTACKSIZE` 划分每核内核栈。
- CPU0 使用 `KSTACKTOP`，CPU1 使用向下平移后的栈顶。
- 只要内存布局中这些栈区域已保留且不会与其它数据重叠，不一定非要在 `.bss` 中声明数组。
- 关键是异常入口和普通内核路径都要按 CPU ID 使用对应栈。

可直接运行：

```bash
rg -n "KSTACKTOP_CPU|KSTACKSHIFT|KSTACKSIZE|kernel_stack_top|li[[:space:]]+sp, KSTACKTOP|subu[[:space:]]+sp" include init kern
```

### Q11: 为什么只有主核清 `.bss`，从核不能也清？

答题要点：

- `.bss` 是全局共享数据区，CPU0 清零后会初始化锁、队列、per-CPU 表等结构。
- 如果 CPU1 也清 `.bss`，可能把 CPU0 已经初始化好的全局状态清掉，导致启动随机失败。
- 当前 `init/start.S` 里 CPU0 清 BSS，非 0 CPU 直接进入 `secondary_wait`。

可直接运行：

```bash
sed -n '1,90p' init/start.S
```

### Q12: 从核启动为什么需要 `smp_boot_ready` 这样的启动栅栏？

答题要点：

- CPU1 可能比 CPU0 更早跑到某些代码位置，但全局内核状态、锁、页表、调度队列尚未初始化完成。
- 启动栅栏让从核等待 CPU0 完成 SMP 基础初始化后再继续。
- 当前项目用 `SMP_BOOT_WAIT/SMP_BOOT_READY` 控制从核进入 `smp_secondary_start` 的时机。

可直接运行：

```bash
rg -n "SMP_BOOT_WAIT|SMP_BOOT_READY|smp_boot_ready|secondary_wait" include/smp.h kern/smp.c init/start.S
```

### Q13: 怎么在汇编级准确获取当前 CPU ID？读 EBase 低位还是 PRId？

答题要点：

- `PRId` 更适合识别处理器型号，不可靠地区分 vCPU 编号。
- 在 QEMU/Malta 当前实现中，可以读取 CP0 `EBase` 的低位作为 CPU 编号来源。
- 当前项目 `cpu_id()` 读取 `$15, sel 1`，取低 10 位，并对超过 `NR_CPUS` 的值做保守处理。

可直接运行：

```bash
rg -n "mfc0.*\\$15, 1|CP0_EBASE|andi.*0x03ff|cpu_id\\(" kern/smp.c init/start.S kern/genex.S include
```

### Q14: EBase 要求 4KB 对齐，是否需要给每个核分配独立异常向量表？

答题要点：

- 完整硬件设计中，异常向量 base 需要满足对齐要求。
- 也可以所有 CPU 共用同一套异常入口，但在入口中根据 CPU ID 选择不同内核栈和 per-CPU 状态。
- 当前项目主要使用共享异常入口，并在 `SAVE_ALL` 中按 `EBase` 低位选择每核栈。

可直接运行：

```bash
rg -n "exc_gen_entry|CP0_EBASE|KSTACKTOP|SAVE_ALL|\\.align|ALIGN" kern/genex.S include/stackframe.h kernel.lds
```

### Q15: 为什么 `SAVE_ALL` 里要按 CPU ID 切换内核栈？

答题要点：

- 两个 CPU 可能同时进入异常或系统调用，如果都把 trapframe 压到同一栈顶，会互相覆盖。
- 每核独立内核栈保证 trapframe、临时保存寄存器、内核调用栈互不破坏。
- 当前项目在异常入口中读取 `EBase`，再计算 `KSTACKTOP - cpu * KSTACKSIZE`。

可直接运行：

```bash
rg -n "SAVE_ALL|CP0_EBASE|KSTACKTOP|KSTACKSHIFT|cpu_trapframe" include/stackframe.h kern/genex.S kern/smp.c
```


## 3. 原子操作、锁与同步

### Q17: `ll/sc` 之间如果发生时钟中断或异常，`sc` 还会成功吗？

答题要点：

- MIPS 架构允许异常、中断、其它核写入等事件破坏 linked 状态，因此 `sc` 可能失败。
- 正确实现不能假设 `sc` 一定成功，必须失败后重试。
- 当前 `atomic_add/sub/cas` 和 `spin_lock` 都围绕 `ll/sc` 循环重试。

可直接运行：

```bash
sed -n '1,120p' kern/spinlock.S
```

### Q18: `atomic_add` 和 `atomic_sub` 返回“操作前的值”，如果 `sc` 失败期间原值被改了，返回哪一次？

答题要点：

- 返回最终成功那次 `ll` 读到的旧值。
- 失败的尝试没有真正完成写入，不应作为返回值。
- 当前实现把 `ll` 读到的值放在 `$v0`，只有 `sc` 成功后才返回。

可直接运行：

```bash
rg -n "LEAF\\(atomic_add\\)|LEAF\\(atomic_sub\\)|ll|sc|beqz" kern/spinlock.S
```

### Q19: `atomic_cas` 发现当前值不等于 `old_value` 时，需要 `sync` 吗？

答题要点：

- 如果没有发生写入，通常不需要像成功写入那样发布内存修改。
- 如果把 CAS 作为 acquire/release 原语使用，需要更严格定义内存序；课程实现里重点是保证成功写入后的可见性。
- 当前实现只有 CAS 成功后执行 `sync`，不相等时直接返回 0。

可直接运行：

```bash
sed -n '21,45p' kern/spinlock.S
```

### Q20: 为什么释放自旋锁时要加 `sync`？

答题要点：

- 释放锁前的 `sync` 确保临界区内写入先于锁变量变成 0 对其它 CPU 可见。
- 如果先暴露锁释放，其它 CPU 可能进入临界区却看不到前一个 CPU 的数据更新。
- 当前项目 `spin_unlock` 在 `sw zero, 0(a0)` 前有 `sync`。

可直接运行：

```bash
rg -n "LEAF\\(spin_unlock\\)|sync|sw[[:space:]]+zero" kern/spinlock.S
```

### Q21: 指导书中释放锁后也有 `sync`，当前项目只在释放前 `sync`，可以吗？

答题要点：

- 释放前 `sync` 是最核心的 release 语义，保证临界区写入先完成。
- 释放后 `sync` 可进一步保守保证释放写入对后续访存排序，但不是所有实现都必须双 `sync`。
- 答辩时可以说明当前实现追求最小必要同步，若要更保守可以在释放后再加一次 `sync`。

可直接运行：

```bash
sed -n '/LEAF(spin_unlock)/,/END(spin_unlock)/p' kern/spinlock.S
```

### Q22: 整个内核只用一把大内核锁可以满足基础任务吗？

答题要点：

- 从正确性角度，大内核锁可以降低并发 bug，可能满足最基础启动和 shell。
- 但它会严重限制并行性，也不利于展示“多核调度、多核内存访问、IPI 同步”的细节。
- 当前项目使用多把锁：`pmap_lock`、`env_sched_lock`、`console_lock`、`asid_lock`、`ipi_mailbox_lock`，粒度更清楚。

可直接运行：

```bash
rg -n "spinlock_t .*lock|console_lock|pmap_lock|env_sched_lock|asid_lock|ipi_mailbox_lock|ide_dev_lock" include kern fs
```

### Q23: 如果一个 CPU 持有自旋锁时发生中断，中断处理里又拿同一把锁，会怎样？

答题要点：

- 会发生本 CPU 自死锁：中断处理等待锁释放，但锁持有者就是被中断打断的当前上下文。
- 解决方法包括：持锁前关本地中断、避免中断处理路径拿同一把锁、让中断处理只做轻量操作。
- 当前远端 destroy 等待中临时关闭本地中断，就是为了避免 timer reentry 造成重复进入复杂路径。

可直接运行：

```bash
rg -n "env_irq_save|env_irq_restore|STATUS_IE|handle_timer_irq|handle_ipi_irq|spin_lock" kern/env.c kern/smp.c kern/sched.c
```

### Q24: `printk` 加串口锁后，如果中断里再次 `printk` 会怎样？

答题要点：

- 如果普通路径持有 `console_lock` 时被中断打断，中断里再拿 `console_lock`，也可能自死锁。
- 可以避免在中断 handler 中打印复杂日志，或在持 console 锁时关本地中断，或实现可重入/trylock 日志。
- 答辩时要承认这是 console 输出的典型风险，并说明项目测试中尽量避免 IPI/timer handler 做大量输出。

可直接运行：

```bash
rg -n "console_lock|printk\\(|handle_ipi_irq|handle_timer_irq" kern include
make -s test lab=5_1
printf 'abcdefghijklmn\r' | timeout 20s make run
```

### Q25: SMP 下“关中断”为什么不能替代自旋锁？

答题要点：

- 关中断只阻止当前 CPU 被中断打断，不能阻止其它 CPU 同时进入内核修改共享数据。
- 多核共享结构必须用跨 CPU 可见的原子锁或无锁算法保护。
- 单核 MOS 中很多“关中断即可安全”的假设在 SMP 下失效。

可直接运行：

```bash
rg -n "spin_lock|spin_unlock|env_irq_save|STATUS_IE" kern include
```

### Q26: 自旋锁适合保护什么？什么时候不适合？

答题要点：

- 适合保护短临界区，例如队列指针、状态位、引用计数、mailbox。
- 不适合持锁期间等待 IPI、做大量 I/O、执行可能阻塞或再次拿锁的复杂操作。
- 当前项目特别避免持 `pmap_lock` 做 TLB shootdown，就是因为 shootdown 会等待远端 CPU。

可直接运行：

```bash
rg -n "pmap_lock|tlb_invalidate|smp_group_function_call|spin_lock|spin_unlock" kern/pmap.c kern/tlbex.c kern/smp.c
```

## 4. IPI 与核间通信

### Q27: Malta 平台 IPI 寄存器基址具体是多少？有没有物理地址分布文档？

答题要点：

- 课程指导书给了抽象的 `IPI_BASE`，当前项目在 `include/malta.h` 保留了 `0x3ff01000` 这一组布局。
- 但 stock QEMU Malta + 24Kc 不一定暴露该 Loongson 风格 IPI MMIO 控制器。
- 当前项目默认 `SMP_USE_MMIO_IPI=0`，使用共享内存 pending 位模拟同一套 mailbox 协议；若平台提供 MMIO，可打开宏走寄存器路径。

可直接运行：

```bash
rg -n "IPI_BASE|IPI_STATUS|IPI_ENABLE|IPI_SET|IPI_CLEAR|SMP_USE_MMIO_IPI" include/malta.h include/smp.h kern/smp.c
```

### Q28: 当前项目默认不使用硬件 MMIO IPI，会不会不满足“支持 IPI”的要求？

答题要点：

- 当前实现抽象出了 `ipi_send`、`handle_ipi_irq`、mailbox 和 `smp_group_function_call`，语义上完成了核间通知和远程函数调用。
- 但默认后端是共享内存 pending 模拟，不是完整硬件中断控制器。
- 答辩时应如实说明这是 QEMU/Malta 环境限制下的阶段性实现，并指出代码保留了 MMIO IPI 后端接口。

可直接运行：

```bash
make -s test lab=6_3
timeout 15s make run
rg -n "SMP_USE_MMIO_IPI|ipi_pending|ipi_send|handle_ipi_irq|smp_group_function_call" kern/smp.c include/smp.h
```

### Q29: Mailbox 是每个 CPU 一份还是所有 CPU 共用一份？

答题要点：

- 正确设计应当是每个目标 CPU 一份 mailbox，否则多个发送方或多个目标会互相覆盖。
- 当前项目使用 `ipi_mailbox[NR_CPUS][IPI_MBOX_NR]`，并为每个目标 CPU 配一把 `ipi_mailbox_lock[cpu]`。
- 这样主核同时向不同从核发送消息时，目标 CPU 的 mailbox 相互独立。

可直接运行：

```bash
rg -n "ipi_mailbox\\[NR_CPUS\\]|ipi_mailbox_lock\\[NR_CPUS\\]|IPI_MBOX_NR" kern/smp.c
```

### Q30: 如果两个 CPU 同时向同一个目标 CPU 发 IPI，mailbox 会不会覆盖？

答题要点：

- 有风险，所以需要按目标 CPU 加锁。
- 当前项目发送前获取 `ipi_mailbox_lock[target]`，等待目标 `ipi_done[target]` 后才释放，避免覆盖目标 mailbox。
- 这个设计让同一目标 CPU 的远程调用串行化。

可直接运行：

```bash
sed -n '/void smp_group_function_call/,/^}/p' kern/smp.c
```

### Q31: 从核执行 `wait` 时，主核发来的 IPI 一定能唤醒它吗？

答题要点：

- 真硬件 IPI 需要从核先设置好 Status 中的全局中断位和对应中断 mask。
- 如果中断位没开，`wait` 可能不会被 IPI 唤醒。
- 当前项目默认共享内存 pending 后端，从核等待循环中主动检查 pending；若启用 MMIO IPI，需要保证 IM6/IE 等位已开启。

可直接运行：

```bash
rg -n "setup_cpu_interrupts|STATUS_IE|STATUS_IM6|wait|ipi_pending|smp_secondary_start" kern/smp.c init/start.S
```

### Q32: `smp_group_function_call` 是同步还是异步？

答题要点：

- 当前项目是同步调用：发送方写 mailbox、发送 IPI，然后等待目标 CPU 设置 `ipi_done`。
- 同步语义适合 TLB shootdown，因为发起方必须确认远端 TLB 已失效后才能继续。
- 代价是发送方会自旋等待，因此不能在持有可能被远端需要的锁时调用。

可直接运行：

```bash
rg -n "ipi_wait_done|ipi_done|smp_group_function_call|tlb_invalidate" kern/smp.c kern/tlbex.c
```

### Q33: 为什么等待 `ipi_done` 时还要主动调用 `handle_ipi_irq()`？

答题要点：

- 两个 CPU 可能互相等待对方处理 IPI，如果等待期间完全不处理本地 IPI，会出现双向死锁。
- 当前 `ipi_wait_done` 在等待目标完成时，如果发现本 CPU 有 pending IPI，会主动处理。
- 这也是 `lab6_3` 双向嵌套 IPI roundtrip 能通过的关键。

可直接运行：

```bash
make -s test lab=6_3
timeout 15s make run
```

预期看到：

```text
smp_ipi_roundtrip passed: cpu1_calls=128 cpu0_callbacks=128
```

### Q34: IPI 嵌套怎么处理？比如处理 IPI 时又收到另一个 IPI？

答题要点：

- 当前项目允许等待期间处理 pending IPI，但避免在 IPI handler 里做复杂的二次 IPI 协议。
- `handle_ipi_irq` 只做轻量函数调用和 ack，不在其中直接释放 env。
- 远端 destroy 的真正释放放到调度安全点，避免 IPI handler 内触发 TLB shootdown 再发 IPI。

可直接运行：

```bash
make -s test lab=6_3
timeout 15s make run
rg -n "handle_ipi_irq|env_check_kill_pending|schedule\\(" kern/smp.c kern/sched.c kern/env.c
```

### Q35: 清除 IPI 状态应在读取 mailbox 前还是后？

答题要点：

- 常见做法是先读状态并清除 pending，再读取 mailbox 和执行处理，避免同一中断重复进入。
- 但发送方必须保证先写完 mailbox，再触发 IPI，并通过 `sync` 保证顺序。
- 当前项目 `handle_ipi_irq` 先读并清 pending，`sync` 后再读取 mailbox；发送端 `ipi_send` 前后也有 `sync`。

可直接运行：

```bash
sed -n '/static void ipi_send/,/}/p' kern/smp.c
sed -n '/void handle_ipi_irq/,/^}/p' kern/smp.c
```

### Q36: IPI handler 为什么不能直接做所有事情？

答题要点：

- IPI handler 处于中断上下文，应尽量短小。
- 如果在 handler 中释放 env、修改页表、触发 TLB shootdown，可能嵌套 IPI 协议并引入死锁。
- 当前策略是 IPI 负责通知，复杂操作推迟到 `schedule()` 开头的安全点。

可直接运行：

```bash
sed -n '/void handle_ipi_irq/,/^}/p' kern/smp.c
sed -n '/void schedule/,/spin_lock/p' kern/sched.c
```

## 5. 异常、中断与时钟

### Q37: 指导书建议用 `KSTACKTOP - KSTACKSIZE * cpu_id` 放 Trapframe，会不会栈溢出覆盖其它 CPU？

答题要点：

- 每核栈按固定大小分段，正常情况下不会互相覆盖。
- 风险在于栈深超过 `KSTACKSIZE`，会向下覆盖相邻 CPU 栈或其它内存。
- 可以通过保留 guard page、减少中断嵌套、控制内核调用深度来降低风险；课程项目主要依赖固定栈空间足够。

可直接运行：

```bash
rg -n "KSTACKSIZE|KSTACKSHIFT|KSTACKTOP_CPU|cpu_trapframe|SAVE_ALL" include kern init
```

### Q38: Count/Compare 是所有核共享一套还是每核独立？

答题要点：

- MIPS CP0 寄存器是每个 CPU 自己的一套，`Count/Compare` 也是每核独立。
- 因此每个 CPU 都需要初始化自己的 timer compare 和 Status 中断 mask。
- 当前项目 CPU0 在 `smp_init()` 中设置，CPU1 在 `smp_secondary_start()` 中设置。

可直接运行：

```bash
rg -n "setup_timer_compare|mtc0.*\\$9|mtc0.*\\$11|setup_cpu_interrupts|smp_init|smp_secondary_start" kern/smp.c
```

### Q39: 为什么 CPU1 不能一启动就参与调度？

答题要点：

- CPU0 可能还没完成 env 初始化和调度队列构造。
- CPU1 如果过早 schedule，会看到空队列或不完整数据。
- 当前项目用 `timer_schedule_ready` 控制，CPU0 第一次进入调度路径后才允许 CPU1 正常调度。

可直接运行：

```bash
rg -n "timer_schedule_ready|smp_note_schedule_ready|schedule\\(0\\)|smp_secondary_start" kern/smp.c kern/sched.c
```

### Q40: `handle_timer_irq()` 里为什么先处理 IPI 再调度？

答题要点：

- IPI 可能携带 TLB shootdown、远端 destroy poke 等请求。
- 如果不先处理，当前 CPU 可能继续使用旧 TLB 或错过 kill pending。
- 当前项目 timer 中断先 `handle_ipi_irq()`，再根据 `timer_schedule_ready` 调用 `schedule(0)`。

可直接运行：

```bash
sed -n '/void handle_timer_irq/,/^}/p' kern/smp.c
```

### Q41: 从用户态进入异常时，如何保证保存的是当前 CPU 的 trapframe？

答题要点：

- 异常入口按 CPU ID 选择对应内核栈。
- `cpu_trapframe()` 根据当前 CPU 的 `kernel_stack_top` 返回该 CPU 栈顶的 trapframe 位置。
- syscall、`env_run`、TLB mod 等路径都应使用 `cpu_trapframe()`，而不是全局 trapframe。

可直接运行：

```bash
rg -n "cpu_trapframe\\(|SAVE_ALL|kernel_stack_top|env_tf" include/stackframe.h kern/syscall_all.c kern/env.c kern/smp.c kern/tlbex.c
```

### Q42: 中断处理里如何区分 timer interrupt 和 IPI？

答题要点：

- 读取 CP0 `Cause` 和 `Status`，根据 pending 位和 mask 位判断。
- 当前项目约定 `STATUS_IM6` 对应 IPI，`STATUS_IM7` 对应 timer。
- `handle_int` 分流后调用 `handle_ipi_irq()` 或 `handle_timer_irq()`。

可直接运行：

```bash
rg -n "STATUS_IM6|STATUS_IM7|handle_int|CP0_CAUSE|CP0_STATUS|handle_ipi_irq|handle_timer_irq" kern/genex.S kern/smp.c include
```

## 6. 内存管理与 TLB

### Q43: 为什么 TLB 失效要广播？

答题要点：

- 每个 CPU 有自己的 TLB。
- CPU0 修改页表后，CPU1 的 TLB 可能还缓存旧映射。
- 如果不广播，CPU1 可能继续访问已撤销映射、旧权限或已释放物理页。
- 当前 `tlb_invalidate()` 先本地失效，再通过 `smp_group_function_call()` 让其它 CPU 执行 `tlb_invalidate_local()`。

可直接运行：

```bash
rg -n "tlb_invalidate|tlb_invalidate_local|smp_group_function_call" kern/tlbex.c kern/pmap.c
make -s test lab=6_3
timeout 15s make run
```

### Q44: TLB shootdown 具体流程是什么？

答题要点：

- 发起 CPU 修改页表后，在锁外调用 `tlb_invalidate(asid, va)`。
- 本地执行 `tlb_invalidate_local(asid, va)`。
- 如果 SMP 已启动，通过 IPI 广播给其它 ready CPU。
- 远端 CPU 在 IPI handler 中读取 mailbox，调用 `tlb_invalidate_local(asid, va)`，完成后设置 `ipi_done`。

可直接运行：

```bash
sed -n '/void tlb_invalidate_local/,/End of Key Code/p' kern/tlbex.c
sed -n '/void smp_group_function_call/,/^}/p' kern/smp.c
```

### Q45: 为什么 TLB 广播不能在 `pmap_lock` 内做？

答题要点：

- TLB 广播会等待其它 CPU 执行 IPI handler。
- 远端处理期间可能需要 `pmap_lock` 或被某个需要 `pmap_lock` 的路径阻塞。
- 本 CPU 持 `pmap_lock` 等远端，远端又等 `pmap_lock`，会死锁。
- 当前项目约定页表修改和引用计数在锁内完成，释放锁后再做 TLB shootdown。

可直接运行：

```bash
rg -n "pmap_lock|tlb_invalidate|spin_unlock\\(&pmap_lock\\)" kern/pmap.c kern/tlbex.c
```

### Q46: 缺页/TLB refill 时只更新当前 CPU 的 TLB，还是要让其它 CPU 也加入这个映射？

答题要点：

- TLB refill 是按需填充，只需要当前发生 miss 的 CPU 填自己的 TLB。
- 其它 CPU 没有访问该虚拟地址时不需要预填。
- 需要广播的是“失效旧映射”，不是“填入新映射”。

可直接运行：

```bash
rg -n "_do_tlb_refill|passive_alloc|tlb_invalidate_local|tlb_invalidate\\(" kern/tlbex.c
```

### Q47: 如果使用 CAS 实现无锁页表，物理页分配也必须无锁吗？

答题要点：

- 不必须。页表项可以 CAS，无锁管理；物理页空闲链表可以用自旋锁保护。
- 只要所有共享结构都有一致的并发保护即可。
- 当前项目没有做无锁页表，而是用 `pmap_lock` 保护 `page_free_list`、`pp_ref` 和页表修改。

可直接运行：

```bash
rg -n "pmap_lock|page_free_list|pp_ref|pgdir_walk|page_insert|page_remove|atomic_cas" kern/pmap.c kern/spinlock.S
```

### Q48: `page_alloc`、`page_free`、`page_decref` 为什么要加锁？

答题要点：

- 多个 CPU 可能同时分配或释放物理页。
- 如果不加锁，空闲链表可能断链，同一页可能被分配给两个 env，引用计数可能丢更新。
- 当前项目用 `pmap_lock` 统一保护这些共享物理内存结构。

可直接运行：

```bash
rg -n "int page_alloc|void page_free|void page_decref|pmap_lock|page_free_list|pp_ref" kern/pmap.c
```

### Q49: ASID 分配为什么也要加锁？

答题要点：

- ASID bitmap 是全局共享资源。
- 多个 CPU 同时创建 env 时，可能分配到同一个 ASID。
- 当前项目使用 `asid_lock` 保护 ASID bitmap。

可直接运行：

```bash
rg -n "asid_bitmap|asid_lock|asid_alloc|asid_free" kern/env.c
```

### Q50: 为什么 `env_free()` 释放地址空间时会触发很多 TLB shootdown？性能问题怎么解释？

答题要点：

- 释放 env 会逐页 remove 映射，每个失效都可能广播一次。
- 这是简单正确的实现，但不是性能最优。
- 优化可以批量 shootdown、按 ASID 整体刷新、延迟合并失效请求；当前项目优先保证正确性。

可直接运行：

```bash
rg -n "void env_free|page_remove|tlb_invalidate" kern/env.c kern/pmap.c kern/tlbex.c
make -s test lab=6_5
timeout 15s make run
```

## 7. 进程调度与 Env 生命周期

### Q51: 为什么调度器需要 `env_running` 和 `env_cpu_id`？

答题要点：

- `ENV_RUNNABLE` 只表示可运行，不表示当前是否已经被某个 CPU 选中。
- 两个 CPU 可能同时从 runnable list 选择同一个 env。
- `env_running` 表示已被占用，`env_cpu_id` 记录占用 CPU；调度器在锁内检查并设置它们。

可直接运行：

```bash
rg -n "env_running|env_cpu_id|env_sched_lock|TAILQ_FIRST|TAILQ_NEXT" include/env.h kern/sched.c kern/env.c
make -s test lab=6_4
timeout 20s make run
```

### Q52: 怎么避免两个 CPU 同时调度到同一个 Env？

答题要点：

- 所有调度队列遍历和 `env_running/env_cpu_id` 修改都在 `env_sched_lock` 内完成。
- 选中 env 前检查它没有被其它 CPU 运行。
- 选中后立即在锁内标记 `env_running = 1`、`env_cpu_id = 当前 CPU`。

可直接运行：

```bash
sed -n '/void schedule/,/^}/p' kern/sched.c
make -s test lab=6_4
timeout 20s make run
```

### Q53: 如果使用全局 `env_sched_lock`，拿不到锁的 CPU 是自旋还是 idle？

答题要点：

- 当前实现中调度器在找不到合适 env 时会短暂释放锁，处理 pending IPI，执行若干 `nop` 后重试。
- 这相当于简单 idle/spin loop。
- 更完整实现可以引入 idle env 或 `wait` 指令降低空转开销。

可直接运行：

```bash
rg -n "No runnable env|handle_ipi_irq\\(\\)|nop|while \\(1\\)|idle" kern/sched.c user/idle.c
```

### Q54: `Env.env_cpu_id` 和 `cpu_data[cpu].curenv` 有什么区别？

答题要点：

- `cpu_data[cpu].curenv` 是“这个 CPU 当前正在运行哪个 env”。
- `Env.env_cpu_id` 是“这个 env 当前被哪个 CPU 占用”。
- 两者从不同方向描述运行关系，调度器用它们防止双重运行，异常/syscall 用 per-CPU `curenv` 找当前进程。

可直接运行：

```bash
rg -n "curenv|env_cpu_id|env_running|cpu_data\\[cpu\\]\\.curenv" include/smp.h include/env.h kern/sched.c kern/env.c
```

### Q55: 如果只有一个就绪进程、两个 CPU，另一个 CPU 应该休眠还是运行 idle？

答题要点：

- 正常 OS 会让另一个 CPU 运行 idle 线程或进入低功耗等待。
- 当前课程实现可以简单自旋等待 runnable env，同时处理 IPI。
- 答辩时可说明这是功能优先的简化，后续可实现 per-CPU idle env。

可直接运行：

```bash
rg -n "idle|while \\(1\\)|handle_ipi_irq|schedule\\(" kern/sched.c kern/smp.c user/idle.c
```

### Q56: 为什么每核要有独立时间片计数？

答题要点：

- 单核代码中函数静态变量 `count` 表示当前 CPU 时间片。
- SMP 中如果所有 CPU 共享一个 `count`，CPU0 会影响 CPU1 的调度决策，时间片混乱。
- 当前项目用 `cpu_data[cpu].sched_count` 保存每核独立计数。

可直接运行：

```bash
rg -n "sched_count|static int count|count = cpu_data" include/smp.h kern/sched.c kern/smp.c
```

### Q57: 为什么远端运行的 env 不能由发起 CPU 直接 `env_free()`？

答题要点：

- 目标 CPU 可能正在使用该 env 的页表、trapframe、`curenv` 和用户上下文。
- 其它 CPU 直接释放会造成 use-after-free。
- 当前项目把远端 destroy 转换为 `env_kill_pending`，由目标 CPU 在调度安全点本地释放。

可直接运行：

```bash
make -s test lab=6_5
timeout 15s make run
rg -n "env_kill_pending|env_kill_done|env_wait_remote_destroy|env_check_kill_pending" kern/env.c kern/sched.c include/env.h
```

### Q58: 为什么不在 IPI handler 中直接释放远端 env？

答题要点：

- `env_free()` 会释放页表并触发 TLB shootdown，而 TLB shootdown 又依赖 IPI。
- 如果在 IPI handler 里嵌套 IPI 协议，容易覆盖 `ipi_done` 或形成死锁。
- 当前项目 IPI 只负责 poke，真正释放在 `schedule()` 开头的安全点执行。

可直接运行：

```bash
sed -n '/void handle_ipi_irq/,/^}/p' kern/smp.c
sed -n '/void env_check_kill_pending/,/^}/p' kern/env.c
make -s test lab=6_5
timeout 15s make run
```

### Q59: 为什么本地 destroy 路径也要先设置 `env_kill_pending`？

答题要点：

- 检查时目标可能没在远端运行，但释放锁后可能马上被另一个 CPU 调度走。
- 先在锁内设置 `env_kill_pending`，调度器会跳过该 env。
- 这样当前 CPU 后续 `env_free()` 时目标不会被抢走。

可直接运行：

```bash
sed -n '/void env_destroy/,/^}/p' kern/env.c
rg -n "env_kill_pending" kern/sched.c kern/env.c
```

### Q60: 等待远端 destroy 时为什么临时关闭本地中断？

答题要点：

- 如果等待过程中被 timer 打断，可能重入调度或测试钩子，重复发起同一个 destroy。
- 临时关闭本地全局中断可以避免 timer reentry。
- 等待循环中仍主动 `handle_ipi_irq()`，所以 TLB shootdown 等 IPI 仍能被处理。

可直接运行：

```bash
sed -n '/static void env_wait_remote_destroy/,/^}/p' kern/env.c
make -s test lab=6_7
timeout 15s make run
```

### Q61: 如果 env slot 被复用，如何避免旧的 destroy 等待误判？

答题要点：

- 等待时不仅看指针，还要看 `env_id` 是否仍是原来的 envid。
- 如果 slot 已经被复用，`env_id` 会变化，旧等待不能把新 env 当成旧目标。
- 当前远端等待循环检查 `e->env_id == envid && !e->env_kill_done`。

可直接运行：

```bash
sed -n '/static void env_wait_remote_destroy/,/^}/p' kern/env.c
make -s test lab=6_6
timeout 20s make run
```

### Q62: 为什么 `fs_serv` 要绑定到 CPU0？

答题要点：

- 文件系统和 IDE 路径中有较多原本单核假设，共享状态复杂。
- 为了降低 SMP 初期风险，当前项目把 `fs_serv` 绑定到 CPU0。
- 其它用户进程仍可在多核调度；FS 绑定是阶段性保守策略，不是最终高性能设计。

可直接运行：

```bash
rg -n "fs_serv|env_pinned_cpu" kern/env.c kern/sched.c include/env.h
make -s test lab=6_2
printf 'ls\ncat motd\n' | timeout 30s make run
```

## 8. IPC、系统调用与文件系统

### Q63: `sys_ipc_try_send()` 中为什么不能先把接收方设为 runnable，再做页映射？

答题要点：

- SMP 中一旦接收方变为 runnable，另一个 CPU 可能立刻调度它。
- 如果页映射还没完成，接收方会看到不完整 IPC 状态。
- 当前项目先完成共享页映射，再在锁内唤醒接收方。

可直接运行：

```bash
rg -n "sys_ipc_try_send|ipc|ENV_RUNNABLE|page_insert|env_sched_lock" kern/syscall_all.c
make -s test lab=4_5
timeout 15s make run
```

### Q64: 系统调用里为什么要把 `curenv` 全部改成 `cpu_curenv()`？

答题要点：

- syscall 在当前 CPU 上代表当前正在运行的 env。
- 如果仍读全局 `curenv`，可能拿到另一个 CPU 的当前进程，造成权限检查、IPC、destroy 目标错误。
- 当前项目在 `sys_getenvid`、`sys_exofork`、IPC、console 等路径改用 per-CPU 接口。

可直接运行：

```bash
rg -n "cpu_curenv\\(|curenv" kern/syscall_all.c kern/env.c kern/pmap.c
```

### Q65: 从核上的进程发起文件读写系统调用，应该转发给主核还是直接 IPC 给 FS？

答题要点：

- MOS 文件操作本来通过用户态 FS 服务进程和 IPC 完成。
- 当前保守策略是限制 `fs_serv` 只在 CPU0 运行；其它 CPU 的进程仍可通过 IPC 向 FS 发请求。
- 如果进一步限制“只有主核文件写”，可以在 syscall/FS 请求层做权限或转发，但当前关键是 FS 服务自身不在多核并发运行。

可直接运行：

```bash
rg -n "fs_serv|env_pinned_cpu|fsipc|ipc" kern/env.c fs user/lib/fsipc.c user/lib/file.c
make -s test lab=6_2
printf 'ls\ncat motd\n' | timeout 30s make run
```

### Q66: 其他核只能文件读取时，从核 page fault 读取 ELF 内容由谁负责？

答题要点：

- 需要区分“内存缺页”和“文件系统读盘”。
- 用户程序触发的内存缺页由当前 CPU 的异常处理和页表逻辑处理。
- 如果缺页需要文件内容，最终仍应通过 FS 服务或内核加载路径完成；在当前 FS 绑定策略下，实际文件服务运行在 CPU0。

可直接运行：

```bash
rg -n "passive_alloc|_do_tlb_refill|fs_serv|spawn|elf_load|load_icode" kern/tlbex.c kern/env.c user/lib/spawn.c fs
```

### Q67: 文件系统只绑定 CPU0 是否会影响 shell 在 CPU1 上运行？

答题要点：

- 不应该影响普通用户进程在 CPU1 上调度。
- shell 或其它进程需要文件服务时，通过 IPC 和 FS 交互，FS 在 CPU0 处理请求。
- 性能可能受限，但功能路径可以保持正确。

可直接运行：

```bash
make -s test lab=6_2
printf 'ls\ncat motd\ncat script\nsh testshell.sh\n' | timeout 30s make run
```

## 9. 测试、验证与答辩展示

### Q68: 你如何验证 IPI 是双向可用的？

答题要点：

- `lab6_3` 中 CPU0 调用 CPU1 handler，CPU1 handler 中反向调用 CPU0 callback。
- 验证两个方向计数、参数、执行 CPU 都正确。
- 该测试还能暴露同步 IPI 等待期间不处理本地 IPI 导致的死锁。

可直接运行：

```bash
make -s test lab=6_3
timeout 15s make run
```

### Q69: 你如何验证调度器没有把同一个 env 跑在两个 CPU 上？

答题要点：

- 调度代码在锁内设置 `env_running/env_cpu_id`。
- 测试钩子 `pre_env_run` 可在 `env_run` 前断言当前 env 的运行状态与当前 CPU 一致。
- `lab6_4` 统计多核调度次数，并检查 CPU0/CPU1 都参与。

可直接运行：

```bash
make -s test lab=6_4
timeout 20s make run
```

### Q70: 你如何验证远端 destroy 的正确性？

答题要点：

- `lab6_5` 验证单次远端销毁不会 panic。
- `lab6_6` 验证多轮远端销毁和 env slot 复用。
- `lab6_7` 验证双向远端销毁竞态和 timer/IPI 交错。

可直接运行：

```bash
make -s test lab=6_5
timeout 15s make run

make -s test lab=6_6
timeout 20s make run

make -s test lab=6_7
timeout 15s make run
```

### Q71: 为什么有些测试用 `timeout` 结束也算通过？

答题要点：

- 某些测试进程打印 `passed` 后系统没有主动 halt，内核会继续 idle/spin。
- 判断通过依据是测试自己的 `passed` 输出，而不是 QEMU 自然退出。
- `lab6_7` 这类测试会主动 `halt()`，因此可以自然退出。

可直接运行：

```bash
make -s test lab=6_5
timeout 15s make run

make -s test lab=6_7
timeout 15s make run
```

### Q72: 如果老师让现场指出核心代码阅读路线，怎么说？

答题要点：

- `init/start.S`：CPU0/CPU1 启动分流和每核栈。
- `include/smp.h`、`kern/smp.c`：per-CPU 数据、IPI、timer。
- `kern/genex.S`、`include/stackframe.h`：异常入口和每核 trapframe。
- `kern/spinlock.S`：`ll/sc` 自旋锁和原子操作。
- `kern/pmap.c`、`kern/tlbex.c`：内存锁和 TLB shootdown。
- `kern/sched.c`、`kern/env.c`：多核调度和远端 destroy。
- `kern/syscall_all.c`：syscall、IPC 的 SMP 改造。

可直接运行：

```bash
sed -n '1,120p' init/start.S
sed -n '1,220p' include/smp.h
sed -n '1,320p' kern/smp.c
sed -n '1,140p' kern/spinlock.S
sed -n '1,120p' kern/tlbex.c
sed -n '1,180p' kern/sched.c
rg -n "env_destroy|env_check_kill_pending|env_run|env_pinned_cpu" kern/env.c
```

### Q73: 这个实现的主要限制是什么？

答题要点：

- 默认 IPI 是共享内存 pending 模拟，不是真硬件 MMIO IPI。
- 当前固定 `NR_CPUS = 2`，扩展更多核需要进一步检查数组、栈、调度公平性和 IPI fanout。
- `fs_serv` 绑定 CPU0，文件系统不是完整细粒度 SMP。
- 没有完整 idle env，空闲 CPU 可能自旋。
- TLB shootdown 是逐次同步广播，正确但性能不优。

可直接运行：

```bash
rg -n "SMP_USE_MMIO_IPI|NR_CPUS|fs_serv|env_pinned_cpu|idle|tlb_invalidate|smp_group_function_call" include kern user
```

### Q74: 如果要扩展到 4 个 CPU，最先检查哪些地方？

答题要点：

- `NR_CPUS`、每核栈布局、`KSTACKTOP_CPU(cpu)` 是否有足够空间。
- `cpu_id()` 获取的 ID 范围是否和 QEMU vCPU 编号一致。
- `cpu_data`、`ipi_mailbox`、`ipi_done`、`ipi_ready` 等数组是否按 `NR_CPUS` 遍历。
- 调度器公平性、FS 绑定策略、测试用例是否覆盖 4 核。
- IPI fanout 从 1 个远端变成多个远端后，等待和锁顺序是否仍无死锁。

可直接运行：

```bash
rg -n "NR_CPUS|KSTACKTOP_CPU|KSTACKSHIFT|cpu_data\\[NR_CPUS\\]|ipi_.*\\[NR_CPUS\\]|for \\(cpu = 0; cpu < NR_CPUS" include kern init tests
```

### Q75: 当前实现里最容易出 bug 的锁顺序是什么？

答题要点：

- 持 `pmap_lock` 时不能做 TLB shootdown。
- 持 `env_sched_lock` 时要避免调用可能再次调度、释放大量页表或等待 IPI 的复杂函数。
- IPI mailbox 锁按目标 CPU 串行使用，等待期间要能处理本地 pending IPI。
- 答辩时可以强调“锁内只改共享状态，跨 CPU 等待放到锁外”。

可直接运行：

```bash
rg -n "pmap_lock|env_sched_lock|ipi_mailbox_lock|smp_group_function_call|tlb_invalidate|spin_lock|spin_unlock" kern
```

### Q76: 如果出现“偶现卡死”，优先怀疑哪些 SMP 问题？

答题要点：

- 持锁等待 IPI 或 IPI handler 中再发 IPI 导致死锁。
- timer interrupt 重入导致重复 destroy 或重复拿锁。
- 某个 CPU 等待期间没有处理 pending IPI。
- 调度器把所有 runnable env 都跳过，且没有 idle env，导致看似卡死。
- TLB shootdown 没完成，导致远端继续使用旧映射后异常。

可直接运行：

```bash
rg -n "ipi_wait_done|handle_ipi_irq\\(\\)|env_wait_remote_destroy|timer_schedule_ready|pmap_lock|tlb_invalidate" kern
make -s test lab=6_3
timeout 15s make run
make -s test lab=6_7
timeout 15s make run
```

### Q77: 如果出现“同一个 env 输出两份日志”或状态异常，优先检查什么？

答题要点：

- 检查 `env_running/env_cpu_id` 是否在锁内设置和清除。
- 检查 `sys_ipc_recv`、`sys_set_env_status`、`env_free` 是否正确维护调度队列。
- 检查当前 env 是否因为 `env_kill_pending` 没被跳过而被调度。
- 用 `pre_env_run` 钩子在 `env_run` 前断言运行状态。

可直接运行：

```bash
make -s test lab=6_4
timeout 20s make run
rg -n "env_running|env_cpu_id|env_kill_pending|pre_env_run" kern/sched.c kern/env.c tests/lab6_4/pre_env_run.c
```

### Q78: 如果出现 page fault 或 TLB 异常不稳定，优先检查什么？

答题要点：

- `cpu_cur_pgdir()` 是否正确，是否还在使用全局 `cur_pgdir`。
- TLB 失效是否广播到其它 CPU。
- 页表修改和物理页引用计数是否在 `pmap_lock` 保护下。
- 是否在持 `pmap_lock` 时等待 IPI。

可直接运行：

```bash
rg -n "cpu_cur_pgdir|cur_pgdir|tlb_invalidate|pmap_lock|page_insert|page_remove|_do_tlb_refill" kern include
make -s test lab=6_2
printf 'ls\ncat motd\n' | timeout 30s make run
```

### Q79: 如果答辩老师问“你们的 SMP 改造核心思想是什么”，怎么一句话回答？

答题要点：

- 把单核中的“当前状态”全部 per-CPU 化，把共享结构加锁，把 TLB 修改广播到所有 CPU，把调度器改成不会双重运行同一个 env，并把远端 env 生命周期操作放到目标 CPU 的安全点执行。

可直接运行，用于快速定位这句话对应的代码：

```bash
rg -n "cpu_data|spinlock_t|tlb_invalidate|env_running|env_cpu_id|env_kill_pending|env_check_kill_pending" include kern
```

### Q80: 如果老师问“最关键的一处 debug 收获是什么”，怎么回答？

答题要点：

- 远端 destroy 不能在 IPI handler 中直接 `env_free()`。
- 因为 `env_free()` 会触发页表释放和 TLB shootdown，而 shootdown 又依赖 IPI。
- 最终把 IPI handler 简化为通知和 ack，把复杂释放放到调度安全点，避免嵌套 IPI 死锁。

可直接运行：

```bash
make -s test lab=6_5
timeout 15s make run
make -s test lab=6_7
timeout 15s make run
rg -n "handle_ipi_irq|env_free|env_check_kill_pending|tlb_invalidate|smp_group_function_call" kern/smp.c kern/env.c kern/tlbex.c
```
