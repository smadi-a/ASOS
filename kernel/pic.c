/*
 * kernel/pic.c — 8259A PIC initialisation and control.
 *
 * Initialisation Command Words (ICWs) for cascaded 8259 setup:
 *
 *   ICW1 (sent to command port): 0x11
 *       bit 4 = 1  (ICW1 indicator)
 *       bit 3 = 0  (edge-triggered)
 *       bit 0 = 1  (ICW4 required)
 *
 *   ICW2 (sent to data port): interrupt vector base
 *       Master → 0x20 (IRQ 0 maps to vector 32)
 *       Slave  → 0x28 (IRQ 8 maps to vector 40)
 *
 *   ICW3 (sent to data port): cascade wiring
 *       Master → 0x04 (slave connected on IRQ2, bit 2 set)
 *       Slave  → 0x02 (cascade identity = 2)
 *
 *   ICW4 (sent to data port): 0x01 (8086 mode, non-buffered)
 */

#include "pic.h"
#include "io.h"
#include "serial.h"

/* Master PIC */
#define PIC1_CMD   0x20
#define PIC1_DATA  0x21

/* Slave PIC */
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

/* Non-specific EOI command */
#define PIC_EOI    0x20

/* ICW1: initialise + expect ICW4 */
#define ICW1_INIT  0x11

/* ICW4: 8086 mode */
#define ICW4_8086  0x01

void pic_init(void)
{
    /* Save existing masks (not strictly necessary since we set 0xFF, but
     * shows the pattern for later suspend/resume). */

    /* ICW1: start initialisation sequence on both PICs. */
    outb(PIC1_CMD, ICW1_INIT); io_wait();
    outb(PIC2_CMD, ICW1_INIT); io_wait();

    /* ICW2: set interrupt vector offsets. */
    outb(PIC1_DATA, 0x20);     io_wait();   /* Master: IRQ0 → vector 32 */
    outb(PIC2_DATA, 0x28);     io_wait();   /* Slave:  IRQ8 → vector 40 */

    /* ICW3: configure cascade. */
    outb(PIC1_DATA, 0x04);     io_wait();   /* Master: slave on IRQ2     */
    outb(PIC2_DATA, 0x02);     io_wait();   /* Slave:  cascade identity  */

    /* ICW4: 8086/88 mode. */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Mask all IRQ lines. Drivers unmask individual lines as needed. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    serial_puts("[PIC] Initialized, IRQs remapped to vectors 32-47.\n");
}

void pic_unmask_irq(uint8_t irq)
{
    uint16_t port;
    uint8_t  mask;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    mask = inb(port) & (uint8_t)~(1u << irq);
    outb(port, mask);
}

void pic_mask_irq(uint8_t irq)
{
    uint16_t port;
    uint8_t  mask;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    mask = inb(port) | (uint8_t)(1u << irq);
    outb(port, mask);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);   /* slave first */
    outb(PIC1_CMD, PIC_EOI);       /* always master */
}
