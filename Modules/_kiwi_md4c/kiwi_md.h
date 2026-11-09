#ifndef KIWI_MD_H
#define KIWI_MD_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} KiwiMdBuffer;

typedef struct {
    bool sanitize;
    bool math;
    const uint64_t *extra_tags;
    size_t extra_tag_count;
} KiwiMdOptions;

int kiwi_md_buf_init(KiwiMdBuffer *buf, size_t initial_cap);
void kiwi_md_buf_free(KiwiMdBuffer *buf);
int kiwi_md_buf_append(KiwiMdBuffer *buf, const char *str, size_t len);
int kiwi_md_buf_putc(KiwiMdBuffer *buf, char c);

/* Pack a lowercase HTML tag name (1..8 ASCII chars) into a 64-bit integer key.
 * Returns UINT64_MAX if the tag is longer than 8 characters or invalid.
 */
uint64_t kiwi_md_pack_tag(const char *name, size_t len);

/* Render Markdown (CommonMark + GFM tables, tasklists, strikethrough,
 * fenced code blocks, links/autolinks, and KaTeX math protection) into HTML.
 */
int kiwi_md_render(const char *input, size_t input_len,
                   const KiwiMdOptions *opts, KiwiMdBuffer *out);

/* Standalone allowlist HTML sanitizer for KiwiSpaceONE. */
int kiwi_md_sanitize_html(const char *input, size_t input_len,
                          const uint64_t *extra_tags, size_t extra_tag_count,
                          KiwiMdBuffer *out);

#ifdef __cplusplus
}
#endif

#endif /* KIWI_MD_H */
