# Worksheet 1 Part 2 - Pico Flash Filesystem

This repository contains Worksheet 1 Part 2 only. It is a Raspberry Pi Pico filesystem project.

The filesystem stores named files in Pico flash. Flash is the non-volatile memory on the board. It keeps data after reset or power loss.

In this worksheet, I built the flash filesystem layer and connected it to the required API. I also added tests and documentation so the behaviour can be checked on the Pico over serial.

## Overview

This repo implements a FAT-style filesystem. FAT means File Allocation Table. A FAT stores links between blocks, so one file can use more than one flash block.

This is not the simple "one block equals one file" design. Files can grow across several 4096 byte blocks.

For the worksheet, I chose the FAT-style design because it proves multi-block file support. That is more advanced than storing one file in one fixed block.

Supported operations:

- `fs_open(path, mode)`
- `fs_close(file)`
- `fs_read(file, buffer, size)`
- `fs_write(file, buffer, size)`
- `fs_seek(file, offset, whence)`

Extra helpers:

- `fs_mount()`
- `fs_format()`
- `fs_test_set_data_block_limit()` when tests are enabled

Not implemented:

- Subdirectories.
- Delete.
- Rename.
- Copy.
- File permissions.
- Sparse files.
- A fixed open-file table.
- Concurrent access protection.

The main work in this section was turning the empty filesystem functions into a real implementation. I made the public API work with named files, modes, file positions, and data stored in flash.

## Build, Flash, And Serial On Linux

The real target name is `my_blink`. It is defined in `CMakeLists.txt`.

For the build part, I kept the existing Pico target and added the filesystem sources to it. I also kept `pico_add_extra_outputs(my_blink)` so the build produces a UF2 file for flashing.

This CMake snippet is copied from this repo. It shows the target, sources, test option, and UF2 generation.

```cmake
option(RUN_FS_TESTS "Run filesystem tests on boot" ON)

set(PROJECT_SOURCES
  main.c
  flash_ops.c
  filesystem.c
)

if(RUN_FS_TESTS)
  list(APPEND PROJECT_SOURCES fs_tests.c)
endif()

add_executable(my_blink
  ${PROJECT_SOURCES}
)

pico_enable_stdio_usb(my_blink 1)
pico_enable_stdio_uart(my_blink 0)

pico_add_extra_outputs(my_blink)
```

`pico_add_extra_outputs(my_blink)` is important. It creates files such as `.uf2`, `.elf`, and `.bin`.

Build from the repository root:

```bash
export PICO_SDK_PATH=$HOME/pico/pico-sdk
pwd
rm -rf build
cmake -S . -B build
cmake --build build
ls -lh build/my_blink.uf2
ls -lh build/my_blink.elf
ls -lh build/my_blink.bin
file build/my_blink.uf2
file build/my_blink.elf
```

Current build artifacts in this repo use these names:

| Artifact | Path |
| --- | --- |
| UF2 image | `build/my_blink.uf2` |
| ELF image | `build/my_blink.elf` |
| BIN image | `build/my_blink.bin` |

Flash the Pico:

```bash
cd "/path/to/this/repo"
export PICO_SDK_PATH=$HOME/pico/pico-sdk
cmake -S . -B build
cmake --build build
ls -lh build/my_blink.uf2
# Hold BOOTSEL while plugging in the Pico.
lsblk
findmnt | grep RPI-RP2
cp build/my_blink.uf2 /media/$USER/RPI-RP2/
sync
```

Serial is over USB CDC. The firmware enables USB stdio and disables UART stdio.

Find the Linux serial device and open it:

```bash
ls /dev/ttyACM*
dmesg | tail -40
stty -F /dev/ttyACM0 115200
screen /dev/ttyACM0 115200
# Alternative if installed:
minicom -D /dev/ttyACM0 -b 115200
# If no ttyACM device appears, reconnect the Pico.
# Then repeat the ls command.
# In screen, exit with:
# Ctrl-A then K, then Y
```

The baud rate used for the serial monitor is `115200`.

For the board check, I built the firmware, copied the UF2 to the Pico in BOOTSEL mode, and viewed the test output over USB serial. This confirmed that the code ran on hardware, not only in the editor.

## Flash Constraints

Flash is not RAM. RAM can change one byte directly. Flash cannot.

In this worksheet, I handled flash as flash, not as normal memory. The code reads from XIP, but every write goes through erase and program operations.

The code follows these rules:

| Rule | Value used in this repo | What it changes in our code |
| --- | --- | --- |
| Filesystem offset | `FLASH_TARGET_OFFSET = 256 * 1024` | Filesystem data starts 256 KiB into flash. |
| Read method | XIP mapping plus `memcpy` | Reads copy from the mapped flash address. |
| Erase unit | `FLASH_OPS_SECTOR_SIZE = 4096` | A whole sector is erased before rewrite. |
| Program unit | `FLASH_OPS_PAGE_SIZE = 256` | A sector is programmed back in 256 byte pages. |
| Interrupt handling | `save_and_disable_interrupts()` | Interrupts are off while flash erase/program runs. |

XIP means execute in place. The RP2040 maps flash into the processor address space. The code can read flash by copying from `XIP_BASE + offset`.

This snippet is copied from `flash_ops.c`. It shows bounds checks and reads from XIP with `memcpy`.

```c
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
```

This matters because filesystem reads do not erase or program flash. They are just bounded memory copies from the XIP mapping.

This snippet is copied from `flash_ops.c`. It shows interrupt disable, sector erase, page programming, and interrupt restore.

```c
static bool flash_sector_offset_is_valid(uint32_t sector_offset) {
    if ((sector_offset % FLASH_OPS_SECTOR_SIZE) != 0u) {
        printf("flash: sector offset %lu is not %u-byte aligned\n",
               (unsigned long)sector_offset,
               FLASH_OPS_SECTOR_SIZE);
        return false;
    }

    return flash_range_is_valid(sector_offset, FLASH_OPS_SECTOR_SIZE);
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
```

Interrupts are disabled because flash erase/program temporarily blocks normal flash access. An interrupt handler may also need code from flash.

This snippet is copied from `flash_ops.c`. It shows erase-before-rewrite for partial writes.

```c
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
```

The helper reads a whole sector into RAM, patches it, erases the sector, and writes the full sector back.

This was important for my implementation because the filesystem updates metadata and file data many times. A direct memory write would be wrong on Pico flash.

## On-Flash Layout

The filesystem starts at `FLASH_TARGET_OFFSET`. That is `256 KiB` into flash.

The filesystem block size is `FS_BLOCK_SIZE`, which is `4096` bytes. It matches the flash erase sector size.

For this worksheet, I designed the flash area as four parts: superblock, FAT, root directory, and file data. This made the layout predictable and easy to document.

Default layout seen in the serial test run:

- `448` total filesystem blocks.
- `444` data blocks.
- `4` metadata blocks.

ASCII layout:

```text
FLASH_TARGET_OFFSET
|
|-- block 0          superblock
|-- block 1          FAT table
|-- blocks 2..3      root directory
|-- blocks 4..447    file data blocks
|
end of filesystem area
block size: 4096 bytes
target offset: 256 KiB
```

The superblock stores the layout. It also stores a CRC32. CRC32 is a checksum used to detect invalid metadata.

The FAT stores one `uint32_t` entry per data block.

| FAT value | Meaning |
| --- | --- |
| `0` | Free data block |
| `0xFFFFFFFF` | End of chain |
| Other block number | Next data block in the file |

Directory entries store file metadata. Metadata means data about a file, not the file contents.

| Directory field | Meaning |
| --- | --- |
| `in_use` | Marks whether the entry is active. |
| `name` | File name, up to 31 characters plus null terminator. |
| `size_bytes` | File size in bytes. |
| `first_block` | First data block in the FAT chain. |
| `reserved` | Space for future flags. Active flags are not implemented. |

This snippet is copied from `filesystem.c`. It shows the magic, version, FAT markers, superblock, directory entry, and mode flags.

```c
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
```

The `reserved` bytes in `FsDirEntry` are present, but active directory flags are not implemented.

I used fixed-size metadata structures so each part of the filesystem has a clear place in flash. That makes formatting, mounting, and testing simpler.

This snippet is copied from `filesystem.c`. It shows how the layout is computed from flash capacity.

```c
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
```

The code does not hard-code the FAT size. It calculates how many FAT blocks are needed.

This snippet is copied from `filesystem.c`. It shows mount logic and automatic formatting when no valid filesystem is found.

```c
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
```

`fs_open()` calls `fs_mount()`. That makes mounting lazy. The filesystem is prepared on first use.

I added this lazy mount behaviour so the user does not need to call setup code before using `fs_open()`. If the flash does not contain a valid filesystem, it formats automatically.

This snippet is copied from `filesystem.c`. It allocates a free data block.

```c
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
```

New blocks start as end-of-chain. They are linked later if the file grows again.

This snippet is copied from `filesystem.c`. It frees a whole FAT chain during truncate.

```c
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
```

The `guard` variable stops an invalid chain from looping forever.

Chain walking means following FAT links until the block for a file position is found.

This snippet is copied from `filesystem.c`. It walks the FAT chain and can allocate missing blocks when writing.

```c
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
```

Reads call this with allocation disabled. Writes call it with allocation enabled.

This is the core of the multi-block work. A read follows an existing chain. A write can extend that chain by allocating a new data block.

## Limits

These values are pulled from this repository.

| Limit | Value |
| --- | --- |
| Filesystem block size | `4096` bytes |
| Flash program page size | `256` bytes |
| Max filename length | `31` characters |
| Max root directory entries | `128` |
| Root directory blocks | `2` |
| Default data blocks seen in tests | `444` |
| Max single-file data on that layout | `444 * 4096 = 1,818,624` bytes |

The maximum file size is not stored as a single constant. It depends on available data blocks.

For the worksheet write-up, I calculated the practical maximum from the observed test layout. With 444 data blocks and 4096 bytes per block, one file can use up to 1,818,624 bytes if no other files use data blocks.

## API Behaviour

The public API is declared in `filesystem.h`.

In this worksheet, I completed all five required API functions. I also made them use the same internal metadata, FAT chain, and write cache instead of acting as separate dummy functions.

### `fs_open(path, mode)`

Success:

- Returns an `FS_FILE *`.

Error:

- Returns `NULL`.

Cursor behaviour:

- `r`, `r+`, `w`, and `w+` start at position `0`.
- `a` and `a+` start at the end of the file.

Create and truncate:

- `w` and `w+` create if missing.
- `w` and `w+` truncate if the file exists.
- `a` and `a+` create if missing.
- `r` and `r+` require the file to exist.

This snippet is copied from `filesystem.c`. It validates accepted path shapes.

```c
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
```

Accepted paths are `name` and `/name`. A second slash is rejected.

This snippet is copied from `filesystem.c`. It parses all supported modes.

```c
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
```

The rest of the function handles `a+` and rejects all other modes.

This snippet is copied from `filesystem.c`. It shows creation, truncation, and handle setup.

```c
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
```

This is where `w` and `w+` clear old file contents.

For `fs_open`, I implemented path validation, mode parsing, create behaviour, append behaviour, and truncate behaviour. This is the entry point for most filesystem operations.

### `fs_close(file)`

Success:

- No return value.

Error:

- No return value.
- `NULL` is ignored.

Cursor behaviour:

- No useful cursor after close.

Close flushes the dirty cached data block, syncs the file size and first block into the directory entry, flushes metadata, and frees the handle.

For `fs_close`, I made sure pending writes are not left only in RAM. Closing a file writes dirty data and metadata back to flash.

### `fs_read(file, buffer, size)`

Success:

- Returns the number of bytes read.

EOF:

- Returns `0`.

Error:

- Returns `-1`.

Cursor behaviour:

- Moves forward by the number of bytes read.

This snippet is copied from `filesystem.c`. It reads across FAT blocks.

```c
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
```

The function limits reads to the bytes left before EOF.

For `fs_read`, I made reads move across FAT blocks. It also handles EOF cleanly by returning `0`.

### `fs_write(file, buffer, size)`

Success:

- Returns the number of bytes written.

Error:

- Returns `-1` if no bytes were written.
- Can return a short positive count if space runs out after a partial write.

Cursor behaviour:

- Moves forward by the number of bytes written.
- In append mode, the cursor is moved to EOF before writing.

This snippet is copied from `filesystem.c`. It grows files by allocating blocks when needed.

```c
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
```

The call to `fs_block_for_position(..., true, ...)` is what allows file growth.

For `fs_write`, I added block allocation on demand. This lets a file grow from empty, overwrite inside an existing block, or continue into another block.

### `fs_seek(file, offset, whence)`

Success:

- Returns `0`.

Error:

- Returns `-1`.

Cursor behaviour:

- Cursor changes only after the target is checked.

Supported origins:

- `SEEK_SET`
- `SEEK_CUR`
- `SEEK_END`

Sparse files are not implemented. Seeking before byte `0` or after `file_size` fails.

This snippet is copied from `filesystem.c`. It shows seek rules and bounds handling.

```c
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
```

The code walks the FAT only when refreshing or using the current position.

For `fs_seek`, I kept the rule simple. Seeking is allowed inside the existing file size only. Sparse files are not implemented.

## Error Handling And Edge Cases

Most errors print a short message and return a failure value.

I added error checks for the cases that are easy to hit during marking: bad paths, bad modes, EOF, out-of-space, invalid seek, and zero-length reads or writes.

| Case | Real behaviour |
| --- | --- |
| Invalid mode | `fs_open()` returns `NULL`. |
| Invalid path | `fs_open()` returns `NULL`. |
| Subdirectory path | Rejected. Subdirectories are not implemented. |
| Root directory full | New file creation returns `NULL`. |
| Too many open files | Not tracked with a fixed table. `calloc()` failure returns `NULL`. |
| Out of space | `fs_write()` returns `-1` or a short write count. |
| Read at EOF | `fs_read()` returns `0`. |
| Seek out of bounds | `fs_seek()` returns `-1`. |
| `size == 0` read | `fs_read()` returns `0`. |
| `size == 0` write | `fs_write()` returns `0`. |

Delete, rename, and copy are not implemented.

The error handling is deliberately simple. It uses return values for the API and `printf` messages for serial debugging.

## Testing

There is a test runner in `fs_tests.c`. There is no serial CLI command. Tests run on boot when `RUN_FS_TESTS` is enabled.

For testing, I added a boot-time test runner instead of a manual command parser. This makes the tests easy to run: flash the board, open serial, and read the PASS/FAIL lines.

`RUN_FS_TESTS` is `ON` by default in `CMakeLists.txt`. `main.c` calls `fs_run_tests()` once after USB serial starts.

Run tests:

```bash
export PICO_SDK_PATH=$HOME/pico/pico-sdk
rm -rf build
cmake -S . -B build -DRUN_FS_TESTS=ON
cmake --build build
ls -lh build/my_blink.uf2
# Hold BOOTSEL and connect the Pico.
cp build/my_blink.uf2 /media/$USER/RPI-RP2/
sync
ls /dev/ttyACM*
screen /dev/ttyACM0 115200
```

The runner prints `RUN: name`, then `PASS: name` or `FAIL: name`. It finishes with `FS_TEST_RESULT: PASS` or `FS_TEST_RESULT: FAIL`.

This snippet is copied from `fs_tests.c`. It shows the test list and PASS/FAIL result.

```c
int fs_run_tests(void) {
    int failures = 0;

    printf("\nFS_TEST_BEGIN\n");
    failures += run_one("format_mount", test_format_mount);
    failures += run_one("small_write_read", test_small_write_read);
    failures += run_one("multi_block_write_read", test_multi_block_write_read);
    failures += run_one("seek_overwrite", test_seek_overwrite);
    failures += run_one("append_mode", test_append_mode);
    failures += run_one("truncate_mode", test_truncate_mode);
    failures += run_one("read_beyond_eof", test_read_beyond_eof);
    failures += run_one("invalid_open_mode", test_invalid_open_mode);
    failures += run_one("invalid_path", test_invalid_path);
    failures += run_one("out_of_space", test_out_of_space);

    if (failures == 0) {
        printf("FS_TEST_RESULT: PASS\n");
    } else {
        printf("FS_TEST_RESULT: FAIL (%d failures)\n", failures);
    }

    printf("FS_TEST_END\n\n");
    return failures == 0 ? 0 : -1;
}
```

The tests cover:

- Format and mount on empty or invalid flash.
- Create, write, and read a small file.
- Multi-block write and read using a 10 KiB file.
- Seek overwrite in the middle.
- Append mode.
- Truncate mode.
- EOF read returning `0`.
- Invalid mode returning an error.
- Invalid path returning an error.
- Out-of-space handling with a forced 3 block data region.

This snippet is copied from `fs_tests.c`. It shows the multi-block and out-of-space tests.

```c
static bool test_multi_block_write_read(void) {
    FS_FILE *file = NULL;

    CHECK(fs_format() == 0);
    fill_pattern(pattern_10k, sizeof(pattern_10k), 17u);

    file = fs_open("multi.bin", "w");
    CHECK(file != NULL);
    CHECK(fs_write(file, pattern_10k, (int)sizeof(pattern_10k)) == (int)sizeof(pattern_10k));
    fs_close(file);

    memset(readback_10k, 0, sizeof(readback_10k));
    file = fs_open("/multi.bin", "r");
    CHECK(file != NULL);
    CHECK(fs_read(file, readback_10k, (int)sizeof(readback_10k)) == (int)sizeof(readback_10k));
    CHECK(memcmp(pattern_10k, readback_10k, sizeof(pattern_10k)) == 0);
    fs_close(file);
    return true;
}

static bool test_out_of_space(void) {
    FS_FILE *file = NULL;

    fill_pattern(block_pattern, sizeof(block_pattern), 91u);
    fs_test_set_data_block_limit(3u);
    CHECK(fs_format() == 0);

    file = fs_open("full.bin", "w");
```

The forced 3 block region makes out-of-space predictable.

This test set proves the main worksheet behaviours. It covers small files, multi-block files, seeking, appending, truncating, EOF, invalid input, and a full filesystem.

## Worksheet checklist

- [x] Simple filesystem OR FAT-style selected: FAT-style is used.
- [x] `fs_open` implemented.
- [x] `fs_close` implemented.
- [x] `fs_read` implemented.
- [x] `fs_write` implemented.
- [x] `fs_seek` implemented.
- [x] Modes supported and documented.
- [x] Error handling documented.
- [x] Testing documented.
- [x] Documentation included.

This checklist is my summary of what I completed for the worksheet. Every checked item is backed by code in this repository and by the serial test runner.

## Key files

These are the files I worked with for the worksheet implementation. The filesystem logic is mainly in `filesystem.c`, while the flash-specific erase/program rules are in `flash_ops.c`.

| File | What it contains |
| --- | --- |
| `CMakeLists.txt` | Pico target, sources, test option, USB serial, and extra outputs. |
| `filesystem.h` | Public API, limits, and `FS_FILE`. |
| `filesystem.c` | FAT-style filesystem implementation. |
| `flash_ops.h` | Flash constants and helper declarations. |
| `flash_ops.c` | XIP reads, safe sector erase/program, and bounds checks. |
| `fs_tests.h` | Test runner declaration. |
| `fs_tests.c` | Serial PASS/FAIL tests. |
| `main.c` | Pico entry point, USB setup, test call, and idle heartbeat. |
| `pico_sdk_import.cmake` | Pico SDK import script. |
| `TESTING.md` | Extra testing notes. |
