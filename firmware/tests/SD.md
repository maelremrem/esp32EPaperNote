# SD mounting diagnostics — Waveshare board V2

## Verified wiring

The official Waveshare ESP-IDF/V2 factory source uses SDMMC in one-bit mode:

- `11_FactoryProgram/components/port_bsp/epaper_config.h`: D0 GPIO40,
  CMD GPIO41, CLK GPIO39 (lines 38–40 in the inspected vendor checkout).
- `11_FactoryProgram/components/port_bsp/port_sdcard.cpp`: default SDMMC
  host, slot configuration width 1 and the three pins above (lines 25–33).

The project uses the same wiring in `firmware/include/board_pins.h` and
`NoteStore::init()`. This comparison verifies declarations, not physical card
communication. Do not permute the pins or add an invented SD power GPIO.
Normal mounting must keep `format_if_mount_failed=false`.

## Capture the real failure

Run `pio device monitor -b 115200` from the repository root. Capture startup or
choose Settings → Storage → Status / retry mount. Record the `sdmmc` and
`SD mount failed` messages and the error name. Do not share tokens/passwords.

`NoteStore::lastError()` preserves the failing stage and ESP-IDF error name:
mount, card status, format, directory creation or cleanup. It clears after a
successful mount/format. An uncertain cleanup requires restart; the consumed
card pointer must not be reused. This diagnostic is main-task-owned.

A formatted card is not proof of a working bus. Conversely, a mount failure is
not proof of incorrect pins. Distinguish SD initialization/timeouts from FAT
mount failure using the low-level serial messages. Card contact, bus conditions,
partition layout and filesystem errors remain hypotheses until observed.
FAT32 is supported; an exFAT format must not be silently reformatted. A card
shown as FAT32 by a partition manager still needs the actual firmware error to
identify the problem. Avoid repeated destructive formatting as a diagnostic.

## Actual capacity and pending cancellation

Storage submenu/status and authenticated station Settings use main-owned
`NoteStore::usage()`, backed by the installed ESP-IDF declaration
`esp_vfs_fat_info(const char*, uint64_t*, uint64_t*)`. Total/free are uint64; reject
zero totals, free > total and any API error. Busy/unavailable mounts yield unknown
capacity, not zero-percent usage. The physical 200x200 gauge uses a floating-point
projection to avoid uint64 multiplication overflow, and stays above the footer.
Queries do not run while recorder/preview workers or a download lease are active.

Physical Settings → Sync offers selective pending cancellation and Cancel all.
Back is selected by default. Confirming preserves the original pending WAV by
archiving it only when the destination does not exist. A collision/rename error
retains the pending file. No transcript is fabricated; history/downloads retain
untranscribed audio. Recording finalization also refuses existing pending/archive
IDs rather than unlinking user audio. Cancellation cannot abort a blocking sync HTTP
request already running on main.

Read-only downloads validate safe IDs, regular non-symlink files and complete
generated Markdown metadata on main, then transfer a FILE through a mailbox lease.
The HTTP task streams bounded chunks and closes before releasing exclusion. It
never accesses a card pointer or invokes NoteStore directly. Format/mount/capture/
sync/cancellation/portal launch are excluded while leased. Real card removal and
stack/heap concurrency still require hardware checks.

```
python3 firmware/tests/test_pending_cancellation.py
python3 firmware/tests/test_runtime_token.py
node firmware/tests/web_download_test.mjs
```

## Local regression commands

### Mounted card rejected by directory creation

If serial output prints the card name/capacity, then
`mkdir(/sdcard/audio/recording) failed: Invalid argument`, do not reformat or
change pins. FatFs with `CONFIG_FATFS_LFN_NONE=y` rejects `recording` because
it exceeds the eight-character base-name limit. Long note IDs and `index.jsonl`
also require long filenames. Enable `CONFIG_FATFS_LFN_HEAP=y` and
`CONFIG_FATFS_MAX_LFN=255` in both the defaults and existing active sdkconfig;
defaults alone do not override an already-generated configuration. Preserve
the directory layout and existing notes. Rebuild and upload with local credentials.

`test_fatfs_paths.py` compiles the installed ESP-IDF FatFs implementation against
a disposable RAM FAT32 disk and single-threaded OS adapters. It reproduces
`FR_INVALID_NAME` with LFN disabled, then verifies production directory paths,
long WAV names, index creation and data preservation across remount with heap
LFN enabled. It never accesses or formats a physical card.

```
python3 firmware/tests/test_fatfs_paths.py
python3 firmware/tests/test_boot_storage_mount.py
python3 firmware/tests/run_note_history_tests.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 firmware/tests/test_boot_storage_mount.py
```

These execute the production NoteStore with fake IDF mounting and temporary
filesystem fixtures. They verify diagnostics, non-destructive retry, pointer
lifetime and format failure handling; they do not verify a real SD card.
