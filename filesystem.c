#include "filesystem.h"

#include <stdlib.h>
#include <string.h>

#include "flash_ops.h"

#define FS_MAGIC 0x53464154u
#define FS_VERSION 1u
#define FS_DIR_BLOCKS 2u
#define FS_DIR_ENTRY_SIZE 64u
#define FS_FAT_FREE 0u
#define FS_FAT_EOC 0xFFFFFFFFu

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t total_blocks;
    uint32_t fat_start;
    uint32_t fat_blocks;
    uint32_t dir_start;
    uint32_t dir_blocks;
    uint32_t data_start;
    uint32_t next_free_hint;
    uint32_t crc32;
} FsSuperblock;

typedef struct __attribute__((packed)) {
    uint8_t in_use;
    char name[FS_MAX_FILENAME + 1u];
    uint32_t size_bytes;
    uint32_t first_block;
    uint8_t reserved[23];
} FsDirEntry;

typedef struct {
    bool readable;
    bool writable;
    bool append;
    bool truncate;
    bool create;
} FsMode;

_Static_assert(sizeof(FsDirEntry) == FS_DIR_ENTRY_SIZE, "directory entry must be 64 bytes");
_Static_assert((FS_MAX_FILES * FS_DIR_ENTRY_SIZE) == (FS_DIR_BLOCKS * FS_BLOCK_SIZE),
               "directory table must fill the configured directory blocks");
_Static_assert(FS_BLOCK_SIZE == FLASH_OPS_SECTOR_SIZE, "filesystem block must match flash sector size");
_Static_assert(FS_PAGE_SIZE == FLASH_OPS_PAGE_SIZE, "filesystem page must match flash page size");

static FsSuperblock g_super;
static FsDirEntry g_dir[FS_MAX_FILES];
static uint32_t *g_fat;
static uint32_t g_fat_entries_capacity;
static bool g_mounted;
static bool g_super_dirty;
static bool g_fat_dirty;
static bool g_dir_dirty;
static uint8_t g_block_buffer[FS_BLOCK_SIZE];

#ifdef FS_ENABLE_TEST_HOOKS
static uint32_t g_test_data_block_limit;
#endif

static uint32_t fs_data_block_count(void) {
    if (g_super.total_blocks < g_super.data_start) {
        return 0u;
    }

    return g_super.total_blocks - g_super.data_start;
}

static uint32_t fs_block_offset(uint32_t block) {
    return block * FS_BLOCK_SIZE;
}

static bool fs_is_data_block(uint32_t block) {
    return block >= g_super.data_start && block < g_super.total_blocks;
}

static uint32_t fs_fat_index(uint32_t block) {
    return block - g_super.data_start;
}

static uint32_t fs_min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static uint32_t fs_crc32(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0u; i < len; ++i) {
        crc ^= bytes[i];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }

    return ~crc;
}

static uint32_t fs_super_crc(FsSuperblock superblock) {
    superblock.crc32 = 0u;
    return fs_crc32(&superblock, sizeof(superblock));
}

static bool fs_compute_layout(FsSuperblock *layout) {
    uint32_t total_blocks = (uint32_t)(flash_user_capacity() / FS_BLOCK_SIZE);

    if (layout == NULL || total_blocks <= (1u + FS_DIR_BLOCKS)) {
        printf("fs: not enough flash for filesystem metadata\n");
        return false;
    }

    uint32_t fat_blocks = 1u;
    while (true) {
        if (total_blocks <= (1u + FS_DIR_BLOCKS + fat_blocks)) {
            printf("fs: not enough flash for filesystem data\n");
            return false;
        }

        uint32_t data_blocks = total_blocks - 1u - FS_DIR_BLOCKS - fat_blocks;
        uint32_t needed_fat_blocks =
            (uint32_t)(((uint64_t)data_blocks * sizeof(uint32_t) + FS_BLOCK_SIZE - 1u) / FS_BLOCK_SIZE);

        if (needed_fat_blocks == 0u) {
            needed_fat_blocks = 1u;
        }

        if (needed_fat_blocks == fat_blocks) {
            break;
        }

        fat_blocks = needed_fat_blocks;
    }

    memset(layout, 0, sizeof(*layout));
    layout->magic = FS_MAGIC;
    layout->version = FS_VERSION;
    layout->total_blocks = total_blocks;
    layout->fat_start = 1u;
    layout->fat_blocks = fat_blocks;
    layout->dir_start = layout->fat_start + layout->fat_blocks;
    layout->dir_blocks = FS_DIR_BLOCKS;
    layout->data_start = layout->dir_start + layout->dir_blocks;

#ifdef FS_ENABLE_TEST_HOOKS
    if (g_test_data_block_limit != 0u) {
        uint32_t available_data_blocks = layout->total_blocks - layout->data_start;
        uint32_t limited_data_blocks = fs_min_u32(g_test_data_block_limit, available_data_blocks);
        layout->total_blocks = layout->data_start + limited_data_blocks;
    }
#endif

    if (layout->total_blocks <= layout->data_start) {
        printf("fs: no data blocks available\n");
        return false;
    }

    layout->next_free_hint = layout->data_start;
    layout->crc32 = fs_super_crc(*layout);

    return true;
}

static bool fs_ensure_fat_capacity(uint32_t entries) {
    if (entries <= g_fat_entries_capacity) {
        return true;
    }

    uint32_t *new_fat = (uint32_t *)realloc(g_fat, entries * sizeof(uint32_t));
    if (new_fat == NULL) {
        printf("fs: cannot allocate FAT cache (%lu entries)\n", (unsigned long)entries);
        return false;
    }

    g_fat = new_fat;
    g_fat_entries_capacity = entries;
    return true;
}

static bool fs_super_matches_layout(const FsSuperblock *actual, const FsSuperblock *expected) {
    return actual->magic == FS_MAGIC &&
           actual->version == FS_VERSION &&
           actual->total_blocks == expected->total_blocks &&
           actual->fat_start == expected->fat_start &&
           actual->fat_blocks == expected->fat_blocks &&
           actual->dir_start == expected->dir_start &&
           actual->dir_blocks == expected->dir_blocks &&
           actual->data_start == expected->data_start &&
           actual->crc32 == fs_super_crc(*actual);
}

static bool fs_read_block(uint32_t block, uint8_t *buffer) {
    if (block >= g_super.total_blocks) {
        printf("fs: read block %lu out of range\n", (unsigned long)block);
        return false;
    }

    return flash_read_safe(fs_block_offset(block), buffer, FS_BLOCK_SIZE);
}

static bool fs_write_block(uint32_t block, const uint8_t *buffer) {
    if (block >= g_super.total_blocks) {
        printf("fs: write block %lu out of range\n", (unsigned long)block);
        return false;
    }

    return flash_write_sector_safe(fs_block_offset(block), buffer);
}

static bool fs_flush_super(void) {
    if (!g_super_dirty) {
        return true;
    }

    FsSuperblock to_write = g_super;
    to_write.crc32 = fs_super_crc(to_write);
    g_super.crc32 = to_write.crc32;

    memset(g_block_buffer, 0, sizeof(g_block_buffer));
    memcpy(g_block_buffer, &to_write, sizeof(to_write));

    if (!fs_write_block(0u, g_block_buffer)) {
        return false;
    }

    g_super_dirty = false;
    return true;
}

static bool fs_flush_fat(void) {
    if (!g_fat_dirty) {
        return true;
    }

    uint32_t data_blocks = fs_data_block_count();
    const uint8_t *fat_bytes = (const uint8_t *)g_fat;
    uint32_t fat_bytes_total = data_blocks * sizeof(uint32_t);

    for (uint32_t i = 0u; i < g_super.fat_blocks; ++i) {
        uint32_t copied = fs_min_u32(FS_BLOCK_SIZE, fat_bytes_total);

        memset(g_block_buffer, 0, sizeof(g_block_buffer));
        if (copied != 0u) {
            memcpy(g_block_buffer, fat_bytes + (i * FS_BLOCK_SIZE), copied);
            fat_bytes_total -= copied;
        }

        if (!fs_write_block(g_super.fat_start + i, g_block_buffer)) {
            return false;
        }
    }

    g_fat_dirty = false;
    return true;
}

static bool fs_flush_dir(void) {
    if (!g_dir_dirty) {
        return true;
    }

    const uint8_t *dir_bytes = (const uint8_t *)g_dir;
    uint32_t dir_bytes_total = sizeof(g_dir);

    for (uint32_t i = 0u; i < g_super.dir_blocks; ++i) {
        uint32_t copied = fs_min_u32(FS_BLOCK_SIZE, dir_bytes_total);

        memset(g_block_buffer, 0, sizeof(g_block_buffer));
        if (copied != 0u) {
            memcpy(g_block_buffer, dir_bytes + (i * FS_BLOCK_SIZE), copied);
            dir_bytes_total -= copied;
        }

        if (!fs_write_block(g_super.dir_start + i, g_block_buffer)) {
            return false;
        }
    }

    g_dir_dirty = false;
    return true;
}

static bool fs_flush_metadata(void) {
    return fs_flush_fat() && fs_flush_dir() && fs_flush_super();
}

static bool fs_load_fat(void) {
    uint32_t data_blocks = fs_data_block_count();
    uint8_t *fat_bytes = (uint8_t *)g_fat;
    uint32_t fat_bytes_total = data_blocks * sizeof(uint32_t);

    memset(g_fat, 0, g_fat_entries_capacity * sizeof(uint32_t));

    for (uint32_t i = 0u; i < g_super.fat_blocks; ++i) {
        uint32_t copied = fs_min_u32(FS_BLOCK_SIZE, fat_bytes_total);

        if (!fs_read_block(g_super.fat_start + i, g_block_buffer)) {
            return false;
        }

        if (copied != 0u) {
            memcpy(fat_bytes + (i * FS_BLOCK_SIZE), g_block_buffer, copied);
            fat_bytes_total -= copied;
        }
    }

    return true;
}

static bool fs_load_dir(void) {
    uint8_t *dir_bytes = (uint8_t *)g_dir;
    uint32_t dir_bytes_total = sizeof(g_dir);

    memset(g_dir, 0, sizeof(g_dir));

    for (uint32_t i = 0u; i < g_super.dir_blocks; ++i) {
        uint32_t copied = fs_min_u32(FS_BLOCK_SIZE, dir_bytes_total);

        if (!fs_read_block(g_super.dir_start + i, g_block_buffer)) {
            return false;
        }

        if (copied != 0u) {
            memcpy(dir_bytes + (i * FS_BLOCK_SIZE), g_block_buffer, copied);
            dir_bytes_total -= copied;
        }
    }

    return true;
}

static bool fs_path_to_name(const char *path, char name[FS_MAX_FILENAME + 1u]) {
    if (path == NULL) {
        printf("fs: NULL path\n");
        return false;
    }

    if (path[0] == '/') {
        ++path;
    }

    if (path[0] == '\0') {
        printf("fs: empty filename\n");
        return false;
    }

    size_t len = 0u;
    while (path[len] != '\0' && len <= FS_MAX_FILENAME) {
        if (path[len] == '/') {
            printf("fs: subdirectories are not supported: %s\n", path);
            return false;
        }
        ++len;
    }

    if (len == 0u || len > FS_MAX_FILENAME) {
        printf("fs: filename too long: %s\n", path);
        return false;
    }

    memcpy(name, path, len);
    name[len] = '\0';
    return true;
}

static bool fs_parse_mode(const char *mode, FsMode *parsed) {
    if (mode == NULL || parsed == NULL) {
        printf("fs: NULL mode\n");
        return false;
    }

    memset(parsed, 0, sizeof(*parsed));

    if (strcmp(mode, "r") == 0) {
        parsed->readable = true;
        return true;
    }

    if (strcmp(mode, "r+") == 0) {
        parsed->readable = true;
        parsed->writable = true;
        return true;
    }

    if (strcmp(mode, "w") == 0) {
        parsed->writable = true;
        parsed->truncate = true;
        parsed->create = true;
        return true;
    }

    if (strcmp(mode, "w+") == 0) {
        parsed->readable = true;
        parsed->writable = true;
        parsed->truncate = true;
        parsed->create = true;
        return true;
    }

    if (strcmp(mode, "a") == 0) {
        parsed->writable = true;
        parsed->append = true;
        parsed->create = true;
        return true;
    }

    if (strcmp(mode, "a+") == 0) {
        parsed->readable = true;
        parsed->writable = true;
        parsed->append = true;
        parsed->create = true;
        return true;
    }

    printf("fs: invalid open mode: %s\n", mode);
    return false;
}

static int fs_find_dir_entry(const char *name) {
    for (uint32_t i = 0u; i < FS_MAX_FILES; ++i) {
        if (g_dir[i].in_use && strncmp(g_dir[i].name, name, FS_MAX_FILENAME + 1u) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int fs_find_free_dir_entry(void) {
    for (uint32_t i = 0u; i < FS_MAX_FILES; ++i) {
        if (!g_dir[i].in_use) {
            return (int)i;
        }
    }

    return -1;
}

static bool fs_fat_next(uint32_t block, uint32_t *next) {
    if (!fs_is_data_block(block)) {
        printf("fs: bad FAT block %lu\n", (unsigned long)block);
        return false;
    }

    if (next != NULL) {
        *next = g_fat[fs_fat_index(block)];
    }

    return true;
}

static bool fs_alloc_block(uint32_t *allocated_block) {
    uint32_t data_blocks = fs_data_block_count();

    if (data_blocks == 0u || allocated_block == NULL) {
        return false;
    }

    uint32_t start = 0u;
    if (fs_is_data_block(g_super.next_free_hint)) {
        start = g_super.next_free_hint - g_super.data_start;
    }

    for (uint32_t scanned = 0u; scanned < data_blocks; ++scanned) {
        uint32_t idx = (start + scanned) % data_blocks;
        uint32_t block = g_super.data_start + idx;

        if (g_fat[idx] == FS_FAT_FREE) {
            g_fat[idx] = FS_FAT_EOC;
            g_super.next_free_hint = g_super.data_start + ((idx + 1u) % data_blocks);
            *allocated_block = block;
            g_fat_dirty = true;
            g_super_dirty = true;
            return true;
        }
    }

    printf("fs: out of data blocks\n");
    return false;
}

static void fs_free_chain(uint32_t first_block) {
    uint32_t block = first_block;
    uint32_t guard = 0u;
    uint32_t data_blocks = fs_data_block_count();

    while (block != FS_BLOCK_NONE && guard <= data_blocks) {
        if (!fs_is_data_block(block)) {
            printf("fs: stopping free at invalid block %lu\n", (unsigned long)block);
            break;
        }

        uint32_t idx = fs_fat_index(block);
        uint32_t next = g_fat[idx];
        g_fat[idx] = FS_FAT_FREE;

        if (block < g_super.next_free_hint || !fs_is_data_block(g_super.next_free_hint)) {
            g_super.next_free_hint = block;
        }

        if (next == FS_FAT_EOC || next == FS_FAT_FREE) {
            break;
        }

        block = next;
        ++guard;
    }

    g_fat_dirty = true;
    g_super_dirty = true;
}

static uint32_t fs_block_for_position(FS_FILE *file, uint32_t position, bool allocate, bool *allocated) {
    uint32_t ordinal = position / FS_BLOCK_SIZE;

    if (allocated != NULL) {
        *allocated = false;
    }

    if (file->first_block == FS_BLOCK_NONE) {
        if (!allocate || !fs_alloc_block(&file->first_block)) {
            return FS_BLOCK_NONE;
        }

        if (allocated != NULL) {
            *allocated = true;
        }
    }

    uint32_t block = file->first_block;
    while (ordinal > 0u) {
        uint32_t next = FS_BLOCK_NONE;

        if (!fs_fat_next(block, &next)) {
            return FS_BLOCK_NONE;
        }

        if (next == FS_FAT_EOC) {
            if (!allocate || !fs_alloc_block(&next)) {
                return FS_BLOCK_NONE;
            }

            g_fat[fs_fat_index(block)] = next;
            g_fat_dirty = true;

            if (allocated != NULL) {
                *allocated = true;
            }
        } else if (!fs_is_data_block(next)) {
            printf("fs: corrupt FAT link %lu -> %lu\n",
                   (unsigned long)block,
                   (unsigned long)next);
            return FS_BLOCK_NONE;
        }

        block = next;
        --ordinal;
    }

    return block;
}

static bool fs_cache_flush(FS_FILE *file) {
    if (file == NULL || !file->cache_dirty) {
        return true;
    }

    if (!fs_is_data_block(file->cached_block)) {
        printf("fs: dirty cache has invalid block\n");
        return false;
    }

    if (!fs_write_block(file->cached_block, file->write_cache)) {
        return false;
    }

    file->cache_dirty = false;
    return true;
}

static bool fs_cache_load(FS_FILE *file, uint32_t block, bool clear_block) {
    if (file == NULL || !fs_is_data_block(block)) {
        return false;
    }

    if (file->cached_block == block) {
        return true;
    }

    if (!fs_cache_flush(file)) {
        return false;
    }

    if (clear_block) {
        memset(file->write_cache, 0, sizeof(file->write_cache));
    } else if (!fs_read_block(block, file->write_cache)) {
        file->cached_block = FS_BLOCK_NONE;
        return false;
    }

    file->cached_block = block;
    file->cache_dirty = false;
    return true;
}

static void fs_sync_file_to_dir(FS_FILE *file) {
    if (file == NULL || file->dir_index >= FS_MAX_FILES) {
        return;
    }

    g_dir[file->dir_index].size_bytes = file->file_size;
    g_dir[file->dir_index].first_block = file->first_block;
    g_dir_dirty = true;
}

static void fs_refresh_current(FS_FILE *file) {
    if (file == NULL) {
        return;
    }

    file->current_block_offset = file->file_pos % FS_BLOCK_SIZE;
    if (file->file_pos < file->file_size) {
        file->current_block = fs_block_for_position(file, file->file_pos, false, NULL);
    } else {
        file->current_block = FS_BLOCK_NONE;
    }
}

int fs_format(void) {
    FsSuperblock layout;

    if (!fs_compute_layout(&layout)) {
        return -1;
    }

    g_super = layout;
    if (!fs_ensure_fat_capacity(fs_data_block_count())) {
        return -1;
    }

    memset(g_fat, 0, fs_data_block_count() * sizeof(uint32_t));
    memset(g_dir, 0, sizeof(g_dir));

    g_mounted = true;
    g_super_dirty = true;
    g_fat_dirty = true;
    g_dir_dirty = true;

    if (!fs_flush_metadata()) {
        g_mounted = false;
        return -1;
    }

    printf("fs: formatted %lu blocks (%lu data blocks)\n",
           (unsigned long)g_super.total_blocks,
           (unsigned long)fs_data_block_count());
    return 0;
}

int fs_mount(void) {
    FsSuperblock expected;

    if (g_mounted) {
        return 0;
    }

    if (!fs_compute_layout(&expected)) {
        return -1;
    }

    g_super = expected;
    if (!fs_read_block(0u, g_block_buffer)) {
        return -1;
    }

    FsSuperblock found;
    memcpy(&found, g_block_buffer, sizeof(found));

    if (!fs_super_matches_layout(&found, &expected)) {
        printf("fs: no valid filesystem found, formatting\n");
        return fs_format();
    }

    g_super = found;
    if (!fs_ensure_fat_capacity(fs_data_block_count()) || !fs_load_fat() || !fs_load_dir()) {
        g_mounted = false;
        return -1;
    }

    g_super_dirty = false;
    g_fat_dirty = false;
    g_dir_dirty = false;
    g_mounted = true;
    return 0;
}

FS_FILE *fs_open(const char *path, const char *mode) {
    char name[FS_MAX_FILENAME + 1u];
    FsMode parsed_mode;

    if (fs_mount() != 0 || !fs_path_to_name(path, name) || !fs_parse_mode(mode, &parsed_mode)) {
        return NULL;
    }

    int index = fs_find_dir_entry(name);
    if (index < 0) {
        if (!parsed_mode.create) {
            printf("fs: file not found: %s\n", name);
            return NULL;
        }

        index = fs_find_free_dir_entry();
        if (index < 0) {
            printf("fs: root directory is full\n");
            return NULL;
        }

        memset(&g_dir[index], 0, sizeof(g_dir[index]));
        g_dir[index].in_use = 1u;
        strncpy(g_dir[index].name, name, FS_MAX_FILENAME);
        g_dir[index].name[FS_MAX_FILENAME] = '\0';
        g_dir[index].first_block = FS_BLOCK_NONE;
        g_dir_dirty = true;
    }

    if (parsed_mode.truncate) {
        if (g_dir[index].first_block != FS_BLOCK_NONE) {
            fs_free_chain(g_dir[index].first_block);
        }

        g_dir[index].size_bytes = 0u;
        g_dir[index].first_block = FS_BLOCK_NONE;
        g_dir_dirty = true;
    }

    if (!fs_flush_metadata()) {
        return NULL;
    }

    FS_FILE *file = (FS_FILE *)calloc(1u, sizeof(FS_FILE));
    if (file == NULL) {
        printf("fs: cannot allocate file handle\n");
        return NULL;
    }

    file->dir_index = (uint32_t)index;
    file->can_read = parsed_mode.readable;
    file->can_write = parsed_mode.writable;
    file->append = parsed_mode.append;
    file->file_size = g_dir[index].size_bytes;
    file->first_block = g_dir[index].first_block;
    file->file_pos = parsed_mode.append ? file->file_size : 0u;
    file->cached_block = FS_BLOCK_NONE;
    file->current_block = FS_BLOCK_NONE;
    file->current_block_offset = 0u;
    file->cache_dirty = false;
    fs_refresh_current(file);

    return file;
}

void fs_close(FS_FILE *file) {
    if (file == NULL) {
        return;
    }

    if (fs_cache_flush(file)) {
        fs_sync_file_to_dir(file);
        (void)fs_flush_metadata();
    }

    free(file);
}

int fs_read(FS_FILE *file, void *buffer, int size) {
    if (file == NULL || size < 0 || (buffer == NULL && size != 0)) {
        printf("fs: invalid read arguments\n");
        return -1;
    }

    if (!file->can_read) {
        printf("fs: file is not open for reading\n");
        return -1;
    }

    if (size == 0 || file->file_pos >= file->file_size) {
        return 0;
    }

    uint8_t *out = (uint8_t *)buffer;
    uint32_t remaining = fs_min_u32((uint32_t)size, file->file_size - file->file_pos);
    uint32_t total_read = 0u;

    while (remaining > 0u) {
        bool allocated = false;
        uint32_t block = fs_block_for_position(file, file->file_pos, false, &allocated);
        uint32_t offset = file->file_pos % FS_BLOCK_SIZE;
        uint32_t chunk = fs_min_u32(FS_BLOCK_SIZE - offset, remaining);

        (void)allocated;
        if (block == FS_BLOCK_NONE) {
            printf("fs: read hit missing block\n");
            return -1;
        }

        if (file->cached_block == block) {
            memcpy(out + total_read, file->write_cache + offset, chunk);
        } else if (!flash_read_safe(fs_block_offset(block) + offset, out + total_read, chunk)) {
            return -1;
        }

        file->file_pos += chunk;
        total_read += chunk;
        remaining -= chunk;
    }

    fs_refresh_current(file);
    return (int)total_read;
}

int fs_write(FS_FILE *file, const void *buffer, int size) {
    if (file == NULL || size < 0 || (buffer == NULL && size != 0)) {
        printf("fs: invalid write arguments\n");
        return -1;
    }

    if (!file->can_write) {
        printf("fs: file is not open for writing\n");
        return -1;
    }

    if (size == 0) {
        return 0;
    }

    if (file->append) {
        file->file_pos = file->file_size;
    }

    const uint8_t *in = (const uint8_t *)buffer;
    uint32_t remaining = (uint32_t)size;
    uint32_t total_written = 0u;

    while (remaining > 0u) {
        bool allocated = false;
        uint32_t block = fs_block_for_position(file, file->file_pos, true, &allocated);
        uint32_t offset = file->file_pos % FS_BLOCK_SIZE;
        uint32_t chunk = fs_min_u32(FS_BLOCK_SIZE - offset, remaining);

        if (block == FS_BLOCK_NONE) {
            return total_written == 0u ? -1 : (int)total_written;
        }

        if (!fs_cache_load(file, block, allocated)) {
            return total_written == 0u ? -1 : (int)total_written;
        }

        memcpy(file->write_cache + offset, in + total_written, chunk);
        file->cache_dirty = true;

        file->file_pos += chunk;
        if (file->file_pos > file->file_size) {
            file->file_size = file->file_pos;
        }

        total_written += chunk;
        remaining -= chunk;
    }

    fs_sync_file_to_dir(file);
    if (!fs_flush_metadata()) {
        return -1;
    }

    fs_refresh_current(file);
    return (int)total_written;
}

int fs_seek(FS_FILE *file, long offset, int whence) {
    if (file == NULL) {
        printf("fs: NULL file in seek\n");
        return -1;
    }

    int64_t base = 0;
    switch (whence) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = file->file_pos;
            break;
        case SEEK_END:
            base = file->file_size;
            break;
        default:
            printf("fs: invalid seek whence %d\n", whence);
            return -1;
    }

    int64_t target = base + offset;
    if (target < 0 || target > (int64_t)file->file_size) {
        printf("fs: seek target %ld outside file size %lu\n",
               (long)target,
               (unsigned long)file->file_size);
        return -1;
    }

    file->file_pos = (uint32_t)target;
    fs_refresh_current(file);
    return 0;
}

#ifdef FS_ENABLE_TEST_HOOKS
void fs_test_set_data_block_limit(uint32_t data_blocks) {
    g_test_data_block_limit = data_blocks;
    g_mounted = false;
}
#endif
