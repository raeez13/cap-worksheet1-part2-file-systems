#ifndef FLASH_OPS_H
#define FLASH_OPS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define FLASH_TARGET_OFFSET (256u * 1024u)
#define FLASH_OPS_SECTOR_SIZE 4096u
#define FLASH_OPS_PAGE_SIZE 256u

size_t flash_user_capacity(void);

bool flash_write_safe(uint32_t offset, const uint8_t *data, size_t data_len);
bool flash_read_safe(uint32_t offset, uint8_t *buffer, size_t buffer_len);
bool flash_erase_safe(uint32_t offset);
bool flash_write_sector_safe(uint32_t sector_offset, const uint8_t *sector_data);

#endif // FLASH_OPS_H
