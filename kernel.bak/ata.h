/*
 * kernel/ata.h — ATA PIO driver (primary IDE, LBA28, polled).
 *
 * Supports master (drive 0) and slave (drive 1) on the primary IDE channel.
 * All I/O is polled — no DMA, no IRQ-based transfers.
 */

#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stdbool.h>

/* Logical drive numbers passed to ata_read_sectors(). */
#define ATA_DRIVE_MASTER  0
#define ATA_DRIVE_SLAVE   1

/*
 * Initialise the ATA subsystem: probe master and slave, print results to
 * serial.  Must be called after serial_init().
 */
void ata_init(void);

/*
 * Read 'count' consecutive 512-byte sectors starting at LBA 'lba' from
 * 'drive' (ATA_DRIVE_MASTER or ATA_DRIVE_SLAVE) into 'buf'.
 * 'buf' must be at least count * 512 bytes.
 *
 * Returns 0 on success, -1 on error (drive error, timeout, missing drive).
 */
int ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void *buf);

/* Returns true if the drive was detected during ata_init(). */
bool ata_drive_present(uint8_t drive);

#endif /* ATA_H */
