#pragma once

#if defined(_WIN32) && !defined(__CYGWIN__)
#include "contrib/android/config/windows_host.h"
#else
#include "contrib/android/config/linux_host.h"
#endif

#define __LIBARCHIVE_CONFIG_H_INCLUDED 1

#if !defined(__linux__)
#undef HAVE_LINUX_FIEMAP_H
#undef HAVE_LINUX_FS_H
#undef HAVE_LINUX_MAGIC_H
#undef HAVE_LINUX_TYPES_H
#undef HAVE_SYS_STATFS_H
#undef HAVE_SYS_VFS_H
#endif

#if defined(__APPLE__)
#undef HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC
#define HAVE_STRUCT_STAT_ST_MTIMESPEC_TV_NSEC 1
#undef HAVE_FUTIMESAT
#undef HAVE_ICONV
#undef HAVE_ICONV_H
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <stddef.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "xmllite.lib")
#undef HAVE_UNISTD_H
#undef HAVE_DECL_SSIZE_MAX
#define HAVE_DECL_SSIZE_MAX 0
#define HAVE__GET_TIMEZONE 1
typedef ptrdiff_t ssize_t;
typedef int pid_t;
typedef unsigned short mode_t;
#ifndef PROV_RSA_AES
#define PROV_RSA_AES 24
#endif
#ifndef CALG_SHA_256
#define CALG_SHA_256 0x0000800c
#endif
#ifndef CALG_SHA_384
#define CALG_SHA_384 0x0000800d
#endif
#ifndef CALG_SHA_512
#define CALG_SHA_512 0x0000800e
#endif
#endif

#undef HAVE_LOCALCHARSET_H
#define HAVE_LOCALCHARSET_H 1

#undef HAVE_LOCALE_CHARSET
#define HAVE_LOCALE_CHARSET 1

#undef HAVE_CLOSEFROM
#define HAVE_CLOSEFROM 0

#undef HAVE_CLOSE_RANGE
#define HAVE_CLOSE_RANGE 0

#ifndef HAVE_DECL_INT32_MAX
#define HAVE_DECL_INT32_MAX 1
#endif

#ifndef HAVE_DECL_INT32_MIN
#define HAVE_DECL_INT32_MIN 1
#endif

#ifndef HAVE_DECL_UINTMAX_MAX
#define HAVE_DECL_UINTMAX_MAX 1
#endif

#ifndef HAVE_DECL_INTMAX_MAX
#define HAVE_DECL_INTMAX_MAX 1
#endif

#ifndef HAVE_DECL_INTMAX_MIN
#define HAVE_DECL_INTMAX_MIN 1
#endif
