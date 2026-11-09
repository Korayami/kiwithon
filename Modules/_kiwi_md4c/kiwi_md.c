/* kiwi_md.c: Fast C11 CommonMark + GFM (tables, tasklists, strikethrough,
 * fenced code, links, autolinks) Markdown-to-HTML engine with single-pass
 * KaTeX math protection and 64-bit packed HTML allowlist sanitizer.
 */

#include "kiwi_md.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* =========================================================================
 * 1. Dynamic Output Buffer
 * ========================================================================= */

int
kiwi_md_buf_init(KiwiMdBuffer *buf, size_t initial_cap)
{
    if (initial_cap < 128) {
        initial_cap = 128;
    }
    buf->data = (char *)malloc(initial_cap);
    if (!buf->data) {
        buf->len = 0;
        buf->cap = 0;
        return -1;
    }
    buf->data[0] = '\0';
    buf->len = 0;
    buf->cap = initial_cap;
    return 0;
}

void
kiwi_md_buf_free(KiwiMdBuffer *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static inline int
kiwi_md_buf_reserve(KiwiMdBuffer *buf, size_t add_len)
{
    if (buf->len + add_len + 1 <= buf->cap) {
        return 0;
    }
    size_t new_cap = buf->cap ? buf->cap * 2 : 256;
    while (new_cap < buf->len + add_len + 1) {
        new_cap *= 2;
    }
    char *nd = (char *)realloc(buf->data, new_cap);
    if (!nd) {
        return -1;
    }
    buf->data = nd;
    buf->cap = new_cap;
    return 0;
}

int
kiwi_md_buf_append(KiwiMdBuffer *buf, const char *str, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (kiwi_md_buf_reserve(buf, len) < 0) {
        return -1;
    }
    memcpy(buf->data + buf->len, str, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return 0;
}

int
kiwi_md_buf_putc(KiwiMdBuffer *buf, char c)
{
    if (kiwi_md_buf_reserve(buf, 1) < 0) {
        return -1;
    }
    buf->data[buf->len++] = c;
    buf->data[buf->len] = '\0';
    return 0;
}

#define APPEND_LIT(buf, lit) kiwi_md_buf_append((buf), (lit), sizeof(lit) - 1)

static int
append_html_escaped(KiwiMdBuffer *buf, const char *s, size_t len)
{
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        const char *rep = NULL;
        size_t rep_len = 0;
        switch (s[i]) {
            case '&': rep = "&amp;";  rep_len = 5; break;
            case '<': rep = "&lt;";   rep_len = 4; break;
            case '>': rep = "&gt;";   rep_len = 4; break;
            case '"': rep = "&quot;"; rep_len = 6; break;
            default: break;
        }
        if (rep) {
            if (i > start) {
                if (kiwi_md_buf_append(buf, s + start, i - start) < 0) return -1;
            }
            if (kiwi_md_buf_append(buf, rep, rep_len) < 0) return -1;
            start = i + 1;
        }
    }
    if (start < len) {
        if (kiwi_md_buf_append(buf, s + start, len - start) < 0) return -1;
    }
    return 0;
}

/* =========================================================================
 * 2. 64-bit Packed Tag & Attribute Classifier for Allowlist Sanitizer
 * ========================================================================= */

#define TAG1(a) ((uint64_t)(unsigned char)(a))
#define TAG2(a,b) (TAG1(a) | ((uint64_t)(unsigned char)(b) << 8))
#define TAG3(a,b,c) (TAG2(a,b) | ((uint64_t)(unsigned char)(c) << 16))
#define TAG4(a,b,c,d) (TAG3(a,b,c) | ((uint64_t)(unsigned char)(d) << 24))
#define TAG5(a,b,c,d,e) (TAG4(a,b,c,d) | ((uint64_t)(unsigned char)(e) << 32))
#define TAG6(a,b,c,d,e,f) (TAG5(a,b,c,d,e) | ((uint64_t)(unsigned char)(f) << 40))
#define TAG7(a,b,c,d,e,f,g) (TAG6(a,b,c,d,e,f) | ((uint64_t)(unsigned char)(g) << 48))
#define TAG8(a,b,c,d,e,f,g,h) (TAG7(a,b,c,d,e,f,g) | ((uint64_t)(unsigned char)(h) << 56))

uint64_t
kiwi_md_pack_tag(const char *name, size_t len)
{
    if (len == 0 || len > 8) {
        return UINT64_MAX;
    }
    uint64_t key = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 'A' && c <= 'Z') {
            c |= 0x20;
        }
        key |= ((uint64_t)c) << (i * 8);
    }
    return key;
}

static inline bool
is_allowed_tag_name(const char *name, size_t len,
                    const uint64_t *extra_tags, size_t extra_tag_count)
{
    if (len == 10) {
        /* Check "blockquote" (10 chars) case-insensitively */
        static const char bq[] = "blockquote";
        for (size_t i = 0; i < 10; i++) {
            unsigned char c = (unsigned char)name[i];
            if (c >= 'A' && c <= 'Z') c |= 0x20;
            if (c != (unsigned char)bq[i]) return false;
        }
        return true;
    }
    uint64_t key = kiwi_md_pack_tag(name, len);
    if (key == UINT64_MAX) {
        return false;
    }
    switch (key) {
        case TAG1('a'):
        case TAG4('a','b','b','r'):
        case TAG7('a','c','r','o','n','y','m'):
        case TAG1('b'):
        case TAG2('b','r'):
        case TAG4('c','o','d','e'):
        case TAG2('d','d'):
        case TAG3('d','e','l'):
        case TAG7('d','e','t','a','i','l','s'):
        case TAG3('d','i','v'):
        case TAG2('d','l'):
        case TAG2('d','t'):
        case TAG2('e','m'):
        case TAG2('h','1'):
        case TAG2('h','2'):
        case TAG2('h','3'):
        case TAG2('h','4'):
        case TAG2('h','5'):
        case TAG2('h','6'):
        case TAG2('h','r'):
        case TAG1('i'):
        case TAG3('i','m','g'):
        case TAG3('i','n','s'):
        case TAG3('k','b','d'):
        case TAG2('l','i'):
        case TAG4('m','a','r','k'):
        case TAG2('o','l'):
        case TAG1('p'):
        case TAG3('p','r','e'):
        case TAG1('q'):
        case TAG2('r','p'):
        case TAG2('r','t'):
        case TAG4('r','u','b','y'):
        case TAG1('s'):
        case TAG4('s','a','m','p'):
        case TAG5('s','m','a','l','l'):
        case TAG4('s','p','a','n'):
        case TAG6('s','t','r','o','n','g'):
        case TAG3('s','u','b'):
        case TAG7('s','u','m','m','a','r','y'):
        case TAG3('s','u','p'):
        case TAG5('t','a','b','l','e'):
        case TAG5('t','b','o','d','y'):
        case TAG2('t','d'):
        case TAG5('t','f','o','o','t'):
        case TAG2('t','h'):
        case TAG5('t','h','e','a','d'):
        case TAG2('t','r'):
        case TAG1('u'):
        case TAG2('u','l'):
        case TAG3('v','a','r'):
            return true;
        default:
            break;
    }
    for (size_t i = 0; i < extra_tag_count; i++) {
        if (extra_tags[i] == key) {
            return true;
        }
    }
    return false;
}

static inline bool
is_allowed_attr(uint64_t attr_key, bool *is_uri_attr)
{
    *is_uri_attr = false;
    switch (attr_key) {
        case TAG4('h','r','e','f'):
        case TAG3('s','r','c'):
            *is_uri_attr = true;
            return true;
        case TAG3('a','l','t'):
        case TAG5('t','i','t','l','e'):
        case TAG5('c','l','a','s','s'):
        case TAG2('i','d'):
        case TAG7('c','o','l','s','p','a','n'):
        case TAG7('r','o','w','s','p','a','n'):
        case TAG5('a','l','i','g','n'):
        case TAG4('o','p','e','n'):
            return true;
        default:
            return false;
    }
}

/* Strip dangerous URI schemes: javascript:, vbscript:, data:text/html */
static bool
is_safe_uri_value(const char *val, size_t len)
{
    char clean[32];
    size_t clen = 0;
    for (size_t i = 0; i < len && clen < sizeof(clean) - 1; i++) {
        unsigned char c = (unsigned char)val[i];
        if (c <= 0x20 || c == 0x7f) {
            continue;
        }
        if (c == '&' && i + 1 < len && val[i + 1] == '#') {
            /* Reject numeric character entity obfuscation in scheme prefix */
            return false;
        }
        if (c >= 'A' && c <= 'Z') {
            c |= 0x20;
        }
        clean[clen++] = (char)c;
    }
    clean[clen] = '\0';

    if (clen >= 11 && memcmp(clean, "javascript:", 11) == 0) {
        return false;
    }
    if (clen >= 9 && memcmp(clean, "vbscript:", 9) == 0) {
        return false;
    }
    if (clen >= 5 && memcmp(clean, "data:", 5) == 0) {
        if (strstr(clean, "text/html") != NULL) {
            return false;
        }
    }
    return true;
}

/* Parse and sanitize a single `<...>` HTML tag at `s[0..len)`.
 * If allowed, writes sanitized `<tag ...>` into `out` and returns bytes consumed.
 * If disallowed or malformed, returns 0 so caller escapes `<` as `&lt;`.
 */
static size_t
sanitize_single_tag(const char *s, size_t len,
                    const uint64_t *extra_tags, size_t extra_tag_count,
                    KiwiMdBuffer *out)
{
    if (len < 3 || s[0] != '<') {
        return 0;
    }
    size_t pos = 1;
    bool is_closing = false;
    if (s[pos] == '/') {
        is_closing = true;
        pos++;
    }
    size_t name_start = pos;
    while (pos < len && ((s[pos] >= 'a' && s[pos] <= 'z') ||
                         (s[pos] >= 'A' && s[pos] <= 'Z') ||
                         (s[pos] >= '0' && s[pos] <= '9'))) {
        pos++;
    }
    size_t name_len = pos - name_start;
    if (name_len == 0 || !is_allowed_tag_name(s + name_start, name_len, extra_tags, extra_tag_count)) {
        return 0;
    }

    /* Locate closing '>' respecting quoted attribute values */
    size_t end_pos = pos;
    char quote = 0;
    while (end_pos < len) {
        char c = s[end_pos];
        if (quote) {
            if (c == quote) quote = 0;
        }
        else {
            if (c == '"' || c == '\'') quote = c;
            else if (c == '>') break;
            else if (c == '<') return 0;
        }
        end_pos++;
    }
    if (end_pos >= len || s[end_pos] != '>') {
        return 0;
    }

    if (kiwi_md_buf_putc(out, '<') < 0) return 0;
    if (is_closing && kiwi_md_buf_putc(out, '/') < 0) return 0;
    for (size_t i = 0; i < name_len; i++) {
        char c = s[name_start + i];
        if (c >= 'A' && c <= 'Z') c |= 0x20;
        if (kiwi_md_buf_putc(out, c) < 0) return 0;
    }

    if (!is_closing) {
        size_t ap = pos;
        while (ap < end_pos) {
            while (ap < end_pos && (s[ap] == ' ' || s[ap] == '\t' || s[ap] == '\r' || s[ap] == '\n' || s[ap] == '/')) {
                ap++;
            }
            if (ap >= end_pos) break;

            size_t aname_start = ap;
            while (ap < end_pos && ((s[ap] >= 'a' && s[ap] <= 'z') ||
                                    (s[ap] >= 'A' && s[ap] <= 'Z') ||
                                    (s[ap] >= '0' && s[ap] <= '9') ||
                                    s[ap] == '-' || s[ap] == '_')) {
                ap++;
            }
            size_t aname_len = ap - aname_start;
            if (aname_len == 0) {
                ap++;
                continue;
            }

            while (ap < end_pos && (s[ap] == ' ' || s[ap] == '\t')) ap++;

            bool has_val = false;
            const char *aval_start = NULL;
            size_t aval_len = 0;

            if (ap < end_pos && s[ap] == '=') {
                has_val = true;
                ap++;
                while (ap < end_pos && (s[ap] == ' ' || s[ap] == '\t')) ap++;
                if (ap < end_pos && (s[ap] == '"' || s[ap] == '\'')) {
                    char q = s[ap++];
                    aval_start = s + ap;
                    while (ap < end_pos && s[ap] != q) ap++;
                    aval_len = (size_t)((s + ap) - aval_start);
                    if (ap < end_pos && s[ap] == q) ap++;
                }
                else {
                    aval_start = s + ap;
                    while (ap < end_pos && s[ap] != ' ' && s[ap] != '\t' && s[ap] != '\r' && s[ap] != '\n' && s[ap] != '>') {
                        ap++;
                    }
                    aval_len = (size_t)((s + ap) - aval_start);
                }
            }

            uint64_t akey = kiwi_md_pack_tag(s + aname_start, aname_len);
            bool is_uri = false;
            if (is_allowed_attr(akey, &is_uri)) {
                if (is_uri && has_val && !is_safe_uri_value(aval_start, aval_len)) {
                    continue;
                }
                if (kiwi_md_buf_putc(out, ' ') < 0) return 0;
                for (size_t i = 0; i < aname_len; i++) {
                    char c = s[aname_start + i];
                    if (c >= 'A' && c <= 'Z') c |= 0x20;
                    if (kiwi_md_buf_putc(out, c) < 0) return 0;
                }
                if (has_val) {
                    if (APPEND_LIT(out, "=\"") < 0) return 0;
                    if (append_html_escaped(out, aval_start, aval_len) < 0) return 0;
                    if (kiwi_md_buf_putc(out, '"') < 0) return 0;
                }
            }
        }
    }

    if (kiwi_md_buf_putc(out, '>') < 0) return 0;
    return end_pos + 1;
}

static size_t
find_html_comment_end(const char *buf, size_t start, size_t len)
{
    for (size_t j = start; j + 3 <= len; j++) {
        if (buf[j] == '-' && buf[j + 1] == '-' && buf[j + 2] == '>') {
            return j + 3;
        }
    }
    return 0;
}

int
kiwi_md_sanitize_html(const char *input, size_t input_len,
                      const uint64_t *extra_tags, size_t extra_tag_count,
                      KiwiMdBuffer *out)
{
    size_t i = 0;
    while (i < input_len) {
        if (input[i] == '<') {
            /* Strip HTML comments <!-- ... --> */
            if (i + 4 <= input_len && memcmp(input + i, "<!--", 4) == 0) {
                size_t next_i = find_html_comment_end(input, i + 4, input_len);
                if (next_i > 0) {
                    i = next_i;
                    continue;
                }
                else {
                    break;
                }
            }
            size_t consumed = sanitize_single_tag(input + i, input_len - i,
                                                  extra_tags, extra_tag_count, out);
            if (consumed > 0) {
                i += consumed;
                continue;
            }
            if (APPEND_LIT(out, "&lt;") < 0) return -1;
            i++;
        }
        else {
            if (kiwi_md_buf_putc(out, input[i]) < 0) return -1;
            i++;
        }
    }
    return 0;
}

/* =========================================================================
 * 3. Inline Markdown + Single-Pass KaTeX Math Span Protector
 * ========================================================================= */

/* Normalize math entities (&amp; -> &, &lt; -> <, &gt; -> >) inside KaTeX
 * spans while re-escaping raw `<`, `>`, `&` for safe HTML output so client-side
 * KaTeX receives clean normalized entities.
 */
static int
append_math_normalized(KiwiMdBuffer *out, const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        if (s[i] == '&') {
            if (i + 5 <= len && memcmp(s + i, "&amp;", 5) == 0) {
                if (APPEND_LIT(out, "&amp;") < 0) return -1;
                i += 5;
                continue;
            }
            if (i + 4 <= len && memcmp(s + i, "&lt;", 4) == 0) {
                if (APPEND_LIT(out, "&lt;") < 0) return -1;
                i += 4;
                continue;
            }
            if (i + 4 <= len && memcmp(s + i, "&gt;", 4) == 0) {
                if (APPEND_LIT(out, "&gt;") < 0) return -1;
                i += 4;
                continue;
            }
            if (i + 6 <= len && memcmp(s + i, "&quot;", 6) == 0) {
                if ( kiwi_md_buf_putc(out, '"') < 0) return -1;
                i += 6;
                continue;
            }
            if (APPEND_LIT(out, "&amp;") < 0) return -1;
            i++;
        }
        else if (s[i] == '<') {
            if (APPEND_LIT(out, "&lt;") < 0) return -1;
            i++;
        }
        else if (s[i] == '>') {
            if (APPEND_LIT(out, "&gt;") < 0) return -1;
            i++;
        }
        else {
            if (kiwi_md_buf_putc(out, s[i]) < 0) return -1;
            i++;
        }
    }
    return 0;
}

static int
render_inline(const char *s, size_t len, const KiwiMdOptions *opts, KiwiMdBuffer *out)
{
    size_t i = 0;
    while (i < len) {
        char c = s[i];

        /* 1. Backslash escape */
        if (c == '\\' && i + 1 < len && ispunct((unsigned char)s[i + 1])) {
            if (append_html_escaped(out, s + i + 1, 1) < 0) return -1;
            i += 2;
            continue;
        }

        /* 2. Code span `...` or ``...`` */
        if (c == '`') {
            size_t ticks = 0;
            while (i + ticks < len && s[i + ticks] == '`') ticks++;
            size_t start = i + ticks;
            size_t scan = start;
            bool found = false;
            while (scan < len) {
                if (s[scan] == '`') {
                    size_t cticks = 0;
                    while (scan + cticks < len && s[scan + cticks] == '`') cticks++;
                    if (cticks == ticks) {
                        found = true;
                        break;
                    }
                    scan += cticks;
                }
                else {
                    scan++;
                }
            }
            if (found) {
                if (APPEND_LIT(out, "<code>") < 0) return -1;
                if (append_html_escaped(out, s + start, scan - start) < 0) return -1;
                if (APPEND_LIT(out, "</code>") < 0) return -1;
                i = scan + ticks;
                continue;
            }
            if (kiwi_md_buf_append(out, s + i, ticks) < 0) return -1;
            i += ticks;
            continue;
        }

        /* 3. Single-pass KaTeX math span protector ($...$ and $$...$$) */
        if (opts->math && c == '$') {
            if (i + 1 < len && s[i + 1] == '$') {
                size_t start = i + 2;
                size_t scan = start;
                bool found = false;
                while (scan + 1 < len) {
                    if (s[scan] == '$' && s[scan + 1] == '$') {
                        found = true;
                        break;
                    }
                    scan++;
                }
                if (found && scan > start) {
                    if (APPEND_LIT(out, "<span class=\"math-display\">$$") < 0) return -1;
                    if (append_math_normalized(out, s + start, scan - start) < 0) return -1;
                    if (APPEND_LIT(out, "$$</span>") < 0) return -1;
                    i = scan + 2;
                    continue;
                }
            }
            else if (i + 1 < len && s[i + 1] != ' ' && s[i + 1] != '\t' && s[i + 1] != '$') {
                size_t start = i + 1;
                size_t scan = start;
                bool found = false;
                while (scan < len && s[scan] != '\n') {
                    if (s[scan] == '$' && s[scan - 1] != '\\' && s[scan - 1] != ' ' && s[scan - 1] != '\t') {
                        /* Ensure closing $ is not followed immediately by a digit */
                        if (scan + 1 >= len || !isdigit((unsigned char)s[scan + 1])) {
                            found = true;
                            break;
                        }
                    }
                    scan++;
                }
                if (found && scan > start) {
                    if (APPEND_LIT(out, "<span class=\"math-inline\">$") < 0) return -1;
                    if (append_math_normalized(out, s + start, scan - start) < 0) return -1;
                    if (APPEND_LIT(out, "$</span>") < 0) return -1;
                    i = scan + 1;
                    continue;
                }
            }
        }

        /* 4. Images ![alt](url) and Links [text](url "title") */
        if ((c == '!' && i + 1 < len && s[i + 1] == '[') || c == '[') {
            bool is_img = (c == '!');
            size_t bracket_start = is_img ? (i + 2) : (i + 1);
            size_t scan = bracket_start;
            int depth = 1;
            while (scan < len && depth > 0) {
                if (s[scan] == '\\' && scan + 1 < len) {
                    scan += 2;
                    continue;
                }
                if (s[scan] == '[') depth++;
                else if (s[scan] == ']') depth--;
                if (depth > 0) scan++;
            }
            if (depth == 0 && scan + 1 < len && s[scan + 1] == '(') {
                size_t label_len = scan - bracket_start;
                size_t url_start = scan + 2;
                while (url_start < len && (s[url_start] == ' ' || s[url_start] == '\t')) url_start++;
                size_t url_scan = url_start;
                while (url_scan < len && s[url_scan] != ')' && s[url_scan] != ' ' && s[url_scan] != '\t' && s[url_scan] != '"') {
                    url_scan++;
                }
                size_t url_len = url_scan - url_start;
                size_t close_paren = url_scan;
                const char *title_start = NULL;
                size_t title_len = 0;
                while (close_paren < len && (s[close_paren] == ' ' || s[close_paren] == '\t')) close_paren++;
                if (close_paren < len && s[close_paren] == '"') {
                    close_paren++;
                    title_start = s + close_paren;
                    while (close_paren < len && s[close_paren] != '"' && s[close_paren] != ')') close_paren++;
                    title_len = (size_t)((s + close_paren) - title_start);
                    if (close_paren < len && s[close_paren] == '"') close_paren++;
                    while (close_paren < len && (s[close_paren] == ' ' || s[close_paren] == '\t')) close_paren++;
                }
                if (close_paren < len && s[close_paren] == ')') {
                    bool safe_url = !opts->sanitize || is_safe_uri_value(s + url_start, url_len);
                    if (is_img) {
                        if (APPEND_LIT(out, "<img") < 0) return -1;
                        if (safe_url) {
                            if (APPEND_LIT(out, " src=\"") < 0) return -1;
                            if (append_html_escaped(out, s + url_start, url_len) < 0) return -1;
                            if (kiwi_md_buf_putc(out, '"') < 0) return -1;
                        }
                        if (APPEND_LIT(out, " alt=\"") < 0) return -1;
                        if (append_html_escaped(out, s + bracket_start, label_len) < 0) return -1;
                        if (kiwi_md_buf_putc(out, '"') < 0) return -1;
                        if (title_start) {
                            if (APPEND_LIT(out, " title=\"") < 0) return -1;
                            if (append_html_escaped(out, title_start, title_len) < 0) return -1;
                            if (kiwi_md_buf_putc(out, '"') < 0) return -1;
                        }
                        if (kiwi_md_buf_putc(out, '>') < 0) return -1;
                    }
                    else {
                        if (APPEND_LIT(out, "<a") < 0) return -1;
                        if (safe_url) {
                            if (APPEND_LIT(out, " href=\"") < 0) return -1;
                            if (append_html_escaped(out, s + url_start, url_len) < 0) return -1;
                            if (kiwi_md_buf_putc(out, '"') < 0) return -1;
                        }
                        if (title_start) {
                            if (APPEND_LIT(out, " title=\"") < 0) return -1;
                            if (append_html_escaped(out, title_start, title_len) < 0) return -1;
                            if (kiwi_md_buf_putc(out, '"') < 0) return -1;
                        }
                        if (kiwi_md_buf_putc(out, '>') < 0) return -1;
                        if (render_inline(s + bracket_start, label_len, opts, out) < 0) return -1;
                        if (APPEND_LIT(out, "</a>") < 0) return -1;
                    }
                    i = close_paren + 1;
                    continue;
                }
            }
        }

        /* 5. GFM Strikethrough ~~...~~ */
        if (c == '~' && i + 1 < len && s[i + 1] == '~') {
            size_t start = i + 2;
            size_t scan = start;
            while (scan + 1 < len) {
                if (s[scan] == '~' && s[scan + 1] == '~') break;
                scan++;
            }
            if (scan + 1 < len && scan > start) {
                if (APPEND_LIT(out, "<del>") < 0) return -1;
                if (render_inline(s + start, scan - start, opts, out) < 0) return -1;
                if (APPEND_LIT(out, "</del>") < 0) return -1;
                i = scan + 2;
                continue;
            }
        }

        /* 6. Strong (**...** or __...__) and Emphasis (*...* or _..._) */
        if (c == '*' || c == '_') {
            if (i + 1 < len && s[i + 1] == c) {
                size_t start = i + 2;
                size_t scan = start;
                while (scan + 1 < len) {
                    if (s[scan] == c && s[scan + 1] == c) break;
                    scan++;
                }
                if (scan + 1 < len && scan > start) {
                    if (APPEND_LIT(out, "<strong>") < 0) return -1;
                    if (render_inline(s + start, scan - start, opts, out) < 0) return -1;
                    if (APPEND_LIT(out, "</strong>") < 0) return -1;
                    i = scan + 2;
                    continue;
                }
            }
            else if (i + 1 < len && s[i + 1] != ' ' && s[i + 1] != c) {
                if (c == '_' && i > 0 && isalnum((unsigned char)s[i - 1])) {
                    /* Intraword underscore is literal in GFM */
                }
                else {
                    size_t start = i + 1;
                    size_t scan = start;
                    while (scan < len) {
                        if (s[scan] == c && s[scan - 1] != ' ') {
                            if (c == '_' && scan + 1 < len && isalnum((unsigned char)s[scan + 1])) {
                                scan++;
                                continue;
                            }
                            break;
                        }
                        scan++;
                    }
                    if (scan < len && scan > start) {
                        if (APPEND_LIT(out, "<em>") < 0) return -1;
                        if (render_inline(s + start, scan - start, opts, out) < 0) return -1;
                        if (APPEND_LIT(out, "</em>") < 0) return -1;
                        i = scan + 1;
                        continue;
                    }
                }
            }
        }

        /* 7. Autolink <https://...> or raw HTML tag */
        if (c == '<') {
            if ((i + 8 < len && memcmp(s + i, "<https://", 9) == 0) ||
                (i + 7 < len && memcmp(s + i, "<http://", 8) == 0)) {
                size_t ustart = i + 1;
                size_t uscan = ustart;
                while (uscan < len && s[uscan] != '>' && s[uscan] != ' ' && s[uscan] != '\n') uscan++;
                if (uscan < len && s[uscan] == '>') {
                    if (APPEND_LIT(out, "<a href=\"") < 0) return -1;
                    if (append_html_escaped(out, s + ustart, uscan - ustart) < 0) return -1;
                    if (APPEND_LIT(out, "\">") < 0) return -1;
                    if (append_html_escaped(out, s + ustart, uscan - ustart) < 0) return -1;
                    if (APPEND_LIT(out, "</a>") < 0) return -1;
                    i = uscan + 1;
                    continue;
                }
            }
            if (opts->sanitize) {
                if (i + 4 <= len && memcmp(s + i, "<!--", 4) == 0) {
                    size_t next_i = find_html_comment_end(s, i + 4, len);
                    if (next_i > 0) {
                        i = next_i;
                        continue;
                    }
                }
                size_t consumed = sanitize_single_tag(s + i, len - i,
                                                      opts->extra_tags, opts->extra_tag_count, out);
                if (consumed > 0) {
                    i += consumed;
                    continue;
                }
                if (APPEND_LIT(out, "&lt;") < 0) return -1;
                i++;
                continue;
            }
            else {
                if (kiwi_md_buf_putc(out, '<') < 0) return -1;
                i++;
                continue;
            }
        }

        /* 8. Bare GFM URL autolink https://... or http://... */
        if ((c == 'h' && i + 8 < len &&
             (memcmp(s + i, "https://", 8) == 0 || memcmp(s + i, "http://", 7) == 0)) &&
            (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == '(')) {
            size_t uend = i;
            while (uend < len && s[uend] != ' ' && s[uend] != '\t' && s[uend] != '\n' &&
                   s[uend] != '<' && s[uend] != '>' && s[uend] != '"') {
                uend++;
            }
            while (uend > i && (s[uend - 1] == '.' || s[uend - 1] == ',' ||
                                s[uend - 1] == ';' || s[uend - 1] == ')')) {
                uend--;
            }
            if (uend > i + 8) {
                if (APPEND_LIT(out, "<a href=\"") < 0) return -1;
                if (append_html_escaped(out, s + i, uend - i) < 0) return -1;
                if (APPEND_LIT(out, "\">") < 0) return -1;
                if (append_html_escaped(out, s + i, uend - i) < 0) return -1;
                if (APPEND_LIT(out, "</a>") < 0) return -1;
                i = uend;
                continue;
            }
        }

        /* 9. Default character handling */
        if (c == '&') {
            /* Preserve well-formed HTML entities, escape bare '&' */
            size_t escan = i + 1;
            if (escan < len && s[escan] == '#') escan++;
            while (escan < len && escan - i < 12 && isalnum((unsigned char)s[escan])) escan++;
            if (escan > i + 1 && escan < len && s[escan] == ';') {
                if (kiwi_md_buf_append(out, s + i, escan - i + 1) < 0) return -1;
                i = escan + 1;
                continue;
            }
            if (APPEND_LIT(out, "&amp;") < 0) return -1;
            i++;
        }
        else if (c == '>') {
            if (APPEND_LIT(out, "&gt;") < 0) return -1;
            i++;
        }
        else {
            if (kiwi_md_buf_putc(out, c) < 0) return -1;
            i++;
        }
    }
    return 0;
}

/* =========================================================================
 * 4. Block-Level CommonMark + GFM Parser
 * ========================================================================= */

typedef struct {
    const char *ptr;
    size_t len;
} MdLine;

static inline void
trim_line_ws(const char **p, size_t *len)
{
    while (*len > 0 && (**p == ' ' || **p == '\t')) {
        (*p)++;
        (*len)--;
    }
    while (*len > 0 && ((*p)[*len - 1] == ' ' || (*p)[*len - 1] == '\t' || (*p)[*len - 1] == '\r')) {
        (*len)--;
    }
}

static bool
is_thematic_break(const char *p, size_t len)
{
    trim_line_ws(&p, &len);
    if (len < 3) return false;
    char marker = p[0];
    if (marker != '-' && marker != '*' && marker != '_') return false;
    size_t count = 0;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == marker) count++;
        else if (p[i] != ' ' && p[i] != '\t') return false;
    }
    return count >= 3;
}

static bool
is_table_delimiter_row(const char *p, size_t len, int *aligns, size_t *col_count, size_t max_cols)
{
    trim_line_ws(&p, &len);
    if (len == 0) return false;
    bool has_pipe = false;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '|') has_pipe = true;
    }
    if (!has_pipe) return false;

    if (p[0] == '|') { p++; len--; }
    if (len > 0 && p[len - 1] == '|') { len--; }

    size_t cols = 0;
    size_t i = 0;
    while (i < len && cols < max_cols) {
        size_t cell_start = i;
        while (i < len && p[i] != '|') i++;
        const char *cp = p + cell_start;
        size_t clen = i - cell_start;
        trim_line_ws(&cp, &clen);
        if (clen == 0) return false;

        bool left_colon = (cp[0] == ':');
        bool right_colon = (cp[clen - 1] == ':');
        size_t dashes = 0;
        for (size_t k = (left_colon ? 1 : 0); k < clen - (right_colon ? 1 : 0); k++) {
            if (cp[k] != '-') return false;
            dashes++;
        }
        if (dashes == 0) return false;

        if (left_colon && right_colon) aligns[cols] = 2; /* center */
        else if (right_colon) aligns[cols] = 3;          /* right */
        else if (left_colon) aligns[cols] = 1;           /* left */
        else aligns[cols] = 0;                           /* default */
        cols++;
        if (i < len && p[i] == '|') i++;
    }
    *col_count = cols;
    return cols > 0;
}

static int
render_table_row_cells(const char *p, size_t len, const char *cell_tag,
                       const int *aligns, size_t num_cols,
                       const KiwiMdOptions *opts, KiwiMdBuffer *out)
{
    trim_line_ws(&p, &len);
    if (len > 0 && p[0] == '|') { p++; len--; }
    if (len > 0 && p[len - 1] == '|') { len--; }

    if (APPEND_LIT(out, "<tr>") < 0) return -1;
    size_t i = 0;
    for (size_t col = 0; col < num_cols; col++) {
        size_t cstart = i;
        while (i < len) {
            if (p[i] == '\\' && i + 1 < len) { i += 2; continue; }
            if (p[i] == '|') break;
            i++;
        }
        const char *cp = (cstart < len) ? (p + cstart) : "";
        size_t clen = (cstart < len) ? (i - cstart) : 0;
        trim_line_ws(&cp, &clen);
        if (i < len && p[i] == '|') i++;

        if (kiwi_md_buf_putc(out, '<') < 0) return -1;
        if (kiwi_md_buf_append(out, cell_tag, 2) < 0) return -1;
        if (aligns[col] == 1 && APPEND_LIT(out, " align=\"left\"") < 0) return -1;
        else if (aligns[col] == 2 && APPEND_LIT(out, " align=\"center\"") < 0) return -1;
        else if (aligns[col] == 3 && APPEND_LIT(out, " align=\"right\"") < 0) return -1;
        if (kiwi_md_buf_putc(out, '>') < 0) return -1;

        if (render_inline(cp, clen, opts, out) < 0) return -1;

        if (APPEND_LIT(out, "</") < 0) return -1;
        if (kiwi_md_buf_append(out, cell_tag, 2) < 0) return -1;
        if (kiwi_md_buf_putc(out, '>') < 0) return -1;
    }
    if (APPEND_LIT(out, "</tr>\n") < 0) return -1;
    return 0;
}

static bool
parse_list_marker(const char *p, size_t len, bool *is_ordered, size_t *content_offset)
{
    size_t i = 0;
    while (i < len && i < 3 && p[i] == ' ') i++;
    if (i >= len) return false;

    if ((p[i] == '-' || p[i] == '*' || p[i] == '+') && i + 1 < len && (p[i + 1] == ' ' || p[i + 1] == '\t')) {
        *is_ordered = false;
        *content_offset = i + 2;
        return true;
    }

    size_t dstart = i;
    while (i < len && isdigit((unsigned char)p[i])) i++;
    if (i > dstart && i - dstart <= 9 && i + 1 < len && (p[i] == '.' || p[i] == ')') && (p[i + 1] == ' ' || p[i + 1] == '\t')) {
        *is_ordered = true;
        *content_offset = i + 2;
        return true;
    }
    return false;
}

int
kiwi_md_render(const char *input, size_t input_len,
               const KiwiMdOptions *opts, KiwiMdBuffer *out)
{
    /* Split input into line views without copying line contents */
    size_t line_cap = 64;
    size_t line_count = 0;
    MdLine *lines = (MdLine *)malloc(line_cap * sizeof(MdLine));
    if (!lines) return -1;

    size_t pos = 0;
    while (pos < input_len) {
        size_t lstart = pos;
        while (pos < input_len && input[pos] != '\n') pos++;
        size_t llen = pos - lstart;
        if (llen > 0 && input[lstart + llen - 1] == '\r') llen--;
        if (line_count >= line_cap) {
            line_cap *= 2;
            MdLine *nl = (MdLine *)realloc(lines, line_cap * sizeof(MdLine));
            if (!nl) { free(lines); return -1; }
            lines = nl;
        }
        lines[line_count].ptr = input + lstart;
        lines[line_count].len = llen;
        line_count++;
        if (pos < input_len && input[pos] == '\n') pos++;
    }

    size_t idx = 0;
    while (idx < line_count) {
        const char *lp = lines[idx].ptr;
        size_t llen = lines[idx].len;

        /* Skip blank lines */
        const char *tp = lp;
        size_t tlen = llen;
        trim_line_ws(&tp, &tlen);
        if (tlen == 0) {
            idx++;
            continue;
        }

        /* 1. Fenced code block ``` or ~~~ */
        if (tlen >= 3 && ((tp[0] == '`' && tp[1] == '`' && tp[2] == '`') ||
                          (tp[0] == '~' && tp[1] == '~' && tp[2] == '~'))) {
            char fence_ch = tp[0];
            size_t fence_len = 0;
            while (fence_len < tlen && tp[fence_len] == fence_ch) fence_len++;
            const char *lang = tp + fence_len;
            size_t lang_len = tlen - fence_len;
            trim_line_ws(&lang, &lang_len);
            /* Only first token of info string is language */
            size_t ltok = 0;
            while (ltok < lang_len && lang[ltok] != ' ' && lang[ltok] != '\t') ltok++;

            if (APPEND_LIT(out, "<pre><code") < 0) goto err;
            if (ltok > 0) {
                if (APPEND_LIT(out, " class=\"language-") < 0) goto err;
                if (append_html_escaped(out, lang, ltok) < 0) goto err;
                if (kiwi_md_buf_putc(out, '"') < 0) goto err;
            }
            if (kiwi_md_buf_putc(out, '>') < 0) goto err;

            idx++;
            while (idx < line_count) {
                const char *cp = lines[idx].ptr;
                size_t clen = lines[idx].len;
                const char *ctp = cp;
                size_t ctlen = clen;
                trim_line_ws(&ctp, &ctlen);
                size_t cf = 0;
                while (cf < ctlen && ctp[cf] == fence_ch) cf++;
                if (cf >= fence_len && cf == ctlen) {
                    idx++;
                    break;
                }
                if (append_html_escaped(out, cp, clen) < 0) goto err;
                if (kiwi_md_buf_putc(out, '\n') < 0) goto err;
                idx++;
            }
            if (APPEND_LIT(out, "</code></pre>\n") < 0) goto err;
            continue;
        }

        /* 2. Multi-line $$...$$ display math block */
        if (opts->math && tlen >= 2 && tp[0] == '$' && tp[1] == '$') {
            if (tlen > 4 && tp[tlen - 1] == '$' && tp[tlen - 2] == '$') {
                if (APPEND_LIT(out, "<div class=\"math-display\">$$") < 0) goto err;
                if (append_math_normalized(out, tp + 2, tlen - 4) < 0) goto err;
                if (APPEND_LIT(out, "$$</div>\n") < 0) goto err;
                idx++;
                continue;
            }
            if (tlen == 2) {
                if (APPEND_LIT(out, "<div class=\"math-display\">$$\n") < 0) goto err;
                idx++;
                while (idx < line_count) {
                    const char *mp = lines[idx].ptr;
                    size_t mlen = lines[idx].len;
                    const char *mtp = mp;
                    size_t mtlen = mlen;
                    trim_line_ws(&mtp, &mtlen);
                    if (mtlen == 2 && mtp[0] == '$' && mtp[1] == '$') {
                        idx++;
                        break;
                    }
                    if (append_math_normalized(out, mp, mlen) < 0) goto err;
                    if (kiwi_md_buf_putc(out, '\n') < 0) goto err;
                    idx++;
                }
                if (APPEND_LIT(out, "$$</div>\n") < 0) goto err;
                continue;
            }
        }

        /* 3. ATX Heading # .. ###### */
        if (tp[0] == '#') {
            size_t hlevel = 0;
            while (hlevel < tlen && tp[hlevel] == '#') hlevel++;
            if (hlevel >= 1 && hlevel <= 6 && (hlevel == tlen || tp[hlevel] == ' ' || tp[hlevel] == '\t')) {
                const char *hcontent = tp + hlevel;
                size_t hlen = tlen - hlevel;
                trim_line_ws(&hcontent, &hlen);
                while (hlen > 0 && hcontent[hlen - 1] == '#') hlen--;
                trim_line_ws(&hcontent, &hlen);

                char hopen[6] = {'<', 'h', (char)('0' + hlevel), '>', '\0'};
                char hclose[7] = {'<', '/', 'h', (char)('0' + hlevel), '>', '\n', '\0'};
                if (kiwi_md_buf_append(out, hopen, 4) < 0) goto err;
                if (render_inline(hcontent, hlen, opts, out) < 0) goto err;
                if (kiwi_md_buf_append(out, hclose, 6) < 0) goto err;
                idx++;
                continue;
            }
        }

        /* 4. Thematic break (---, ***, ___) */
        if (is_thematic_break(tp, tlen)) {
            if (APPEND_LIT(out, "<hr>\n") < 0) goto err;
            idx++;
            continue;
        }

        /* 5. Blockquote > ... */
        if (tp[0] == '>') {
            KiwiMdBuffer bq_in;
            if (kiwi_md_buf_init(&bq_in, 256) < 0) goto err;
            while (idx < line_count) {
                const char *bqp = lines[idx].ptr;
                size_t bqlen = lines[idx].len;
                trim_line_ws(&bqp, &bqlen);
                if (bqlen == 0 || bqp[0] != '>') break;
                bqp++; bqlen--;
                if (bqlen > 0 && bqp[0] == ' ') { bqp++; bqlen--; }
                if (kiwi_md_buf_append(&bq_in, bqp, bqlen) < 0 ||
                    kiwi_md_buf_putc(&bq_in, '\n') < 0) {
                    kiwi_md_buf_free(&bq_in);
                    goto err;
                }
                idx++;
            }
            if (APPEND_LIT(out, "<blockquote>\n") < 0) {
                kiwi_md_buf_free(&bq_in);
                goto err;
            }
            if (kiwi_md_render(bq_in.data, bq_in.len, opts, out) < 0) {
                kiwi_md_buf_free(&bq_in);
                goto err;
            }
            kiwi_md_buf_free(&bq_in);
            if (APPEND_LIT(out, "</blockquote>\n") < 0) goto err;
            continue;
        }

        /* 6. GFM Table (Header row + delimiter row `| --- | --- |`) */
        int aligns[64];
        size_t num_cols = 0;
        if (idx + 1 < line_count &&
            memchr(tp, '|', tlen) != NULL &&
            is_table_delimiter_row(lines[idx + 1].ptr, lines[idx + 1].len, aligns, &num_cols, 64)) {
            if (APPEND_LIT(out, "<table>\n<thead>\n") < 0) goto err;
            if (render_table_row_cells(tp, tlen, "th", aligns, num_cols, opts, out) < 0) goto err;
            if (APPEND_LIT(out, "</thead>\n<tbody>\n") < 0) goto err;
            idx += 2;
            while (idx < line_count) {
                const char *rp = lines[idx].ptr;
                size_t rlen = lines[idx].len;
                trim_line_ws(&rp, &rlen);
                if (rlen == 0 || memchr(rp, '|', rlen) == NULL) break;
                if (render_table_row_cells(rp, rlen, "td", aligns, num_cols, opts, out) < 0) goto err;
                idx++;
            }
            if (APPEND_LIT(out, "</tbody>\n</table>\n") < 0) goto err;
            continue;
        }

        /* 7. Lists (Unordered, Ordered, and GFM Task Lists `- [ ]` / `- [x]`) */
        bool is_ord = false;
        size_t item_off = 0;
        if (parse_list_marker(lp, llen, &is_ord, &item_off)) {
            const char *list_open = is_ord ? "<ol>\n" : "<ul>\n";
            const char *list_close = is_ord ? "</ol>\n" : "</ul>\n";
            if (kiwi_md_buf_append(out, list_open, 5) < 0) goto err;

            while (idx < line_count) {
                bool cur_ord = false;
                size_t cur_off = 0;
                if (!parse_list_marker(lines[idx].ptr, lines[idx].len, &cur_ord, &cur_off) ||
                    cur_ord != is_ord) {
                    break;
                }
                const char *ic = lines[idx].ptr + cur_off;
                size_t ic_len = lines[idx].len - cur_off;
                trim_line_ws(&ic, &ic_len);

                if (ic_len >= 3 && ic[0] == '[' &&
                    (ic[1] == ' ' || ic[1] == 'x' || ic[1] == 'X') &&
                    ic[2] == ']' && (ic_len == 3 || ic[3] == ' ' || ic[3] == '\t')) {
                    bool checked = (ic[1] == 'x' || ic[1] == 'X');
                    ic += (ic_len > 3) ? 4 : 3;
                    ic_len -= (ic_len > 3) ? 4 : 3;
                    if (APPEND_LIT(out, "<li class=\"task-list-item\"><input type=\"checkbox\" disabled") < 0) goto err;
                    if (checked && APPEND_LIT(out, " checked") < 0) goto err;
                    if (APPEND_LIT(out, "> ") < 0) goto err;
                }
                else {
                    if (APPEND_LIT(out, "<li>") < 0) goto err;
                }

                if (render_inline(ic, ic_len, opts, out) < 0) goto err;
                if (APPEND_LIT(out, "</li>\n") < 0) goto err;
                idx++;
            }
            if (kiwi_md_buf_append(out, list_close, 6) < 0) goto err;
            continue;
        }

        /* 8. Paragraph (accumulate consecutive non-blank, non-special lines) */
        if (APPEND_LIT(out, "<p>") < 0) goto err;
        bool first_pline = true;
        while (idx < line_count) {
            const char *pp = lines[idx].ptr;
            size_t plen = lines[idx].len;
            const char *ptp = pp;
            size_t ptlen = plen;
            trim_line_ws(&ptp, &ptlen);
            if (ptlen == 0) break;

            bool dummy_ord = false;
            size_t dummy_off = 0;
            if (!first_pline &&
                (ptp[0] == '#' || ptp[0] == '>' ||
                 (ptlen >= 3 && ((ptp[0] == '`' && ptp[1] == '`' && ptp[2] == '`') ||
                                 (ptp[0] == '~' && ptp[1] == '~' && ptp[2] == '~'))) ||
                 is_thematic_break(ptp, ptlen) ||
                 parse_list_marker(pp, plen, &dummy_ord, &dummy_off))) {
                break;
            }

            if (!first_pline) {
                if (kiwi_md_buf_putc(out, '\n') < 0) goto err;
            }
            first_pline = false;

            /* Hard line break if line ends with 2+ spaces */
            bool hard_br = (plen >= 2 && pp[plen - 1] == ' ' && pp[plen - 2] == ' ');
            if (render_inline(ptp, ptlen, opts, out) < 0) goto err;
            if (hard_br) {
                if (APPEND_LIT(out, "<br>") < 0) goto err;
            }
            idx++;
        }
        if (APPEND_LIT(out, "</p>\n") < 0) goto err;
    }

    free(lines);
    return 0;

err:
    free(lines);
    return -1;
}
