// Copyright (c) 2020-present, HexHacking Team. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

// Created by caikelun on 2020-10-04.

#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <android/api-level.h>
#include "xc_dl_util.h"
#include "xc_dl_const.h"
#include "xc_dl.h"

bool xc_dl_util_starts_with(const char *str, const char *start)
{
    while(*str && *str == *start)
    {
        str++;
        start++;
    }

    return '\0' == *start;
}

size_t xc_dl_util_trim_ending(char *start)
{
    char *end = start + strlen(start);
    while(start < end && isspace((int)(*(end - 1))))
    {
        end--;
        *end = '\0';
    }
    return (size_t)(end - start);
}

static int bh_dl_util_get_api_level_from_build_prop(void)
{
    char buf[128];
    int api_level = -1;

    FILE *fp = fopen("/system/build.prop", "r");
    if(NULL == fp) goto end;

    while(fgets(buf, sizeof(buf), fp))
    {
        if(xc_dl_util_starts_with(buf, "ro.build.version.sdk="))
        {
            api_level = atoi(buf + 21);
            break;
        }
    }
    fclose(fp);

 end:
    return (api_level > 0) ? api_level : -1;
}

int xc_dl_util_get_api_level(void)
{
    static int bh_util_api_level = -1;

    if(bh_util_api_level < 0)
    {
        int api_level = android_get_device_api_level();
        if(api_level < 0)
            api_level = bh_dl_util_get_api_level_from_build_prop(); // compatible with unusual models
        if(api_level < __ANDROID_API_J__)
            api_level = __ANDROID_API_J__;

        __atomic_store_n(&bh_util_api_level, api_level, __ATOMIC_SEQ_CST);
    }

    return bh_util_api_level;
}

// LZMA data type definition
#define SZ_OK 0
typedef struct ISzAlloc ISzAlloc;
typedef const ISzAlloc * ISzAllocPtr;
struct ISzAlloc
{
    void *(*Alloc)(ISzAllocPtr p, size_t size);
    void (*Free)(ISzAllocPtr p, void *address); /* address can be 0 */
};
typedef enum
{
    CODER_STATUS_NOT_SPECIFIED,               /* use main error code instead */
    CODER_STATUS_FINISHED_WITH_MARK,          /* stream was finished with end mark. */
    CODER_STATUS_NOT_FINISHED,                /* stream was not finished */
    CODER_STATUS_NEEDS_MORE_INPUT             /* you must provide more input bytes */
} ECoderStatus;
typedef enum
{
    CODER_FINISH_ANY,   /* finish at any point */
    CODER_FINISH_END    /* block must be finished at the end */
} ECoderFinishMode;

// LZMA function type definition
typedef void (*xc_dl_util_lzma_crcgen_t)(void);
typedef void (*xc_dl_util_lzma_crc64gen_t)(void);
typedef void (*xc_dl_util_lzma_construct_t)(void *, ISzAllocPtr);
typedef int  (*xc_dl_util_lzma_isfinished_t)(const void *);
typedef void (*xc_dl_util_lzma_free_t)(void *);
typedef int  (*xc_dl_util_lzma_code_t)(void *, uint8_t *, size_t *, const uint8_t *, size_t *, ECoderFinishMode, ECoderStatus *);
typedef int  (*xc_dl_util_lzma_code_q_t)(void *, uint8_t *, size_t *, const uint8_t *, size_t *, int, ECoderFinishMode, ECoderStatus *);

// LZMA function pointor
static xc_dl_util_lzma_construct_t xc_dl_util_lzma_construct = NULL;
static xc_dl_util_lzma_isfinished_t xc_dl_util_lzma_isfinished = NULL;
static xc_dl_util_lzma_free_t xc_dl_util_lzma_free = NULL;
static void *xc_dl_util_lzma_code = NULL;

// LZMA init
static void xc_dl_util_lzma_init()
{
    xc_dl_t *lzma = xc_dl_open(XC_DL_CONST_PATHNAME_LZMA, XC_DL_DYNSYM);
    if(NULL == lzma) return;

    xc_dl_util_lzma_crcgen_t crcgen = NULL;
    xc_dl_util_lzma_crc64gen_t crc64gen = NULL;
    if(NULL == (crcgen = (xc_dl_util_lzma_crcgen_t)xc_dl_dynsym_func(lzma, XC_DL_CONST_SYM_LZMA_CRCGEN))) goto end;
    if(NULL == (crc64gen = (xc_dl_util_lzma_crc64gen_t)xc_dl_dynsym_func(lzma, XC_DL_CONST_SYM_LZMA_CRC64GEN))) goto end;
    if(NULL == (xc_dl_util_lzma_construct = (xc_dl_util_lzma_construct_t)xc_dl_dynsym_func(lzma, XC_DL_CONST_SYM_LZMA_CONSTRUCT))) goto end;
    if(NULL == (xc_dl_util_lzma_isfinished = (xc_dl_util_lzma_isfinished_t)xc_dl_dynsym_func(lzma, XC_DL_CONST_SYM_LZMA_ISFINISHED))) goto end;
    if(NULL == (xc_dl_util_lzma_free = (xc_dl_util_lzma_free_t)xc_dl_dynsym_func(lzma, XC_DL_CONST_SYM_LZMA_FREE))) goto end;
    if(NULL == (xc_dl_util_lzma_code = xc_dl_dynsym_func(lzma, XC_DL_CONST_SYM_LZMA_CODE))) goto end;
    crcgen();
    crc64gen();

 end:
    xc_dl_close(&lzma);
}

// LZMA internal alloc / free
static void* xc_dl_util_lzma_internal_alloc(ISzAllocPtr p, size_t size)
{
    (void)p;
    return malloc(size);
}
static void xc_dl_util_lzma_internal_free(ISzAllocPtr p, void *address)
{
    (void)p;
    free(address);
}

int xc_dl_util_lzma_decompress(uint8_t *src, size_t src_size, uint8_t **dst, size_t *dst_size)
{
    size_t       src_offset = 0;
    size_t       dst_offset = 0;
    size_t       src_remaining;
    size_t       dst_remaining;
    ISzAlloc     alloc = {.Alloc = xc_dl_util_lzma_internal_alloc, .Free = xc_dl_util_lzma_internal_free};
    uint8_t      state[4096]; //must be enough
    ECoderStatus status;
    int          api_level = xc_dl_util_get_api_level();

    // init and check
    static bool inited = false;
    if(!inited)
    {
        xc_dl_util_lzma_init();
        inited = true;
    }
    if(NULL == xc_dl_util_lzma_code) return -1;

    xc_dl_util_lzma_construct(&state, &alloc);

    *dst_size = 2 * src_size;
    *dst = NULL;
    do
    {
        *dst_size *= 2;
        if(NULL == (*dst = realloc(*dst, *dst_size)))
        {
            xc_dl_util_lzma_free(&state);
            return -1;
        }

        src_remaining = src_size - src_offset;
        dst_remaining = *dst_size - dst_offset;

        int result;
        if(api_level >= __ANDROID_API_Q__)
        {
            xc_dl_util_lzma_code_q_t lzma_code_q = (xc_dl_util_lzma_code_q_t)xc_dl_util_lzma_code;
            result = lzma_code_q(&state, *dst + dst_offset, &dst_remaining, src + src_offset, &src_remaining, 1, CODER_FINISH_ANY, &status);
        }
        else
        {
            xc_dl_util_lzma_code_t lzma_code = (xc_dl_util_lzma_code_t)xc_dl_util_lzma_code;
            result = lzma_code(&state, *dst + dst_offset, &dst_remaining, src + src_offset, &src_remaining, CODER_FINISH_ANY, &status);
        }
        if(SZ_OK != result)
        {
            free(*dst);
            xc_dl_util_lzma_free(&state);
            return -1;
        }

        src_offset += src_remaining;
        dst_offset += dst_remaining;
    } while (status == CODER_STATUS_NOT_FINISHED);

    xc_dl_util_lzma_free(&state);

    if(!xc_dl_util_lzma_isfinished(&state))
    {
        free(*dst);
        return -1;
    }

    *dst_size = dst_offset;
    *dst = realloc(*dst, *dst_size);
    return 0;
}
