#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define FS_BLOCK_SIZE 4096u
#define FS_PAGE_SIZE 256u
#define FS_MAX_FILENAME 31u
#define FS_MAX_FILES 128u
#define FS_BLOCK_NONE 0xFFFFFFFFu

typedef struct {
    uint32_t dir_index;
    bool can_read;
    bool can_write;
    bool append;
    uint32_t file_pos;
    uint32_t file_size;
    uint32_t first_block;
    uint32_t current_block;
    uint32_t current_block_offset;
    uint8_t write_cache[FS_BLOCK_SIZE];
    uint32_t cached_block;
    bool cache_dirty;
} FS_FILE;

int fs_mount(void);
int fs_format(void);

FS_FILE *fs_open(const char *path, const char *mode);
void fs_close(FS_FILE *file);
int fs_read(FS_FILE *file, void *buffer, int size);
int fs_write(FS_FILE *file, const void *buffer, int size);
int fs_seek(FS_FILE *file, long offset, int whence);

#ifdef FS_ENABLE_TEST_HOOKS
void fs_test_set_data_block_limit(uint32_t data_blocks);
#endif

#endif // FILESYSTEM_H
