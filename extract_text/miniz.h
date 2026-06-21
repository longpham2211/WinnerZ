#ifndef MINIZ_H
#define MINIZ_H

#include <stdlib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char mz_uint8;
typedef unsigned short mz_uint16;
typedef unsigned int mz_uint32;
typedef unsigned long mz_ulong;

#define MZ_OK 0
#define MZ_STREAM_END 1
#define MZ_FINISH 4
#define MZ_BUF_ERROR (-5)
#define MZ_DATA_ERROR (-3)
#define MZ_PARAM_ERROR (-10000)


int mz_uncompress(unsigned char *pDest, mz_ulong *pDest_len, const unsigned char *pSource, mz_ulong source_len);

#ifdef __cplusplus
}
#endif

#endif