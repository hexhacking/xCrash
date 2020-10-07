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

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <ctype.h>
#include <elf.h>
#include <link.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <android/api-level.h>
#include "xc_dl_iterator.h"
#include "xc_dl.h"
#include "xc_dl_util.h"
#include "xc_dl_const.h"

/*
 * ============================================================================
 * API-LEVEL  ANDROID-VERSION  ARCH    SOLUTION
 * ============================================================================
 * 16         4.1              arm32   read /proc/self/maps
 *                             others  dl_iterate_phdr()
 * 17         4.2              arm32   read /proc/self/maps
 *                             others  dl_iterate_phdr()
 * 18         4.3              arm32   read /proc/self/maps
 *                             others  dl_iterate_phdr()
 * 19         4.4              arm32   read /proc/self/maps
 *                             others  dl_iterate_phdr()
 * 20         4.4W             arm32   read /proc/self/maps
 *                             others  dl_iterate_phdr()
 * ----------------------------------------------------------------------------
 * 21         5.0              all     dl_iterate_phdr() + __dl__ZL10g_dl_mutex
 * 22         5.1              all     dl_iterate_phdr() + __dl__ZL10g_dl_mutex
 * ----------------------------------------------------------------------------
 * >= 23      >= 6.0           all     dl_iterate_phdr()
 * ============================================================================
 */

extern __attribute((weak)) int dl_iterate_phdr(int (*)(struct dl_phdr_info *, size_t, void *), void *);

// Android 5.0/5.1 linker's global mutex in .symtab
static pthread_mutex_t *xc_dl_iterator_linker_mutex = NULL;

static void xc_dl_iterator_linker_mutex_init()
{
    xc_dl_t *linker = xc_dl_open(XC_DL_CONST_PATHNAME_LINKER, XC_DL_SYMTAB);
    if(NULL == linker) return;

    xc_dl_iterator_linker_mutex = xc_dl_symtab_object(linker, XC_DL_CONST_SYM_LINKER_MUTEX);

    xc_dl_close(&linker);
}

static uintptr_t xc_dl_iterator_get_min_vaddr(struct dl_phdr_info *info)
{
    uintptr_t min_vaddr = UINTPTR_MAX;
    for(size_t i = 0; i < info->dlpi_phnum; i++)
    {
        const ElfW(Phdr) *phdr = &(info->dlpi_phdr[i]);
        if(PT_LOAD == phdr->p_type)
        {
            if(min_vaddr > phdr->p_vaddr) min_vaddr = phdr->p_vaddr;
        }
    }
    return min_vaddr;
}

static int xc_dl_iterator_open_or_rewind_maps(FILE **maps)
{
    if(NULL == *maps)
    {
        *maps = fopen("/proc/self/maps", "r");
        if(NULL == *maps) return -1;
    }
    else
    {
        // seek to the beginning of the maps
        rewind(*maps);
    }

    return 0;
}

static uintptr_t xc_dl_iterator_get_pathname_from_maps(struct dl_phdr_info *info, char *buf, size_t buf_len, FILE **maps)
{
    // get base address
    uintptr_t min_vaddr = xc_dl_iterator_get_min_vaddr(info);
    if(UINTPTR_MAX == min_vaddr) return 0; // failed
    uintptr_t base = (uintptr_t)(info->dlpi_addr + min_vaddr);

    // open or rewind maps-file
    if(0 != xc_dl_iterator_open_or_rewind_maps(maps)) return 0; // failed

    char line[1024];
    while(fgets(line, sizeof(line), *maps))
    {
        // check base address
        uintptr_t start, end;
        if(2 != sscanf(line, "%"SCNxPTR"-%"SCNxPTR" r", &start, &end)) continue;
        if(base < start) break; // failed
        if(base >= end) continue;

        // get pathname
        char *pathname = strchr(line, '/');
        if(NULL == pathname) break; // failed
        xc_dl_util_trim_ending(pathname);

        // found it
        strlcpy(buf, pathname, buf_len);
        return (uintptr_t)buf; // OK
    }

    return 0; // failed
}

static int xc_dl_iterator_internal_cb(struct dl_phdr_info *info, size_t size, void *arg)
{
    uintptr_t *pkg = (uintptr_t *)arg;
    xc_dl_iterator_cb_t cb = (xc_dl_iterator_cb_t)*pkg++;
    void *cb_arg = (void *)*pkg++;
    FILE **maps = (FILE **)*pkg;

    if(NULL == info->dlpi_name || '\0' == info->dlpi_name[0]) return 0; // ignore this ELF

    if('/' != info->dlpi_name[0] && '[' != info->dlpi_name[0])
    {
        // get pathname from /proc/self/maps
        char buf[512];
        uintptr_t pathname = xc_dl_iterator_get_pathname_from_maps(info, buf, sizeof(buf), maps);
        if(0 == pathname) return 0; // ignore this ELF

        // callback
        struct dl_phdr_info info_fixed;
        info_fixed.dlpi_addr = info->dlpi_addr;
        info_fixed.dlpi_name = (const char *)pathname;
        info_fixed.dlpi_phdr = info->dlpi_phdr;
        info_fixed.dlpi_phnum = info->dlpi_phnum;
        return cb(&info_fixed, size, cb_arg);
    }
    else
    {
        // callback
        return cb(info, size, cb_arg);
    }
}

static uintptr_t xc_dl_iterator_find_linker_base(FILE **maps)
{
    // open or rewind maps-file
    if(0 != xc_dl_iterator_open_or_rewind_maps(maps)) return 0; // failed

    size_t linker_pathname_len = strlen(" "XC_DL_CONST_PATHNAME_LINKER);

    char line[1024];
    while(fgets(line, sizeof(line), *maps))
    {
        // check pathname
        size_t line_len = xc_dl_util_trim_ending(line);
        if(line_len < linker_pathname_len)continue;
        if(0 != memcmp(line + line_len - linker_pathname_len, " "XC_DL_CONST_PATHNAME_LINKER, linker_pathname_len)) continue;

        // get base address
        uintptr_t base, offset;
        if(2 != sscanf(line, "%"SCNxPTR"-%*"SCNxPTR" r%*2sp %"SCNxPTR" ", &base, &offset)) continue;
        if(0 != offset) continue;
        if(0 != memcmp((void *)base, ELFMAG, SELFMAG)) continue;

        // find it
        return base;
    }

    return 0;
}

static int xc_dl_iterator_callback(xc_dl_iterator_cb_t cb, void *cb_arg, uintptr_t base, const char *pathname)
{
    ElfW(Ehdr) *ehdr = (ElfW(Ehdr) *)base;

    struct dl_phdr_info info;
    info.dlpi_name = pathname;
    info.dlpi_phdr = (const ElfW(Phdr) *)(base + ehdr->e_phoff);
    info.dlpi_phnum = ehdr->e_phnum;

    // get load bias
    uintptr_t min_vaddr = xc_dl_iterator_get_min_vaddr(&info);
    if(UINTPTR_MAX == min_vaddr) return 0; // ignore invalid ELF
    info.dlpi_addr = (ElfW(Addr))(base - min_vaddr);

    return cb(&info, sizeof(struct dl_phdr_info), cb_arg);
}

static int xc_dl_iterator_iterate_by_linker(xc_dl_iterator_cb_t cb, void *cb_arg, int flags)
{
    if(NULL == dl_iterate_phdr) return -1;

    FILE *maps = NULL;

    // for linker/linker64 in Android version < 8.1 (API level 27)
    if((flags & XC_DL_WITH_LINKER) && xc_dl_util_get_api_level() < __ANDROID_API_O_MR1__)
    {
        uintptr_t base = xc_dl_iterator_find_linker_base(&maps);
        if(0 != base)
        {
            if(0 != xc_dl_iterator_callback(cb, cb_arg, base, XC_DL_CONST_PATHNAME_LINKER)) return 0;
        }
    }

    // for others
    uintptr_t pkg[3] = {(uintptr_t)cb, (uintptr_t)cb_arg, (uintptr_t)&maps};
    if(NULL != xc_dl_iterator_linker_mutex) pthread_mutex_lock(xc_dl_iterator_linker_mutex);
    dl_iterate_phdr(xc_dl_iterator_internal_cb, pkg);
    if(NULL != xc_dl_iterator_linker_mutex) pthread_mutex_unlock(xc_dl_iterator_linker_mutex);

    if(NULL != maps) fclose(maps);
    return 0;
}

#if defined(__arm__)
static int xc_dl_iterator_iterate_by_maps(xc_dl_iterator_cb_t cb, void *cb_arg)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    if(NULL == maps) return 0;

    char line[1024];
    while(fgets(line, sizeof(line), maps))
    {
        // Try to find an ELF which loaded by linker. This is almost always correct in android 4.x.
        uintptr_t base, offset;
        if(2 != sscanf(line, "%"SCNxPTR"-%*"SCNxPTR" r-xp %"SCNxPTR" ", &base, &offset)) continue;
        if(0 != offset) continue;
        if(0 != memcmp((void *)base, ELFMAG, SELFMAG)) continue;

        // get pathname
        char *pathname = strchr(line, '/');
        if(NULL == pathname) break;
        xc_dl_util_trim_ending(pathname);

        // callback
        if(0 != xc_dl_iterator_callback(cb, cb_arg, base, pathname)) break;
    }

    fclose(maps);
    return 0;
}
#endif

int xc_dl_iterator_iterate(xc_dl_iterator_cb_t cb, void *cb_arg, int flags)
{
    int api_level = xc_dl_util_get_api_level();

    // get linker's __dl__ZL10g_dl_mutex for Android 5.0/5.1
    static bool linker_mutex_inited = false;
    if(__ANDROID_API_L__ == api_level || __ANDROID_API_L_MR1__ == api_level)
    {
        if(!linker_mutex_inited)
        {
            linker_mutex_inited = true;
            xc_dl_iterator_linker_mutex_init();
        }
    }

    // iterate by /proc/self/maps in Android 4.x on arm32 arch
#if defined(__arm__)
    if(api_level < __ANDROID_API_L__)
        return xc_dl_iterator_iterate_by_maps(cb, cb_arg);
#endif

    // iterate by dl_iterate_phdr()
    return xc_dl_iterator_iterate_by_linker(cb, cb_arg, flags);
}
