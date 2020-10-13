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

#include <inttypes.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <link.h>
#include <elf.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <android/api-level.h>
#include "xc_dl.h"
#include "xc_dl_iterate.h"
#include "xc_dl_util.h"
#include "xc_dl_const.h"

#define XC_DL_DYNSYM_IS_EXPORT_SYM(shndx) (SHN_UNDEF != (shndx))
#define XC_DL_SYMTAB_IS_EXPORT_SYM(shndx) (SHN_UNDEF != (shndx) && !((shndx) >= SHN_LORESERVE && (shndx) <= SHN_HIRESERVE))

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpadded"

struct xc_dl
{
    int       flags;
    uintptr_t load_bias;

    //
    // (1) for searching symbols from .dynsym
    //

    ElfW(Sym)  *dynsym;  // .dynsym
    const char *dynstr;  // .dynstr

    // .hash (SYSV hash for .dynstr)
    struct
    {
        const uint32_t *buckets;
        uint32_t        buckets_cnt;
        const uint32_t *chains;
        uint32_t        chains_cnt;
    } sysv_hash;

    // .gnu.hash (GNU hash for .dynstr)
    struct
    {
        const uint32_t   *buckets;
        uint32_t          buckets_cnt;
        const uint32_t   *chains;
        uint32_t          symoffset;
        const ElfW(Addr) *bloom;
        uint32_t          bloom_cnt;
        uint32_t          bloom_shift;
    } gnu_hash;

    //
    // (2) for searching symbols from .symtab
    //

    uintptr_t  base;

    int        file_fd;
    uint8_t   *file;
    size_t     file_sz;

    uint8_t   *debugdata; // decompressed .gnu_debugdata
    size_t     debugdata_sz;

    ElfW(Sym) *symtab;  // .symtab
    size_t     symtab_cnt;
    char      *strtab;  // .strtab
    size_t     strtab_sz;
};

#pragma clang diagnostic pop

// load from memory
static int xc_dl_dynsym_load(xc_dl_t *self, struct dl_phdr_info *info)
{
    // find the dynamic segment
    ElfW(Dyn) *dynamic = NULL;
    for(size_t i = 0; i < info->dlpi_phnum; i++)
    {
        const ElfW(Phdr) *phdr = &(info->dlpi_phdr[i]);
        if(PT_DYNAMIC == phdr->p_type)
        {
            dynamic = (ElfW(Dyn) *)(self->load_bias + phdr->p_vaddr);
            break;
        }
    }
    if(NULL == dynamic) return -1;

    // iterate the dynamic segment
    for(ElfW(Dyn) * entry = dynamic; entry && entry->d_tag != DT_NULL; entry++)
    {
        switch (entry->d_tag)
        {
            case DT_SYMTAB: //.dynsym
                self->dynsym = (ElfW(Sym) *)(self->load_bias + entry->d_un.d_ptr);
                break;
            case DT_STRTAB: //.dynstr
                self->dynstr = (const char *)(self->load_bias + entry->d_un.d_ptr);
                break;
            case DT_HASH: //.hash
                self->sysv_hash.buckets_cnt = ((const uint32_t *)(self->load_bias + entry->d_un.d_ptr))[0];
                self->sysv_hash.chains_cnt = ((const uint32_t *)(self->load_bias + entry->d_un.d_ptr))[1];
                self->sysv_hash.buckets = &(((const uint32_t *)(self->load_bias + entry->d_un.d_ptr))[2]);
                self->sysv_hash.chains = &(self->sysv_hash.buckets[self->sysv_hash.buckets_cnt]);
                break;
            case DT_GNU_HASH: //.gnu.hash
                self->gnu_hash.buckets_cnt = ((const uint32_t *)(self->load_bias + entry->d_un.d_ptr))[0];
                self->gnu_hash.symoffset = ((const uint32_t *)(self->load_bias + entry->d_un.d_ptr))[1];
                self->gnu_hash.bloom_cnt = ((const uint32_t *)(self->load_bias + entry->d_un.d_ptr))[2];
                self->gnu_hash.bloom_shift = ((const uint32_t *)(self->load_bias + entry->d_un.d_ptr))[3];
                self->gnu_hash.bloom = (const ElfW(Addr) *)(self->load_bias + entry->d_un.d_ptr + 16);
                self->gnu_hash.buckets = (const uint32_t *)(&(self->gnu_hash.bloom[self->gnu_hash.bloom_cnt]));
                self->gnu_hash.chains = (const uint32_t *)(&(self->gnu_hash.buckets[self->gnu_hash.buckets_cnt]));
            default:
                break;
        }
    }
    if(NULL == self->dynsym || NULL == self->dynstr ||
       (0 == self->sysv_hash.buckets_cnt && 0 == self->gnu_hash.buckets_cnt)) return -1;

    return 0;
}

// load from disk and memory
static int xc_dl_symtab_load(xc_dl_t *self, struct dl_phdr_info *info, ElfW(Shdr) *shdr_debugdata)
{
    ElfW(Ehdr) *ehdr;
    uint8_t *elf;
    size_t elf_sz;

    if(NULL == shdr_debugdata)
    {
        // get base address
        uintptr_t vaddr_min = UINTPTR_MAX;
        for(size_t i = 0; i < info->dlpi_phnum; i++)
        {
            const ElfW(Phdr) *phdr = &(info->dlpi_phdr[i]);
            if(PT_LOAD == phdr->p_type)
            {
                if(vaddr_min > phdr->p_vaddr) vaddr_min = phdr->p_vaddr;
            }
        }
        if(UINTPTR_MAX == vaddr_min) return -1;
        self->base = self->load_bias + vaddr_min;

        // open file
        if(0 > (self->file_fd = open(info->dlpi_name, O_RDONLY | O_CLOEXEC))) return -1;

        // get file size
        struct stat st;
        if(0 != fstat(self->file_fd, &st)) return -1;
        self->file_sz = (size_t)st.st_size;

        // mmap file
        if(MAP_FAILED == (self->file = (uint8_t *)mmap(NULL, self->file_sz, PROT_READ, MAP_PRIVATE, self->file_fd, 0))) return -1;

        // for ELF parsing
        ehdr = (ElfW(Ehdr) *)self->base;
        elf = self->file;
        elf_sz = self->file_sz;
    }
    else
    {
        // decompress the .gnu_debugdata section
        if(0 != xc_dl_util_lzma_decompress(self->file + shdr_debugdata->sh_offset, shdr_debugdata->sh_size, &self->debugdata, &self->debugdata_sz)) return -1;

        // for ELF parsing
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-align"
        ehdr = (ElfW(Ehdr) *)self->debugdata;
#pragma clang diagnostic pop
        elf = self->debugdata;
        elf_sz = self->debugdata_sz;
    }

    // check ELF size
    if(0 == ehdr->e_shnum) return -1;
    if(elf_sz < ehdr->e_shoff + ehdr->e_shentsize * ehdr->e_shnum) return -1;

    // get .shstrtab
    if(SHN_UNDEF == ehdr->e_shstrndx) return -1;
    ElfW(Shdr) *shdr_shstrtab = (ElfW(Shdr) *)((uintptr_t)elf + ehdr->e_shoff + ehdr->e_shstrndx * ehdr->e_shentsize);
    char *shstrtab = (char *)((uintptr_t)elf + shdr_shstrtab->sh_offset);

    for(size_t i = 0; i < ehdr->e_shnum; i++)
    {
        ElfW(Shdr) *shdr = (ElfW(Shdr) *)((uintptr_t)elf + ehdr->e_shoff + i * ehdr->e_shentsize);

        if(SHT_SYMTAB == shdr->sh_type && 0 == strcmp(".symtab", shstrtab + shdr->sh_name))
        {
            // get and check .strtab
            if(shdr->sh_link >= ehdr->e_shnum) continue;
            ElfW(Shdr) *shdr_strtab = (ElfW(Shdr) *)((uintptr_t)elf + ehdr->e_shoff + shdr->sh_link * ehdr->e_shentsize);
            if(SHT_STRTAB != shdr_strtab->sh_type) continue;

            // found the .symtab and .strtab
            self->symtab     = (ElfW(Sym) *)((uintptr_t)elf + shdr->sh_offset);
            self->symtab_cnt = shdr->sh_size / shdr->sh_entsize;
            self->strtab     = (char *)((uintptr_t)elf + shdr_strtab->sh_offset);
            self->strtab_sz  = shdr_strtab->sh_size;
            return 0; // OK
        }
        else if(SHT_PROGBITS == shdr->sh_type && 0 == strcmp(".gnu_debugdata", shstrtab + shdr->sh_name) && NULL == shdr_debugdata)
        {
            return xc_dl_symtab_load(self, info, shdr);
        }
    }

    return -1; // not found
}

static int xc_dl_iterate_cb(struct dl_phdr_info *info, size_t size, void *arg)
{
    (void)size;

    uintptr_t *pkg = (uintptr_t *)arg;
    xc_dl_t **self = (xc_dl_t **)*pkg++;
    const char *pathname = (const char *)*pkg++;
    int flags = (int)*pkg;

    // check load_bias and pathname
    if(0 == info->dlpi_addr || NULL == info->dlpi_name) return 0;
    if('/' == pathname[0] || '[' == pathname[0])
    {
        // full pathname
        if(0 != strcmp(info->dlpi_name, pathname)) return 0;
    }
    else
    {
        // basename ?
        size_t basename_len = strlen(pathname);
        size_t pathname_len = strlen(info->dlpi_name);
        if(1 + basename_len > pathname_len) return 0;
        if(0 != strcmp(info->dlpi_name + (pathname_len - basename_len), pathname)) return 0;
        if('/' != *(info->dlpi_name + (pathname_len - basename_len) - 1)) return 0;
    }

    // found the target ELF
    if(NULL == ((*self) = calloc(1, sizeof(xc_dl_t)))) return 1;
    (*self)->flags = flags;
    (*self)->load_bias = info->dlpi_addr;
    (*self)->file_fd = -1;
    (*self)->file = MAP_FAILED;

    // load info about .dynsym
    if(0 != (flags & XC_DL_DYNSYM))
        if(0 != xc_dl_dynsym_load(*self, info)) goto err;

    // load info about .symtab
    if(0 != (flags & XC_DL_SYMTAB))
        if(0 != xc_dl_symtab_load(*self, info, NULL)) goto err;

    return 1;

 err:
    xc_dl_close(self);
    return 1;
}

xc_dl_t *xc_dl_open(const char *pathname, int flags)
{
    if(NULL == pathname || 0 == flags) return NULL;

    xc_dl_t *self = NULL;
    uintptr_t pkg[3] = {(uintptr_t)&self, (uintptr_t)pathname, (uintptr_t)flags};

    bool is_linker = xc_dl_util_ends_with(pathname, XC_DL_CONST_BASENAME_LINKER);
    xc_dl_iterate(xc_dl_iterate_cb, pkg, is_linker ? (int)XC_DL_WITH_LINKER : (int)XC_DL_DEFAULT);

    return self;
}

void xc_dl_close(xc_dl_t **self)
{
    if(NULL == self || NULL == *self) return;

    if(MAP_FAILED != (*self)->file) munmap((*self)->file, (*self)->file_sz);
    if((*self)->file_fd >= 0) close((*self)->file_fd);
    if(NULL != (*self)->debugdata) free((*self)->debugdata);
    free(*self);
    *self = NULL;
}

static uint32_t xc_dl_sysv_hash(const uint8_t *name)
{
    uint32_t h = 0, g;

    while(*name)
    {
        h = (h << 4) + *name++;
        g = h & 0xf0000000;
        h ^= g;
        h ^= g >> 24;
    }
    return h;
}

static uint32_t xc_dl_gnu_hash(const uint8_t *name)
{
    uint32_t h = 5381;

    while(*name)
    {
        h += (h << 5) + *name++;
    }
    return h;
}

static ElfW(Sym) *xc_dl_dynsym_find_symbol_use_sysv_hash(xc_dl_t *self, const char* sym_name, bool need_func)
{
    uint32_t hash = xc_dl_sysv_hash((const uint8_t *)sym_name);

    for(uint32_t i = self->sysv_hash.buckets[hash % self->sysv_hash.buckets_cnt]; 0 != i; i = self->sysv_hash.chains[i])
    {
        ElfW(Sym) *sym = self->dynsym + i;
        if((need_func ? STT_FUNC : STT_OBJECT) != ELF_ST_TYPE(sym->st_info)) continue;
        if(0 != strcmp(self->dynstr + sym->st_name, sym_name)) continue;
        return sym;
    }

    return NULL;
}

static ElfW(Sym) *xc_dl_dynsym_find_symbol_use_gnu_hash(xc_dl_t *self, const char* sym_name, bool need_func)
{
    uint32_t hash = xc_dl_gnu_hash((const uint8_t *)sym_name);

    static uint32_t elfclass_bits = sizeof(ElfW(Addr)) * 8;
    size_t word = self->gnu_hash.bloom[(hash / elfclass_bits) % self->gnu_hash.bloom_cnt];
    size_t mask = 0 | (size_t)1 << (hash % elfclass_bits)
                  | (size_t)1 << ((hash >> self->gnu_hash.bloom_shift) % elfclass_bits);

    //if at least one bit is not set, this symbol is surely missing
    if((word & mask) != mask) return NULL;

    //ignore STN_UNDEF
    uint32_t i = self->gnu_hash.buckets[hash % self->gnu_hash.buckets_cnt];
    if(i < self->gnu_hash.symoffset) return NULL;

    //loop through the chain
    while(1)
    {
        ElfW(Sym) *sym = self->dynsym + i;
        uint32_t sym_hash = self->gnu_hash.chains[i - self->gnu_hash.symoffset];

        if((hash | (uint32_t)1) == (sym_hash | (uint32_t)1) &&
            (need_func ? STT_FUNC : STT_OBJECT) == ELF_ST_TYPE(sym->st_info))
        {
            if(0 == strcmp(self->dynstr + sym->st_name, sym_name))
            {
                return sym;
            }
        }

        //chain ends with an element with the lowest bit set to 1
        if(sym_hash & (uint32_t)1) break;

        i++;
    }

    return NULL;
}

static void *xc_dl_dynsym(xc_dl_t *self, const char *sym_name, bool need_func)
{
    if(0 == (self->flags & XC_DL_DYNSYM)) return NULL;

    ElfW(Sym) *sym = NULL;
    if(self->gnu_hash.buckets_cnt > 0)
    {
        // use GNU hash (.gnu.hash -> .dynsym -> .dynstr), O(x) + O(1) + O(1)
        sym = xc_dl_dynsym_find_symbol_use_gnu_hash(self, sym_name, need_func);
    }
    if(NULL == sym && self->sysv_hash.buckets_cnt > 0)
    {
        // use SYSV hash (.hash -> .dynsym -> .dynstr), O(x) + O(1) + O(1)
        sym = xc_dl_dynsym_find_symbol_use_sysv_hash(self, sym_name, need_func);
    }
    if(NULL == sym || !XC_DL_DYNSYM_IS_EXPORT_SYM(sym->st_shndx)) return NULL;

    return (void *)(self->load_bias + sym->st_value);
}

void *xc_dl_dynsym_func(xc_dl_t *self, const char *sym_name)
{
    return xc_dl_dynsym(self, sym_name, true);
}

void *xc_dl_dynsym_object(xc_dl_t *self, const char *sym_name)
{
    return xc_dl_dynsym(self, sym_name, false);
}

static void *xc_dl_symtab(xc_dl_t *self, const char *sym_name, bool need_func)
{
    if(0 == (self->flags & XC_DL_SYMTAB)) return NULL;

    for(size_t i = 0; i < self->symtab_cnt; i++)
    {
        ElfW(Sym) *sym = self->symtab + i;

        if(!XC_DL_SYMTAB_IS_EXPORT_SYM(sym->st_shndx)) continue;
        if((need_func ? STT_FUNC : STT_OBJECT) != ELF_ST_TYPE(sym->st_info)) continue;
        if(0 != strncmp(self->strtab + sym->st_name, sym_name, self->strtab_sz - sym->st_name)) continue;

        return (void *)(self->load_bias + sym->st_value);
    }

    return NULL;
}

void *xc_dl_symtab_func(xc_dl_t *self, const char *sym_name)
{
    return xc_dl_symtab(self, sym_name, true);
}

void *xc_dl_symtab_object(xc_dl_t *self, const char *sym_name)
{
    return xc_dl_symtab(self, sym_name, false);
}
