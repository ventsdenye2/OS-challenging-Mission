#ifndef MALTA_H
#define MALTA_H

/*
 * QEMU MMIO address definitions.
 */
#define MALTA_PCIIO_BASE 0x18000000
#define MALTA_FPGA_BASE 0x1f000000

/*
 * 16550 Serial UART device definitions.
 */
#define MALTA_SERIAL_BASE (MALTA_PCIIO_BASE + 0x3f8)
#define MALTA_SERIAL_DATA (MALTA_SERIAL_BASE + 0x0)
#define MALTA_SERIAL_LSR (MALTA_SERIAL_BASE + 0x5)
#define MALTA_SERIAL_DATA_READY 0x1
#define MALTA_SERIAL_THR_EMPTY 0x20

/*
 * Intel PIIX4 IDE Controller device definitions.
 * Hardware documentation available at
 * https://www.intel.com/Assets/PDF/datasheet/290562.pdf
 */
#define MALTA_IDE_BASE (MALTA_PCIIO_BASE + 0x01f0)
#define MALTA_IDE_DATA (MALTA_IDE_BASE + 0x00)
#define MALTA_IDE_ERR (MALTA_IDE_BASE + 0x01)
#define MALTA_IDE_NSECT (MALTA_IDE_BASE + 0x02)
#define MALTA_IDE_LBAL (MALTA_IDE_BASE + 0x03)
#define MALTA_IDE_LBAM (MALTA_IDE_BASE + 0x04)
#define MALTA_IDE_LBAH (MALTA_IDE_BASE + 0x05)
#define MALTA_IDE_DEVICE (MALTA_IDE_BASE + 0x06)
#define MALTA_IDE_STATUS (MALTA_IDE_BASE + 0x07)
#define MALTA_IDE_LBA 0xE0
#define MALTA_IDE_BUSY 0x80
#define MALTA_IDE_CMD_PIO_READ 0x20  /* Read sectors with retry */
#define MALTA_IDE_CMD_PIO_WRITE 0x30 /* write sectors with retry */

/*
 * MALTA Power Management device definitions.
 */
#define MALTA_FPGA_HALT (MALTA_FPGA_BASE + 0x500)

/*
 * IPI mailbox register layout used by the challenge documentation.
 *
 * Stock QEMU Malta with a 24Kc CPU does not expose this Loongson-style IPI
 * block, so kern/smp.c keeps MMIO access disabled by default and uses the same
 * mailbox protocol through shared memory. Defining SMP_USE_MMIO_IPI=1 enables
 * these addresses for environments that provide the documented controller.
 */
#define IPI_BASE 0x3ff01000
#define IPI_CPU_STRIDE 0x100
#define IPI_STATUS(cpu) (IPI_BASE + (cpu) * IPI_CPU_STRIDE + 0x00)
#define IPI_ENABLE(cpu) (IPI_BASE + (cpu) * IPI_CPU_STRIDE + 0x04)
#define IPI_SET(cpu) (IPI_BASE + (cpu) * IPI_CPU_STRIDE + 0x08)
#define IPI_CLEAR(cpu) (IPI_BASE + (cpu) * IPI_CPU_STRIDE + 0x0c)
#define IPI_MAILBOX(cpu, slot) (IPI_BASE + (cpu) * IPI_CPU_STRIDE + 0x20 + (slot) * 0x08)

#endif
