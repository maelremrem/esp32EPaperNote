/* Real IDF FatFs with only the disk, allocation, time and mutex adapters mocked. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"
#include "paths.h"

#define SECTORS 131072
#define SECTOR_SIZE 512
static unsigned char *disk;
const PARTITION VolToPart[FF_VOLUMES] = {{0, 0}};
DSTATUS disk_initialize(BYTE drive) { return drive ? STA_NODISK : 0; }
DSTATUS disk_status(BYTE drive) { return drive ? STA_NODISK : 0; }
DRESULT disk_read(BYTE drive, BYTE *data, LBA_t sector, UINT count) {
    if (drive || sector >= SECTORS || count > SECTORS - sector) return RES_PARERR;
    memcpy(data, disk + sector * SECTOR_SIZE, count * SECTOR_SIZE);
    return RES_OK;
}
DRESULT disk_write(BYTE drive, const BYTE *data, LBA_t sector, UINT count) {
    if (drive || sector >= SECTORS || count > SECTORS - sector) return RES_PARERR;
    memcpy(disk + sector * SECTOR_SIZE, data, count * SECTOR_SIZE);
    return RES_OK;
}
DRESULT disk_ioctl(BYTE drive, BYTE command, void *data) {
    if (drive) return RES_PARERR;
    switch (command) {
    case CTRL_SYNC: case CTRL_TRIM: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)data = SECTORS; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)data = SECTOR_SIZE; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)data = 1; return RES_OK;
    default: return RES_PARERR;
    }
}
void *ff_memalloc(unsigned size) { return malloc(size); }
void ff_memfree(void *memory) { free(memory); }
DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (10 << 21) | (7 << 16); }
int ff_mutex_create(int volume) { (void)volume; return 1; }
void ff_mutex_delete(int volume) { (void)volume; }
int ff_mutex_take(int volume) { (void)volume; return 1; }
void ff_mutex_give(int volume) { (void)volume; }

int main(int argc, char **argv) {
    assert(argc == 2);
    disk = calloc(SECTORS, SECTOR_SIZE);
    assert(disk);
    FATFS fs;
    unsigned char work[4096];
    MKFS_PARM parameters = {.fmt = FM_FAT32 | FM_SFD, .au_size = SECTOR_SIZE};
    assert(f_mkfs("0:", &parameters, work, sizeof(work)) == FR_OK);
    assert(f_mount(&fs, "0:", 1) == FR_OK);
    assert(fs.fs_type == FS_FAT32);
    assert(f_mkdir("0:/audio") == FR_OK);
    FRESULT result = f_mkdir(RECORDING_DIR);
    if (!strcmp(argv[1], "none")) {
        assert(result == FR_INVALID_NAME);
        puts("Reproduced: recording -> FR_INVALID_NAME with LFN disabled");
    } else {
        assert(result == FR_OK);
        assert(f_mkdir(PENDING_DIR) == FR_OK);
        assert(f_mkdir(ARCHIVE_DIR) == FR_OK);
        assert(f_mkdir(NOTES_DIR) == FR_OK);
        const char *path = RECORDING_DIR "/20261007_123456_123456789.wav";
        const char content[] = "preserve existing note";
        FIL file;
        UINT transferred;
        assert(f_open(&file, path, FA_CREATE_NEW | FA_WRITE) == FR_OK);
        assert(f_write(&file, content, sizeof(content), &transferred) == FR_OK);
        assert(transferred == sizeof(content));
        assert(f_close(&file) == FR_OK);
        assert(f_open(&file, INDEX_FILE, FA_CREATE_NEW | FA_WRITE) == FR_OK);
        assert(f_close(&file) == FR_OK);
        assert(f_mount(NULL, "0:", 0) == FR_OK);
        assert(f_mount(&fs, "0:", 1) == FR_OK);
        assert(f_mkdir(RECORDING_DIR) == FR_EXIST);
        FILINFO info;
        assert(f_stat(INDEX_FILE, &info) == FR_OK);
        assert(f_open(&file, path, FA_READ) == FR_OK);
        char readback[sizeof(content)];
        assert(f_read(&file, readback, sizeof(readback), &transferred) == FR_OK);
        assert(transferred == sizeof(content));
        assert(!memcmp(readback, content, sizeof(content)));
        assert(f_close(&file) == FR_OK);
        puts("LFN heap: FAT32 recording + long WAV filename survive remount");
    }
    assert(f_mount(NULL, "0:", 0) == FR_OK);
    free(disk);
    return 0;
}
