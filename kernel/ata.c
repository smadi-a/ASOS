/*
 * kernel/ata.c — ATA PIO driver (primary IDE channel, LBA28).
 *
 * Port map — primary IDE channel
 * ────────────────────────────────
 *  0x1F0  Data register (16-bit)
 *  0x1F1  Error / Features
 *  0x1F2  Sector count
 *  0x1F3  LBA bits  7:0
 *  0x1F4  LBA bits 15:8
 *  0x1F5  LBA bits 23:16
 *  0x1F6  Drive/Head   — bits[7:5]=101b (LBA28 master) or 111b (slave)
 *                        bits[3:0]=LBA bits 27:24
 *  0x1F7  Command / Status
 *  0x3F6  Alternate status / Device control (read / write)
 *
 * LBA28 drive-select byte
 * ───────────────────────
 *  Master : 0xE0 | (lba[27:24])   (bits 7,6,5 = 1,1,1 — obsolete, LBA, ...)
 *  Slave  : 0xF0 | (lba[27:24])
 */

#include "ata.h"
#include "io.h"
#include "serial.h"
#include <stddef.h>

/* Primary IDE channel ports. */
#define ATA_DATA        0x1F0U
#define ATA_ERROR       0x1F1U
#define ATA_SECCOUNT    0x1F2U
#define ATA_LBA_LO      0x1F3U
#define ATA_LBA_MID     0x1F4U
#define ATA_LBA_HI      0x1F5U
#define ATA_DRIVE_HEAD  0x1F6U
#define ATA_CMD         0x1F7U
#define ATA_STATUS      0x1F7U
#define ATA_ALT_STATUS  0x3F6U

/* Status register bits. */
#define ATA_SR_BSY  0x80U   /* Controller busy          */
#define ATA_SR_DRQ  0x08U   /* Data request ready       */
#define ATA_SR_ERR  0x01U   /* Error bit set            */

/* Commands. */
#define ATA_CMD_READ_SECTORS   0x20U
#define ATA_CMD_WRITE_SECTORS  0x30U
#define ATA_CMD_CACHE_FLUSH    0xE7U
#define ATA_CMD_IDENTIFY       0xECU

/* Drive-select bytes (LBA28, no LBA[27:24] bits set here). */
#define ATA_SEL_MASTER  0xE0U
#define ATA_SEL_SLAVE   0xF0U

/* ── Module state ─────────────────────────────────────────────────────────*/

static bool g_present[2] = { false, false };

/* ── Helpers ──────────────────────────────────────────────────────────────*/

/* 400 ns delay: read alternate-status four times (each read ≈ 100 ns on ISA). */
static void ata_delay400(void)
{
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
}

/*
 * Spin until BSY clears.  Returns 0 on success, -1 if BSY never clears
 * (after ~10 M iterations — no real-time clock, so this is approximate).
 */
static int ata_wait_bsy(void)
{
    for (uint32_t i = 0; i < 0x800000U; i++) {
        if (!(inb(ATA_STATUS) & ATA_SR_BSY))
            return 0;
    }
    return -1;   /* Timeout */
}

/*
 * Spin until BSY=0 and DRQ=1 (or ERR=1).
 * Returns 0 on success (DRQ set), -1 on error or timeout.
 */
static int ata_wait_drq(void)
{
    for (uint32_t i = 0; i < 0x800000U; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ATA_SR_ERR) return -1;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return 0;
    }
    return -1;   /* Timeout */
}

/* ── Drive detection ──────────────────────────────────────────────────────*/

static bool ata_identify_drive(uint8_t drive)
{
    /* A status of 0xFF means no device on the bus (floating data lines). */
    if (inb(ATA_STATUS) == 0xFF) return false;

    /* Select drive; LBA bits [27:24] = 0 for IDENTIFY. */
    outb(ATA_DRIVE_HEAD, drive == ATA_DRIVE_MASTER ? ATA_SEL_MASTER : ATA_SEL_SLAVE);
    ata_delay400();

    /* Zero sector count / LBA registers. */
    outb(ATA_SECCOUNT,  0);
    outb(ATA_LBA_LO,    0);
    outb(ATA_LBA_MID,   0);
    outb(ATA_LBA_HI,    0);

    /* Issue IDENTIFY command. */
    outb(ATA_CMD, ATA_CMD_IDENTIFY);

    /* If status immediately 0 → no drive. */
    if (inb(ATA_STATUS) == 0) return false;

    /* Wait for BSY to clear. */
    if (ata_wait_bsy() != 0) return false;

    /* Non-zero LBA_MID or LBA_HI means ATAPI — skip. */
    if (inb(ATA_LBA_MID) || inb(ATA_LBA_HI)) return false;

    /* Wait for DRQ or ERR. */
    if (ata_wait_drq() != 0) return false;

    /* Drain the 256-word IDENTIFY data buffer. */
    for (int i = 0; i < 256; i++) inw(ATA_DATA);

    return true;
}

/* ── Public API ───────────────────────────────────────────────────────────*/

void ata_init(void)
{
    g_present[ATA_DRIVE_MASTER] = ata_identify_drive(ATA_DRIVE_MASTER);
    g_present[ATA_DRIVE_SLAVE]  = ata_identify_drive(ATA_DRIVE_SLAVE);

    serial_puts("[ATA] master: ");
    serial_puts(g_present[ATA_DRIVE_MASTER] ? "present" : "absent");
    serial_puts("  slave: ");
    serial_puts(g_present[ATA_DRIVE_SLAVE]  ? "present" : "absent");
    serial_puts("\n");
}

bool ata_drive_present(uint8_t drive)
{
    if (drive > 1) return false;
    return g_present[drive];
}

int ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void *buf)
{
    if (!g_present[drive] || count == 0) return -1;

    /* Wait for controller to be idle. */
    if (ata_wait_bsy() != 0) return -1;

    /* Select drive and load the top 4 LBA bits into the drive/head register. */
    uint8_t sel = (drive == ATA_DRIVE_MASTER ? ATA_SEL_MASTER : ATA_SEL_SLAVE)
                  | (uint8_t)((lba >> 24) & 0x0FU);
    outb(ATA_DRIVE_HEAD, sel);
    ata_delay400();

    /* Load LBA and sector count. */
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA_LO,   (uint8_t)(lba));
    outb(ATA_LBA_MID,  (uint8_t)(lba >>  8));
    outb(ATA_LBA_HI,   (uint8_t)(lba >> 16));

    /* Issue READ SECTORS command. */
    outb(ATA_CMD, ATA_CMD_READ_SECTORS);

    uint16_t *ptr = (uint16_t *)buf;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_drq() != 0) return -1;

        /* Read 256 words (512 bytes) from the data port. */
        for (int i = 0; i < 256; i++)
            *ptr++ = inw(ATA_DATA);

        /* A brief delay between sectors lets the drive queue the next one. */
        ata_delay400();
    }

    return 0;
}

int ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t count,
                      const void *buf)
{
    if (!g_present[drive] || count == 0) return -1;

    if (ata_wait_bsy() != 0) return -1;

    uint8_t sel = (drive == ATA_DRIVE_MASTER ? ATA_SEL_MASTER : ATA_SEL_SLAVE)
                  | (uint8_t)((lba >> 24) & 0x0FU);
    outb(ATA_DRIVE_HEAD, sel);
    ata_delay400();

    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA_LO,   (uint8_t)(lba));
    outb(ATA_LBA_MID,  (uint8_t)(lba >>  8));
    outb(ATA_LBA_HI,   (uint8_t)(lba >> 16));

    outb(ATA_CMD, ATA_CMD_WRITE_SECTORS);

    const uint16_t *ptr = (const uint16_t *)buf;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_drq() != 0) return -1;

        for (int i = 0; i < 256; i++)
            outw(ATA_DATA, ptr[s * 256 + i]);

        ata_delay400();
    }

    /* Flush write cache. */
    outb(ATA_CMD, ATA_CMD_CACHE_FLUSH);
    if (ata_wait_bsy() != 0) return -1;

    return 0;
}
