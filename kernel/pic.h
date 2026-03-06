/*
 * kernel/pic.h — 8259A Programmable Interrupt Controller.
 *
 * The IBM-PC BIOS leaves the 8259 PICs with IRQ 0–7 mapped to vectors
 * 0x08–0x0F, which collides with the CPU's protected-mode exception
 * vectors.  pic_init() remaps them to vectors 32–47 (IRQ 0–15 → 0x20–0x2F).
 *
 * All IRQs are masked after init.  Call pic_unmask_irq() to enable
 * individual lines before installing the corresponding handler.
 */

#ifndef PIC_H
#define PIC_H

#include <stdint.h>

/* Initialise both PICs, remap IRQs to vectors 32–47, mask all lines. */
void pic_init(void);

/* Clear the mask bit for the given IRQ line (0–15). */
void pic_unmask_irq(uint8_t irq);

/* Set the mask bit for the given IRQ line (0–15). */
void pic_mask_irq(uint8_t irq);

/*
 * Send End-Of-Interrupt for the given IRQ (0–15).
 * For IRQ 8–15 (slave PIC), EOI is sent to the slave first, then to
 * the master (which handles the cascade line IRQ2).
 */
void pic_send_eoi(uint8_t irq);

#endif /* PIC_H */
