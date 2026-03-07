/*
 * kernel/gpt.h — Minimal GPT partition table scanner.
 *
 * Reads the primary GPT header (LBA 1) and scans the partition entry
 * array to locate the EFI System Partition by its type GUID.
 *
 * Only the first 2 TB of the disk are addressable (LBA fits in 32 bits,
 * matching the LBA28 ATA driver limit).
 */

#ifndef GPT_H
#define GPT_H

#include <stdint.h>

/*
 * Scan the GPT on 'drive' (ATA drive number) and return the first LBA
 * of the EFI System Partition, or 0 on failure (I/O error, not a GPT
 * disk, or no ESP found).
 */
uint32_t gpt_find_esp(uint8_t drive);

#endif /* GPT_H */
