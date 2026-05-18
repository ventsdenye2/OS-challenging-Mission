#include <asm/asm.h>

/* 原子执行 *ptr += value，返回修改前的旧值。 */
LEAF(atomic_add)
.set noreorder
1:
	ll      v0, 0(a0)
	addu    t0, v0, a1
	sc      t0, 0(a0)
	beqz    t0, 1b
	nop
	sync
	jr      ra
	nop
.set reorder
END(atomic_add)

/* 原子执行 *ptr -= value，返回修改前的旧值。 */
LEAF(atomic_sub)
.set noreorder
1:
	ll      v0, 0(a0)
	subu    t0, v0, a1
	sc      t0, 0(a0)
	beqz    t0, 1b
	nop
	sync
	jr      ra
	nop
.set reorder
END(atomic_sub)

/* 原子比较交换：*ptr == old_value 时写入 new_value，成功返回 1。 */
LEAF(atomic_cas)
.set noreorder
1:
	ll      t0, 0(a0)
	bne     t0, a1, 2f
	move    v0, zero
	move    t1, a2
	sc      t1, 0(a0)
	beqz    t1, 1b
	nop
	sync
	li      v0, 1
2:
	jr      ra
	nop
.set reorder
END(atomic_cas)

/* 获取自旋锁：用 ll/sc 把 0 原子改成 1，失败则继续等待。 */
LEAF(spin_lock)
.set noreorder
	li      t1, 1
1:
	ll      t0, 0(a0)
	bnez    t0, 1b
	nop
	sc      t1, 0(a0)
	beqz    t1, 1b
	li      t1, 1
	sync
	jr      ra
	nop
.set reorder
END(spin_lock)

/* 释放自旋锁：写回 0，sync 保证临界区内存访问先完成。 */
LEAF(spin_unlock)
.set noreorder
	sync
	sw      zero, 0(a0)
	jr      ra
	nop
.set reorder
END(spin_unlock)
