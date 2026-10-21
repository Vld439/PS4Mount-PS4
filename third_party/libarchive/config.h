/* PS4Mount's configuration of libarchive 3.8.10 (third_party/libarchive holds only the
 * files it needs): reading RAR (4 and 5, in volumes) and ZIP, nothing written. The same on
 * the console (FreeBSD's libc, built with -fno-short-wchar so L"" matches its 32-bit
 * wchar_t) and in the host tests (glibc). Memory goes through src/la_alloc.h. Changes of
 * ours, marked PS4Mount: in archive_read_support_format_rar5.c, the padding after a file's
 * last block (an encrypted RAR5 made plain by src/rar5dec.c) is skipped; in
 * archive_cryptor* and archive_hmac*, BearSSL serves ZIPs with AES (ARCHIVE_CRYPTO_BEARSSL,
 * its headers on the include path). */
#define ARCHIVE_CRYPTO_BEARSSL 1
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_CTYPE_H 1
#define HAVE_ERRNO_H 1
#define HAVE_EILSEQ 1
#define HAVE_FCNTL_H 1
#define HAVE_INTMAX_T 1
#define HAVE_UINTMAX_T 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_LONG_LONG_INT 1
#define HAVE_UNSIGNED_LONG_LONG 1
#define HAVE_UNSIGNED_LONG_LONG_INT 1
#define HAVE_MBRTOWC 1
#define HAVE_WCRTOMB 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCTOMB 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_WCHAR_H 1
#define HAVE_WCHAR_T 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRERROR 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_TIMEGM 1
#define HAVE_ZLIB_H 1
#define HAVE_LIBZ 1
#define HAVE_ARC4RANDOM_BUF 1
