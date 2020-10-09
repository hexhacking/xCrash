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

#pragma once

#define XC_DL_DYNSYM 0x01
#define XC_DL_SYMTAB 0x02
#define XC_DL_ALL    (XC_DL_DYNSYM | XC_DL_SYMTAB)

typedef struct xc_dl xc_dl_t;

xc_dl_t *xc_dl_open(const char *pathname, int flags);
void xc_dl_close(xc_dl_t **self);

void *xc_dl_dynsym_func(xc_dl_t *self, const char *sym_name);
void *xc_dl_dynsym_object(xc_dl_t *self, const char *sym_name);

void *xc_dl_symtab_func(xc_dl_t *self, const char *sym_name);
void *xc_dl_symtab_object(xc_dl_t *self, const char *sym_name);
