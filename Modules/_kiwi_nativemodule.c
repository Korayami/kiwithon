/* _kiwi_native: High-performance native filesystem, SIMD search, and security
 * accelerator module for Kiwithon / KiwiSpaceONE.
 *
 * Built as a core/builtin module with free-threading (Py_MOD_GIL_NOT_USED)
 * support and zero-GIL release during I/O-bound and compute-heavy paths.
 */

#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE 1
#endif

#include "Python.h"
#include "pycore_dict.h"          // _PyDict_NewPresized()

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

#if defined(__aarch64__)
#  include <arm_neon.h>
#elif defined(__AVX2__) || defined(__SSE2__) || defined(__x86_64__)
#  include <immintrin.h>
#endif

/* =========================================================================
 * 1. 64-bit Packed Integer Extension Classifier (O(1) switch table)
 * ========================================================================= */

#define EXT1(a) \
    ((uint64_t)(unsigned char)(a))

#define EXT2(a,b) \
    (EXT1(a) | ((uint64_t)(unsigned char)(b) << 8))

#define EXT3(a,b,c) \
    (EXT2(a,b) | ((uint64_t)(unsigned char)(c) << 16))

#define EXT4(a,b,c,d) \
    (EXT3(a,b,c) | ((uint64_t)(unsigned char)(d) << 24))

#define EXT5(a,b,c,d,e) \
    (EXT4(a,b,c,d) | ((uint64_t)(unsigned char)(e) << 32))

#define EXT6(a,b,c,d,e,f) \
    (EXT5(a,b,c,d,e) | ((uint64_t)(unsigned char)(f) << 40))

#define EXT7(a,b,c,d,e,f,g) \
    (EXT6(a,b,c,d,e,f) | ((uint64_t)(unsigned char)(g) << 48))

#define EXT8(a,b,c,d,e,f,g,h) \
    (EXT7(a,b,c,d,e,f,g) | ((uint64_t)(unsigned char)(h) << 56))

typedef enum {
    KIWI_CAT_VIDEO = 0,
    KIWI_CAT_IMAGE = 1,
    KIWI_CAT_AUDIO = 2,
    KIWI_CAT_DOCUMENT = 3,
    KIWI_CAT_ARCHIVE = 4,
    KIWI_CAT_CODE = 5,
    KIWI_CAT_DATABASE = 6,
    KIWI_CAT_OTHER = 7,
    KIWI_CAT_COUNT = 8
} KiwiCategory;

static const char * const KIWI_CATEGORY_NAMES[KIWI_CAT_COUNT] = {
    "video",
    "image",
    "audio",
    "document",
    "archive",
    "code",
    "database",
    "other"
};

static inline uint64_t
pack_lower_ext(const char *filename, size_t *out_len)
{
    const char *dot = strrchr(filename, '.');
    if (dot == NULL || dot == filename || dot[1] == '\0') {
        if (out_len) *out_len = 0;
        return 0;
    }
    const char *ext = dot + 1;
    uint64_t packed = 0;
    size_t i = 0;
    while (ext[i] != '\0') {
        if (i >= 8) {
            if (out_len) *out_len = i;
            return UINT64_MAX;
        }
        unsigned char c = (unsigned char)ext[i];
        if (c >= 'A' && c <= 'Z') {
            c |= 0x20;
        }
        packed |= ((uint64_t)c) << (i * 8);
        i++;
    }
    if (out_len) *out_len = i;
    return packed;
}

static inline KiwiCategory
classify_extension_key(uint64_t ext_key)
{
    switch (ext_key) {
        /* Video (14) */
        case EXT3('m','p','4'):
        case EXT3('m','k','v'):
        case EXT4('w','e','b','m'):
        case EXT3('a','v','i'):
        case EXT3('m','o','v'):
        case EXT3('w','m','v'):
        case EXT3('f','l','v'):
        case EXT3('m','4','v'):
        case EXT3('m','p','g'):
        case EXT4('m','p','e','g'):
        case EXT3('3','g','p'):
        case EXT3('o','g','v'):
        case EXT2('t','s'):
        case EXT3('v','o','b'):
            return KIWI_CAT_VIDEO;

        /* Image (15) */
        case EXT3('j','p','g'):
        case EXT4('j','p','e','g'):
        case EXT3('p','n','g'):
        case EXT3('g','i','f'):
        case EXT4('w','e','b','p'):
        case EXT3('s','v','g'):
        case EXT3('b','m','p'):
        case EXT3('i','c','o'):
        case EXT3('t','i','f'):
        case EXT4('t','i','f','f'):
        case EXT4('a','v','i','f'):
        case EXT4('h','e','i','c'):
        case EXT4('h','e','i','f'):
        case EXT3('r','a','w'):
        case EXT3('p','s','d'):
            return KIWI_CAT_IMAGE;

        /* Audio (12) */
        case EXT3('m','p','3'):
        case EXT3('w','a','v'):
        case EXT4('f','l','a','c'):
        case EXT3('a','a','c'):
        case EXT3('o','g','g'):
        case EXT3('m','4','a'):
        case EXT3('w','m','a'):
        case EXT4('o','p','u','s'):
        case EXT4('a','i','f','f'):
        case EXT3('m','i','d'):
        case EXT4('m','i','d','i'):
        case EXT3('a','m','r'):
            return KIWI_CAT_AUDIO;

        /* Document (19) */
        case EXT3('p','d','f'):
        case EXT3('d','o','c'):
        case EXT4('d','o','c','x'):
        case EXT3('x','l','s'):
        case EXT4('x','l','s','x'):
        case EXT3('p','p','t'):
        case EXT4('p','p','t','x'):
        case EXT3('t','x','t'):
        case EXT2('m','d'):
        case EXT8('m','a','r','k','d','o','w','n'):
        case EXT3('r','t','f'):
        case EXT3('o','d','t'):
        case EXT3('o','d','s'):
        case EXT3('o','d','p'):
        case EXT3('c','s','v'):
        case EXT3('t','s','v'):
        case EXT4('e','p','u','b'):
        case EXT3('r','s','t'):
        case EXT3('t','e','x'):
            return KIWI_CAT_DOCUMENT;

        /* Archive (13) */
        case EXT3('z','i','p'):
        case EXT3('t','a','r'):
        case EXT2('g','z'):
        case EXT3('t','g','z'):
        case EXT3('b','z','2'):
        case EXT2('x','z'):
        case EXT3('z','s','t'):
        case EXT2('7','z'):
        case EXT3('r','a','r'):
        case EXT3('i','s','o'):
        case EXT3('d','m','g'):
        case EXT3('d','e','b'):
        case EXT3('r','p','m'):
            return KIWI_CAT_ARCHIVE;

        /* Code (24) */
        case EXT2('p','y'):
        case EXT3('p','y','i'):
        case EXT3('p','y','x'):
        case EXT2('j','s'):
        case EXT3('m','j','s'):
        case EXT3('j','s','x'):
        case EXT3('t','s','x'):
        case EXT4('h','t','m','l'):
        case EXT3('h','t','m'):
        case EXT3('c','s','s'):
        case EXT4('s','c','s','s'):
        case EXT4('j','s','o','n'):
        case EXT4('y','a','m','l'):
        case EXT3('y','m','l'):
        case EXT4('t','o','m','l'):
        case EXT3('x','m','l'):
        case EXT1('c'):
        case EXT1('h'):
        case EXT3('c','p','p'):
        case EXT3('h','p','p'):
        case EXT2('r','s'):
        case EXT2('g','o'):
        case EXT4('j','a','v','a'):
        case EXT2('s','h'):
            return KIWI_CAT_CODE;

        /* Database (8) */
        case EXT2('d','b'):
        case EXT6('s','q','l','i','t','e'):
        case EXT7('s','q','l','i','t','e','3'):
        case EXT3('s','q','l'):
        case EXT3('m','d','b'):
        case EXT5('a','c','c','d','b'):
        case EXT3('d','b','f'):
        case EXT3('l','d','b'):
            return KIWI_CAT_DATABASE;

        default:
            return KIWI_CAT_OTHER;
    }
}

static inline bool
is_knowledge_extension(uint64_t ext_key)
{
    switch (ext_key) {
        case EXT2('m','d'):
        case EXT8('m','a','r','k','d','o','w','n'):
        case EXT3('t','x','t'):
        case EXT3('r','s','t'):
        case EXT4('j','s','o','n'):
        case EXT4('y','a','m','l'):
        case EXT3('y','m','l'):
        case EXT4('t','o','m','l'):
        case EXT3('c','s','v'):
        case EXT2('p','y'):
        case EXT2('j','s'):
        case EXT2('t','s'):
        case EXT4('h','t','m','l'):
        case EXT3('h','t','m'):
        case EXT3('x','m','l'):
        case EXT3('l','o','g'):
        case EXT3('i','n','i'):
        case EXT3('c','f','g'):
        case EXT4('c','o','n','f'):
            return true;
        default:
            return false;
    }
}

/* =========================================================================
 * 2. Fast Directory Walking (`openat` / `fdopendir` / Linux `statx`)
 * ========================================================================= */

#define KIWI_TOP_FILES_LIMIT 32

typedef struct {
    char *path;
    uint64_t size;
    double mtime;
    KiwiCategory category;
} KiwiFileEntry;

typedef struct {
    char **exclude_paths;
    size_t *exclude_lens;
    size_t exclude_count;
    int max_depth;

    uint64_t total_size;
    uint64_t total_files;
    uint64_t total_dirs;
    uint64_t error_count;
    uint64_t cat_sizes[KIWI_CAT_COUNT];
    uint64_t cat_counts[KIWI_CAT_COUNT];

    KiwiFileEntry top_files[KIWI_TOP_FILES_LIMIT];
    size_t top_count;
} KiwiScanState;

static inline bool
is_excluded_path(const KiwiScanState *state, const char *path, size_t path_len)
{
    for (size_t i = 0; i < state->exclude_count; i++) {
        size_t ex_len = state->exclude_lens[i];
        const char *ex = state->exclude_paths[i];
        if (path_len >= ex_len && memcmp(path, ex, ex_len) == 0) {
            if (path_len == ex_len || path[ex_len] == '/' || ex[ex_len - 1] == '/') {
                return true;
            }
        }
    }
    return false;
}

static void
record_top_file(KiwiScanState *state, const char *rel_path, uint64_t size,
                double mtime, KiwiCategory cat)
{
    if (state->top_count < KIWI_TOP_FILES_LIMIT) {
        char *copy = strdup(rel_path);
        if (!copy) return;
        size_t pos = state->top_count++;
        while (pos > 0 && state->top_files[pos - 1].size < size) {
            state->top_files[pos] = state->top_files[pos - 1];
            pos--;
        }
        state->top_files[pos].path = copy;
        state->top_files[pos].size = size;
        state->top_files[pos].mtime = mtime;
        state->top_files[pos].category = cat;
    }
    else if (size > state->top_files[KIWI_TOP_FILES_LIMIT - 1].size) {
        char *copy = strdup(rel_path);
        if (!copy) return;
        free(state->top_files[KIWI_TOP_FILES_LIMIT - 1].path);
        size_t pos = KIWI_TOP_FILES_LIMIT - 1;
        while (pos > 0 && state->top_files[pos - 1].size < size) {
            state->top_files[pos] = state->top_files[pos - 1];
            pos--;
        }
        state->top_files[pos].path = copy;
        state->top_files[pos].size = size;
        state->top_files[pos].mtime = mtime;
        state->top_files[pos].category = cat;
    }
}

static inline int
stat_entry_at(int dirfd, const char *name, unsigned char d_type,
              bool *is_dir, bool *is_reg, bool *is_lnk,
              uint64_t *size_out, double *mtime_out)
{
#if defined(__linux__) && defined(STATX_SIZE)
    struct statx stx;
    int flags = AT_SYMLINK_NOFOLLOW | AT_STATX_DONT_SYNC;
    unsigned int mask = STATX_SIZE | STATX_MTIME | STATX_TYPE;
    if (statx(dirfd, name, flags, mask, &stx) == 0) {
        mode_t mode = stx.stx_mode;
        *is_dir = S_ISDIR(mode);
        *is_reg = S_ISREG(mode);
        *is_lnk = S_ISLNK(mode);
        *size_out = (uint64_t)stx.stx_size;
        *mtime_out = (double)stx.stx_mtime.tv_sec +
                     (double)stx.stx_mtime.tv_nsec * 1e-9;
        return 0;
    }
#endif
    struct stat st;
    if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return -1;
    }
    *is_dir = S_ISDIR(st.st_mode);
    *is_reg = S_ISREG(st.st_mode);
    *is_lnk = S_ISLNK(st.st_mode);
    *size_out = (uint64_t)st.st_size;
#if defined(__APPLE__)
    *mtime_out = (double)st.st_mtimespec.tv_sec +
                 (double)st.st_mtimespec.tv_nsec * 1e-9;
#else
    *mtime_out = (double)st.st_mtim.tv_sec +
                 (double)st.st_mtim.tv_nsec * 1e-9;
#endif
    (void)d_type;
    return 0;
}

static void
scan_dir_recursive(KiwiScanState *state, int parent_fd, const char *name,
                   char *path_buf, size_t path_len, size_t path_cap, int depth)
{
    if (depth > state->max_depth) {
        return;
    }

    int open_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int fd = (parent_fd == AT_FDCWD)
             ? open(name, open_flags)
             : openat(parent_fd, name, open_flags);
    if (fd < 0) {
        state->error_count++;
        return;
    }

    DIR *dir = fdopendir(fd);
    if (dir == NULL) {
        close(fd);
        state->error_count++;
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *dname = entry->d_name;
        if (dname[0] == '.' &&
            (dname[1] == '\0' || (dname[1] == '.' && dname[2] == '\0'))) {
            continue;
        }

        size_t dlen = strlen(dname);
        if (path_len + 1 + dlen + 1 > path_cap) {
            continue;
        }

        size_t orig_len = path_len;
        if (path_len > 0 && path_buf[path_len - 1] != '/') {
            path_buf[path_len++] = '/';
        }
        memcpy(path_buf + path_len, dname, dlen);
        path_len += dlen;
        path_buf[path_len] = '\0';

        if (state->exclude_count > 0 && is_excluded_path(state, path_buf, path_len)) {
            path_len = orig_len;
            path_buf[path_len] = '\0';
            continue;
        }

        unsigned char dtype = entry->d_type;
        if (dtype == DT_LNK) {
            /* Skip symlinks without calling realpath unless needed */
            path_len = orig_len;
            path_buf[path_len] = '\0';
            continue;
        }

        if (dtype == DT_DIR) {
            state->total_dirs++;
            scan_dir_recursive(state, fd, dname, path_buf, path_len, path_cap, depth + 1);
        }
        else {
            bool is_dir = false, is_reg = false, is_lnk = false;
            uint64_t fsize = 0;
            double fmtime = 0.0;

            if (stat_entry_at(fd, dname, dtype, &is_dir, &is_reg, &is_lnk, &fsize, &fmtime) == 0) {
                if (is_lnk) {
                    /* Do not follow symlinks */
                }
                else if (is_dir) {
                    state->total_dirs++;
                    scan_dir_recursive(state, fd, dname, path_buf, path_len, path_cap, depth + 1);
                }
                else if (is_reg) {
                    uint64_t ext_key = pack_lower_ext(dname, NULL);
                    KiwiCategory cat = classify_extension_key(ext_key);
                    state->total_files++;
                    state->total_size += fsize;
                    state->cat_counts[cat]++;
                    state->cat_sizes[cat] += fsize;
                    record_top_file(state, path_buf, fsize, fmtime, cat);
                }
            }
            else {
                state->error_count++;
            }
        }

        path_len = orig_len;
        path_buf[path_len] = '\0';
    }

    closedir(dir);
}

/* =========================================================================
 * 3. SIMD Case-Insensitive Substring Search (`kiwi_strcasestr_simd`)
 * ========================================================================= */

static inline unsigned char
kiwi_tolower_ascii(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c | 0x20) : c;
}

static inline bool
kiwi_memcasecmp_ascii(const char *hay, const char *needle, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char hc = kiwi_tolower_ascii((unsigned char)hay[i]);
        unsigned char nc = kiwi_tolower_ascii((unsigned char)needle[i]);
        if (hc != nc) {
            return false;
        }
    }
    return true;
}

/* Find needle inside haystack using case-insensitive comparison.
 * Uses ARM64 NEON (`vld1q_u8`, `vorrq_u8`, `vceqq_u8`, `vmaxvq_u8`) or
 * x86_64 AVX2 (`_mm256_loadu_si256`, `_mm256_or_si256`, `_mm256_cmpeq_epi8`,
 * `_mm256_movemask_epi8`) with portable scalar fallback.
 */
static const char *
kiwi_strcasestr_simd(const char *haystack, size_t hay_len,
                     const char *needle_lower, size_t needle_len)
{
    if (needle_len == 0) {
        return haystack;
    }
    if (hay_len < needle_len) {
        return NULL;
    }

    const unsigned char first_c = kiwi_tolower_ascii((unsigned char)needle_lower[0]);
    const unsigned char last_c  = kiwi_tolower_ascii((unsigned char)needle_lower[needle_len - 1]);
    const size_t limit = hay_len - needle_len;
    size_t i = 0;

#if defined(__aarch64__)
    if (limit >= 16) {
        const uint8x16_t v_mask20 = vdupq_n_u8(0x20);
        const uint8x16_t v_first  = vdupq_n_u8(first_c | 0x20);
        const uint8x16_t v_last   = vdupq_n_u8(last_c | 0x20);

        for (; i + 16 <= limit; i += 16) {
            uint8x16_t h0 = vld1q_u8((const uint8_t *)(haystack + i));
            uint8x16_t h1 = vld1q_u8((const uint8_t *)(haystack + i + needle_len - 1));
            uint8x16_t l0 = vorrq_u8(h0, v_mask20);
            uint8x16_t l1 = vorrq_u8(h1, v_mask20);
            uint8x16_t eq = vandq_u8(vceqq_u8(l0, v_first), vceqq_u8(l1, v_last));

            if (vmaxvq_u8(eq) != 0) {
                uint8_t lanes[16];
                vst1q_u8(lanes, eq);
                for (size_t k = 0; k < 16; k++) {
                    if (lanes[k] && kiwi_memcasecmp_ascii(haystack + i + k, needle_lower, needle_len)) {
                        return haystack + i + k;
                    }
                }
            }
        }
    }
#elif defined(__AVX2__)
    if (limit >= 32) {
        const __m256i v_mask20 = _mm256_set1_epi8(0x20);
        const __m256i v_first  = _mm256_set1_epi8((char)(first_c | 0x20));
        const __m256i v_last   = _mm256_set1_epi8((char)(last_c | 0x20));

        for (; i + 32 <= limit; i += 32) {
            __m256i h0 = _mm256_loadu_si256((const __m256i *)(haystack + i));
            __m256i h1 = _mm256_loadu_si256((const __m256i *)(haystack + i + needle_len - 1));
            __m256i l0 = _mm256_or_si256(h0, v_mask20);
            __m256i l1 = _mm256_or_si256(h1, v_mask20);
            __m256i eq0 = _mm256_cmpeq_epi8(l0, v_first);
            __m256i eq1 = _mm256_cmpeq_epi8(l1, v_last);
            uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_and_si256(eq0, eq1));

            while (mask != 0) {
                unsigned int bit = (unsigned int)__builtin_ctz(mask);
                if (kiwi_memcasecmp_ascii(haystack + i + bit, needle_lower, needle_len)) {
                    return haystack + i + bit;
                }
                mask &= (mask - 1);
            }
        }
    }
#endif

    /* Portable scalar fallback for short needles and tail bytes */
    for (; i <= limit; i++) {
        if (kiwi_tolower_ascii((unsigned char)haystack[i]) == first_c &&
            kiwi_tolower_ascii((unsigned char)haystack[i + needle_len - 1]) == last_c) {
            if (kiwi_memcasecmp_ascii(haystack + i, needle_lower, needle_len)) {
                return haystack + i;
            }
        }
    }
    return NULL;
}

/* =========================================================================
 * 4. Knowledge File Search Engine (GIL-free file read + SIMD search)
 * ========================================================================= */

typedef struct {
    char *path;
    char *snippet;
    size_t snippet_len;
    size_t match_offset;
    size_t line_number;
    size_t match_count;
} KiwiSearchHit;

typedef struct {
    const char *needle_lower;
    size_t needle_len;
    size_t max_results;
    size_t snippet_radius;
    KiwiSearchHit *hits;
    size_t hit_count;
    size_t hit_cap;
    char *read_buf;
    size_t read_buf_cap;
} KiwiSearchState;

static inline size_t
utf8_safe_start(const char *buf, size_t pos)
{
    while (pos > 0 && (((unsigned char)buf[pos] & 0xC0) == 0x80)) {
        pos--;
    }
    return pos;
}

static inline size_t
utf8_safe_end(const char *buf, size_t len, size_t pos)
{
    while (pos < len && (((unsigned char)buf[pos] & 0xC0) == 0x80)) {
        pos++;
    }
    return pos;
}

static void
search_single_file(KiwiSearchState *st, int dirfd, const char *dname,
                   const char *full_path, uint64_t fsize)
{
    /* Skip empty or overly huge (> 16 MiB) files */
    if (fsize == 0 || fsize > (16ULL * 1024ULL * 1024ULL)) {
        return;
    }

    int fd = openat(dirfd, dname, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return;
    }

    if (fsize + 1 > st->read_buf_cap) {
        size_t new_cap = (size_t)fsize + 4096;
        char *nb = (char *)realloc(st->read_buf, new_cap);
        if (!nb) {
            close(fd);
            return;
        }
        st->read_buf = nb;
        st->read_buf_cap = new_cap;
    }

    ssize_t nread = read(fd, st->read_buf, (size_t)fsize);
    close(fd);
    if (nread <= 0) {
        return;
    }
    size_t len = (size_t)nread;
    st->read_buf[len] = '\0';

    /* Quick binary check on first 512 bytes */
    size_t check_len = len < 512 ? len : 512;
    if (memchr(st->read_buf, '\0', check_len) != NULL) {
        return;
    }

    const char *match = kiwi_strcasestr_simd(st->read_buf, len,
                                             st->needle_lower, st->needle_len);
    if (match == NULL) {
        return;
    }

    size_t match_offset = (size_t)(match - st->read_buf);

    /* Count line number of first match and total occurrences (up to 64) */
    size_t line_no = 1;
    for (size_t i = 0; i < match_offset; i++) {
        if (st->read_buf[i] == '\n') {
            line_no++;
        }
    }

    size_t match_count = 1;
    const char *cur = match + st->needle_len;
    while (match_count < 64 && (size_t)(cur - st->read_buf) + st->needle_len <= len) {
        const char *next_m = kiwi_strcasestr_simd(
            cur, len - (size_t)(cur - st->read_buf),
            st->needle_lower, st->needle_len);
        if (!next_m) break;
        match_count++;
        cur = next_m + st->needle_len;
    }

    size_t s_start = (match_offset > st->snippet_radius)
                     ? (match_offset - st->snippet_radius) : 0;
    size_t s_end   = (match_offset + st->needle_len + st->snippet_radius < len)
                     ? (match_offset + st->needle_len + st->snippet_radius) : len;
    s_start = utf8_safe_start(st->read_buf, s_start);
    s_end   = utf8_safe_end(st->read_buf, len, s_end);

    size_t snip_len = s_end - s_start;
    char *snip = (char *)malloc(snip_len + 1);
    if (!snip) {
        return;
    }
    for (size_t i = 0; i < snip_len; i++) {
        char c = st->read_buf[s_start + i];
        snip[i] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    }
    snip[snip_len] = '\0';

    if (st->hit_count >= st->hit_cap) {
        size_t ncap = st->hit_cap ? st->hit_cap * 2 : 32;
        KiwiSearchHit *nh = (KiwiSearchHit *)realloc(st->hits, ncap * sizeof(KiwiSearchHit));
        if (!nh) {
            free(snip);
            return;
        }
        st->hits = nh;
        st->hit_cap = ncap;
    }

    char *path_copy = strdup(full_path);
    if (!path_copy) {
        free(snip);
        return;
    }

    KiwiSearchHit *hit = &st->hits[st->hit_count++];
    hit->path = path_copy;
    hit->snippet = snip;
    hit->snippet_len = snip_len;
    hit->match_offset = match_offset;
    hit->line_number = line_no;
    hit->match_count = match_count;
}

static void
search_dir_recursive(KiwiSearchState *st, int parent_fd, const char *name,
                     char *path_buf, size_t path_len, size_t path_cap, int depth)
{
    if (depth > 24 || st->hit_count >= st->max_results) {
        return;
    }

    int open_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int fd = (parent_fd == AT_FDCWD)
             ? open(name, open_flags)
             : openat(parent_fd, name, open_flags);
    if (fd < 0) {
        return;
    }

    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (st->hit_count >= st->max_results) {
            break;
        }
        const char *dname = entry->d_name;
        if (dname[0] == '.') {
            /* Skip hidden directories and dotfiles during knowledge search */
            continue;
        }

        size_t dlen = strlen(dname);
        if (path_len + 1 + dlen + 1 > path_cap) {
            continue;
        }

        size_t orig_len = path_len;
        if (path_len > 0 && path_buf[path_len - 1] != '/') {
            path_buf[path_len++] = '/';
        }
        memcpy(path_buf + path_len, dname, dlen);
        path_len += dlen;
        path_buf[path_len] = '\0';

        unsigned char dtype = entry->d_type;
        if (dtype == DT_LNK) {
            path_len = orig_len;
            path_buf[path_len] = '\0';
            continue;
        }

        if (dtype == DT_DIR) {
            search_dir_recursive(st, fd, dname, path_buf, path_len, path_cap, depth + 1);
        }
        else {
            uint64_t ext_key = pack_lower_ext(dname, NULL);
            if (dtype == DT_REG && !is_knowledge_extension(ext_key)) {
                path_len = orig_len;
                path_buf[path_len] = '\0';
                continue;
            }

            bool is_dir = false, is_reg = false, is_lnk = false;
            uint64_t fsize = 0;
            double fmtime = 0.0;
            if (stat_entry_at(fd, dname, dtype, &is_dir, &is_reg, &is_lnk, &fsize, &fmtime) == 0) {
                if (is_dir && !is_lnk) {
                    search_dir_recursive(st, fd, dname, path_buf, path_len, path_cap, depth + 1);
                }
                else if (is_reg && !is_lnk && is_knowledge_extension(ext_key)) {
                    search_single_file(st, fd, dname, path_buf, fsize);
                }
            }
        }

        path_len = orig_len;
        path_buf[path_len] = '\0';
    }

    closedir(dir);
}

/* =========================================================================
 * 5. Python C-API Functions (`METH_FASTCALL | METH_KEYWORDS`)
 * ========================================================================= */

static PyObject *
kiwi_scan_storage_tree(PyObject *Py_UNUSED(module),
                       PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    static const char * const kwlist[] = {"root_path", "exclude_paths", "max_depth", NULL};
    PyObject *root_obj = NULL;
    PyObject *exclude_obj = NULL;
    int max_depth = 32;

    if (nargs < 1 || nargs > 3) {
        PyErr_SetString(PyExc_TypeError,
                        "scan_storage_tree() takes from 1 to 3 positional arguments");
        return NULL;
    }
    root_obj = args[0];
    if (nargs >= 2) {
        if (PyLong_Check(args[1])) {
            max_depth = PyLong_AsInt(args[1]);
            if (max_depth == -1 && PyErr_Occurred()) return NULL;
        }
        else {
            exclude_obj = args[1];
        }
    }
    if (nargs >= 3) {
        max_depth = PyLong_AsInt(args[2]);
        if (max_depth == -1 && PyErr_Occurred()) return NULL;
    }

    if (kwnames != NULL) {
        Py_ssize_t nkw = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < nkw; i++) {
            PyObject *kw = PyTuple_GET_ITEM(kwnames, i);
            PyObject *val = args[nargs + i];
            if (PyUnicode_CompareWithASCIIString(kw, kwlist[0]) == 0) {
                root_obj = val;
            }
            else if (PyUnicode_CompareWithASCIIString(kw, kwlist[1]) == 0) {
                exclude_obj = val;
            }
            else if (PyUnicode_CompareWithASCIIString(kw, kwlist[2]) == 0) {
                max_depth = PyLong_AsInt(val);
                if (max_depth == -1 && PyErr_Occurred()) return NULL;
            }
            else {
                PyErr_Format(PyExc_TypeError,
                             "scan_storage_tree() got an unexpected keyword argument %R", kw);
                return NULL;
            }
        }
    }

    PyObject *root_bytes = NULL;
    if (!PyUnicode_FSConverter(root_obj, &root_bytes)) {
        return NULL;
    }
    const char *root_cstr = PyBytes_AS_STRING(root_bytes);
    Py_ssize_t root_len = PyBytes_GET_SIZE(root_bytes);

    KiwiScanState state;
    memset(&state, 0, sizeof(state));
    state.max_depth = (max_depth < 0) ? 0 : max_depth;

    if (exclude_obj != NULL && exclude_obj != Py_None) {
        PyObject *seq = PySequence_Fast(exclude_obj, "exclude_paths must be an iterable");
        if (seq == NULL) {
            Py_DECREF(root_bytes);
            return NULL;
        }
        Py_ssize_t n_ex = PySequence_Fast_GET_SIZE(seq);
        if (n_ex > 0) {
            state.exclude_paths = (char **)calloc((size_t)n_ex, sizeof(char *));
            state.exclude_lens  = (size_t *)calloc((size_t)n_ex, sizeof(size_t));
            if (!state.exclude_paths || !state.exclude_lens) {
                Py_DECREF(seq);
                Py_DECREF(root_bytes);
                free(state.exclude_paths);
                free(state.exclude_lens);
                return PyErr_NoMemory();
            }
            for (Py_ssize_t i = 0; i < n_ex; i++) {
                PyObject *item = PySequence_Fast_GET_ITEM(seq, i);
                PyObject *eb = NULL;
                if (!PyUnicode_FSConverter(item, &eb)) {
                    Py_DECREF(seq);
                    Py_DECREF(root_bytes);
                    for (size_t k = 0; k < state.exclude_count; k++) free(state.exclude_paths[k]);
                    free(state.exclude_paths);
                    free(state.exclude_lens);
                    return NULL;
                }
                state.exclude_paths[state.exclude_count] = strdup(PyBytes_AS_STRING(eb));
                state.exclude_lens[state.exclude_count]  = (size_t)PyBytes_GET_SIZE(eb);
                state.exclude_count++;
                Py_DECREF(eb);
            }
        }
        Py_DECREF(seq);
    }

    size_t path_cap = (size_t)root_len + 4096;
    char *path_buf = (char *)malloc(path_cap);
    if (!path_buf) {
        Py_DECREF(root_bytes);
        for (size_t k = 0; k < state.exclude_count; k++) free(state.exclude_paths[k]);
        free(state.exclude_paths);
        free(state.exclude_lens);
        return PyErr_NoMemory();
    }
    memcpy(path_buf, root_cstr, (size_t)root_len + 1);

    /* Release GIL during entire directory tree traversal */
    Py_BEGIN_ALLOW_THREADS
    scan_dir_recursive(&state, AT_FDCWD, root_cstr, path_buf, (size_t)root_len, path_cap, 0);
    Py_END_ALLOW_THREADS

    free(path_buf);
    Py_DECREF(root_bytes);
    for (size_t k = 0; k < state.exclude_count; k++) {
        free(state.exclude_paths[k]);
    }
    free(state.exclude_paths);
    free(state.exclude_lens);

    /* Construct Python result dictionary directly via _PyDict_NewPresized */
    PyObject *result = _PyDict_NewPresized(8);
    PyObject *categories_dict = _PyDict_NewPresized(KIWI_CAT_COUNT);
    PyObject *top_list = PyList_New((Py_ssize_t)state.top_count);
    if (!result || !categories_dict || !top_list) {
        Py_XDECREF(result);
        Py_XDECREF(categories_dict);
        Py_XDECREF(top_list);
        for (size_t i = 0; i < state.top_count; i++) free(state.top_files[i].path);
        return NULL;
    }

    for (int c = 0; c < KIWI_CAT_COUNT; c++) {
        PyObject *cat_info = _PyDict_NewPresized(2);
        PyObject *sz = PyLong_FromUnsignedLongLong(state.cat_sizes[c]);
        PyObject *cnt = PyLong_FromUnsignedLongLong(state.cat_counts[c]);
        if (!cat_info || !sz || !cnt) {
            Py_XDECREF(cat_info);
            Py_XDECREF(sz);
            Py_XDECREF(cnt);
            goto fail_cleanup;
        }
        PyDict_SetItemString(cat_info, "size", sz);
        PyDict_SetItemString(cat_info, "count", cnt);
        Py_DECREF(sz);
        Py_DECREF(cnt);
        PyDict_SetItemString(categories_dict, KIWI_CATEGORY_NAMES[c], cat_info);
        Py_DECREF(cat_info);
    }

    for (size_t i = 0; i < state.top_count; i++) {
        PyObject *entry_dict = _PyDict_NewPresized(4);
        PyObject *p_str = PyUnicode_DecodeFSDefault(state.top_files[i].path);
        PyObject *s_int = PyLong_FromUnsignedLongLong(state.top_files[i].size);
        PyObject *m_flt = PyFloat_FromDouble(state.top_files[i].mtime);
        PyObject *c_str = PyUnicode_FromString(KIWI_CATEGORY_NAMES[state.top_files[i].category]);
        free(state.top_files[i].path);
        state.top_files[i].path = NULL;

        if (!entry_dict || !p_str || !s_int || !m_flt || !c_str) {
            Py_XDECREF(entry_dict);
            Py_XDECREF(p_str);
            Py_XDECREF(s_int);
            Py_XDECREF(m_flt);
            Py_XDECREF(c_str);
            goto fail_cleanup;
        }
        PyDict_SetItemString(entry_dict, "path", p_str);
        PyDict_SetItemString(entry_dict, "size", s_int);
        PyDict_SetItemString(entry_dict, "mtime", m_flt);
        PyDict_SetItemString(entry_dict, "category", c_str);
        Py_DECREF(p_str);
        Py_DECREF(s_int);
        Py_DECREF(m_flt);
        Py_DECREF(c_str);
        PyList_SET_ITEM(top_list, (Py_ssize_t)i, entry_dict);
    }

    PyObject *v_total_size  = PyLong_FromUnsignedLongLong(state.total_size);
    PyObject *v_total_files = PyLong_FromUnsignedLongLong(state.total_files);
    PyObject *v_total_dirs  = PyLong_FromUnsignedLongLong(state.total_dirs);
    PyObject *v_errors      = PyLong_FromUnsignedLongLong(state.error_count);

    PyDict_SetItemString(result, "total_size", v_total_size);
    PyDict_SetItemString(result, "total_files", v_total_files);
    PyDict_SetItemString(result, "total_dirs", v_total_dirs);
    PyDict_SetItemString(result, "total_bytes", v_total_size);
    PyDict_SetItemString(result, "files", v_total_files);
    PyDict_SetItemString(result, "dirs", v_total_dirs);
    PyDict_SetItemString(result, "errors", v_errors);
    PyDict_SetItemString(result, "categories", categories_dict);
    PyDict_SetItemString(result, "top_files", top_list);

    Py_XDECREF(v_total_size);
    Py_XDECREF(v_total_files);
    Py_XDECREF(v_total_dirs);
    Py_XDECREF(v_errors);
    Py_DECREF(categories_dict);
    Py_DECREF(top_list);
    return result;

fail_cleanup:
    for (size_t i = 0; i < state.top_count; i++) {
        free(state.top_files[i].path);
    }
    Py_XDECREF(result);
    Py_XDECREF(categories_dict);
    Py_XDECREF(top_list);
    return NULL;
}

static PyObject *
kiwi_search_knowledge_files(PyObject *Py_UNUSED(module),
                            PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    static const char * const kwlist[] = {"root_dir", "query", "max_results", "snippet_radius", NULL};
    PyObject *root_obj = NULL;
    PyObject *query_obj = NULL;
    Py_ssize_t max_results = 50;
    Py_ssize_t snippet_radius = 80;

    if (nargs < 2 || nargs > 4) {
        PyErr_SetString(PyExc_TypeError,
                        "search_knowledge_files() takes from 2 to 4 positional arguments");
        return NULL;
    }
    root_obj = args[0];
    query_obj = args[1];
    if (nargs >= 3) {
        max_results = PyLong_AsSsize_t(args[2]);
        if (max_results == -1 && PyErr_Occurred()) return NULL;
    }
    if (nargs >= 4) {
        snippet_radius = PyLong_AsSsize_t(args[3]);
        if (snippet_radius == -1 && PyErr_Occurred()) return NULL;
    }

    if (kwnames != NULL) {
        Py_ssize_t nkw = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < nkw; i++) {
            PyObject *kw = PyTuple_GET_ITEM(kwnames, i);
            PyObject *val = args[nargs + i];
            if (PyUnicode_CompareWithASCIIString(kw, kwlist[0]) == 0) {
                root_obj = val;
            }
            else if (PyUnicode_CompareWithASCIIString(kw, kwlist[1]) == 0) {
                query_obj = val;
            }
            else if (PyUnicode_CompareWithASCIIString(kw, kwlist[2]) == 0) {
                max_results = PyLong_AsSsize_t(val);
                if (max_results == -1 && PyErr_Occurred()) return NULL;
            }
            else if (PyUnicode_CompareWithASCIIString(kw, kwlist[3]) == 0) {
                snippet_radius = PyLong_AsSsize_t(val);
                if (snippet_radius == -1 && PyErr_Occurred()) return NULL;
            }
            else {
                PyErr_Format(PyExc_TypeError,
                             "search_knowledge_files() got an unexpected keyword argument %R", kw);
                return NULL;
            }
        }
    }

    if (!PyUnicode_Check(query_obj)) {
        PyErr_SetString(PyExc_TypeError, "query must be a str");
        return NULL;
    }
    Py_ssize_t q_len = 0;
    const char *q_utf8 = PyUnicode_AsUTF8AndSize(query_obj, &q_len);
    if (q_utf8 == NULL) {
        return NULL;
    }
    if (q_len == 0 || max_results <= 0) {
        return PyList_New(0);
    }

    PyObject *root_bytes = NULL;
    if (!PyUnicode_FSConverter(root_obj, &root_bytes)) {
        return NULL;
    }
    const char *root_cstr = PyBytes_AS_STRING(root_bytes);
    Py_ssize_t root_len = PyBytes_GET_SIZE(root_bytes);

    char *needle_lower = (char *)malloc((size_t)q_len + 1);
    if (!needle_lower) {
        Py_DECREF(root_bytes);
        return PyErr_NoMemory();
    }
    for (Py_ssize_t i = 0; i < q_len; i++) {
        needle_lower[i] = (char)kiwi_tolower_ascii((unsigned char)q_utf8[i]);
    }
    needle_lower[q_len] = '\0';

    KiwiSearchState st;
    memset(&st, 0, sizeof(st));
    st.needle_lower = needle_lower;
    st.needle_len = (size_t)q_len;
    st.max_results = (size_t)max_results;
    st.snippet_radius = (snippet_radius < 0) ? 0 : (size_t)snippet_radius;

    size_t path_cap = (size_t)root_len + 4096;
    char *path_buf = (char *)malloc(path_cap);
    if (!path_buf) {
        free(needle_lower);
        Py_DECREF(root_bytes);
        return PyErr_NoMemory();
    }
    memcpy(path_buf, root_cstr, (size_t)root_len + 1);

    /* Release GIL during file search & SIMD scanning */
    Py_BEGIN_ALLOW_THREADS
    search_dir_recursive(&st, AT_FDCWD, root_cstr, path_buf, (size_t)root_len, path_cap, 0);
    Py_END_ALLOW_THREADS

    free(path_buf);
    free(st.read_buf);
    free(needle_lower);
    Py_DECREF(root_bytes);

    PyObject *results = PyList_New((Py_ssize_t)st.hit_count);
    if (!results) {
        for (size_t i = 0; i < st.hit_count; i++) {
            free(st.hits[i].path);
            free(st.hits[i].snippet);
        }
        free(st.hits);
        return NULL;
    }

    for (size_t i = 0; i < st.hit_count; i++) {
        KiwiSearchHit *h = &st.hits[i];
        PyObject *item = _PyDict_NewPresized(5);
        PyObject *p_obj = PyUnicode_DecodeFSDefault(h->path);
        PyObject *s_obj = PyUnicode_DecodeUTF8(h->snippet, (Py_ssize_t)h->snippet_len, "replace");
        PyObject *o_obj = PyLong_FromSize_t(h->match_offset);
        PyObject *l_obj = PyLong_FromSize_t(h->line_number);
        PyObject *c_obj = PyLong_FromSize_t(h->match_count);

        free(h->path);
        free(h->snippet);

        if (!item || !p_obj || !s_obj || !o_obj || !l_obj || !c_obj) {
            Py_XDECREF(item);
            Py_XDECREF(p_obj);
            Py_XDECREF(s_obj);
            Py_XDECREF(o_obj);
            Py_XDECREF(l_obj);
            Py_XDECREF(c_obj);
            for (size_t j = i + 1; j < st.hit_count; j++) {
                free(st.hits[j].path);
                free(st.hits[j].snippet);
            }
            free(st.hits);
            Py_DECREF(results);
            return NULL;
        }

        PyDict_SetItemString(item, "path", p_obj);
        PyDict_SetItemString(item, "snippet", s_obj);
        PyDict_SetItemString(item, "offset", o_obj);
        PyDict_SetItemString(item, "line", l_obj);
        PyDict_SetItemString(item, "matches", c_obj);
        Py_DECREF(p_obj);
        Py_DECREF(s_obj);
        Py_DECREF(o_obj);
        Py_DECREF(l_obj);
        Py_DECREF(c_obj);

        PyList_SET_ITEM(results, (Py_ssize_t)i, item);
    }

    free(st.hits);
    return results;
}

/* =========================================================================
 * 6. Constant-Time Auth Verification & Fast FNV-1a / Classification Helpers
 * ========================================================================= */

static PyObject *
kiwi_verify_auth_token(PyObject *Py_UNUSED(module),
                       PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    static const char * const kwlist[] = {"token", "expected", NULL};
    PyObject *token_obj = NULL;
    PyObject *expected_obj = NULL;

    if (nargs > 2) {
        PyErr_SetString(PyExc_TypeError, "verify_auth_token() takes at most 2 arguments");
        return NULL;
    }
    if (nargs >= 1) token_obj = args[0];
    if (nargs >= 2) expected_obj = args[1];

    if (kwnames != NULL) {
        Py_ssize_t nkw = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < nkw; i++) {
            PyObject *kw = PyTuple_GET_ITEM(kwnames, i);
            PyObject *val = args[nargs + i];
            if (PyUnicode_CompareWithASCIIString(kw, kwlist[0]) == 0) {
                token_obj = val;
            }
            else if (PyUnicode_CompareWithASCIIString(kw, kwlist[1]) == 0) {
                expected_obj = val;
            }
            else {
                PyErr_Format(PyExc_TypeError,
                             "verify_auth_token() got an unexpected keyword argument %R", kw);
                return NULL;
            }
        }
    }

    if (token_obj == NULL || expected_obj == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "verify_auth_token() missing required arguments 'token' and 'expected'");
        return NULL;
    }

    const unsigned char *a_buf = NULL;
    const unsigned char *b_buf = NULL;
    Py_ssize_t a_len = 0, b_len = 0;

    if (PyUnicode_Check(token_obj) && PyUnicode_Check(expected_obj)) {
        a_buf = (const unsigned char *)PyUnicode_AsUTF8AndSize(token_obj, &a_len);
        if (!a_buf) return NULL;
        b_buf = (const unsigned char *)PyUnicode_AsUTF8AndSize(expected_obj, &b_len);
        if (!b_buf) return NULL;
    }
    else if (PyBytes_Check(token_obj) && PyBytes_Check(expected_obj)) {
        a_buf = (const unsigned char *)PyBytes_AS_STRING(token_obj);
        a_len = PyBytes_GET_SIZE(token_obj);
        b_buf = (const unsigned char *)PyBytes_AS_STRING(expected_obj);
        b_len = PyBytes_GET_SIZE(expected_obj);
    }
    else {
        PyErr_SetString(PyExc_TypeError,
                        "verify_auth_token() requires both arguments to be str or both bytes");
        return NULL;
    }

    /* Constant-time comparison resilient to timing side-channels */
    volatile unsigned char diff = (unsigned char)((a_len != b_len) ? 1 : 0);
    Py_ssize_t min_len = (a_len < b_len) ? a_len : b_len;
    for (Py_ssize_t i = 0; i < min_len; i++) {
        diff |= (unsigned char)(a_buf[i] ^ b_buf[i]);
    }
    if (diff == 0 && a_len == b_len && a_len > 0) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject *
kiwi_fnv1a_hash64(PyObject *Py_UNUSED(module), PyObject *arg)
{
    const unsigned char *data = NULL;
    Py_ssize_t len = 0;

    if (PyUnicode_Check(arg)) {
        data = (const unsigned char *)PyUnicode_AsUTF8AndSize(arg, &len);
        if (!data) return NULL;
    }
    else if (PyBytes_Check(arg)) {
        data = (const unsigned char *)PyBytes_AS_STRING(arg);
        len = PyBytes_GET_SIZE(arg);
    }
    else {
        PyErr_SetString(PyExc_TypeError, "fnv1a_hash64() argument must be str or bytes");
        return NULL;
    }

    uint64_t hash = 14695981039346656037ULL;
    for (Py_ssize_t i = 0; i < len; i++) {
        hash ^= (uint64_t)data[i];
        hash *= 1099511628211ULL;
    }
    return PyLong_FromUnsignedLongLong(hash);
}

static PyObject *
kiwi_classify_extension(PyObject *Py_UNUSED(module), PyObject *arg)
{
    if (!PyUnicode_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "classify_extension() argument must be str");
        return NULL;
    }
    const char *s = PyUnicode_AsUTF8(arg);
    if (!s) return NULL;
    uint64_t key = pack_lower_ext(s, NULL);
    KiwiCategory cat = classify_extension_key(key);
    return PyUnicode_FromString(KIWI_CATEGORY_NAMES[cat]);
}

static PyObject *
kiwi_simd_case_insensitive_find(PyObject *Py_UNUSED(module), PyObject *const *args, Py_ssize_t nargs)
{
    if (nargs != 2) {
        PyErr_SetString(PyExc_TypeError, "simd_case_insensitive_find(haystack, needle) requires 2 arguments");
        return NULL;
    }
    PyObject *hay_obj = args[0];
    PyObject *nee_obj = args[1];
    if (!PyUnicode_Check(hay_obj) || !PyUnicode_Check(nee_obj)) {
        PyErr_SetString(PyExc_TypeError, "arguments must be str");
        return NULL;
    }
    Py_ssize_t hay_len, nee_len;
    const char *hay = PyUnicode_AsUTF8AndSize(hay_obj, &hay_len);
    const char *nee = PyUnicode_AsUTF8AndSize(nee_obj, &nee_len);
    if (!hay || !nee) return NULL;
    if (nee_len == 0) return PyLong_FromLong(0);
    if (hay_len < nee_len) return PyLong_FromLong(-1);

    const char *match = kiwi_strcasestr_simd(hay, (size_t)hay_len, nee, (size_t)nee_len);
    if (match) {
        return PyLong_FromSsize_t(match - hay);
    }
    return PyLong_FromLong(-1);
}

static PyObject *
kiwi_score_knowledge_entries(PyObject *Py_UNUSED(module), PyObject *const *args, Py_ssize_t nargs)
{
    if (nargs < 2) {
        PyErr_SetString(PyExc_TypeError, "score_knowledge_entries(entries, query) requires at least 2 arguments");
        return NULL;
    }
    PyObject *entries = args[0];
    PyObject *query_obj = args[1];
    if (!PySequence_Check(entries) || !PyUnicode_Check(query_obj)) {
        PyErr_SetString(PyExc_TypeError, "entries must be a sequence and query must be str");
        return NULL;
    }
    Py_ssize_t q_len;
    const char *query = PyUnicode_AsUTF8AndSize(query_obj, &q_len);
    if (!query) return NULL;

    Py_ssize_t n = PySequence_Size(entries);
    if (n < 0) return NULL;
    PyObject *results = PyList_New(0);
    if (!results) return NULL;

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PySequence_GetItem(entries, i);
        if (!item) { Py_DECREF(results); return NULL; }

        if (PyTuple_Check(item) && PyTuple_GET_SIZE(item) >= 3) {
            PyObject *entry_id = PyTuple_GET_ITEM(item, 0);
            PyObject *t_obj = PyTuple_GET_ITEM(item, 1);
            PyObject *c_obj = PyTuple_GET_ITEM(item, 2);
            int score = 0;
            if (PyUnicode_Check(t_obj)) {
                Py_ssize_t t_len;
                const char *t_str = PyUnicode_AsUTF8AndSize(t_obj, &t_len);
                if (t_str && q_len > 0 && kiwi_strcasestr_simd(t_str, (size_t)t_len, query, (size_t)q_len)) {
                    score += 10;
                }
            }
            if (PyUnicode_Check(c_obj)) {
                Py_ssize_t c_len;
                const char *c_str = PyUnicode_AsUTF8AndSize(c_obj, &c_len);
                if (c_str && q_len > 0 && kiwi_strcasestr_simd(c_str, (size_t)c_len, query, (size_t)q_len)) {
                    score += 3;
                }
            }
            if (score > 0) {
                PyObject *tuple = Py_BuildValue("(Oi)", entry_id, score);
                if (tuple) {
                    PyList_Append(results, tuple);
                    Py_DECREF(tuple);
                }
            }
        }
        else {
            PyObject *text_obj = item;
            if (PyDict_Check(item)) {
                PyObject *val = PyDict_GetItemString(item, "content");
                if (val) text_obj = val;
            }
            if (PyUnicode_Check(text_obj)) {
                Py_ssize_t t_len;
                const char *text = PyUnicode_AsUTF8AndSize(text_obj, &t_len);
                if (text && q_len > 0) {
                    const char *m = kiwi_strcasestr_simd(text, (size_t)t_len, query, (size_t)q_len);
                    if (m) {
                        PyObject *tuple = Py_BuildValue("(inO)", 100, (Py_ssize_t)(m - text), item);
                        if (tuple) {
                            PyList_Append(results, tuple);
                            Py_DECREF(tuple);
                        }
                    }
                }
            }
        }
        Py_DECREF(item);
    }
    return results;
}

/* =========================================================================
 * 7. Module Definition & Initialization
 * ========================================================================= */

PyDoc_STRVAR(_kiwi_native_doc,
"Kiwithon native C11 filesystem, SIMD knowledge search, and security accelerator.");

static PyMethodDef _kiwi_native_methods[] = {
    {"scan_storage_tree", _PyCFunction_CAST(kiwi_scan_storage_tree),
     METH_FASTCALL | METH_KEYWORDS,
     PyDoc_STR("scan_storage_tree(root_path, exclude_paths=(), max_depth=32)\n--\n\n"
               "Fast GIL-free directory scanner with O(1) 64-bit extension classification.")},
    {"scan_directory", _PyCFunction_CAST(kiwi_scan_storage_tree),
     METH_FASTCALL | METH_KEYWORDS,
     PyDoc_STR("scan_directory(root_path, exclude_paths=(), max_depth=32)\n--\n\n"
               "Alias for scan_storage_tree.")},
    {"search_knowledge_files", _PyCFunction_CAST(kiwi_search_knowledge_files),
     METH_FASTCALL | METH_KEYWORDS,
     PyDoc_STR("search_knowledge_files(root_dir, query, max_results=50, snippet_radius=80)\n--\n\n"
               "SIMD-accelerated (ARM64 NEON / x86_64 AVX2) case-insensitive file search.")},
    {"score_knowledge_entries", _PyCFunction_CAST(kiwi_score_knowledge_entries),
     METH_FASTCALL,
     PyDoc_STR("score_knowledge_entries(entries, query)\n--\n\n"
               "In-memory SIMD scoring of knowledge entries.")},
    {"simd_case_insensitive_find", _PyCFunction_CAST(kiwi_simd_case_insensitive_find),
     METH_FASTCALL,
     PyDoc_STR("simd_case_insensitive_find(haystack, needle) -> int\n--\n\n"
               "Direct SIMD case-insensitive substring search kernel.")},
    {"verify_auth_token", _PyCFunction_CAST(kiwi_verify_auth_token),
     METH_FASTCALL | METH_KEYWORDS,
     PyDoc_STR("verify_auth_token(token, expected)\n--\n\n"
               "Constant-time token comparison for authentication headers/cookies.")},
    {"fnv1a_hash64", kiwi_fnv1a_hash64, METH_O,
     PyDoc_STR("fnv1a_hash64(data)\n--\n\n"
               "Compute 64-bit FNV-1a hash of str or bytes.")},
    {"classify_extension", kiwi_classify_extension, METH_O,
     PyDoc_STR("classify_extension(filename)\n--\n\n"
               "Classify a filename into a KiwiSpaceONE storage category in O(1).")},
    {NULL, NULL, 0, NULL}
};

static PyModuleDef_Slot _kiwi_native_slots[] = {
    _Py_ABI_SLOT,
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};

static struct PyModuleDef _kiwi_nativemodule = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "_kiwi_native",
    .m_doc = _kiwi_native_doc,
    .m_size = 0,
    .m_methods = _kiwi_native_methods,
    .m_slots = _kiwi_native_slots,
};

PyMODINIT_FUNC
PyInit__kiwi_native(void)
{
    return PyModuleDef_Init(&_kiwi_nativemodule);
}
