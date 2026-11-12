/* libc.so -- <string.h> and the __aeabi_mem* helpers the ARM compiler emits.
 *
 * Everything works directly on the flat guest buffer. Where the operands are
 * known to be in bounds the host's own memcpy/memcmp is used (they are the
 * fastest thing available and these are on the hottest path in the whole
 * emulator); the byte-at-a-time loops are for the cases where the length is
 * discovered as we go, which cannot be delegated safely.
 */

#include <pvz2native/dependencies/dependency.h>
#include <pvz2native/presence.h>
#include <cctype>
#include <cstring>
#include <string>

#include <pvz2native/dependencies/libc_internal.h>

namespace pvz2native {

/* --- shared by libc_string, libc_wchar and libc_locale ------------------- *
 *
 * Declared in libc_internal.h; defined here because <string.h> is where the
 * canonical versions (strcmp, strncmp) live. See that header for why one loop
 * replaced six copies. */

void libc::compare_bytes(GuestCall &c, std::uint32_t limit, bool stop_at_nul, bool fold_case) {
    const std::uint32_t a = c.arg(0), b = c.arg(1);
    int result = 0;
    for (std::uint32_t i = 0; i < limit; ++i) {
        int ca = c.read8(a + i), cb = c.read8(b + i);
        if (fold_case) {
            ca = std::tolower(ca);
            cb = std::tolower(cb);
        }
        if (ca != cb) {
            result = ca - cb;
            break;
        }
        if (stop_at_nul && ca == 0) break;
    }
    c.set_result((std::uint32_t)result);
}

void libc::compare_wide(GuestCall &c, std::uint32_t limit, bool stop_at_nul) {
    const std::uint32_t a = c.arg(0), b = c.arg(1);
    int result = 0;
    for (std::uint32_t i = 0; i < limit; ++i) {
        /* Unsigned, so the sign of the difference cannot be taken directly the
         * way it can for bytes -- a wchar_t here is a full uint32. */
        const std::uint32_t ca = c.read32(a + i * 4), cb = c.read32(b + i * 4);
        if (ca != cb) {
            result = ca < cb ? -1 : 1;
            break;
        }
        if (stop_at_nul && ca == 0) break;
    }
    c.set_result((std::uint32_t)result);
}

namespace {

/* ------------------------------------------------------------------ memory */

void c_memcpy(GuestCall &c) {
    std::uint32_t dst = c.arg(0), src = c.arg(1), n = c.arg(2);
    /* memmove semantics for both: the engine does overlap these in practice
     * (in-place vector shifts), and on a flat host buffer it costs nothing. */
    if (c.in_bounds(dst, n) && c.in_bounds(src, n)) {
        std::memmove(&c.img->mem[dst], &c.img->mem[src], n);
    }
    c.set_result(dst);
}

void c_memset(GuestCall &c) {
    std::uint32_t dst = c.arg(0), val = c.arg(1), n = c.arg(2);
    if (c.in_bounds(dst, n)) std::memset(&c.img->mem[dst], (int)val, n);
    c.set_result(dst);
}

/* __aeabi_memset(dst, n, val) -- note the argument order is NOT memset's. */
void c_aeabi_memset(GuestCall &c) {
    std::uint32_t dst = c.arg(0), n = c.arg(1), val = c.arg(2);
    if (c.in_bounds(dst, n)) std::memset(&c.img->mem[dst], (int)val, n);
}

void c_aeabi_memclr(GuestCall &c) {
    std::uint32_t dst = c.arg(0), n = c.arg(1);
    if (c.in_bounds(dst, n)) std::memset(&c.img->mem[dst], 0, n);
}

void c_memcmp(GuestCall &c) {
    std::uint32_t a = c.arg(0), b = c.arg(1), n = c.arg(2);
    int r = (c.in_bounds(a, n) && c.in_bounds(b, n))
                ? std::memcmp(&c.img->mem[a], &c.img->mem[b], n)
                : 0;
    c.set_result((std::uint32_t)r);
}

void c_memchr(GuestCall &c) {
    std::uint32_t s = c.arg(0), ch = c.arg(1) & 0xFFu, n = c.arg(2);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (c.read8(s + i) == ch) { c.set_result(s + i); return; }
    }
    c.set_result(0);
}

/* ----------------------------------------------------------------- strings */

std::uint32_t guest_strlen(GuestCall &c, std::uint32_t s) {
    std::uint32_t len = 0;
    while (c.in_bounds(s + len, 1) && c.img->mem[s + len] != 0) ++len;
    return len;
}

void c_strlen(GuestCall &c) { c.set_result(guest_strlen(c, c.arg(0))); }

void c_strcpy(GuestCall &c) {
    std::uint32_t dst = c.arg(0), src = c.arg(1);
    for (std::uint32_t i = 0;; ++i) {
        std::uint8_t ch = c.read8(src + i);
        c.write8(dst + i, ch);
        if (ch == 0) break;
    }
    c.set_result(dst);
}

void c_strncpy(GuestCall &c) {
    std::uint32_t dst = c.arg(0), src = c.arg(1), n = c.arg(2);
    bool ended = false;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint8_t ch = ended ? 0 : c.read8(src + i);
        if (ch == 0) ended = true; /* strncpy pads the whole tail with NULs */
        c.write8(dst + i, ch);
    }
    c.set_result(dst);
}

void append(GuestCall &c, std::uint32_t max_n) {
    std::uint32_t dst = c.arg(0), src = c.arg(1);
    std::uint32_t dlen = guest_strlen(c, dst);
    std::uint32_t i = 0;
    for (; i < max_n; ++i) {
        std::uint8_t ch = c.read8(src + i);
        if (ch == 0) break;
        c.write8(dst + dlen + i, ch);
    }
    c.write8(dst + dlen + i, 0);
    c.set_result(dst);
}

void c_strcat(GuestCall &c) { append(c, 0xFFFFFFFFu); }
void c_strncat(GuestCall &c) { append(c, c.arg(2)); }

/* The comparison loop itself is shared -- see libc/compare_bytes. */
void c_strcmp(GuestCall &c) { libc::compare_bytes(c, libc::kUnbounded, true, false); }
void c_strncmp(GuestCall &c) { libc::compare_bytes(c, c.arg(2), true, false); }
void c_strcasecmp(GuestCall &c) { libc::compare_bytes(c, libc::kUnbounded, true, true); }
void c_strncasecmp(GuestCall &c) { libc::compare_bytes(c, c.arg(2), true, true); }

void c_strchr(GuestCall &c) {
    std::uint32_t s = c.arg(0);
    std::uint8_t want = (std::uint8_t)c.arg(1);
    for (std::uint32_t i = 0;; ++i) {
        std::uint8_t ch = c.read8(s + i);
        if (ch == want) { c.set_result(s + i); return; } /* a NUL search finds the terminator, per the standard */
        if (ch == 0) break;
    }
    c.set_result(0);
}

void c_strrchr(GuestCall &c) {
    std::uint32_t s = c.arg(0), found = 0;
    std::uint8_t want = (std::uint8_t)c.arg(1);
    for (std::uint32_t i = 0;; ++i) {
        std::uint8_t ch = c.read8(s + i);
        if (ch == want) found = s + i;
        if (ch == 0) break;
    }
    c.set_result(found);
}

void c_strstr(GuestCall &c) {
    std::string hay = c.cstr(c.arg(0)), needle = c.cstr(c.arg(1), 256);
    std::size_t pos = hay.find(needle);
    c.set_result(pos == std::string::npos ? 0 : c.arg(0) + (std::uint32_t)pos);
}

void c_strdup(GuestCall &c) {
    c.set_result(c.dup_cstr(c.cstr(c.arg(0))));
}

/* char *strpbrk(const char *s, const char *accept) -- first character of s that
 * appears in accept, or NULL. */
void c_strpbrk(GuestCall &c) {
    const std::uint32_t s = c.arg(0);
    const std::string accept = c.cstr(c.arg(1), 256);
    for (std::uint32_t i = 0;; ++i) {
        const std::uint8_t ch = c.read8(s + i);
        if (ch == 0) break;
        if (accept.find((char)ch) != std::string::npos) {
            c.set_result(s + i);
            return;
        }
    }
    c.set_result(0);
}

/* size_t strlcpy(char *dst, const char *src, size_t size) -- the BSD one.
 *
 * Returns the length of SRC, not of what it copied: that is how the caller
 * detects truncation (result >= size), and returning the copied length instead
 * would make every truncation look like a success. Always NUL-terminates when
 * size is non-zero, which is the whole reason the function exists. */
void c_strlcpy(GuestCall &c) {
    const std::uint32_t dst = c.arg(0), size = c.arg(2);
    const std::string src = c.cstr(c.arg(1));
    if (dst != 0 && size > 0) {
        const std::uint32_t n = std::min<std::uint32_t>((std::uint32_t)src.size(), size - 1);
        for (std::uint32_t i = 0; i < n; ++i) c.write8(dst + i, (std::uint8_t)src[i]);
        c.write8(dst + n, 0);
    }
    c.set_result((std::uint32_t)src.size());
}

/* The body shared by strtok and strtok_r -- they differ only in where the
 * cursor lives, so `pos` comes in and goes out through the caller. */
std::uint32_t tokenize(GuestCall &c, std::uint32_t pos, std::uint32_t delim,
                       std::uint32_t *pos_out) {
    auto is_delim = [&](std::uint8_t ch) {
        for (std::uint32_t i = 0; c.read8(delim + i) != 0; ++i) {
            if (c.read8(delim + i) == ch) return true;
        }
        return false;
    };

    while (c.read8(pos) != 0 && is_delim(c.read8(pos))) ++pos;
    if (c.read8(pos) == 0) {
        *pos_out = pos;
        return 0;
    }
    std::uint32_t tok_start = pos;
    while (c.read8(pos) != 0 && !is_delim(c.read8(pos))) ++pos;
    if (c.read8(pos) != 0) {
        c.write8(pos, 0);
        ++pos;
    }
    *pos_out = pos;
    return tok_start;
}

/* strtok keeps its cursor in per-thread storage, exactly like the real one. */
void c_strtok(GuestCall &c) {
    std::uint32_t str = c.arg(0), delim = c.arg(1);
    std::uint32_t pos = (str != 0) ? str : guest_tls::strtok_next;
    std::uint32_t next = 0;
    std::uint32_t tok = tokenize(c, pos, delim, &next);
    guest_tls::strtok_next = next;
    c.set_result(tok);
}

/* char *strtok_r(char *str, const char *delim, char **saveptr) -- the same, with
 * the cursor in the caller's own variable rather than thread-local state. */
void c_strtok_r(GuestCall &c) {
    const std::uint32_t str = c.arg(0), delim = c.arg(1), saveptr = c.arg(2);
    std::uint32_t pos = (str != 0) ? str : (saveptr != 0 ? c.read32(saveptr) : 0);
    if (pos == 0) {
        c.set_result(0);
        return;
    }
    std::uint32_t next = 0;
    std::uint32_t tok = tokenize(c, pos, delim, &next);
    if (saveptr != 0) c.write32(saveptr, next);
    c.set_result(tok);
}

/* char *strsep(char **stringp, const char *delim)
 *
 * NOT strtok: it returns EMPTY tokens for adjacent delimiters instead of
 * skipping them, which is exactly why parsers that care about empty fields use
 * it. Collapsing runs here would silently drop fields. */
void c_strsep(GuestCall &c) {
    const std::uint32_t stringp = c.arg(0), delim = c.arg(1);
    if (stringp == 0) {
        c.set_result(0);
        return;
    }
    const std::uint32_t start = c.read32(stringp);
    if (start == 0) {
        c.set_result(0);
        return;
    }
    const std::string set = c.cstr(delim, 256);
    std::uint32_t pos = start;
    for (;; ++pos) {
        const std::uint8_t ch = c.read8(pos);
        if (ch == 0) {
            /* No delimiter left: this is the final token and the next call
             * must return NULL. */
            c.write32(stringp, 0);
            c.set_result(start);
            return;
        }
        if (set.find((char)ch) != std::string::npos) {
            c.write8(pos, 0);
            c.write32(stringp, pos + 1);
            c.set_result(start);
            return;
        }
    }
}

/* size_t strspn(const char *s, const char *accept) -- length of the initial run
 * made ONLY of characters from accept. */
void c_strspn(GuestCall &c) {
    const std::uint32_t s = c.arg(0);
    const std::string accept = c.cstr(c.arg(1), 256);
    std::uint32_t n = 0;
    for (;; ++n) {
        const std::uint8_t ch = c.read8(s + n);
        if (ch == 0 || accept.find((char)ch) == std::string::npos) break;
    }
    c.set_result(n);
}

/* size_t strcspn(const char *s, const char *reject) -- length of the initial run
 * containing NONE of the characters in reject. The complement of strspn. */
void c_strcspn(GuestCall &c) {
    const std::uint32_t s = c.arg(0);
    const std::string reject = c.cstr(c.arg(1), 256);
    std::uint32_t n = 0;
    for (;; ++n) {
        const std::uint8_t ch = c.read8(s + n);
        if (ch == 0 || reject.find((char)ch) != std::string::npos) break;
    }
    c.set_result(n);
}

/* void *memrchr(const void *s, int c, size_t n) -- LAST occurrence in the first
 * n bytes, or NULL. Unlike memchr it does not stop at a NUL. */
void c_memrchr(GuestCall &c) {
    const std::uint32_t s = c.arg(0);
    const std::uint8_t want = (std::uint8_t)(c.arg(1) & 0xFF);
    const std::uint32_t n = c.arg(2);
    for (std::uint32_t i = n; i > 0; --i) {
        if (c.read8(s + i - 1) == want) {
            c.set_result(s + i - 1);
            return;
        }
    }
    c.set_result(0);
}

}  // namespace

void register_libc_string(ImportTable &t) {
    t.add("memcpy", c_memcpy);
    t.add("memmove", c_memcpy);
    t.add("memset", c_memset);
    t.add("memcmp", c_memcmp);
    t.add("memchr", c_memchr);

    /* Compiler-emitted helpers. The 4/8-suffixed forms differ only in the
     * alignment they promise, which is nothing to us -- registering them keeps
     * a differently-optimised build of the game working without changes. */
    t.add("__aeabi_memcpy", c_memcpy);
    t.add("__aeabi_memcpy4", c_memcpy);
    t.add("__aeabi_memcpy8", c_memcpy);
    t.add("__aeabi_memmove", c_memcpy);
    t.add("__aeabi_memmove4", c_memcpy);
    t.add("__aeabi_memmove8", c_memcpy);
    t.add("__aeabi_memset", c_aeabi_memset);
    t.add("__aeabi_memset4", c_aeabi_memset);
    t.add("__aeabi_memset8", c_aeabi_memset);
    t.add("__aeabi_memclr", c_aeabi_memclr);
    t.add("__aeabi_memclr4", c_aeabi_memclr);
    t.add("__aeabi_memclr8", c_aeabi_memclr);

    t.add("strlen", c_strlen);
    t.add("strcpy", c_strcpy);
    t.add("strncpy", c_strncpy);
    t.add("strcat", c_strcat);
    t.add("strncat", c_strncat);
    t.add("strcmp", c_strcmp);
    t.add("strncmp", c_strncmp);
    t.add("strcasecmp", c_strcasecmp);
    t.add("strncasecmp", c_strncasecmp);
    t.add("strchr", c_strchr);
    t.add("strrchr", c_strrchr);
    t.add("strstr", c_strstr);
    t.add("strpbrk", c_strpbrk);
    t.add("strdup", c_strdup);
    t.add("strlcpy", c_strlcpy);
    t.add("strtok", c_strtok);

    /* Added for 9.6.1 (PvZ2 9.6.1 / Reflourished). */
    t.add("strtok_r", c_strtok_r);
    t.add("strsep", c_strsep);
    t.add("strspn", c_strspn);
    t.add("strcspn", c_strcspn);
    t.add("memrchr", c_memrchr);
}

}  // namespace pvz2native
