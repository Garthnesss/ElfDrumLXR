/* Native stand-in for FatFs: the host loads kits itself, so only the types are needed. */
#pragma once
#include <stdint.h>
typedef unsigned int UINT; typedef uint8_t BYTE; typedef uint32_t DWORD; typedef uint16_t WORD; typedef char TCHAR;
typedef enum { FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH, FR_INVALID_NAME, FR_DENIED, FR_EXIST } FRESULT;
typedef struct { int dummy; } FATFS; typedef struct { int dummy; } FIL; typedef struct { int dummy; } DIR;
typedef struct { DWORD fsize; WORD fdate, ftime; BYTE fattrib; TCHAR fname[13]; } FILINFO;
#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_ALWAYS 0x08
#define AM_DIR 0x10
extern FATFS SDFatFS; extern FIL SDFile; extern char SDPath[4];
