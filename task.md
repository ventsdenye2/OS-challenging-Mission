# MIPS多核 移植

> 本文档仅供参考，如存在模糊、歧义、错误之处，欢迎向课程组提出反馈。
>
> 有关 MIPS 架构的细节，请参考 [See MIPS Run Linux](https://www.amazon.com/See-MIPS-Run-Linux/dp/0120884216)。
>
> 本文档并非最终版本，可能会有更新。最近一次更新时间为 2026 年 4 月 17 日。
>
> 更新日志：
>
> 2026 年 4 月 17 日：初始版本。

## 任务要求

你需要基于 MIPS R24K 架构，移植MOS内核，并通过 QEMU 仿真运行。本任务的目标是支持至少两个处理器，并成功启动 SHELL。任务分为 **核间通信**、**内核启动**、**异常处理**、**内存管理**、**进程调度**、**文件系统** 六个部分。

在该任务中，您将学习到：

- MIPS 架构及多核支持的基本知识
- 使用 QEMU 进行多核仿真与内核开发
- 多核情况下的内存管理
- 多核同步原语与锁机制的实现
- 核间中断（IPI）与通信机制

本次挑战性任务要求：

- 支持至少两个处理器核
- 支持IPI中断
- 支持启动SHELL
- 进程在多核上进行调度
- 支持多核对内存的读写
- 完成相应的实现文档
- 课程组会对上述实现进行检查

本任务不限制实现语言（推荐 C 或 Rust），允许使用符合开源协议的代码以减少工作量（请保留协议声明与出处，并在提交文档中注明），最多可两人组队完成。

## 相关背景知识

> **Note**
> 以下内容为MIPS架构相关，在完成该任务时，你可以利用这些来复习MIPS知识。

### 参考文档

你可以参照以下文档来获取更多的背景知识

1. [See MIPS Run Linux](https://www.amazon.com/See-MIPS-Run-Linux/dp/0120884216)
2. [QEMU 文档](https://www.qemu.org/docs/master/)
3. [MIPS 文档](https://archive.org/details/MIPS_Technologies_MD00086)

### 多核处理器

在当今的计算机体系结构领域，多核处理器系统的设计与实现主要分为两个流派：非对称多处理（AMP）和对称多处理（SMP）。

- **非对称多处理架构**中，每个处理核心具有高的自主性，拥有独立的操作系统和资源，这要求用户直接介入管理系统资源和任务调度，虽然这种设计在某些特定场景下能提供灵活的解决方案，但在操作复杂性和系统维护上却带来了不小的挑战。
- 与之形成鲜明对比的是，**对称多处理架构**中，处理器之间的结构完全一致，它们共享内存资源和输入/输出设备，并由单一操作系统统一调度，确保了任务分配的高效和公平，同时也便于系统的负载均衡和容错处理。

综合考量，SMP架构以其结构的简洁性、更优的性能功耗比以及更加高效的处理器协同工作能力，更适合于充分发挥现代多核处理器的性能潜力。这种架构不仅简化了系统设计与管理的复杂度，还为高效处理大规模计算任务提供了坚实的基础，是当前多核处理器系统设计中的首选方案。

实现多核操作系统，可以实现更有效的任务并行处理，提高系统性能。

在本次挑战性任务中，我们使用**对称多处理（SMP）**架构，所有核心共享内存和外设，由单一操作系统调度。

### MIPS多核处理器

MIPS R24K 是 MIPS 公司推出的一款经典 RISC 架构处理器，支持 32 位和 64 位模式（本任务以 32 位模式为主）。

相比单核系统，多核 MIPS 系统引入了对称多处理（SMP）支持，所有核心共享内存和外设，由单一操作系统调度。MOS 微内核原本为单核设计，此次移植需将其扩展为多核调度。

QEMU 仿真的 MIPS 多核系统基于虚构的 `malta` 平台，支持多个 MIPS R24K 核心（**本任务要求至少两个**）。

### 通用寄存器

MIPS 提供 32 个通用寄存器（`$0` 至 `$31`），用途如下：

| 寄存器 | 别名 | 用途说明 |
|-------|------|---------|
| `$0` | `$zero` | 恒为 0 |
| `$1` | `$at` | 汇编器临时寄存器 |
| `$2`-`$3` | `$v0`-`$v1` | 函数返回值 |
| `$4`-`$7` | `$a0`-`$a3` | 函数参数 |
| `$8`-`$15` | `$t0`-`$t7` | 临时寄存器 |
| `$16`-`$23` | `$s0`-`$s7` | 被调用者保存寄存器 |
| `$24`-`$25` | `$t8`-`$t9` | 临时寄存器 |
| `$26`-`$27` | `$k0`-`$k1` | 被调用者保存寄存器 |
| `$28` | `$gp` | 全局指针 |
| `$29` | `$sp` | 栈指针 |
| `$30` | `$fp` | 帧指针 |
| `$31` | `$ra` | 返回地址 |

### 调用约定

MIPS 使用 `$a0-$a3` 传递前 4 个参数，超出部分通过栈传递；返回值通过 `$v0-$v1` 返回。栈需 8 字节对齐。

### 全局指针

MIPS 使用 `$gp` 作为全局指针，指向数据段基址。

在MOS的实现和使用中，并没有对`$gp`的使用，我们可以用`$gp`来保存每个核**特有数据的地址**，这样每个核都有自己独立的特有数据。

```c
// Define $gp as local data pointer (Global Pointer Register $28)
#define DECLARE_LOCAL_DATA_PTR register volatile ld_t *ld asm("$28")

typedef struct local_data {
  // Per cpu data structure defined hef.
  int cpu_id;
  Pde *cur_pgdir;
  struct Env *curenv;
  // other data you need or the padding space
} ld_t;
```

在这样进行定义后，可以为每个核分配独立的特有数据，并使用`ld`访问。

```c
// Get the cpu id
int cpu_id = ld->cpu_id;
// Get the current environment
struct Env *curenv = ld->curenv;
```

### 协处理器 CP0

CP0 是 MIPS 的关键协处理器，用于管理核心状态、中断和异常。以下是多核相关寄存器：

| 寄存器 | 编号 | 功能描述 |
|-------|------|---------|
| PRId | 15,0 | 处理器 ID，区分不同核心 |
| EBase | 15,1 | 异常向量基址，可独立设置 |
| Status | 12,0 | 状态寄存器，控制中断使能 |
| Cause | 13,0 | 中断原因寄存器 |
| Count | 9,0 | 时钟计数器 |
| Compare | 11,0 | 时钟比较器，触发时钟中断 |

## 工具链安装

**JumpServer跳板机已经提供了MIPS移植开发的完备工具链**，如果你想要在本地开发，需要安装以下工具:

1. `mips-linux-gnu`交叉工具编译链与`gdb-multiarch`工具链，这些工具可以直接通过对应包管理器安装，以下以Debian/Ubuntu为例:

```bash
sudo apt-get install gcc-mips-linux-gnu binutils-mips-linux-gnu gdb-multiarch
```

2. `qemu-system-mips`模拟器

QEMU模拟器可以编译安装也可以直接使用包管理器安装。如果使用编译安装请前往[QEMU](https://github.com/qemu/qemu)下载源码并按照其中教程编译安装；如果使用包管理器安装请参考使用以下命令：

```bash
sudo apt-get install qemu-system-mips
```

> **Note**
> 如果你使用Rust进行开发，请参考Rust帮助文档进行环境配置。

## 内核同步与锁机制

MIPS架构提供了原子指令`ll`与`sc`用于实现常用的锁机制,你可以通过这些指令实现自旋锁与互斥锁。

并且，你需要使用`sync`指令确保前序的所有访存指令都已经产生效果。

### `sync`指令

对于内存的读写，部分现代处理器设计了**乱序访存**的优化机制，需要使用`sync`指令确保前序的所有访存指令都已经产生效果。你可以自行了解相关内容，但和本次挑战性任务无关。

`sync`指令的定义可以参照[MIPS指令集](https://archive.org/details/MIPS_Technologies_MD00086)中的定义。

在C语言中，你可以使用`asm volatile("sync");`来实现`sync`指令。

### 原子操作

利用`ll`与`sc`指令，我们可以实现原子操作：

- **`ll rd, offset(base)`**：从内存地址 `[base + offset]` 加载一个字到寄存器 `rd`，并在硬件上标记该内存地址为"已链接"。这意味着后续的 `sc` 指令会检查该地址是否被其他核心修改。
- **`sc rt, offset(base)`**：尝试将寄存器 `rt` 的值存储到内存地址 `[base + offset]`。如果自 `ll` 执行以来该地址未被修改（即链接未断开），存储成功，且 `rt` 被置为 1；否则存储失败，`rt` 被置为 0。

#### 原子加 `atomic_add`

请你自行设计合适的原子加实现方式：`int atomic_add(int *ptr, int value)`

- 对`ptr`指针对应的值进行加`value`操作
- 使用`ll`与`sc`指令实现
- 使用`sync`指令确保内存操作对其他核心可见
- 使用`jr`指令返回，返回值为操作前的值
- 如果失败，则一直重试直到成功

```asm
LEAF(atomic_add)
/* your code here */
END(atomic_add)
```

#### 原子减 `atomic_sub`

请你自行设计合适的原子加实现方式：`int atomic_add(int *ptr, int value)`

- 对`ptr`指针对应的值进行减`value`操作
- 使用`ll`与`sc`指令实现
- 使用`sync`指令确保内存操作对其他核心可见
- 使用`jr`指令返回，返回值为操作前的值
- 如果失败，则一直重试直到成功

```asm
LEAF(atomic_sub)
/* your code here */
END(atomic_sub)
```

#### 原子交换 `atomic_cas`

请你自行设计合适的原子加实现方式：`int atomic_cas(void *ptr, int old_value, int new_value)`

- 检查`ptr`指针对应的值是否等于`old_value`
- 如果等于，则将`ptr`指针对应的值设置为`new_value`，返回1
- 如果不等于，失败，不需要进行任何操作，返回0
- 使用`ll`与`sc`指令实现
- 使用`sync`指令确保内存操作对其他核心可见
- 使用`jr`指令返回

```asm
LEAF(atomic_cas)
/* your code here */
END(atomic_cas)
```

### 自旋锁

自旋锁是一种简单的锁机制，通过轮询来判断是否获得锁，适用于持有时间短的场景。以下是基于 `ll` 和 `sc` 的自旋锁实现，锁变量地址由 `$a0` 传入，锁值为 0 表示未锁定，1 表示锁定。

#### `spin_lock`的实现

```asm
LEAF(spin_lock)
    li      t1, 1             # t1 = 1（表示锁定状态）
try_lock:
    ll      t0, 0(a0)         # 加载锁变量的值，并建立链接
    bne     t0, zero, try_lock  # 如果锁已被占用（t0 != 0），继续尝试
    nop                       # 延迟槽
    sc      t1, 0(a0)         # 尝试将锁设为 1
    beq     t1, zero, try_lock  # 如果 sc 失败（t1 = 0），重试
    nop                       # 延迟槽
    sync                      # 确保锁状态对其他核心可见
    jr      ra                # 返回
END(spin_lock)
```

#### `spin_unlock`的实现

```asm
LEAF(spin_unlock)
    sync                      # 确保之前操作完成
    sw      zero, 0(a0)       # 将锁变量置为 0（释放锁）
    sync                      # 确保释放操作对其他核心可见
    jr      ra                # 返回
END(spin_unlock)
```

当获取锁时，核心会"自旋"等待，直到锁可用；释放锁时，直接写入 0，无需 `ll/sc`，但需用 `sync` 指令确保该内存修改对于其他核心的可见性。

### 票据锁

> **Note**
> 上一节提到的自旋锁已经足够本次任务的使用，这一节介绍的票据锁仅供扩展，你可以直接跳过本节。

票证锁是一种改进的自旋锁，通过分配"票号"避免无序竞争，提高公平性。锁结构包含两个字段：`ticket`（当前票号）和 `now_serving`（当前服务票号）。每个核心获取一个票号，等待轮到自己。这样做的好处是每个核心都只会访问各自的**票据**，而非访问同一个，这样可以较好地减少缓存一致性开销。

```c
struct ticket_lock {
    volatile uint32_t ticket;      // 当前票号
    volatile uint32_t now_serving; // 当前服务票号
};
```

#### `ticket lock`的实现

```asm
LEAF(ticket_lock)
    # 获取票号
    ll      t0, 0(a0)         # 加载 ticket 值
    addu    t1, t0, 1         # ticket + 1
    sc      t1, 0(a0)         # 尝试存储新 ticket
    beq     t1, zero, ticket_lock  # 如果 sc 失败，重试
    nop                       # 延迟槽
    sync                      # 确保 ticket 更新可见

wait_turn:
    lw      t2, 4(a0)         # 加载 now_serving
    bne     t2, t0, wait_turn  # 如果 now_serving != ticket，继续等待
    nop                       # 延迟槽
    jr      ra                # 返回
END(ticket_lock)
```

#### `ticket unlock`的实现

```asm
LEAF(ticket_unlock)
    lw      t0, 4(a0)         # 加载 now_serving
    addu    t0, t0, 1         # now_serving + 1
    sync                      # 确保之前操作完成
    sw      t0, 4(a0)         # 更新 now_serving
    sync                      # 确保更新可见
    jr      ra                # 返回
END(ticket_unlock)
```

#### 说明

`ticket` 使用 `ll/sc` 保证原子性递增，`now_serving` 的更新无需原子操作，因为每次只有一个核心持有锁。

## 核间通信

核间同步原语是多核环境中实现同步和互斥的关键。在 MIPS 架构中，可以使用核间中断（Inter-Processor Interrupt, IPI）来实现核间同步。

对于QEMU上IPI中断的实现，你需要查阅相应的文档以获得信息。

### 核间中断

核间中断是一种常用的核间同步机制。一个核心可以通过发送核间中断来唤醒另一个核心，或者通知另一个核心执行某个操作。

你需要设置相应的中断位来发出IPI中断，通过核间中断来提醒其他核进行相应的操作。

### 核间通信

核间消息通过`mailbox`传递，`mailbox`的本质是内存中的一片连续空间，每个CPU都有自己独立的`mailbox`，用于接收其他CPU发送的消息。

在MIPS多核系统中，每个CPU都有自己独立的`mailbox`，用于接收其他CPU发送的消息。

一种可能的信息传送包括：

```c
// IPI message passed by mailbox
/**
 * Message: start (0x1)
 * mailbox0: ra
 * mailbox2: sp
 * mailbox4: gp
 * mailbox6: a1
*/
#define IPI_START 0b00000001

/**
 * Message: call function
 * mailbox0: function address
 * mailbox2: a0
 * mailbox4: a1
 * mailbox6: none
*/
#define IPI_CALL 0b00000010
```

此外，IPI中断的实现包括以下几个关键部分：

- 地址设置
- 消息函数
- 初始化

#### 地址设置

```c
#define IPI_BASE            your_ipi_base_address
#define IPI_STATUS(cpuid)   (IPI_BASE + (cpuid)*some_offset + some_value)
#define IPI_ENABLE(cpuid)   (IPI_BASE + (cpuid)*some_offset + some_value)
#define IPI_SET(cpuid)      (IPI_BASE + (cpuid)*some_offset + some_value)
#define IPI_CLEAR(cpuid)    (IPI_BASE + (cpuid)*some_offset + some_value)
#define IPI_MAILBOX(cpuid)  (IPI_BASE + (cpuid)*some_offset + some_value)

// 记录每个CPU的初始化状态：0表示未初始化，1表示已初始化
static int initialized[NR_CPUS]; // no need to use atomic_add
// IPI状态寄存器数组：用于读取每个CPU的中断状态
static volatile u_int *ipi_status[NR_CPUS];
// IPI使能寄存器数组：用于控制每个CPU的中断使能
static volatile u_int *ipi_en[NR_CPUS];
// IPI触发寄存器数组：用于向指定CPU发送中断信号
static volatile u_int *ipi_set[NR_CPUS];
// IPI清除寄存器数组：用于清除指定CPU的中断状态
static volatile u_int *ipi_clear[NR_CPUS];
// IPI邮箱寄存器数组：用于CPU间传递数据和函数指针
static volatile u_int *ipi_mailbox[NR_CPUS];
```

#### 消息函数

为了简化设计难度，给出可以实现IPI操作的函数集合。

> **Note**
> 这些函数不是强制要求的，你也可以设计自己的实现

```c
/**
 * @brief SMP系统初始化函数
 *
 * 该函数负责初始化多处理器系统的IPI（处理器间中断）相关寄存器和数据结构：
 * 1. 设置每个CPU核心的IPI相关寄存器地址（状态、使能、触发、清除和邮箱）
 * 2. 启用所有CPU的IPI中断功能
 * 3. 初始化每个CPU的锁和初始化状态
 * 4. 设置每个CPU核心的ID
 */
void smp_init();

/**
 * @brief 向指定CPU发送IPI信号
 *
 * @param cpu_id 目标CPU的ID
 * @param signal IPI信号类型（如IPI_START, IPI_CALL等）
 *
 * 通过写入目标CPU的IPI_SET寄存器来触发处理器间中断
 */
static void ipi_send(u_int cpu_id, u_int signal);

/**
 * @brief 启动从核心函数
 *
 * 该函数负责启动除当前CPU外的所有CPU核心：
 * 1. 设置每个从核的入口点（PC）为slave_entry
 * 2. 设置每个从核的栈指针（SP）
 * 3. 设置每个从核的线程指针（TP）
 * 4. 向每个从核发送IPI_START信号以启动它们
 */
void smp_secondary_start();

/**
 * @brief 远程函数调用，实现跨CPU核心的函数执行
 *
 * @param function 要在其他CPU上执行的函数指针
 * @param param0 传递给函数的第一个参数
 * @param param1 传递给函数的第二个参数
 *
 * 该函数会：
 * 1. 遍历所有已初始化的其他CPU核心
 * 2. 通过邮箱寄存器传递函数指针和参数
 * 3. 发送IPI_CALL信号触发远程执行
 */
void smp_group_function_call(void *function, u_int param0, u_int param1);

/**
 * @brief IPI中断处理函数
 *
 * 当CPU收到IPI中断时的处理函数：
 * 1. 读取并清除IPI状态
 * 2. 从邮箱中获取函数指针和参数
 * 3. 根据不同的IPI信号类型执行相应操作：
 * 4. 释放IPI锁
 */
void handle_ipi_irq();
```

### 功能验证

为了验证核间通信的正确性，你可以实现以下测试用例：

1. 主核向从核发送启动消息
2. 从核接收消息并回复确认
3. 测试TLB失效的核间同步

下面给出一个简单的测试代码示例，你需要根据自己的实现进行修改：

```c
void test_handler(void) {
    /* your code here, you can design your get_cpu_id() */
    printk("[%d] Received IPI message\n", get_cpu_id());
}

void test_ipi_communication(void) {
    int cpu_id = get_cpu_id();

    if (cpu_id == 0) {
        // 主核发送消息
        printk("[%d] Sending IPI message to slace CPs\n");
        smp_group_function_call(test_handler, 0, 0);
    } else {
        /* your code here */
        void (* func)(int, int) = (void*) ipi_mailbox[ld->cpu_id][0];
        u_int param0 = ipi_mailbox[ld->cpu_id][2];
        u_int param1 = ipi_mailbox[ld->cpu_id][4];
        func(param0, param1);
    }
}
```

## 内核启动

本部分需要你完成以下内容:

1. 搭建内核基本结构
2. 编写内核启动代码
3. 内核同步与锁机制
4. 编写链接脚本
5. 编写Makefile
6. 成功启动并输出字符

> 由于多核启动涉及了多个核的启动，所以需要使用IPI中断进行核间通信。
>
> - 请参考指导书[中断异常处理](#中断异常处理)章节以了解有关中断处理的部分
> - 请参考[QEMU 文档](https://www.qemu.org/documentation/)了解IPI中断的使用。

### 基本结构

```
.
├── Makefile
├── kernel
│   ├── init.c
│   └── start.S
|   └── Makefile
└── kernel.ld
```

- Makefile: 用于编译内核
- kernel: 内核代码目录
- init.c: 内核初始化代码
- start.S: 内核启动代码
- kernel.ld: 内核链接脚本

### 链接脚本编写

链接脚本是内核编译的关键,我们需要通过链接脚本设置入口函数，并配置内核各个段的地址安排,以下是可供参考的链接脚本:

```ld
ENTRY(_start)
SECTIONS
{
    . = 0x80000000; /* MIPS 典型内核起始地址 */
    __start = .;
    .text :
    {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }
    . = ALIGN(4096);
    __text_end = .;

    .rodata :
    {
        *(.rodata .rodata.*)
    }
    . = ALIGN(4096);

    .data :
    {
        *(.data .data.*)
    }
    . = ALIGN(4096);

    .bss :
    {
        __bss_start = .;
        *(.bss .bss.*)
        __bss_end = .;
    }
    . = ALIGN(4096);
    __end = .;
}
```

在上述链接脚本中，我们分别定义了`.text`,`.rodata`,`.data`,`.stack`等段，并使用相关符号诸如`__bss_start`与`__bss_end`来标记bss段的开始与结束地址，这些符号在后续的代码中会用到。

### 启动代码

基于上面提供的参考结构，我们需要完成start.S文件，该文件是主核与从核启动的入口。

对于主核来说，你需要完成以下内容:

- 设置处理器的运行模式
- 设置Status寄存器
- 获取处理器ID
- 如果处理器ID不为0，则跳转到从核启动代码
- 如果处理器ID为0，则设置bss段，进行正常的内核初始化
- 主核完成初始化后，通过IPI中断唤醒从核，至此，平静的湖面泛起涟漪，湖面下内核启动的波涛唤起进程调度的巨浪

```asm
.text
EXPORT(_start)
.set at
.set reorder

/* Set processor running mode and exception handling entry */
/* Set Status register:
   - Disable all interrupts
   - Set kernel mode
   - Clear BEV bit, use normal exception vector
   - Enable necessary CP0 access permissions */
mfc0    t0, CP0_STATUS
/* your code here */

/* Get processor ID, supporse you store it in $t0 */
/* your code here , you can store the processor id in $t0 or other register */
bnez $t0, slave_uboot

/* the main core will set the bss segment
   and jump to mips_init */
  la      v0, bss_start
  la      v1, bss_end
  # ...
```

本仓库当前阶段实现中，从核汇编路径最终跳转到 `smp_secondary_start`，`slave_uboot` 可理解为从核启动入口的示例名称。

对于从核来说，你需要完成以下内容:

- 完成`slave_uboot`代码，作用是初始化从核，关闭相关的中断
- 设置异常处理入口`slave_exc_gen_entry`，你需要自己完成`slave_exc_gen_entry`的内容，作用是读取IPI中断发送的响应信息
- 设置全局中断
- 在设置完成后，使用`wait`指令等待主核完成初始化，主核使用IPI进行通知

```asm
.text
.globl  slave_uboot
slave_uboot:

/* Set the exception handling entry for the slave core */
la      a0, slave_exc_gen_entry
mtc0    a0, CP0_EBASE

/* Set global interrupts
    1. only enable processor-to-processor interrupts (IPI)
    2. enable global interrupts through the Status register */
/* your code here */

/* Wait for the main core to finish initialization */
wait
```

### Makefile

参考MOS的Makefile文件，修改QEMU启动参数以支持多核启动:

```makefile
QEMU_FLAGS := -smp 2 -cpu 24Kc -m 64 -nographic -M malta -no-reboot
```

上述启动参数将核心数通过`-smp`设置为2，并将所使用处理器通过CPU设置为 QEMU 支持的 `24Kc`。部分 QEMU 版本不提供 `r24k` 这个 CPU 型号名，可用 `qemu-system-mipsel -cpu help` 检查本机支持的名称。

### 字符输出

MOS中提供了使用`NS16550A`串口输入输出字符的方式，请参考该方式实现字符输入与输出以及`printk`，需要注意的的是，由于处于多核环境，所以你需要利用**内核同步与锁机制**一节中实现的锁对`NS16550A`进行加锁以防止资源竞争。

### 功能验证

你可以通过读取 CP0 `EBase` 低位等 QEMU/Malta 可用来源来得到当前启动核心ID，并通过在**字符输出**一节中实现的`printk`进行输出，如果成功则表示实现正确。`PRId` 更适合识别处理器型号，不同 QEMU/CPU 配置下未必能区分核心编号。

在本次挑战性任务中，我们要求对`printk`函数进行修改，在每次打印前**输出当前进程所在的cpu processor id**，即：

```c
printk("[%d] ...", cpu_id, ...);
```

在终端的效果为：

```
[0] mips_init_success
[0] hello send from cpu 0
[1] hello received from cpu 0, i am cpu 1
...
```

## 中断异常处理

在这一部分，你需要完成以下内容:

1. 创建多核的异常上下文
2. 实现多核异常处理函数
3. 利用`EBase`寄存器设置异常向量表
4. 实现中断处理与时钟中断

### 多核的异常上下文Trapframe

异常处理需要保存当前的上下文，以便在异常处理完成后恢复现场。在该移植任务中，你可以使用MOS课程代码的`trapframe`结构体。

为了保证每个核不会覆盖彼此的`trapframe`，你需要为每个核分配独立的`trapframe`。

建议将原先的栈空间为每个核进行分配，可以使用`cpu_id`设置相应的平移量

```c
*((struct Trapframe *)(KSTACKTOP - KSTACKSIZE * cpu_id) - 1)
```

### 异常部分

MIPS架构提供了`EBase`寄存器用于为每个处理器设置独立的异常处理程序地址，下面是`EBase`寄存器的布局:

| 位域 | 名称 | 描述 |
|-----|------|------|
| [31:12] | Base | 异常向量基地址（高 20 位，4KB 对齐） |
| [11:1] | - | 保留，通常为 0 |
| [0] | CPUNum | 只读，当前核心编号（多核系统中由硬件设置） |

- **Base 字段**：定义异常向量表的起始地址，必须 4KB 对齐（低 12 位由硬件补 0）。
- **CPUNum 字段**：在多核系统中标识当前核心号，通常由硬件自动填充。

默认情况下，`EBase` 的 Base 字段为 `0x80000`，对应地址 `0x80000000`（MOS课程代码即是使用默认地址）。我们可以修改此字段为每个核心指向自定义向量表。

你可以为每个核设置相应的异常处理地址`slave_exc_gen_entry`，其中的内容可以参考如下：

```asm
.globl  slave_exc_gen_entry
slave_exc_gen_entry:
    /* Get current CPU ID */

    /* Compute the IPI base address for the current CPU */
    li      t0, IPI_BASE
    /* Your code here */

    /* Handle the IPI data
      1. Read the status information and clear the interrupt
      2. Read the startup parameters (PC, SP, GP, a1)
      注：这些参数是由主核预先设置好的 */
    lw      t0, 0x0(t1)         /* Read the IPI status */
    sw      t0, 0xc(t1)         /* Clear the interrupt */

    /* Read the startup parameters (PC, SP, GP, a1) */
    lw      ra, 0x20(t1)        /* Read the program counter PC */
    lw      sp, 0x28(t1)        /* Read the stack pointer SP */
    lw      gp, 0x30(t1)        /* Read the global pointer GP */
    lw      a1, 0x38(t1)        /* Read the parameter a1 */

    /* Your code here */
```

与异常相关的CP0协处理器如下:

![MIPS CP0协处理器](OSome%20-%20教程内容_files/exc_regs.png)

上面这部分内容是MIPS异常处理的基础，在MOS课程代码中也有所涉及，你需要根据这些内容完成异常处理函数的编写。

### 中断处理

MIPS架构提供了`Count`与`Compare`寄存器用于实现时钟中断，其中`Count`寄存器用于读取当前时钟计数器的值，`Compare`寄存器用于存储时钟中断的触发时间，你需要在异常处理函数中实现时钟中断的处理。

MIPS中的`status`寄存器可以用于控制中断使能，在完成了上述两个寄存器以及时钟中断的处理后，你需要设置`status`寄存器以开启时钟中断,其寄存器布局如下:

![MIPS STATUS寄存器](OSome%20-%20教程内容_files/status_reg.png)

你需要自己设计新的中断方案，以支持IPI中断的实现。

### 功能验证

你需要自己编写相应的测试方案，以验证在多核环境下，异常中断的正确性。

## 内存管理

在多核环境中，内存管理需要考虑多个核心的并发访问。在本部分，你需要完成以下内容：

- 设计相应的页表锁。将MOS的页表修改为支持多核的管理方式
- 对于TLB的修改，需要进行合适的管理，**将TLB的修改同步到所有核心**

### 基于CAS的页表管理

在多核环境中，页表的管理需要保证原子性。可以使用比较并交换（CAS）操作来实现页表的无锁管理。

CAS操作的基本思想是：在修改页表项之前，先比较当前值和预期值，如果相等，则更新页表项；否则，重新尝试。

### 对TLB的管理

在进行TLB的修改时，需要考虑TLB的同步问题。

对于SMP系统，每个CPU有自己的TLB，但需要确保TLB中的信息是同步的（想一想这是为什么）。

TLB的修改需要同步到所有核心，一种比较低效的方式是**刷掉所有TLB表项**，需要通过IPI中断进行同步。

你可以设计一个类似下面的函数，在原先`tlb_invalidate`函数的基础上，通过`smp_group_function_call`将信息进行广播，进行TLB的同步。

```c
void tlb_invalidate(u_int asid, u_long virtual_address) {
  tlb_invalidate_local(asid, virtual_address);
  smp_group_function_call(&tlb_invalidate_local, asid, virtual_address);
}
```

## 进程调度

在该部分，你需要完成以下内容:

1. 创建进程控制块结构体
2. 实现多核下的时间片轮转调度算法

### 进程控制块结构体

对于进程控制块，需要增加**进程被哪个CPU运行**的信息，这样在调度时，可以根据进程被哪个CPU运行，选择相应的CPU进行调度。

```c
struct Env {
  // 当前进程在哪个CPU上运行
  int cpu_id;
  // ...其余的进程控制块内容
};
```

### 调度算法实现

原先的进程调度方式是**轮转调度**，即每个进程在CPU上运行一个时间片，然后切换到下一个进程。

为了减小开发难度，可以使用**抢占式**的调度方式：

- 对于原先的进程队列，设置读进程队列锁`envs_lock`，多个CPU竞争该锁
- 每个CPU在调度时，先获取锁`envs_lock`，如果获取成功，则进行调度，否则等待
- 调度时，选择下一个进程，并设置其为当前进程
- 设置当前进程的CPUID
- 释放锁`envs_lock`

你也可以设计其他调度算法，如**多队列调度**等，进行更为高效的调度算法实现。

### 调度验证

**我们要求**：进程应该在多个核心上调度。无论采用什么调度策略，都应该尽量保证在多个核上均匀调度。

你应该在实现报告中详细描述你的实现，我们会对你的代码进行检查。

## 文件系统

为了降低开发难度，我们允许同学们设置**只有主核**可以进行文件操作，其他核只能进行文件读取。

你可以思考怎么判断当前进程为文件系统进程。

> **Note**
> 注意：对于其他进程，你依旧需要在多核之间调度。我们会对你的实现代码进行检查。

## 其他可选项

以下可选项均不计入总分，供同学们进行探索：

1. 支持更多的处理器：在完成支持两个处理器的基础上，可以尝试支持更多的处理器。
2. 优化锁的性能：在实现锁的基础上，可以尝试优化锁的性能，例如使用自旋锁的变种，如教程中提及的票证锁，或其他更多的锁。
3. 实现负载均衡：在多核调度的基础上，可以尝试实现负载均衡算法，使任务能够在多个核心之间均匀分配。
4. 优化调度算法：在多核调度的基础上，可以尝试优化调度算法，例如实现多队列调度。
5. 实现文件系统：在完成文件系统的基础上，可以尝试实现其他文件系统，如FAT，以及其他对多核读写性能更好的文件系统。
6. 实现非对称多处理：在完成对称多处理的基础上，可以尝试实现非对称多处理，即每个处理器具有不同的功能，如主核负责调度，从核负责文件操作。



