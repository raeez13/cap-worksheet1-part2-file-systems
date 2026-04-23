#include "fs_tests.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "filesystem.h"

#define CHECK(condition)                                      \
    do {                                                      \
        if (!(condition)) {                                   \
            printf("  check failed at line %d: %s\n",         \
                   __LINE__,                                  \
                   #condition);                               \
            return false;                                     \
        }                                                     \
    } while (0)

typedef bool (*FsTestFn)(void);

static uint8_t pattern_10k[10u * 1024u];
static uint8_t readback_10k[10u * 1024u];
static uint8_t block_pattern[FS_BLOCK_SIZE];

static void fill_pattern(uint8_t *buffer, size_t len, uint8_t seed) {
    for (size_t i = 0u; i < len; ++i) {
        buffer[i] = (uint8_t)(seed + (i * 31u) + (i / 7u));
    }
}

static bool test_format_mount(void) {
    fs_test_set_data_block_limit(0u);
    CHECK(fs_format() == 0);
    fs_test_set_data_block_limit(0u);
    CHECK(fs_mount() == 0);
    return true;
}

static bool test_small_write_read(void) {
    const char message[] = "small flash file";
    char buffer[sizeof(message)] = {0};
    FS_FILE *file = NULL;

    CHECK(fs_format() == 0);
    file = fs_open("/small.txt", "w+");
    CHECK(file != NULL);
    CHECK(fs_write(file, message, (int)strlen(message)) == (int)strlen(message));
    CHECK(fs_seek(file, 0, SEEK_SET) == 0);
    CHECK(fs_read(file, buffer, (int)strlen(message)) == (int)strlen(message));
    CHECK(memcmp(buffer, message, strlen(message)) == 0);
    fs_close(file);
    return true;
}

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

static bool test_seek_overwrite(void) {
    const char start[] = "hello simple fat";
    const char patch[] = "PICO";
    const char expected[] = "hello PICOle fat";
    char buffer[sizeof(expected)] = {0};
    FS_FILE *file = NULL;

    CHECK(fs_format() == 0);
    file = fs_open("seek.txt", "w+");
    CHECK(file != NULL);
    CHECK(fs_write(file, start, (int)strlen(start)) == (int)strlen(start));
    CHECK(fs_seek(file, 6, SEEK_SET) == 0);
    CHECK(fs_write(file, patch, (int)strlen(patch)) == (int)strlen(patch));
    CHECK(fs_seek(file, 0, SEEK_SET) == 0);
    CHECK(fs_read(file, buffer, (int)strlen(expected)) == (int)strlen(expected));
    CHECK(memcmp(buffer, expected, strlen(expected)) == 0);
    fs_close(file);
    return true;
}

static bool test_append_mode(void) {
    char buffer[8] = {0};
    FS_FILE *file = NULL;

    CHECK(fs_format() == 0);
    file = fs_open("append.txt", "w");
    CHECK(file != NULL);
    CHECK(fs_write(file, "one", 3) == 3);
    fs_close(file);

    file = fs_open("append.txt", "a+");
    CHECK(file != NULL);
    CHECK(fs_write(file, "two", 3) == 3);
    CHECK(fs_seek(file, 0, SEEK_SET) == 0);
    CHECK(fs_read(file, buffer, 6) == 6);
    CHECK(memcmp(buffer, "onetwo", 6) == 0);
    fs_close(file);
    return true;
}

static bool test_truncate_mode(void) {
    char buffer[16] = {0};
    FS_FILE *file = NULL;

    CHECK(fs_format() == 0);
    file = fs_open("truncate.txt", "w");
    CHECK(file != NULL);
    CHECK(fs_write(file, "old content", 11) == 11);
    fs_close(file);

    file = fs_open("truncate.txt", "w");
    CHECK(file != NULL);
    CHECK(fs_write(file, "new", 3) == 3);
    fs_close(file);

    file = fs_open("truncate.txt", "r");
    CHECK(file != NULL);
    CHECK(fs_read(file, buffer, sizeof(buffer)) == 3);
    CHECK(memcmp(buffer, "new", 3) == 0);
    fs_close(file);
    return true;
}

static bool test_read_beyond_eof(void) {
    char buffer[8] = {0};
    FS_FILE *file = NULL;

    CHECK(fs_format() == 0);
    file = fs_open("eof.txt", "w+");
    CHECK(file != NULL);
    CHECK(fs_write(file, "abc", 3) == 3);
    CHECK(fs_seek(file, 0, SEEK_SET) == 0);
    CHECK(fs_read(file, buffer, sizeof(buffer)) == 3);
    CHECK(fs_read(file, buffer, sizeof(buffer)) == 0);
    fs_close(file);
    return true;
}

static bool test_invalid_open_mode(void) {
    CHECK(fs_format() == 0);
    CHECK(fs_open("bad.txt", "q") == NULL);
    return true;
}

static bool test_invalid_path(void) {
    CHECK(fs_format() == 0);
    CHECK(fs_open("", "w") == NULL);
    CHECK(fs_open("/dir/file.txt", "r") == NULL);
    return true;
}

static bool test_out_of_space(void) {
    FS_FILE *file = NULL;

    fill_pattern(block_pattern, sizeof(block_pattern), 91u);
    fs_test_set_data_block_limit(3u);
    CHECK(fs_format() == 0);

    file = fs_open("full.bin", "w");
    CHECK(file != NULL);
    CHECK(fs_write(file, block_pattern, FS_BLOCK_SIZE) == (int)FS_BLOCK_SIZE);
    CHECK(fs_write(file, block_pattern, FS_BLOCK_SIZE) == (int)FS_BLOCK_SIZE);
    CHECK(fs_write(file, block_pattern, FS_BLOCK_SIZE) == (int)FS_BLOCK_SIZE);
    CHECK(fs_write(file, block_pattern, FS_BLOCK_SIZE) == -1);
    fs_close(file);

    fs_test_set_data_block_limit(0u);
    CHECK(fs_format() == 0);
    return true;
}

static int run_one(const char *name, FsTestFn test) {
    printf("RUN: %s\n", name);
    if (test()) {
        printf("PASS: %s\n", name);
        return 0;
    }

    printf("FAIL: %s\n", name);
    return 1;
}

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
