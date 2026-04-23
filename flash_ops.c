#include "flash_ops.h"

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

static uint8_t flash_sector_scratch[FLASH_OPS_SECTOR_SIZE];

static bool flash_range_is_valid(uint32_t offset, size_t len) {
    size_t capacity = flash_user_capacity();

    if (offset > capacity) {
        printf("flash: offset %lu outside user flash\n", (unsigned long)offset);
        return false;
    }

    if (len > capacity - offset) {
        printf("flash: range %lu..%lu outside user flash\n",
               (unsigned long)offset,
               (unsigned long)(offset + len));
        return false;
    }

    return true;
}

static bool flash_sector_offset_is_valid(uint32_t sector_offset) {
    if ((sector_offset % FLASH_OPS_SECTOR_SIZE) != 0u) {
        printf("flash: sector offset %lu is not %u-byte aligned\n",
               (unsigned long)sector_offset,
               FLASH_OPS_SECTOR_SIZE);
        return false;
    }

    return flash_range_is_valid(sector_offset, FLASH_OPS_SECTOR_SIZE);
}

size_t flash_user_capacity(void) {
    if (PICO_FLASH_SIZE_BYTES <= FLASH_TARGET_OFFSET) {
        return 0u;
    }

    return PICO_FLASH_SIZE_BYTES - FLASH_TARGET_OFFSET;
}

bool flash_read_safe(uint32_t offset, uint8_t *buffer, size_t buffer_len) {
    if ((buffer == NULL && buffer_len != 0u) || !flash_range_is_valid(offset, buffer_len)) {
        return false;
    }

    if (buffer_len == 0u) {
        return true;
    }

    memcpy(buffer, (const void *)(XIP_BASE + FLASH_TARGET_OFFSET + offset), buffer_len);
    return true;
}

bool flash_erase_safe(uint32_t offset) {
    if (!flash_sector_offset_is_valid(offset)) {
        return false;
    }

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET + offset, FLASH_OPS_SECTOR_SIZE);
    restore_interrupts(ints);

    return true;
}

bool flash_write_sector_safe(uint32_t sector_offset, const uint8_t *sector_data) {
    if (sector_data == NULL || !flash_sector_offset_is_valid(sector_offset)) {
        return false;
    }

    uint32_t absolute_offset = FLASH_TARGET_OFFSET + sector_offset;
    uint32_t ints = save_and_disable_interrupts();

    flash_range_erase(absolute_offset, FLASH_OPS_SECTOR_SIZE);
    for (uint32_t page = 0u; page < FLASH_OPS_SECTOR_SIZE; page += FLASH_OPS_PAGE_SIZE) {
        flash_range_program(absolute_offset + page, sector_data + page, FLASH_OPS_PAGE_SIZE);
    }

    restore_interrupts(ints);
    return true;
}

bool flash_write_safe(uint32_t offset, const uint8_t *data, size_t data_len) {
    if ((data == NULL && data_len != 0u) || !flash_range_is_valid(offset, data_len)) {
        return false;
    }

    if (data_len == 0u) {
        return true;
    }

    size_t written = 0u;
    while (written < data_len) {
        uint32_t current = offset + (uint32_t)written;
        uint32_t sector_offset = current - (current % FLASH_OPS_SECTOR_SIZE);
        uint32_t in_sector = current - sector_offset;
        size_t chunk = FLASH_OPS_SECTOR_SIZE - in_sector;

        if (chunk > data_len - written) {
            chunk = data_len - written;
        }

        if (!flash_read_safe(sector_offset, flash_sector_scratch, sizeof(flash_sector_scratch))) {
            return false;
        }

        memcpy(flash_sector_scratch + in_sector, data + written, chunk);

        if (!flash_write_sector_safe(sector_offset, flash_sector_scratch)) {
            return false;
        }

        written += chunk;
    }

    return true;
}
