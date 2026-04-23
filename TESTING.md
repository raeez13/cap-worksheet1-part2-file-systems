# Filesystem Testing

The firmware includes a serial test runner in `fs_tests.c`.

`RUN_FS_TESTS` is `ON` by default in `CMakeLists.txt`. When it is enabled, `main.c` calls `fs_run_tests()` once after USB serial starts.

## Build

From the repository root:

```sh
export PICO_SDK_PATH=$HOME/pico/pico-sdk
rm -rf build
cmake -S . -B build -DRUN_FS_TESTS=ON
cmake --build build
```

Expected output file:

```text
build/my_blink.uf2
```

If you are inside `build`, the file is:

```text
my_blink.uf2
```

## Flash

Hold BOOTSEL while connecting the Pico. On Linux, copy the UF2 to the `RPI-RP2` drive:

```sh
cp build/my_blink.uf2 /media/$USER/RPI-RP2/
sync
```

On macOS the mount path is usually:

```sh
cp build/my_blink.uf2 /Volumes/RPI-RP2/
```

## Serial

On Linux:

```sh
screen /dev/ttyACM0 115200
```

On macOS:

```sh
screen /dev/cu.usbmodem101 115200
```

The exact macOS device number can change. Check with:

```sh
ls /dev/cu.usbmodem*
```

## Expected Output

The test runner prints one `RUN:` line and one `PASS:` or `FAIL:` line per test.

Expected success pattern:

```text
FS_TEST_BEGIN
PASS: format_mount
PASS: small_write_read
PASS: multi_block_write_read
PASS: seek_overwrite
PASS: append_mode
PASS: truncate_mode
PASS: read_beyond_eof
PASS: invalid_open_mode
PASS: invalid_path
PASS: out_of_space
FS_TEST_RESULT: PASS
FS_TEST_END
```

After the tests finish, the firmware prints:

```text
FS_TEST_IDLE
```

That means the test run is complete and the firmware is in its heartbeat loop.

## Expected Error Messages

Some tests intentionally trigger errors.

These messages are expected:

```text
fs: invalid open mode: q
fs: empty filename
fs: subdirectories are not supported: dir/file.txt
fs: out of data blocks
```

They are acceptable if the final result is:

```text
FS_TEST_RESULT: PASS
```

## Coverage

The suite covers:

- Format and remount.
- Create, write, and read a small file.
- Multi-block write and read with a 10 KiB file.
- Seek and overwrite in the middle of a file.
- Append mode.
- Truncate mode through `w`.
- Read at EOF returning `0`.
- Invalid open mode returning `NULL`.
- Invalid path returning `NULL`.
- Out-of-space handling with a forced 3 block data region.
