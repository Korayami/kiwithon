/* kiwi_simd_json.h: ARM64 NEON and x86_64 AVX2/SSE2 SIMD JSON string scanning,
 * unescaped ASCII run detection, whitespace skipping, and fast integer helpers
 * for Kiwithon's _json accelerator.
 */

#ifndef KIWI_SIMD_JSON_H
#define KIWI_SIMD_JSON_H

#include "Python.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(__aarch64__)
#  include <arm_neon.h>
#elif defined(__AVX2__) || defined(__SSE2__) || defined(__x86_64__)
#  include <immintrin.h>
#endif

/* =========================================================================
 * 1. SIMD Unescaped ASCII Run Scanner (for `ascii_escape_size` / `escape_size`
 *    and `ascii_escape_unicode_and_size` / `escape_unicode_and_size`)
 *
 * Scans 1-byte (`Py_UCS1`) buffer for characters in `[0x20, 0x7e]` excluding
 * `"` (0x22) and `\` (0x5c). Returns index of first character needing escape
 * or `len` if all remaining bytes are safe printable ASCII.
 * ========================================================================= */

static inline Py_ssize_t
kiwi_simd_find_escape_ascii(const Py_UCS1 *buf, Py_ssize_t i, Py_ssize_t len)
{
#if defined(__aarch64__)
    if (len - i >= 16) {
        const uint8x16_t v_quote = vdupq_n_u8((uint8_t)'"');
        const uint8x16_t v_bslash = vdupq_n_u8((uint8_t)'\\');
        const uint8x16_t v_ctrl = vdupq_n_u8(0x1f);
        const uint8x16_t v_del  = vdupq_n_u8(0x7f);

        for (; i + 16 <= len; i += 16) {
            uint8x16_t chunk = vld1q_u8((const uint8_t *)(buf + i));
            uint8x16_t m_q   = vceqq_u8(chunk, v_quote);
            uint8x16_t m_b   = vceqq_u8(chunk, v_bslash);
            uint8x16_t m_lo  = vcleq_u8(chunk, v_ctrl);
            uint8x16_t m_hi  = vcgeq_u8(chunk, v_del);
            uint8x16_t bad   = vorrq_u8(vorrq_u8(m_q, m_b), vorrq_u8(m_lo, m_hi));
            if (vmaxvq_u8(bad) != 0) {
                break;
            }
        }
    }
#elif defined(__AVX2__)
    if (len - i >= 32) {
        const __m256i v_quote  = _mm256_set1_epi8('"');
        const __m256i v_bslash = _mm256_set1_epi8('\\');
        const __m256i v_space  = _mm256_set1_epi8(0x20);
        const __m256i v_del    = _mm256_set1_epi8(0x7f);

        for (; i + 32 <= len; i += 32) {
            __m256i chunk = _mm256_loadu_si256((const __m256i *)(buf + i));
            __m256i m_q   = _mm256_cmpeq_epi8(chunk, v_quote);
            __m256i m_b   = _mm256_cmpeq_epi8(chunk, v_bslash);
            /* Signed comparison: bytes < 0x20 (including >= 0x80 high-bit set) or == 0x7f */
            __m256i m_lt  = _mm256_cmpgt_epi8(v_space, chunk);
            __m256i m_del = _mm256_cmpeq_epi8(chunk, v_del);
            __m256i bad   = _mm256_or_si256(_mm256_or_si256(m_q, m_b),
                                            _mm256_or_si256(m_lt, m_del));
            uint32_t mask = (uint32_t)_mm256_movemask_epi8(bad);
            if (mask != 0) {
                return i + (Py_ssize_t)__builtin_ctz(mask);
            }
        }
    }
#elif defined(__SSE2__) || defined(__x86_64__)
    if (len - i >= 16) {
        const __m128i v_quote  = _mm_set1_epi8('"');
        const __m128i v_bslash = _mm_set1_epi8('\\');
        const __m128i v_space  = _mm_set1_epi8(0x20);
        const __m128i v_del    = _mm_set1_epi8(0x7f);

        for (; i + 16 <= len; i += 16) {
            __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
            __m128i m_q   = _mm_cmpeq_epi8(chunk, v_quote);
            __m128i m_b   = _mm_cmpeq_epi8(chunk, v_bslash);
            __m128i m_lt  = _mm_cmpgt_epi8(v_space, chunk);
            __m128i m_del = _mm_cmpeq_epi8(chunk, v_del);
            __m128i bad   = _mm_or_si128(_mm_or_si128(m_q, m_b),
                                         _mm_or_si128(m_lt, m_del));
            uint32_t mask = (uint32_t)_mm_movemask_epi8(bad);
            if (mask != 0) {
                return i + (Py_ssize_t)__builtin_ctz(mask);
            }
        }
    }
#endif
    for (; i < len; i++) {
        Py_UCS1 c = buf[i];
        if (c < ' ' || c > '~' || c == '\\' || c == '"') {
            break;
        }
    }
    return i;
}

/* =========================================================================
 * 2. SIMD JSON String Scanner (`scanstring_unicode` 1-byte fast path)
 *
 * Scans `Py_UCS1` buffer from `next` to `len` stopping at `"`, `\`, or
 * control character `<= 0x1f` (when `strict != 0`).
 * ========================================================================= */

static inline Py_ssize_t
kiwi_simd_scan_json_str_1byte(const Py_UCS1 *buf, Py_ssize_t next, Py_ssize_t len, int strict)
{
#if defined(__aarch64__)
    if (strict && len - next >= 16) {
        const uint8x16_t v_quote  = vdupq_n_u8((uint8_t)'"');
        const uint8x16_t v_bslash = vdupq_n_u8((uint8_t)'\\');
        const uint8x16_t v_ctrl   = vdupq_n_u8(0x1f);

        for (; next + 16 <= len; next += 16) {
            uint8x16_t chunk = vld1q_u8((const uint8_t *)(buf + next));
            uint8x16_t m_q   = vceqq_u8(chunk, v_quote);
            uint8x16_t m_b   = vceqq_u8(chunk, v_bslash);
            uint8x16_t m_c   = vcleq_u8(chunk, v_ctrl);
            uint8x16_t stop  = vorrq_u8(vorrq_u8(m_q, m_b), m_c);
            if (vmaxvq_u8(stop) != 0) {
                break;
            }
        }
    }
#elif defined(__AVX2__)
    if (strict && len - next >= 32) {
        const __m256i v_quote  = _mm256_set1_epi8('"');
        const __m256i v_bslash = _mm256_set1_epi8('\\');
        const __m256i v_ctrl   = _mm256_set1_epi8(0x1f);
        const __m256i v_zero   = _mm256_setzero_si256();

        for (; next + 32 <= len; next += 32) {
            __m256i chunk = _mm256_loadu_si256((const __m256i *)(buf + next));
            __m256i m_q   = _mm256_cmpeq_epi8(chunk, v_quote);
            __m256i m_b   = _mm256_cmpeq_epi8(chunk, v_bslash);
            /* Unsigned <= 0x1f via min_epu8(chunk, 0x1f) == chunk */
            __m256i m_c   = _mm256_cmpeq_epi8(_mm256_subs_epu8(chunk, v_ctrl), v_zero);
            __m256i stop  = _mm256_or_si256(_mm256_or_si256(m_q, m_b), m_c);
            uint32_t mask = (uint32_t)_mm256_movemask_epi8(stop);
            if (mask != 0) {
                return next + (Py_ssize_t)__builtin_ctz(mask);
            }
        }
    }
#elif defined(__SSE2__) || defined(__x86_64__)
    if (strict && len - next >= 16) {
        const __m128i v_quote  = _mm_set1_epi8('"');
        const __m128i v_bslash = _mm_set1_epi8('\\');
        const __m128i v_ctrl   = _mm_set1_epi8(0x1f);
        const __m128i v_zero   = _mm_setzero_si128();

        for (; next + 16 <= len; next += 16) {
            __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + next));
            __m128i m_q   = _mm_cmpeq_epi8(chunk, v_quote);
            __m128i m_b   = _mm_cmpeq_epi8(chunk, v_bslash);
            __m128i m_c   = _mm_cmpeq_epi8(_mm_subs_epu8(chunk, v_ctrl), v_zero);
            __m128i stop  = _mm_or_si128(_mm_or_si128(m_q, m_b), m_c);
            uint32_t mask = (uint32_t)_mm_movemask_epi8(stop);
            if (mask != 0) {
                return next + (Py_ssize_t)__builtin_ctz(mask);
            }
        }
    }
#endif
    return next;
}

/* =========================================================================
 * 3. SIMD JSON Whitespace Skipping (` `, `\t`, `\n`, `\r`)
 * ========================================================================= */

static inline Py_ssize_t
kiwi_simd_skip_whitespace(const void *str, int kind, Py_ssize_t idx, Py_ssize_t end_idx)
{
    if (kind == PyUnicode_1BYTE_KIND) {
        const Py_UCS1 *p = (const Py_UCS1 *)str;
#if defined(__aarch64__)
        if (end_idx - idx >= 16) {
            const uint8x16_t v_sp = vdupq_n_u8(' ');
            const uint8x16_t v_tb = vdupq_n_u8('\t');
            const uint8x16_t v_nl = vdupq_n_u8('\n');
            const uint8x16_t v_cr = vdupq_n_u8('\r');
            while (idx + 16 <= end_idx) {
                uint8x16_t c = vld1q_u8((const uint8_t *)(p + idx));
                uint8x16_t ws = vorrq_u8(vorrq_u8(vceqq_u8(c, v_sp), vceqq_u8(c, v_tb)),
                                         vorrq_u8(vceqq_u8(c, v_nl), vceqq_u8(c, v_cr)));
                if (vminvq_u8(ws) == 0) {
                    break;
                }
                idx += 16;
            }
        }
#elif defined(__SSE2__) || defined(__x86_64__)
        if (end_idx - idx >= 16) {
            const __m128i v_sp = _mm_set1_epi8(' ');
            const __m128i v_tb = _mm_set1_epi8('\t');
            const __m128i v_nl = _mm_set1_epi8('\n');
            const __m128i v_cr = _mm_set1_epi8('\r');
            while (idx + 16 <= end_idx) {
                __m128i c = _mm_loadu_si128((const __m128i *)(p + idx));
                __m128i ws = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(c, v_sp), _mm_cmpeq_epi8(c, v_tb)),
                                          _mm_or_si128(_mm_cmpeq_epi8(c, v_nl), _mm_cmpeq_epi8(c, v_cr)));
                uint32_t non_ws = (~(uint32_t)_mm_movemask_epi8(ws)) & 0xFFFFu;
                if (non_ws != 0) {
                    return idx + (Py_ssize_t)__builtin_ctz(non_ws);
                }
                idx += 16;
            }
        }
#endif
        while (idx <= end_idx) {
            Py_UCS1 c = p[idx];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            idx++;
        }
        return idx;
    }

    while (idx <= end_idx) {
        Py_UCS4 c = PyUnicode_READ(kind, str, idx);
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        idx++;
    }
    return idx;
}

/* =========================================================================
 * 4. Fast 64-bit Integer Parsing Helper (avoids temporary `PyBytes` alloc
 *    in `_match_number_unicode` when number fits in 1..18 ASCII digits)
 * ========================================================================= */

static inline PyObject *
kiwi_fast_parse_int_1byte(const Py_UCS1 *p, Py_ssize_t start, Py_ssize_t end)
{
    Py_ssize_t n = end - start;
    if (n <= 0 || n > 18) {
        return NULL;
    }
    Py_ssize_t i = start;
    bool neg = false;
    if (p[i] == '-') {
        neg = true;
        i++;
        if (i >= end) return NULL;
    }
    uint64_t val = 0;
    for (; i < end; i++) {
        val = val * 10 + (uint64_t)(p[i] - '0');
    }
    if (neg) {
        return PyLong_FromLongLong(-(long long)val);
    }
    return PyLong_FromUnsignedLongLong(val);
}

#endif /* KIWI_SIMD_JSON_H */
