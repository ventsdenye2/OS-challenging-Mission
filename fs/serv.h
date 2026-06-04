#include <fs.h>
#include <lib.h>
#include <mmu.h>

#define PTE_DIRTY 0x0004 // file system block cache is dirty

#define SECT_SIZE 512			  /* Bytes per disk sector */
#define SECT2BLK (BLOCK_SIZE / SECT_SIZE) /* sectors to a block */

/* Disk block n, when in memory, is mapped into the file system
 * server's address space at DISKMAP+(n*BLOCK_SIZE). */
#define DISKMAP 0x10000000

/* Maximum disk size we can handle (1GB) */
#define DISKMAX 0x40000000

/* SMP 阶段 7: 用户态自旋锁（基于 MIPS ll/sc 指令）。
 * FS 服务固定在 CPU0，此锁提供防御性保护。 */
typedef volatile int user_spinlock_t;
#define USER_SPINLOCK_INIT 0

static inline void user_spin_lock(user_spinlock_t *lk) {
	int tmp;
	__asm__ __volatile__(
		"1:  ll   %0, 0(%1)    \n"
		"    bnez %0, 1b       \n"
		"    li   %0, 1        \n"
		"    sc   %0, 0(%1)    \n"
		"    beqz %0, 1b       \n"
		"    sync              \n"
		: "=&r"(tmp) : "r"(lk) : "memory");
}

static inline void user_spin_unlock(user_spinlock_t *lk) {
	__asm__ __volatile__(
		"    sync              \n"
		"    sw   $0, 0(%0)    \n"
		: : "r"(lk) : "memory");
}

/* FS 全局锁，保护 block cache、bitmap 和文件元数据。
 * 由 fs/fs.c 定义，serv.c 和 ide.c 引用。 */
extern user_spinlock_t fs_lock;

/* ide.c */
void ide_read(u_int diskno, u_int secno, void *dst, u_int nsecs);
void ide_write(u_int diskno, u_int secno, void *src, u_int nsecs);

/* fs.c */
int file_open(char *path, struct File **pfile);
int file_create(char *path, struct File **file);
int file_get_block(struct File *f, u_int blockno, void **pblk);
int file_set_size(struct File *f, u_int newsize);
void file_close(struct File *f);
int file_remove(char *path);
int file_dirty(struct File *f, u_int offset);
void file_flush(struct File *);

void fs_init(void);
void fs_sync(void);
extern uint32_t *bitmap;
int map_block(u_int);
int alloc_block(void);
