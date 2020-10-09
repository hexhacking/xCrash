# xCrash DL

## The Problem

All the time, Android's DL series functions have some compatibility and ability issues:

### dl\_iterate\_phdr()

1. On arm32 arch, only available when Android >= 5.0.
2. It does not hold the linker's global lock in Android 5.x and 4.x, this may cause a crash during iterating.
3. In some Android 4.x and 5.x devices, it returns basename instead of full pathname.
4. In some Android 4.x and 5.x devices, it returns package name for app_process instead of /system/bin/app_process32 or /system/bin/app_process64.
5. linker/linker64 is only included since Android 8.1. (AOSP has included linker/linker64 since 8.0, but a large number of devices from other manufacturers have included linker/linker64 since Android 8.1)

### dlopen() & dlsym()

1. Since Android 7.0, dlopen() and dlsym() cannot operate system libraries. (Although in most cases, we don’t really need to load the dynamic library from the disk, but just need to get the address of a function to call it.)
2. dlsym() can only obtain symbols in .dynsym, but we sometimes need to obtain internal symbols in .symtab and ".symtab in .gnu_debugdata".
3. dlsym() does not distinguish between functions and objects when searching for symbols. In ELF files with a lot of symbols, this will reduce search efficiency, especially for .symtab.

## The Solution

xCrash DL tries to make up the above problems. Enjoy the code ~

In addition, the source code of this module (in the current directory) does not depend on any other source code of xCrash, so you can easily use them in your own projects.

## The History

xCrash 2.x contains a very rudimentary module "xc_dl" for searching system library symbols, which has many problems in performance and compatibility. xCrash uses it to search a few symbols from libart, libc and libc++.

Later, some other projects began to use the xc_dl module alone, including in some performance-sensitive usage scenarios. At this time, we began to realize that we need to rewrite this module, and we need a better implementation.
