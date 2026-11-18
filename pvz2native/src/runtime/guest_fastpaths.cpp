/* In-JIT versions of a few hot imported libc functions.
 *
 * Every imported call normally traps to the host (SVC in the trampoline table),
 * which costs a JIT exit and re-entry. That is fine for most imports, but some
 * screens of 6.6.1 -- the Battlez seed chooser above all -- run the engine's own
 * linear string lookups, which call memcmp, wcscmp, __aeabi_memclr8 and memcpy around
 * 150,000 times per frame. Trapping that often held the game at 22-25 fps.
 *
 * For these functions the trampoline is rewritten from "SVC #idx" to a branch
 * into a short ARM routine placed in guest memory (memmap::kFastPathAddr), so
 * the JIT compiles the work inline. Sizes above 256 bytes still go to the host
 * handler: each routine ends in the original "SVC #idx" and branches to it
 * before touching any argument, and the handler returns through LR as usual.
 * The encodings were checked with a disassembler; the comments show the result.
 */

#include <pvz2native/runtime/dynarmic_config.h>
#include <pvz2native/runtime/guest_memmap.h>
#include <pvz2native/log.h>

#include <cstdint>
#include <cstring>
#include <cstdlib>

namespace pvz2native {
namespace runtime {
namespace {

/* int memcmp(const void *a, const void *b, size_t n) -- byte difference, like bionic. */
constexpr std::uint32_t kMemcmp[] = {
    0xE3520C01u, /*  0: cmp r2, #0x100 */
    0x8A00000Bu, /*  1: bhi 14 (host) */
    0xE3520000u, /*  2: cmp r2, #0 */
    0x03A00000u, /*  3: moveq r0, #0 */
    0x012FFF1Eu, /*  4: bxeq lr */
    0xE4D03001u, /*  5: ldrb r3, [r0], #1 */
    0xE4D1C001u, /*  6: ldrb ip, [r1], #1 */
    0xE053300Cu, /*  7: subs r3, r3, ip */
    0x11A00003u, /*  8: movne r0, r3 */
    0x112FFF1Eu, /*  9: bxne lr */
    0xE2522001u, /* 10: subs r2, r2, #1 */
    0x1AFFFFF8u, /* 11: bne 5 */
    0xE3A00000u, /* 12: mov r0, #0 */
    0xE12FFF1Eu, /* 13: bx lr */
    0xEF000000u, /* 14: svc #idx (filled in) */
};

/* int wcscmp(const wchar_t *a, const wchar_t *b) -- 32-bit units compared unsigned,
 * -1/0/1, the same as the host handler (libc::compare_wide). */
constexpr std::uint32_t kWcscmp[] = {
    0xE4902004u, /*  0: ldr r2, [r0], #4 */
    0xE4913004u, /*  1: ldr r3, [r1], #4 */
    0xE1520003u, /*  2: cmp r2, r3 */
    0x1A000003u, /*  3: bne 8 */
    0xE3520000u, /*  4: cmp r2, #0 */
    0x1AFFFFF9u, /*  5: bne 0 */
    0xE3A00000u, /*  6: mov r0, #0 */
    0xE12FFF1Eu, /*  7: bx lr */
    0x33E00000u, /*  8: mvnlo r0, #0 */
    0x83A00001u, /*  9: movhi r0, #1 */
    0xE12FFF1Eu, /* 10: bx lr */
};

/* void __aeabi_memclr{,4,8}(void *dst, size_t n) -- words while aligned, then bytes. */
constexpr std::uint32_t kMemclr[] = {
    0xE3510C01u, /*  0: cmp r1, #0x100 */
    0x8A00000Du, /*  1: bhi 16 (host) */
    0xE3A02000u, /*  2: mov r2, #0 */
    0xE3100003u, /*  3: tst r0, #3 */
    0x1A000004u, /*  4: bne 10 */
    0xE3510004u, /*  5: cmp r1, #4 */
    0x3A000002u, /*  6: blo 10 */
    0xE4802004u, /*  7: str r2, [r0], #4 */
    0xE2411004u, /*  8: sub r1, r1, #4 */
    0xEAFFFFFAu, /*  9: b 5 */
    0xE3510000u, /* 10: cmp r1, #0 */
    0x012FFF1Eu, /* 11: bxeq lr */
    0xE4C02001u, /* 12: strb r2, [r0], #1 */
    0xE2511001u, /* 13: subs r1, r1, #1 */
    0x1AFFFFFCu, /* 14: bne 12 */
    0xE12FFF1Eu, /* 15: bx lr */
    0xEF000000u, /* 16: svc #idx (filled in) */
};

/* void *memcpy/memmove(void *dst, const void *src, size_t n) and the __aeabi_
 * forms -- memmove semantics for all of them, like the host handler (the engine
 * overlaps these in practice): forward when dst <= src, backward otherwise.
 * Returns dst. */
constexpr std::uint32_t kMemmove[] = {
    0xE3520C01u, /*  0: cmp r2, #0x100 */
    0x8A00000Eu, /*  1: bhi 17 (host) */
    0xE1A0C000u, /*  2: mov ip, r0 */
    0xE1500001u, /*  3: cmp r0, r1 */
    0x8A000004u, /*  4: bhi 10 */
    0xE2522001u, /*  5: subs r2, r2, #1 */
    0x24D13001u, /*  6: ldrbhs r3, [r1], #1 */
    0x24CC3001u, /*  7: strbhs r3, [ip], #1 */
    0x2AFFFFFBu, /*  8: bhs 5 */
    0xE12FFF1Eu, /*  9: bx lr */
    0xE0811002u, /* 10: add r1, r1, r2 */
    0xE08CC002u, /* 11: add ip, ip, r2 */
    0xE2522001u, /* 12: subs r2, r2, #1 */
    0x25713001u, /* 13: ldrbhs r3, [r1, #-1]! */
    0x256C3001u, /* 14: strbhs r3, [ip, #-1]! */
    0x2AFFFFFBu, /* 15: bhs 12 */
    0xE12FFF1Eu, /* 16: bx lr */
    0xEF000000u, /* 17: svc #idx (filled in) */
};

struct FastPath {
    const char *name;
    const std::uint32_t *code;
    std::uint32_t words;
    bool host_fallback; /* last word is the "SVC #idx" back to the host handler */
};

constexpr FastPath kFastPaths[] = {
    {"memcmp", kMemcmp, sizeof(kMemcmp) / 4, true},
    {"wcscmp", kWcscmp, sizeof(kWcscmp) / 4, false},
    {"__aeabi_memclr", kMemclr, sizeof(kMemclr) / 4, true},
    {"__aeabi_memclr4", kMemclr, sizeof(kMemclr) / 4, true},
    {"__aeabi_memclr8", kMemclr, sizeof(kMemclr) / 4, true},
    {"memcpy", kMemmove, sizeof(kMemmove) / 4, true},
    {"memmove", kMemmove, sizeof(kMemmove) / 4, true},
    {"__aeabi_memcpy", kMemmove, sizeof(kMemmove) / 4, true},
    {"__aeabi_memcpy4", kMemmove, sizeof(kMemmove) / 4, true},
    {"__aeabi_memcpy8", kMemmove, sizeof(kMemmove) / 4, true},
    {"__aeabi_memmove", kMemmove, sizeof(kMemmove) / 4, true},
    {"__aeabi_memmove4", kMemmove, sizeof(kMemmove) / 4, true},
    {"__aeabi_memmove8", kMemmove, sizeof(kMemmove) / 4, true},
};

/* --- the seed chooser's card grid (6.6.1) ---------------------------------------
 *
 * The seed chooser draws its plant cards from a vector it owns (begin at
 * this+0x154, end at this+0x158, 64 bytes per entry; an entry starts with the
 * card's x and y inside the scrolling grid as floats). The loop positions and
 * draws EVERY card each frame -- a hundred animated plants in Battlez, where the
 * whole collection is on offer -- although the panel shows five rows of five and
 * clips the rest away. That cost ~37 million guest instructions a frame, which
 * held the screen at 25-30 fps here; with only the cards in view it is ~10.
 *
 * The first instruction of the loop body is redirected to kCardCull, which skips
 * an entry whose row is clearly outside the panel:
 *     rel = entry.y - scroll            (scroll: int at this+0x174)
 *     skip when rel > panel height + 60 (height: int at this+0x1e8)
 *            or rel < -260              (a row is 165 tall)
 * and otherwise runs the instruction it replaced. A skipped card is not moved
 * either, so it keeps the position it had when it was last drawn, which the
 * margins put outside the panel.
 *
 * The site is matched by its original instruction words, so this stays off on
 * any other build of the game (4.5.2 does not have this loop). */
constexpr std::uint32_t kCardLoopSite = 0x009b704c;   /* bl 0xc54198: first instruction of the loop body */
constexpr std::uint32_t kCardLoopWord = 0xEB0A7451u;
constexpr std::uint32_t kCardLoopCallee = 0x00c54198;
constexpr std::uint32_t kCardNextSite = 0x009b73e0;   /* add sl, sl, #0x40: on to the next entry */
constexpr std::uint32_t kCardNextWord = 0xE28AA040u;
constexpr std::uint32_t kCardScrollUse = 0x009b70b8;  /* vldr s4, [r0, #0x174]: the loop reading the scroll itself */
constexpr std::uint32_t kCardScrollWord = 0xED902A5Du;

constexpr std::uint32_t kCardCull[] = {
    0xE59DC020u, /*  0: ldr ip, [sp, #0x20]       this */
    0xED9A0A01u, /*  1: vldr s0, [sl, #4]         entry.y */
    0xE59C0174u, /*  2: ldr r0, [ip, #0x174]      scroll */
    0xEE000A90u, /*  3: vmov s1, r0 */
    0xEEF80AE0u, /*  4: vcvt.f32.s32 s1, s1 */
    0xEE300A60u, /*  5: vsub.f32 s0, s0, s1       rel */
    0xE59C01E8u, /*  6: ldr r0, [ip, #0x1e8]      panel height */
    0xEE000A90u, /*  7: vmov s1, r0 */
    0xEEF80AE0u, /*  8: vcvt.f32.s32 s1, s1 */
    0xED9F1A0Du, /*  9: vldr s2, [pc, #0x34]      60.0 */
    0xEE700A81u, /* 10: vadd.f32 s1, s1, s2 */
    0xEEB40AE0u, /* 11: vcmpe.f32 s0, s1 */
    0xEEF1FA10u, /* 12: vmrs apsr_nzcv, fpscr */
    0xCA000006u, /* 13: bgt 21                    below the panel */
    0xEDDF0A07u, /* 14: vldr s1, [pc, #0x1c]      -260.0 */
    0xEEB40AE0u, /* 15: vcmpe.f32 s0, s1 */
    0xEEF1FA10u, /* 16: vmrs apsr_nzcv, fpscr */
    0xBA000002u, /* 17: blt 21                    above the panel */
    0xE59FE008u, /* 18: ldr lr, [pc, #8]          return into the loop body */
    0xEA000000u, /* 19: b <the replaced call>     (filled in) */
    0xE1A00000u, /* 20: nop */
    0xEA000000u, /* 21: b <next entry>            (filled in) */
    0x00000000u, /* 22: loop body return address  (filled in) */
    0xC3820000u, /* 23: -260.0f */
    0x42700000u, /* 24: 60.0f */
};

void write_word(pvz2_elf_image_t *img, std::uint32_t addr, std::uint32_t v) {
    std::memcpy(img->mem + addr, &v, 4);
}

std::uint32_t branch(std::uint32_t from, std::uint32_t to) {
    return 0xEA000000u | (((to - (from + 8)) >> 2) & 0x00FFFFFFu);
}

std::uint32_t word_at(const pvz2_elf_image_t *img, std::uint32_t addr) {
    std::uint32_t v = 0;
    std::memcpy(&v, img->mem + addr, 4);
    return v;
}

void install_card_cull(pvz2_elf_image_t *img, std::uint32_t at) {
    const std::uint32_t base = img->so_base;
    if (base + kCardNextSite + 4 > img->mem_size) return;
    if (word_at(img, base + kCardLoopSite) != kCardLoopWord || word_at(img, base + kCardNextSite) != kCardNextWord ||
        word_at(img, base + kCardScrollUse) != kCardScrollWord) {
        return; /* not this build */
    }
    const std::uint32_t words = sizeof(kCardCull) / 4;
    if (at + words * 4 > memmap::kFastPathAddr + memmap::kFastPathSize) return;
    for (std::uint32_t w = 0; w < words; ++w) write_word(img, at + w * 4, kCardCull[w]);
    write_word(img, at + 19 * 4, branch(at + 19 * 4, base + kCardLoopCallee));
    write_word(img, at + 21 * 4, branch(at + 21 * 4, base + kCardNextSite));
    write_word(img, at + 22 * 4, base + kCardLoopSite + 4);
    write_word(img, base + kCardLoopSite, branch(base + kCardLoopSite, at));
    pvz2_log("[jit] seed chooser draws only the cards in view");
}

}  // namespace

void install_guest_fastpaths(pvz2_elf_image_t *img) {
    std::uint32_t at = memmap::kFastPathAddr;
    unsigned installed = 0;
    for (const FastPath &fp : kFastPaths) {
        std::uint32_t idx = img->trampoline_count;
        for (std::uint32_t i = 0; i < img->trampoline_count; ++i) {
            if (img->trampoline_names[i] && std::strcmp(img->trampoline_names[i], fp.name) == 0) {
                idx = i;
                break;
            }
        }
        if (idx == img->trampoline_count) continue; /* this build does not import it */
        if (at + fp.words * 4 > memmap::kFastPathAddr + memmap::kFastPathSize) {
            pvz2_log("[jit] fast path area full, %s stays a host call", fp.name);
            break;
        }
        for (std::uint32_t w = 0; w < fp.words; ++w) write_word(img, at + w * 4, fp.code[w]);
        if (fp.host_fallback) write_word(img, at + (fp.words - 1) * 4, 0xEF000000u | (idx & 0x00FFFFFFu));
        /* The trampoline itself: B to the routine (both live in the low 1 MB). */
        const std::uint32_t slot = img->trampoline_base + idx * 4;
        const std::uint32_t off = ((at - (slot + 8)) >> 2) & 0x00FFFFFFu;
        write_word(img, slot, 0xEA000000u | off);
        at += fp.words * 4;
        ++installed;
    }
    install_card_cull(img, at);
    pvz2_log("[jit] %u hot imports run inside the JIT (memcmp, wcscmp, memcpy/memmove, __aeabi_memclr*)", installed);
}

}  // namespace runtime
}  // namespace pvz2native
