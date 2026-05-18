#ifndef _SPINLOCK_H_
#define _SPINLOCK_H_

/* 自旋锁状态：0 表示未持有，1 表示已持有。 */
typedef volatile int spinlock_t;

/* 静态初始化一个未上锁的 spinlock。 */
#define SPINLOCK_INIT 0

/* 获取自旋锁，直到成功为止。 */
void spin_lock(spinlock_t *lock);
/* 释放自旋锁。 */
void spin_unlock(spinlock_t *lock);

/* 原子加法，返回修改前的旧值。 */
int atomic_add(int *ptr, int value);
/* 原子减法，返回修改前的旧值。 */
int atomic_sub(int *ptr, int value);
/* 原子比较交换，成功返回 1，失败返回 0。 */
int atomic_cas(void *ptr, int old_value, int new_value);

#endif
