/* Guest-code patches -- see include/pvz2native/game/patches.h for why. */

#include <pvz2native/game/patches.h>

#include <pvz2native/config.h>
#include <pvz2native/dependencies/dependency.h>
#include <pvz2native/dex/dex.h>
#include <pvz2native/game/symbols.h>
#include <pvz2native/diagnostics/diagnostics.h>
#include <pvz2native/host_time.h>
#include <pvz2native/diagnostics/verbose.h>
#include <pvz2native/log.h>
#include <pvz2native/presence.h>
#include <pvz2native/credits_screen.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

namespace pvz2native {
namespace {

/* ARM (never Thumb here -- the engine's ARM32 code in both mapped builds is
 * A32): cond 1011 imm24 is BL. Masking the condition off means a conditional
 * BL still matches, and B (1010) still does not. */
constexpr std::uint32_t kOpcodeMask = 0x0F000000u;
constexpr std::uint32_t kOpcodeBl   = 0x0B000000u;

/* MOV R0, #1 -- the value the patched-out call used to return for "online". */
constexpr std::uint32_t kMovR0One = 0xE3A00001u;

/* ARM NOP (MOV R0, R0) -- used to neutralize early-exit BNEs on LoD apply. */
constexpr std::uint32_t kNopMovR0R0 = 0xE1A00000u;

/* The two receipt instructions, before and after. MOV R1,#1 -> #0 turns "POST
 * this receipt and wait" into "finish it now"; MOV R0,#0 -> #1 turns that
 * branch's verdict from "not validated" into "validated". */
constexpr std::uint32_t kMovR1One  = 0xE3A01001u;
constexpr std::uint32_t kMovR1Zero = 0xE3A01000u;
constexpr std::uint32_t kMovR0Zero = 0xE3A00000u;

bool in_image(const pvz2_elf_image_t *img, std::uint32_t offset) {
    const std::uint64_t addr = (std::uint64_t)img->so_base + offset;
    return offset != 0 && addr + 4 <= img->mem_size;
}

std::uint32_t read_word(const pvz2_elf_image_t *img, std::uint32_t offset) {
    std::uint32_t w = 0;
    std::memcpy(&w, &img->mem[img->so_base + offset], 4);
    return w;
}

void write_word(pvz2_elf_image_t *img, std::uint32_t offset, std::uint32_t word) {
    std::memcpy(&img->mem[img->so_base + offset], &word, 4);
}

/* Every probe below replaces one instruction, the `PUSH {regs, lr}` at a function's entry, with an SVC into
 * the host. The handler has to leave the guest as that PUSH would have: registers stored in ascending order
 * with `saved_lr` above them, sp lowered to match. The caller passes the real lr to run the function
 * unchanged, or an exit stub to see it return. Execution then resumes at the next instruction. */
void emulate_push(GuestCall &c, std::initializer_list<int> regs, std::uint32_t saved_lr) {
    const std::uint32_t count = (std::uint32_t)regs.size();
    const std::uint32_t sp = c.sp() - 4 * (count + 1);
    std::uint32_t slot = sp;
    for (int r : regs) {
        c.write32(slot, c.regs[r]);
        slot += 4;
    }
    c.write32(slot, saved_lr);
    c.regs[13] = sp;
}

/* Rewrites one `BL <online?>` into `MOV R0, #1`, i.e. makes that one call site
 * read "connected" without changing what the callee returns to anyone else.
 * Refuses -- loudly -- if the instruction is not the BL the table promised,
 * because writing over something else would corrupt the engine silently. */
bool force_online_call(pvz2_elf_image_t *img, std::uint32_t offset, const char *kind) {
    if (!in_image(img, offset)) {
        pvz2_log("[patch] %s gate 0x%x is outside the image -- skipped", kind, offset);
        return false;
    }
    const std::uint32_t word = read_word(img, offset);
    if ((word & kOpcodeMask) != kOpcodeBl) {
        pvz2_log("[patch] %s gate 0x%x is not a BL (found 0x%08x) -- skipped; the "
                 "address in src/game/symbols.cpp is wrong for this build",
                 kind, offset, word);
        return false;
    }
    write_word(img, offset, kMovR0One);
    return true;
}

/* Same, but the call site is made to read "offline" (MOV R0, #0). */
bool force_offline_call(pvz2_elf_image_t *img, std::uint32_t offset, const char *kind) {
    if (!in_image(img, offset)) {
        pvz2_log("[patch] %s gate 0x%x is outside the image -- skipped", kind, offset);
        return false;
    }
    const std::uint32_t word = read_word(img, offset);
    if ((word & kOpcodeMask) != kOpcodeBl) {
        pvz2_log("[patch] %s gate 0x%x is not a BL (found 0x%08x) -- skipped; the "
                 "address in src/game/symbols.cpp is wrong for this build",
                 kind, offset, word);
        return false;
    }
    write_word(img, offset, kMovR0Zero);
    return true;
}

unsigned patch_gate_list(pvz2_elf_image_t *img, const std::uint32_t *gates, std::size_t count,
                         const char *kind, bool online = true) {
    unsigned patched = 0;
    for (std::size_t i = 0; i < count && gates[i] != 0; ++i) {
        if (online ? force_online_call(img, gates[i], kind) : force_offline_call(img, gates[i], kind))
            ++patched;
    }
    return patched;
}

/* Replaces one instruction, but only if it is exactly the one the table
 * promised -- a mismatch means the address belongs to another build and writing
 * over it would corrupt the engine silently, so it is reported and skipped. */
bool rewrite_word(pvz2_elf_image_t *img, std::uint32_t offset, std::uint32_t expect,
                  std::uint32_t replace, const char *what) {
    if (offset == 0) return false;
    if (!in_image(img, offset)) {
        pvz2_log("[patch] %s (0x%x) is outside the image -- skipped", what, offset);
        return false;
    }
    const std::uint32_t word = read_word(img, offset);
    if (word != expect) {
        pvz2_log("[patch] %s (0x%x) is 0x%08x, expected 0x%08x -- skipped; the address in "
                 "src/game/symbols.cpp is wrong for this build",
                 what, offset, word, expect);
        return false;
    }
    write_word(img, offset, replace);
    return true;
}

/* ARM BL encoding: PC-relative, PC = from + 8. */
std::uint32_t encode_bl(std::uint32_t from, std::uint32_t to) {
    const std::int32_t delta = (std::int32_t)(to - (from + 8));
    return 0xEB000000u | (((std::uint32_t)delta >> 2) & 0x00FFFFFFu);
}

/* 4.5.2: wire the LoD manager into the real apply (sub_3487AC) instead of the
 * BX LR stubs at sub_40F440, and NOP the two property-gate BNEs that otherwise
 * exit before schedule matching on this offline port. The map-tick BLEQ is left
 * alone -- forcing it unconditional invoked apply with r0=predicate (0/1). */
bool force_lod_apply(pvz2_elf_image_t *img) {
    bool any = false;
    /* BNE that skips the apply BL when the LevelOfTheDay toggle probe != 1. */
    if (rewrite_word(img, sym().patch.lod_apply_stub_skip_bne, 0x1A000001u, kNopMovR0R0,
                     "lod: stub skip BNE->NOP"))
        any = true;
    const std::uint32_t stub = sym().patch.lod_apply_stub_call;
    if (stub != 0) {
        /* Expected: BL sub_40F440. Replacement: BL real apply. */
        const std::uint32_t apply = sym().patch.lod_apply_fn;
        const std::uint32_t expect = encode_bl(stub, 0x40f440);
        const std::uint32_t replace = encode_bl(stub, apply != 0 ? apply : 0x3487ac);
        if (rewrite_word(img, stub, expect, replace, "lod: stub->apply"))
            any = true;
    }
    /* BNE after CMP R0,#1 (ready gate) -- expected imm24 encodes +0x21. */
    if (rewrite_word(img, sym().patch.lod_apply_ready_bne, 0x1A000021u, kNopMovR0R0,
                     "lod: ready BNE->NOP"))
        any = true;
    /* BNE after second property gate inside sub_40F44C. */
    if (rewrite_word(img, sym().patch.lod_apply_match_bne, 0x1A000060u, kNopMovR0R0,
                     "lod: match BNE->NOP"))
        any = true;
    /* BNE when profile +0x178 < +0x184 -- sticky Active LoD after a write that
     * refreshed alod/end but not the "last scheduled" field. */
    if (pvz2_config()->lod_force_repeat &&
        rewrite_word(img, sym().patch.lod_match_progress_bne, 0x1A000035u, kNopMovR0R0,
                     "lod: progress BNE->NOP"))
        any = true;
    /* BLS skipping UID writer when schedule_uid <= +0x178. */
    if (pvz2_config()->lod_force_repeat &&
        rewrite_word(img, sym().patch.lod_match_uid_bls, 0x9A000008u, kNopMovR0R0,
                     "lod: uid-order BLS->NOP"))
        any = true;
    return any;
}

/* "Is the engine clock synced with the game server?" (sub_81E948: `VLDR S0,[R0,#0x48]; MOV R0,#0; VCMPE; VMRS; MOVWNE R0,#1;
 * BX LR`, true when the server-time offset at clock+0x48 is non-zero). The port has no server, so it was always false, and
 * the world-map banner then never finds the next Level of the Day (sub_4117D4 returns null at once), so the countdown to the
 * next piñata ("10h 32m 33s hasta la siguiente fiesta") was never shown. The engine's own "now" (sub_81E960) is already
 * replaced by game_install_engine_clock with what the original returned once synced, so the flag is made to say so:
 * `MOV R0,#1; BX LR`. Also read by sub_69F830 (time left of the active event), which then takes its synced branch. */
bool enable_company_link_all_locales(pvz2_elf_image_t *img) {
    bool ok = false;
    const std::uint32_t fn = sym().patch.company_link_allowed_fn;
    if (fn != 0 && in_image(img, fn + 4)) {
        write_word(img, fn, kMovR0One);
        write_word(img, fn + 4, 0xE12FFF1Eu); /* BX LR */
        ok = true;
    }
    const std::uint32_t branch = sym().patch.company_button_skip_branch;
    if (branch != 0 && in_image(img, branch)) {
        write_word(img, branch, kNopMovR0R0); /* NOP the skip branch so the cloud button is always instantiated */
        ok = true;
    }
    return ok;
}

bool force_clock_synced(pvz2_elf_image_t *img) {
    const std::uint32_t fn = sym().patch.clock_synced_fn;
    if (fn == 0 || !in_image(img, fn + 4)) return false;
    if (read_word(img, fn) != 0xED900A12u || read_word(img, fn + 4) != kMovR0Zero) {
        pvz2_log("[patch] clock synced (0x%x) is not the expected function -- skipped; the address in "
                 "src/game/symbols.cpp is wrong for this build",
                 fn);
        return false;
    }
    write_word(img, fn, kMovR0One);
    write_word(img, fn + 4, 0xE12FFF1Eu); /* BX LR */
    return true;
}

/* Makes a completed payment finish locally AND finish as validated. Both halves
 * are required: the first alone leaves the engine at its "Unable to contact
 * store" dialog, because the branch it selects is the one that records a receipt
 * as unvalidated. */
bool accept_receipt_locally(pvz2_elf_image_t *img) {
    const bool finish_now = rewrite_word(img, sym().patch.purchase_local_receipt, kMovR1One,
                                         kMovR1Zero, "receipt: finish without the server");
    const bool verdict = rewrite_word(img, sym().patch.purchase_receipt_verdict, kMovR0Zero,
                                      kMovR0One, "receipt: verdict");
    if (finish_now == verdict) return finish_now;

    /* Exactly one landed. Say so plainly rather than leaving a half-patched
     * flow to be diagnosed from the game's behaviour. */
    pvz2_log("[patch] only half of the receipt patch applied -- purchases will %s",
             finish_now ? "report \"Unable to contact store\""
                        : "hang waiting for server validation");
    return false;
}

static std::string s_locked_boss_name;

void zomboss_stage_hook(GuestCall &call) {
    const std::uint32_t zomboss = call.arg(0);
    const std::uint32_t stage = call.arg(1);
    if (pvz2_presence_enabled()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Board BossPhase=%u", stage + 1);
        pvz2_presence_note("state", buf);

        std::string detected_boss;
        if (stage == 0) {
            s_locked_boss_name.clear();
        } else if (stage >= 3) {
            detected_boss = "eighties";
            s_locked_boss_name = "eighties";
        } else if (!s_locked_boss_name.empty()) {
            detected_boss = s_locked_boss_name;
        }

        if (detected_boss.empty() && zomboss != 0 && call.in_bounds(zomboss, 0x800)) {
            // Check RtWeakString at zomboss + 0x50
            if (call.in_bounds(zomboss + 0x50, 8)) {
                const std::uint32_t pstr = call.read32(zomboss + 0x50);
                if (pstr > 0x10000 && call.in_bounds(pstr, 16)) {
                    std::string s = call.cstr(pstr, 64);
                    for (char &c : s) c = (char)std::tolower((unsigned char)c);
                    if (s.find("eighties") != std::string::npos) detected_boss = "eighties";
                    else if (s.find("pirate") != std::string::npos) detected_boss = "pirate";
                    else if (s.find("cowboy") != std::string::npos) detected_boss = "cowboy";
                    else if (s.find("future") != std::string::npos) detected_boss = "future";
                    else if (s.find("dark") != std::string::npos) detected_boss = "dark";
                    else if (s.find("beach") != std::string::npos) detected_boss = "beach";
                    else if (s.find("iceage") != std::string::npos) detected_boss = "iceage";
                    else if (s.find("lostcity") != std::string::npos) detected_boss = "lostcity";
                    else if (s.find("dino") != std::string::npos) detected_boss = "dino";
                    else if (s.find("egypt") != std::string::npos) detected_boss = "egypt";
                }
            }
            // Check pointers inside the object for boss identifiers
            for (std::uint32_t off = 0; detected_boss.empty() && off < 0x800; off += 4) {
                const std::uint32_t val = call.read32(zomboss + off);
                if (val > 0x10000 && call.in_bounds(val, 16)) {
                    std::string s = call.cstr(val, 64);
                    if (!s.empty()) {
                        for (char &c : s) c = (char)std::tolower((unsigned char)c);
                        if (s.find("eighties") != std::string::npos ||
                            s.find("multiescenario") != std::string::npos) {
                            detected_boss = "eighties";
                            break;
                        } else if (s.find("pirate") != std::string::npos) {
                            detected_boss = "pirate";
                        } else if (s.find("cowboy") != std::string::npos) {
                            detected_boss = "cowboy";
                        } else if (s.find("future") != std::string::npos) {
                            detected_boss = "future";
                        } else if (s.find("dark") != std::string::npos) {
                            detected_boss = "dark";
                        } else if (s.find("beach") != std::string::npos) {
                            detected_boss = "beach";
                        } else if (s.find("iceage") != std::string::npos) {
                            detected_boss = "iceage";
                        } else if (s.find("lostcity") != std::string::npos) {
                            detected_boss = "lostcity";
                        } else if (s.find("dino") != std::string::npos) {
                            detected_boss = "dino";
                        } else if (s.find("egypt") != std::string::npos) {
                            detected_boss = "egypt";
                        }
                    }
                }
            }
            // Also check inline bytes on stage 0 in case strings are short/inline
            if (detected_boss.empty() && stage == 0) {
                for (std::uint32_t off = 0; off + 8 <= 0x800; ++off) {
                    char b[9];
                    for (int k = 0; k < 8; ++k) b[k] = (char)std::tolower((unsigned char)call.read8(zomboss + off + k));
                    b[8] = '\0';
                    if (std::memcmp(b, "eighties", 8) == 0) {
                        detected_boss = "eighties";
                        break;
                    }
                }
            }
            if (!detected_boss.empty() && s_locked_boss_name.empty()) {
                s_locked_boss_name = detected_boss;
            }
        }

        if (!detected_boss.empty()) {
            pvz2_presence_note("state", ("Board BossName=" + detected_boss).c_str());
            if (detected_boss == "eighties") {
                pvz2_presence_note("state", "Board EightiesBoss=true");
            }
        }
    }
    call.returns = false;
    call.regs[15] = call.img->so_base + sym().patch.zomboss_setup_stage;
}

bool patch_call_site(pvz2_elf_image_t *img, std::uint32_t call_offset, std::uint32_t target_addr) {
    if (!in_image(img, call_offset)) return false;
    const std::uint32_t word = read_word(img, call_offset);
    if ((word & kOpcodeMask) != kOpcodeBl) {
        std::printf("pvz2: [patch] call site 0x%x is not a BL (found 0x%08x)\n", call_offset, word);
        return false;
    }
    const std::uint32_t pc = img->so_base + call_offset;
    const std::int32_t diff = (std::int32_t)target_addr - (std::int32_t)(pc + 8);
    const std::uint32_t imm24 = ((std::uint32_t)(diff >> 2)) & 0x00FFFFFFu;
    const std::uint32_t new_bl = 0xEB000000u | imm24;
    write_word(img, call_offset, new_bl);
    return true;
}

}  // namespace

void game_apply_patches(pvz2_elf_image_t *img) {
    if (img == nullptr) return;

    /* LoD/CDN sites are independent of the emulated store: they separate "apply
     * schedule while online" from GetNetworkStatus during boot (wifi-global
     * hangs the loading screen on resumable/PvZ2Web waits). */
    {
        const std::uint32_t *gates = sym().patch.lod_cdn_online_gate;
        const std::size_t count = sizeof(sym().patch.lod_cdn_online_gate) / sizeof(gates[0]);
        const unsigned n = patch_gate_list(img, gates, count, "lod/cdn");
        if (n != 0) {
            pvz2_log("[patch] lod/cdn: %u connectivity check(s) forced to \"online\" "
                     "so LoD can apply while boot stays offline",
                     n);
        }
    }

    /* LoD schedule commit (alod / wm) -- independent of connectivity patches. */
    if (force_lod_apply(img)) {
        pvz2_log("[patch] lod apply: stub->real apply, ready/match gates NOP'd so schedule can "
                 "commit Active LoD (tick BLEQ left alone)%s",
                 pvz2_config()->lod_force_repeat
                     ? "; force-repeat: progress + uid-order gates NOP'd, completed-field reset on"
                     : "; the engine's own completed-level rules are kept");
    }

    if (force_clock_synced(img)) {
        pvz2_log("[patch] engine clock reports \"synced\" so the world map can find the next Level of the Day "
                 "(countdown to the next piñata)");
    }

    if (enable_company_link_all_locales(img)) {
        pvz2_log("[patch] company link check forced to true (shows \"P\" Cloud Save button in all locales including French)");
    }

    /* With the emulated store off the engine is meant to find no store at all,
     * and "no connection" is a perfectly good reason for it to say so. The
     * broker's connectivity checks read GetNetworkStatus, which is promoted to
     * wifi after boot (so LoD/CDN work); left alone they would let a purchase
     * start, RequestPayment is ignored and the dialog would sit on "Buying..."
     * forever. Force them offline instead. */
    if (pvz2_config()->emulate_iap == 0) {
        const std::uint32_t *gates = sym().patch.purchase_online_gate;
        const std::size_t count = sizeof(sym().patch.purchase_online_gate) / sizeof(gates[0]);
        const unsigned n = patch_gate_list(img, gates, count, "purchase", false);
        if (n != 0) {
            pvz2_log("[patch] purchase broker: %u connectivity check(s) forced to \"offline\" "
                     "(emulate_iap=0) so Buy reports \"no connection\" instead of hanging",
                     n);
        }
        return;
    }

    {
        const std::uint32_t *gates = sym().patch.purchase_online_gate;
        const std::size_t count = sizeof(sym().patch.purchase_online_gate) / sizeof(gates[0]);
        const unsigned patched = patch_gate_list(img, gates, count, "purchase");
        unsigned listed = 0;
        for (std::size_t i = 0; i < count && gates[i] != 0; ++i) ++listed;
        if (patched != 0) {
            pvz2_log("[patch] purchase broker: %u connectivity check(s) forced to \"online\" "
                     "so the emulated store is reachable offline",
                     patched);
        }
        if (patched < listed) {
            pvz2_log("[patch] %u purchase gate(s) left alone -- buying will report "
                     "\"Service Unavailable\"",
                     listed - patched);
        }
    }

    if (accept_receipt_locally(img)) {
        pvz2_log("[patch] purchase receipts are accepted locally, exactly as a validation "
                 "server answering \"passed\"");
    }
}

void game_apply_late_patches(pvz2_elf_image_t *img) {
    if (img == nullptr) return;
    if (sym().patch.zomboss_setup_stage == 0) return;

    const std::uint32_t trampoline =
        make_guest_callback("ZombieZombossMech.SetupStage", zomboss_stage_hook);
    if (trampoline == 0) {
        std::printf("pvz2: [patch] failed to allocate trampoline for ZombieZombossMech.SetupStage\n");
        return;
    }

    if (sym().patch.zomboss_stage_init_call != 0) {
        patch_call_site(img, sym().patch.zomboss_stage_init_call, trampoline);
    }
    if (sym().patch.zomboss_stage_change_call != 0) {
        patch_call_site(img, sym().patch.zomboss_stage_change_call, trampoline);
    }
}

namespace {

/* Runtime probes for sub_3487AC / sub_40F44C. Entry is overwritten with SVC
 * (same index as a make_guest_callback stub). The handler emulates the original
 * PUSH but parks an exit trampoline as the saved LR so the matching POP reports
 * the return value without a nested call_guest (which would lose stack args). */
constexpr std::uint32_t kLodApplyPush = 0xE92D4070u; /* PUSH {r4,r5,r6,lr} */
constexpr std::uint32_t kLodMatchPush = 0xE92D4BF0u; /* PUSH {r4-r9,r11,lr} */
constexpr std::uint32_t kLodWriterPush = 0xE92D4010u; /* PUSH {r4,lr} */

/* Profile/LoD object fields touched by sub_69F8DC / getters next to it. */
constexpr std::uint32_t kLodOffPrev = 0x178;
constexpr std::uint32_t kLodOffAlod = 0x184;
constexpr std::uint32_t kLodOffWmish = 0x190;

struct LodProbeFrame {
    std::uint32_t lr = 0;
    std::uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
    std::uint32_t seq = 0;
    std::uint32_t pre_178 = 0, pre_184 = 0, pre_190 = 0;
    bool obj_ok = false;
    std::chrono::steady_clock::time_point t0{};
};

thread_local std::vector<LodProbeFrame> g_lod_apply_frames;
thread_local std::vector<LodProbeFrame> g_lod_match_frames;
std::uint32_t g_lod_apply_exit_stub = 0;
std::uint32_t g_lod_match_exit_stub = 0;
std::atomic<std::uint32_t> g_lod_apply_seq{0};
std::atomic<std::uint32_t> g_lod_match_seq{0};

bool lod_obj_readable(GuestCall &c, std::uint32_t obj) {
    return obj >= 0x01000000u && c.in_bounds(obj, 0x200);
}

bool lod_interesting_str(const std::string &s) {
    if (s == "none" || s == "default" || s == "Default") return true;
    if (s.size() >= 5 && s.compare(0, 4, "lod_") == 0) return true;
    return false;
}

/* Dump alod-ish dwords + any nearby cstring/"none"/"lod_*" pointers or SSO. */
void dump_lod_object_state(GuestCall &c, std::uint32_t obj, const char *tag, std::uint32_t pre_178,
                           std::uint32_t pre_184, std::uint32_t pre_190, bool have_pre) {
    if (!lod_obj_readable(c, obj)) {
        c.log("[lod-apply] %s obj=0x%08x not a dumpable pointer", tag, obj);
        return;
    }
    const std::uint32_t v178 = c.read32(obj + kLodOffPrev);
    const std::uint32_t v184 = c.read32(obj + kLodOffAlod);
    const std::uint32_t v190 = c.read32(obj + kLodOffWmish);
    if (have_pre) {
        c.log("[lod-apply] %s obj=0x%08x +0x178=%u->%u +0x184(alod?)=%u->%u +0x190=%u->%u", tag,
              obj, pre_178, v178, pre_184, v184, pre_190, v190);
    } else {
        c.log("[lod-apply] %s obj=0x%08x +0x178=%u +0x184(alod?)=%u +0x190=%u", tag, obj, v178,
              v184, v190);
    }

    unsigned hits = 0;
    for (std::uint32_t off = 0; off < 0x200 && hits < 12; off += 4) {
        /* Inline short string (SSO / embedded). */
        if (c.in_bounds(obj + off, 8)) {
            const std::string inline_s = c.cstr(obj + off, 16);
            if (lod_interesting_str(inline_s)) {
                c.log("[lod-apply] %s obj+0x%x inline \"%s\"", tag, off, inline_s.c_str());
                ++hits;
                continue;
            }
        }
        const std::uint32_t p = c.read32(obj + off);
        if (!c.in_bounds(p, 8)) continue;
        const std::string s = c.cstr(p, 32);
        if (!lod_interesting_str(s)) continue;
        c.log("[lod-apply] %s obj+0x%x -> \"%s\" (ptr=0x%08x)", tag, off, s.c_str(), p);
        ++hits;
    }
}

void lod_apply_exit(GuestCall &c) {
    if (g_lod_apply_frames.empty()) {
        c.log("[lod-apply] EXIT without matching ENTER (r0=0x%08x lr=0x%08x) -- probe stack "
              "corrupt?",
              c.arg(0), c.lr());
        return;
    }
    const LodProbeFrame fr = g_lod_apply_frames.back();
    g_lod_apply_frames.pop_back();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                             fr.t0)
                        .count();
    const std::time_t wall = (std::time_t)(std::time(nullptr) + pvz2_time_offset_sec());
    c.log("[lod-apply] EXIT  #%u r0=0x%08x (%.3f ms) wall=%lld caller_lr=0x%08x "
          "enter_args=(0x%08x,0x%08x,0x%08x,0x%08x)",
          fr.seq, c.arg(0), ms, (long long)wall, fr.lr, fr.r0, fr.r1, fr.r2, fr.r3);
    if (fr.obj_ok) {
        dump_lod_object_state(c, fr.r0, "post", fr.pre_178, fr.pre_184, fr.pre_190, true);
    } else if (lod_obj_readable(c, fr.r0)) {
        dump_lod_object_state(c, fr.r0, "post", 0, 0, 0, false);
    }
    c.regs[14] = fr.lr;
}

void lod_apply_enter(GuestCall &c) {
    LodProbeFrame fr;
    fr.lr = c.lr();
    fr.r0 = c.arg(0);
    fr.r1 = c.arg(1);
    fr.r2 = c.arg(2);
    fr.r3 = c.arg(3);
    fr.seq = g_lod_apply_seq.fetch_add(1) + 1;
    fr.t0 = std::chrono::steady_clock::now();
    fr.obj_ok = lod_obj_readable(c, fr.r0);
    if (fr.obj_ok) {
        fr.pre_178 = c.read32(fr.r0 + kLodOffPrev);
        fr.pre_184 = c.read32(fr.r0 + kLodOffAlod);
        fr.pre_190 = c.read32(fr.r0 + kLodOffWmish);
    }
    g_lod_apply_frames.push_back(fr);

    const std::time_t wall = (std::time_t)(std::time(nullptr) + pvz2_time_offset_sec());
    c.log("[lod-apply] ENTER #%u r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x lr=0x%08x "
          "sp=0x%08x wall=%lld",
          fr.seq, fr.r0, fr.r1, fr.r2, fr.r3, fr.lr, c.sp(), (long long)wall);
    if (fr.obj_ok) {
        dump_lod_object_state(c, fr.r0, "pre", fr.pre_178, fr.pre_184, fr.pre_190, false);
    }

    /* PUSH {r4,r5,r6,lr} with the exit stub as the saved return address. */
    emulate_push(c, {4, 5, 6}, g_lod_apply_exit_stub);
    /* Resume at the instruction after the SVC we planted (original SUB sp,#8). */
    c.no_return();
}

/* Profile object is r1 (sb) in sub_40F44C -- r0 is the LoD props/manager. */
constexpr std::uint32_t kLodMaxSaneEndSkewSec = 2u * 86400u; /* LoD days are 1-long */

void lod_match_exit(GuestCall &c) {
    if (g_lod_match_frames.empty()) {
        c.log("[lod-match] EXIT without matching ENTER (r0=0x%08x lr=0x%08x) -- probe stack "
              "corrupt?",
              c.arg(0), c.lr());
        return;
    }
    const LodProbeFrame fr = g_lod_match_frames.back();
    g_lod_match_frames.pop_back();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                             fr.t0)
                        .count();
    const std::time_t wall = (std::time_t)(std::time(nullptr) + pvz2_time_offset_sec());

    /* sub_40F44C keeps the out-byte in R6 (= original R2). The path that
     * actually runs the UID writer (BL 69F8DC) then falls into the epilogue
     * with the writer's R0 (=0) and never executes the success STRB R0,[R6]
     * at 0x40F564. Apply reads that stack byte to arm the map LoD strip --
     * so a fresh commit looks "applied" in the profile but the banner stays
     * dark. Force the out-flag + return when Active LoD landed. */
    if (fr.obj_ok && lod_obj_readable(c, fr.r1)) {
        const std::uint32_t alod = c.read32(fr.r1 + kLodOffAlod);
        const std::uint32_t end = c.read32(fr.r1 + kLodOffWmish);
        const std::uint32_t now = (std::uint32_t)(std::time(nullptr) + pvz2_time_offset_sec());
        const bool active = alod != 0 && end > now && (end - now) <= kLodMaxSaneEndSkewSec;
        if (active) {
            if (c.in_bounds(fr.r2, 1)) {
                const std::uint8_t prev = c.read8(fr.r2);
                if (prev != 1) {
                    c.write8(fr.r2, 1);
                    c.log("[lod-match] force out-flag @0x%08x %u -> 1 (alod=%u end=%u)", fr.r2,
                          prev, alod, end);
                }
            }
            if (c.arg(0) == 0) {
                c.regs[0] = 1;
                c.log("[lod-match] force return 1 (writer left r0=0 after alod=%u)", alod);
            }
        }
    }

    c.log("[lod-match] EXIT  #%u r0=0x%08x (%.3f ms) wall=%lld caller_lr=0x%08x "
          "enter_args=(0x%08x,0x%08x,0x%08x,0x%08x)",
          fr.seq, c.arg(0), ms, (long long)wall, fr.lr, fr.r0, fr.r1, fr.r2, fr.r3);
    if (fr.obj_ok) {
        dump_lod_object_state(c, fr.r1, "profile-post", fr.pre_178, fr.pre_184, fr.pre_190, true);
    } else if (lod_obj_readable(c, fr.r1)) {
        dump_lod_object_state(c, fr.r1, "profile-post", 0, 0, 0, false);
    }
    c.regs[14] = fr.lr;
}

void lod_match_enter(GuestCall &c) {
    LodProbeFrame fr;
    fr.lr = c.lr();
    fr.r0 = c.arg(0);
    fr.r1 = c.arg(1);
    fr.r2 = c.arg(2);
    fr.r3 = c.arg(3);
    fr.seq = g_lod_match_seq.fetch_add(1) + 1;
    fr.t0 = std::chrono::steady_clock::now();
    /* Active-UID fields live on the profile (r1), not the props object (r0). */
    fr.obj_ok = lod_obj_readable(c, fr.r1);
    if (fr.obj_ok) {
        fr.pre_178 = c.read32(fr.r1 + kLodOffPrev);
        fr.pre_184 = c.read32(fr.r1 + kLodOffAlod);
        fr.pre_190 = c.read32(fr.r1 + kLodOffWmish);

        const std::uint32_t now = (std::uint32_t)(std::time(nullptr) + pvz2_time_offset_sec());
        /* Far-future alodet (e.g. 2031 birthday) sticks the map banner at
         * ~40000h and blocks re-match. Clear so today's schedule can commit. */
        if (fr.pre_190 > now && fr.pre_190 - now > kLodMaxSaneEndSkewSec) {
            c.log("[lod-match] clearing stale Active LoD alod=%u end=%u (now=%u, +%.1f days) "
                  "on profile 0x%08x",
                  fr.pre_184, fr.pre_190, now, (fr.pre_190 - now) / 86400.0, fr.r1);
            c.write32(fr.r1 + kLodOffPrev, 0);
            c.write32(fr.r1 + kLodOffAlod, 0);
            c.write32(fr.r1 + kLodOffWmish, 0);
            fr.pre_178 = fr.pre_184 = fr.pre_190 = 0;
        } else if (pvz2_config()->lod_force_repeat && fr.pre_184 != 0 && fr.pre_178 == fr.pre_184 && fr.pre_190 > now &&
                   (fr.pre_190 - now) <= kLodMaxSaneEndSkewSec) {
            /* Prior probe sync set +0x178==alod; that makes apply's R4!=0 and
             * clobbers the map out-flag. Real writer leaves +0x178 behind. */
            c.log("[lod-match] un-sync +0x178 %u -> 0 (was == alod; map out-flag needs "
                  "+0x178 < +0x184) on 0x%08x",
                  fr.pre_178, fr.r1);
            c.write32(fr.r1 + kLodOffPrev, 0);
            fr.pre_178 = 0;
        }
        /* Do NOT raise +0x178 to match alod — see above. */
    }
    g_lod_match_frames.push_back(fr);

    const std::time_t wall = (std::time_t)(std::time(nullptr) + pvz2_time_offset_sec());
    c.log("[lod-match] ENTER #%u r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x lr=0x%08x "
          "sp=0x%08x wall=%lld",
          fr.seq, fr.r0, fr.r1, fr.r2, fr.r3, fr.lr, c.sp(), (long long)wall);
    if (fr.obj_ok) {
        dump_lod_object_state(c, fr.r1, "profile-pre", fr.pre_178, fr.pre_184, fr.pre_190, false);
    }

    /* PUSH {r4-r9,r11,lr} with the exit stub as the saved return address. */
    emulate_push(c, {4, 5, 6, 7, 8, 9, 11}, g_lod_match_exit_stub);
    /* Resume at SUB sp,#0x10 after the SVC. */
    c.no_return();
}

/* sub_69F8DC -- Active UID / state writer (STR to +0x184 / +0x190). Enter-only. */
void lod_uid_writer_enter(GuestCall &c) {
    const std::uint32_t obj = c.arg(0);
    const std::uint32_t a = c.arg(1);
    const std::uint32_t b = c.arg(2);
    const std::uint32_t prev_alod =
        lod_obj_readable(c, obj) ? c.read32(obj + kLodOffAlod) : 0xffffffffu;
    if (a != prev_alod) {
        c.log("[lod-apply] UID-WRITER obj=0x%08x alod %u -> %u end=%u lr=0x%08x", obj, prev_alod, a,
              b, c.lr());
    }
    /* Leave +0x178 alone — see lod_match_enter. Syncing it to alod made
     * apply's R4!=0 path clobber the world-map LoD out-flag. */
    /* PUSH {r4,lr} with the real return address. */
    emulate_push(c, {4}, c.lr());
    c.no_return();
}

bool install_svc_at(pvz2_elf_image_t *img, std::uint32_t offset, std::uint32_t expect_word,
                    const char *enter_name, ImportHandler enter_fn, const char *what) {
    if (offset == 0) return false;
    if (!in_image(img, offset)) {
        pvz2_log("[lod-apply] %s skipped -- 0x%x outside image", what, offset);
        return false;
    }
    const std::uint32_t word = read_word(img, offset);
    if (word != expect_word) {
        pvz2_log("[lod-apply] %s skipped -- entry 0x%x is 0x%08x, expected 0x%08x", what, offset,
                 word, expect_word);
        return false;
    }
    const std::uint32_t enter_stub = make_guest_callback(enter_name, enter_fn);
    if (enter_stub == 0) {
        pvz2_log("[lod-apply] %s skipped -- trampoline table full", what);
        return false;
    }
    std::uint32_t svc = 0;
    std::memcpy(&svc, &img->mem[enter_stub], 4);
    write_word(img, offset, svc);
    pvz2_log("[lod-apply] %s armed at 0x%x (svc via 0x%08x)", what, offset, enter_stub);
    return true;
}

}  // namespace

void game_install_lod_apply_probe(pvz2_elf_image_t *img) {
    if (img == nullptr) return;

    g_lod_apply_exit_stub = make_guest_callback("lod.apply.exit", lod_apply_exit);
    if (g_lod_apply_exit_stub == 0) {
        pvz2_log("[lod-apply] probe skipped -- trampoline table full (exit stub)");
        return;
    }
    g_lod_match_exit_stub = make_guest_callback("lod.match.exit", lod_match_exit);
    if (g_lod_match_exit_stub == 0) {
        pvz2_log("[lod-match] probe skipped -- trampoline table full (exit stub)");
        return;
    }

    install_svc_at(img, sym().patch.lod_apply_fn, kLodApplyPush, "lod.apply.enter", lod_apply_enter,
                   "apply probe");
    install_svc_at(img, sym().patch.lod_match_fn, kLodMatchPush, "lod.match.enter", lod_match_enter,
                   "match probe");
    install_svc_at(img, sym().patch.lod_uid_writer_fn, kLodWriterPush, "lod.uid.writer",
                   lod_uid_writer_enter, "UID-writer probe");
}

/* --- reward-screen singleton fix ------------------------------------------ *
 *
 * Each handler runs in place of ONE instruction (the PUSH at a function entry), so it
 * has to do what that instruction did; install_svc_at checks it is that instruction. */
namespace {

constexpr std::uint32_t kScrCreatePush = 0xE92D48F0u;  /* PUSH {r4-r7,fp,lr} */
constexpr std::uint32_t kScrDestroyPush = 0xE92D4070u; /* PUSH {r4,r5,r6,lr} */

/* The reward-screen singleton (see GameSymbols::patch.scr_global).
 *
 * Its create function stores the new object in a global without
 * checking it, and its destroy function destroys whatever the global holds. The epic
 * finale opens two of these at once -- the journal's "continue / back to map" screen,
 * then the quest-award screen a second later -- so the first is orphaned: the award
 * screen's destroy clears the global, and when the journal screen later closes itself
 * its destroy finds 0 and does nothing. The screen stays registered as a widget and
 * swallows every click on the map afterwards.
 *
 * Nested screens are meant to behave like a stack, so that is what this makes them:
 * create remembers the instance it is about to overwrite, and a destroy that finds the
 * global empty takes the remembered one back out first. Plain, non-overlapping use
 * (the only case that worked before) never touches the stack. */
std::vector<std::uint32_t> g_scr_stack;

/* A libc++ std::string (12 bytes, short-string flag in bit 0 of byte 0). */
std::string guest_std_string(GuestCall &c, std::uint32_t p) {
    if (!c.in_bounds(p, 12)) return "<unreadable>";
    std::uint32_t size, data;
    const std::uint8_t b0 = c.read8(p);
    if (b0 & 1) {
        size = c.read32(p + 4);
        data = c.read32(p + 8);
    } else {
        size = b0 >> 1;
        data = p + 1;
    }
    if (size > 4000) size = 4000;
    if (!c.in_bounds(data, size)) return "<unreadable>";
    std::string out;
    out.reserve(size);
    for (std::uint32_t i = 0; i < size; ++i) out.push_back((char)c.read8(data + i));
    return out;
}

std::uint32_t read_guest_lawn_app(GuestCall &c) {
    const std::uint32_t got_slot = c.img->so_base + 0x110c734u;
    if (!c.in_bounds(got_slot, 4)) return 0;
    const std::uint32_t ptr_addr = c.read32(got_slot);
    if (ptr_addr == 0 || !c.in_bounds(ptr_addr, 4)) return 0;
    return c.read32(ptr_addr);
}

std::uint32_t read_guest_gsm(GuestCall &c) {
    const std::uint32_t got_slot = c.img->so_base + 0x110c7a0u;
    if (!c.in_bounds(got_slot, 4)) return 0;
    const std::uint32_t ptr_addr = c.read32(got_slot);
    if (ptr_addr == 0 || !c.in_bounds(ptr_addr, 4)) return 0;
    return c.read32(ptr_addr);
}

std::string read_guest_worldmap_node(GuestCall &c) {
    if (!c.img || c.img->so_base == 0) return "";
    const std::uint32_t slot = c.img->so_base + 0x115bc00u;
    if (!c.in_bounds(slot, 4)) return "";
    const std::uint32_t mgr = c.read32(slot);
    if (mgr == 0 || !c.in_bounds(mgr + 0x84u, 12)) return "";
    return guest_std_string(c, mgr + 0x84u);
}

bool is_modern34_board_context(GuestCall &c) {
    const std::string node = read_guest_worldmap_node(c);
    if (node == "modern34") return true;
    if (credits_is_modern34_won()) return true;
    if (credits_get_modern34_active()) return true;

    const std::string b_lvl = dex::current_board_level();
    const std::int32_t known_num = dex::current_board_level_number();
    const bool is_modern = (dex::current_world() == "modern" || b_lvl.find("modern") != std::string::npos ||
                            node.rfind("modern", 0) == 0);
    const bool is_day34 = (known_num == 34 || b_lvl.find("34") != std::string::npos ||
                           b_lvl == "modern34" || (is_modern && b_lvl.find("modern_zomboss_") != std::string::npos));
    return is_modern && is_day34;
}

void scr_create(GuestCall &c) {
    const std::uint32_t global = c.img->so_base + sym().patch.scr_global;
    const std::uint32_t live = c.read32(global);
    if (live != 0) {
        g_scr_stack.push_back(live);
        pvz2_log("[screen] a reward screen is created while 0x%08x is still alive (caller .so+0x%x, "
                 "board '%s'): keeping it so its own destroy still finds it (%zu nested)", live,
                 c.lr() - c.img->so_base, dex::current_board_level().c_str(), g_scr_stack.size());
    }
    pvz2native::diagnostics::vlog("screen", "CREATE  reward-screen singleton, caller .so+0x%x "
                                  "(board='%s', live=0x%08x)", c.lr() - c.img->so_base,
                                  dex::current_board_level().c_str(), live);

    const std::uint32_t lawn_app = read_guest_lawn_app(c);
    const bool in_board = (lawn_app != 0 && c.in_bounds(lawn_app + 0x684u, 4) && c.read32(lawn_app + 0x684u) != 0);
    const std::string node = read_guest_worldmap_node(c);
    const std::string b_lvl = dex::current_board_level();
    const std::int32_t known_num = dex::current_board_level_number();
    const bool is_modern = (dex::current_world() == "modern" || b_lvl.find("modern") != std::string::npos ||
                            node.rfind("modern", 0) == 0);
    const bool is_day34 = (known_num == 34 || b_lvl.find("34") != std::string::npos ||
                           b_lvl == "modern34" || node == "modern34" ||
                           (is_modern && b_lvl.find("modern_zomboss_") != std::string::npos));

    if (credits_is_modern34_won() || (in_board && is_modern && is_day34)) {
        credits_notify_modern34_reward_create();
        credits_mark_modern34_won();
        pvz2_log("[credits] Modern Day 34 reward screen created (in_board=%d, node='%s', board='%s')",
                 in_board ? 1 : 0, node.c_str(), b_lvl.c_str());
    }
    emulate_push(c, {4, 5, 6, 7, 11}, c.lr());
    c.no_return();
}

/* Native post-Modern-Day-34 Credits screen state & hooks (4.5.2) */
constexpr std::uint32_t kGsmTransitionToFn     = 0x34bcdcu;
constexpr std::uint32_t kGsmTransitionToPush   = 0xE92D4BF0u; /* PUSH {r4-r9,r11,lr} */
constexpr std::uint32_t kGsmEnterMainMenuFn    = 0x3484acu;
constexpr std::uint32_t kGsmEnterMainMenuPush  = 0xE92D48F0u; /* PUSH {r4-r7,r11,lr} */
constexpr std::uint32_t kMainMenuUpdateFn      = 0x451f18u;
constexpr std::uint32_t kMainMenuUpdatePush    = 0xE92D47F0u; /* PUSH {r4-r10,lr} */
constexpr std::uint32_t kRewardBtnCreateFn     = 0x4b3164u;
constexpr std::uint32_t kRewardBtnCreatePush   = 0xE92D4BF0u; /* PUSH {r4-r9,r11,lr} */
constexpr std::uint32_t kCreditsWidgetCtorFn   = 0x4adb68u;
constexpr std::uint32_t kCreditsWidgetCtorPush = 0xE92D4FF0u; /* PUSH {r4-r11,lr} */
constexpr std::uint32_t kCreditsWidgetUpdFn    = 0x4b0ebcu;
constexpr std::uint32_t kCreditsWidgetUpdPush  = 0xE92D4070u; /* PUSH {r4-r6,lr} */

/* Travel Log (QuestHotListDialog) open & destroy hooks for Discord Rich Presence (4.5.2) */
constexpr std::uint32_t kQuestDialogOpenFn    = 0x748390u;
constexpr std::uint32_t kQuestDialogOpenPush  = 0xE92D4FF0u; /* PUSH {r4-r11,lr} */
constexpr std::uint32_t kQuestDialogDtorFn    = 0x748550u;
constexpr std::uint32_t kQuestDialogDtorPush  = 0xE92D48F0u; /* PUSH {r4-r7,r11,lr} */

bool g_pending_md34_credits = false;
bool g_md34_credits_mode = false;
bool g_md34_exit_triggered = false;
std::uint32_t g_md34_credits_widget = 0;
std::uint32_t g_menu_credits_widget = 0;
std::uint32_t g_md34_hidden_mainmenu = 0;
std::uint32_t g_md34_dummy_mainmenu = 0;
std::uint32_t g_md34_str_modern = 0;
std::uint32_t g_md34_wstr_view_credits = 0;
std::uint32_t g_gsm_enter_mainmenu_exit = 0;
std::vector<std::uint32_t> g_gsm_enter_mainmenu_lrs;
std::uint32_t g_credits_music_started_widget = 0;
std::uint32_t g_cr_state2_widget = 0;
std::chrono::steady_clock::time_point g_cr_state2_enter_time;

const char32_t *localized_view_credits_wstr() {
    const pvz2_config_t *cfg = pvz2_config();
    const char *loc = cfg ? cfg->user_locale : nullptr;
    if (!loc || !loc[0]) return U"Ver los cr\u00e9ditos";
    if (std::strncmp(loc, "es", 2) == 0) return U"Ver los cr\u00e9ditos";
    if (std::strncmp(loc, "en", 2) == 0) return U"View Credits";
    if (std::strncmp(loc, "fr", 2) == 0) return U"Voir les cr\u00e9dits";
    if (std::strncmp(loc, "de", 2) == 0) return U"Abspann ansehen";
    if (std::strncmp(loc, "pt", 2) == 0) return U"Ver os cr\u00e9ditos";
    if (std::strncmp(loc, "it", 2) == 0) return U"Vedi i crediti";
    return U"View Credits";
}

std::uint32_t ensure_guest_view_credits_wstring(GuestCall &c) {
    const char32_t *ws = localized_view_credits_wstr();
    std::uint32_t len = 0;
    while (ws[len] != 0 && len < 48u) ++len;

    if (g_md34_wstr_view_credits == 0) {
        g_md34_wstr_view_credits = c.rt->heap.alloc(16u + 64u * 4u);
        if (g_md34_wstr_view_credits == 0) return 0;
    }

    const std::uint32_t block = g_md34_wstr_view_credits;
    const std::uint32_t data_ptr = block + 16u;
    for (std::uint32_t i = 0; i <= len; ++i) {
        c.write32(data_ptr + i * 4u, (std::uint32_t)ws[i]);
    }
    const std::uint32_t cap = (len + 4u) & ~3u;
    c.write32(block + 0u, cap | 1u); /* ARMv7 libc++ std::wstring long-mode cap | 1 */
    c.write32(block + 4u, len);      /* length in 4-byte wchar_t codepoints */
    c.write32(block + 8u, data_ptr);
    return block;
}

std::uint32_t ensure_guest_const_string(GuestCall &c, std::uint32_t &slot, const char *s) {
    if (slot != 0) return slot;
    const std::uint32_t len = (std::uint32_t)std::strlen(s);
    const std::uint32_t block = c.rt->heap.alloc(16u + len + 1u);
    if (block == 0) return 0;
    const std::uint32_t data_ptr = block + 16u;
    for (std::uint32_t i = 0; i <= len; ++i) {
        c.write8(data_ptr + i, (std::uint8_t)s[i]);
    }
    c.write32(block + 0u, ((len + 16u) & ~15u) | 1u); /* libc++ long-string cap | 1 */
    c.write32(block + 4u, len);
    c.write32(block + 8u, data_ptr);
    slot = block;
    return slot;
}

void gsm_transition_to_enter(GuestCall &c) {
    const std::uint32_t next_state = c.arg(1);
    if ((g_pending_md34_credits || credits_is_modern34_won() || credits_is_modern34_reward_active()) &&
        (next_state == 6u || next_state == 12u)) {
        g_pending_md34_credits = false;
        credits_notify_modern34_reward_destroy();
        credits_set_modern34_active(0);
        g_md34_credits_mode = true;
        g_md34_exit_triggered = false;
        g_md34_credits_widget = 0;
        c.regs[1] = 4u; /* GAME_MainMenu (loads MainMenu_Background swirl + UI_MainMenu) */
        if (c.regs[2] == 0u) c.regs[2] = 5u; /* smooth black fade-out */
        if (c.regs[3] == 0u) c.regs[3] = 5u; /* smooth black fade-in */
        pvz2_log("[credits] Redirecting post-MD34 transition (state %u -> GAME_MainMenu) for native credits",
                 next_state);
    }
    emulate_push(c, {4, 5, 6, 7, 8, 9, 11}, c.lr());
    c.no_return();
}

void gsm_enter_mainmenu_enter(GuestCall &c) {
    if (!g_md34_credits_mode) {
        g_md34_hidden_mainmenu = 0;
    }
    if (g_gsm_enter_mainmenu_exit != 0) {
        g_gsm_enter_mainmenu_lrs.push_back(c.lr());
        emulate_push(c, {4, 5, 6, 7, 11}, g_gsm_enter_mainmenu_exit);
    } else {
        emulate_push(c, {4, 5, 6, 7, 11}, c.lr());
    }
    c.no_return();
}

void gsm_enter_mainmenu_exit(GuestCall &c) {
    if (g_gsm_enter_mainmenu_lrs.empty()) return;
    const std::uint32_t ret_lr = g_gsm_enter_mainmenu_lrs.back();
    g_gsm_enter_mainmenu_lrs.pop_back();

    if (g_md34_credits_mode) {
        const std::uint32_t lawn_app = read_guest_lawn_app(c);
        const std::uint32_t gsm = read_guest_gsm(c);
        if (lawn_app != 0 && gsm != 0) {
            /* 1. Stop any lingering Wwise world/splash music.
             * Silence during fade transition into credits. */
            const std::uint32_t stop_world_args[2] = {lawn_app, c.img->so_base + 0x1039c6du};
            c.call_guest(c.img->so_base + 0x3f9920u, stop_world_args, 2);
            const std::uint32_t stop_splash_args[2] = {lawn_app, c.img->so_base + 0x101cb1au};
            c.call_guest(c.img->so_base + 0x3f9920u, stop_splash_args, 2);
            c.write8(gsm + 0x615u, 0u); /* m_isStartupMusicPlaying = false */
            c.write8(gsm + 0x616u, 1u); /* m_hasStoppedStartupMusic = true */

            /* 2. Hide and disable MainMenu's own buttons while keeping its swirl + vignette background */
            const std::uint32_t main_menu = c.read32(lawn_app + 0x744u);
            if (main_menu != 0) {
                g_md34_hidden_mainmenu = main_menu;
                c.write32(main_menu + 0xa0u, 0u); /* 0.0f UI alpha */
                c.write8(main_menu + 0x9cu, 1u);  /* disable MainMenu button callbacks */
            }

            /* 3. Construct native CreditsWidget with dummy MainMenu target so real MainMenu stays at 0.0f */
            if (g_md34_dummy_mainmenu == 0) {
                g_md34_dummy_mainmenu = c.rt->heap.alloc(0xa4u);
                if (g_md34_dummy_mainmenu != 0) {
                    for (std::uint32_t off = 0; off < 0xa4u; off += 4u) {
                        c.write32(g_md34_dummy_mainmenu + off, 0u);
                    }
                }
            }

            const std::uint32_t widget_mgr = c.read32(lawn_app + 0x2d0u);
            if (widget_mgr != 0 && g_md34_dummy_mainmenu != 0) {
                const std::uint32_t alloc_args[1] = {0xa4u};
                const std::uint32_t widget = c.call_guest(c.img->so_base + 0xd854cu, alloc_args, 1);
                if (widget != 0) {
                    const std::uint32_t ctor_args[2] = {widget, g_md34_dummy_mainmenu};
                    c.call_guest(c.img->so_base + 0x4adb68u, ctor_args, 2);
                    /* Note: leaving m_stateStartTime at current time lets state 0 run during black fade-in,
                     * followed by state 1 smoothly fading in credits over the visible swirling background. */

                    const std::uint32_t wm_vt = c.read32(widget_mgr);
                    const std::uint32_t add_fn = c.read32(wm_vt + 0x28u);
                    const std::uint32_t front_fn = c.read32(wm_vt + 0x54u);
                    const std::uint32_t wm_args[2] = {widget_mgr, widget};
                    c.call_guest(add_fn, wm_args, 2);
                    c.call_guest(front_fn, wm_args, 2);

                    /* Keep GameStateTransitionController's black fade overlay above CreditsWidget */
                    const std::uint32_t app_args[1] = {lawn_app};
                    c.call_guest(c.img->so_base + 0x3e7330u, app_args, 1);

                    g_md34_credits_widget = widget;
                    if (pvz2_presence_enabled()) {
                        pvz2_presence_note("state", "Credits Shown=true");
                    }
                    pvz2_log("[credits] Native CreditsWidget 0x%08x active for post-MD34 credits", widget);
                }
            }
        }
    }

    c.regs[14] = ret_lr;
}

void mainmenu_update_enter(GuestCall &c) {
    const std::uint32_t main_menu = c.arg(0);
    if (main_menu != 0 && main_menu == g_md34_hidden_mainmenu) {
        c.write32(main_menu + 0xa0u, 0u); /* keep MainMenu UI invisible */
        c.write8(main_menu + 0x9cu, 1u);  /* keep MainMenu buttons disabled */
    }

    if (g_md34_credits_mode && !g_md34_exit_triggered && g_md34_credits_widget != 0 &&
        c.in_bounds(g_md34_credits_widget + 0x98u, 4)) {
        const std::uint32_t cr_state = c.read32(g_md34_credits_widget + 0x98u);
        if (cr_state == 3u) {
            credits_music_fade_out();
        } else if (cr_state >= 4u) {
            credits_music_stop();
            g_credits_music_started_widget = 0;
            g_cr_state2_widget = 0;
            g_md34_exit_triggered = true;
            g_md34_credits_mode = false;
            const std::uint32_t widget = g_md34_credits_widget;
            g_md34_credits_widget = 0;

            const std::uint32_t lawn_app = read_guest_lawn_app(c);
            const std::uint32_t gsm = read_guest_gsm(c);
            if (lawn_app != 0) {
                const std::uint32_t widget_mgr = c.read32(lawn_app + 0x2d0u);
                if (widget_mgr != 0) {
                    const std::uint32_t wm_vt = c.read32(widget_mgr);
                    const std::uint32_t rem_fn = c.read32(wm_vt + 0x2cu);
                    const std::uint32_t rem_args[2] = {widget_mgr, widget};
                    c.call_guest(rem_fn, rem_args, 2);
                }
                const std::uint32_t w_vt = c.read32(widget);
                const std::uint32_t dtor_fn = c.read32(w_vt + 0x0cu);
                const std::uint32_t dtor_args[1] = {widget};
                c.call_guest(dtor_fn, dtor_args, 1);
            }

            if (pvz2_presence_enabled()) {
                pvz2_presence_note("state", "Credits Shown=false");
            }

            if (gsm != 0) {
                const std::uint32_t str_modern =
                    ensure_guest_const_string(c, g_md34_str_modern, "modern");
                if (str_modern != 0) {
                    const std::uint32_t map_args[4] = {gsm, str_modern, 5u, 5u};
                    pvz2_log("[credits] Credits finished/dismissed -- transitioning smoothly to Modern Day map");
                    c.call_guest(c.img->so_base + 0x346024u, map_args, 4);
                }
            }
        }
    }

    emulate_push(c, {4, 5, 6, 7, 8, 9, 10}, c.lr());
    c.no_return();
}

void reward_btn_create_enter(GuestCall &c) {
    if (!g_md34_credits_mode &&
        (credits_is_modern34_reward_active() || credits_is_modern34_won() || is_modern34_board_context(c))) {
        const std::uint32_t btn_id = c.arg(1);
        if (btn_id == 0x3e8u) {
            const std::uint32_t wstr = ensure_guest_view_credits_wstring(c);
            if (wstr != 0) {
                c.regs[2] = wstr;
                pvz2_log("[credits] Replaced MD34 native Continue button (id=0x%x) with localized 'Ver los creditos'",
                         btn_id);
            }
        }
    }
    emulate_push(c, {4, 5, 6, 7, 8, 9, 11}, c.lr());
    c.no_return();
}

void credits_widget_ctor_enter(GuestCall &c) {
    if (!g_md34_credits_mode) {
        g_menu_credits_widget = c.arg(0);
        g_credits_music_started_widget = 0;
        g_cr_state2_widget = 0;
        const std::uint32_t lawn_app = read_guest_lawn_app(c);
        if (lawn_app != 0) {
            const std::uint32_t stop_splash_args[2] = {lawn_app, c.img->so_base + 0x101cb1au};
            c.call_guest(c.img->so_base + 0x3f9920u, stop_splash_args, 2);
        }
        if (pvz2_presence_enabled()) {
            pvz2_presence_note("state", "Credits Shown=true");
        }
        pvz2_log("[credits] Menu CreditsWidget 0x%08x opened", g_menu_credits_widget);
    }
    emulate_push(c, {4, 5, 6, 7, 8, 9, 10, 11}, c.lr());
    c.no_return();
}

void credits_widget_update_enter(GuestCall &c) {
    const std::uint32_t widget = c.arg(0);
    const bool is_target_widget = (widget != 0 && (widget == g_menu_credits_widget || widget == g_md34_credits_widget));
    if (is_target_widget && c.in_bounds(widget + 0x98u, 4)) {
        const std::uint32_t cr_state = c.read32(widget + 0x98u);

        /* Start music when credits reach state 2 (and begin scrolling downward) */
        if (cr_state == 2u) {
            if (g_cr_state2_widget != widget) {
                g_cr_state2_widget = widget;
                g_cr_state2_enter_time = std::chrono::steady_clock::now();
            }
            if (g_credits_music_started_widget != widget) {
                const std::uint32_t scroll = (c.in_bounds(widget + 0x88u, 4)) ? c.read32(widget + 0x88u) : 0u;
                const bool scroll_started = (scroll != 0 && c.in_bounds(scroll + 0x9cu, 1) && c.read8(scroll + 0x9cu) != 0);
                const auto elapsed_sec = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_cr_state2_enter_time).count();
                if (scroll_started || elapsed_sec >= 3.6f) {
                    g_credits_music_started_widget = widget;
                    credits_music_start();
                    pvz2_log("[credits] Started 'Zombie Time' credits music (widget=0x%08x, scroll_started=%d, elapsed=%.2fs)",
                             widget, scroll_started ? 1 : 0, elapsed_sec);
                }
            }
        } else if (g_cr_state2_widget == widget) {
            g_cr_state2_widget = 0;
        }

        /* Fade out music when user presses back button or credits finish */
        if (cr_state == 3u) {
            credits_music_fade_out();
        } else if (cr_state >= 4u) {
            credits_music_stop();
            g_credits_music_started_widget = 0;

            if (!g_md34_credits_mode && widget == g_menu_credits_widget) {
                g_menu_credits_widget = 0;
                if (pvz2_presence_enabled()) {
                    pvz2_presence_note("state", "Credits Shown=false");
                }
                const std::uint32_t lawn_app = read_guest_lawn_app(c);
                const std::uint32_t gsm = read_guest_gsm(c);
                if (lawn_app != 0 && gsm != 0 && c.read8(gsm + 0x615u) != 0) {
                    const std::uint32_t play_splash_args[2] = {lawn_app, c.img->so_base + 0x101ca96u};
                    c.call_guest(c.img->so_base + 0x3f9920u, play_splash_args, 2);
                }
            }
        }
    }
    emulate_push(c, {4, 5, 6}, c.lr());
    c.no_return();
}

void quest_dialog_open_enter(GuestCall &c) {
    if (pvz2_presence_enabled()) {
        pvz2_presence_note("state", "TravelLog Shown=true");
    }
    emulate_push(c, {4, 5, 6, 7, 8, 9, 10, 11}, c.lr());
    c.no_return();
}

void quest_dialog_dtor_enter(GuestCall &c) {
    if (pvz2_presence_enabled()) {
        pvz2_presence_note("state", "TravelLog Shown=false");
    }
    emulate_push(c, {4, 5, 6, 7, 11}, c.lr());
    c.no_return();
}

void scr_destroy(GuestCall &c) {
    const std::uint32_t global = c.img->so_base + sym().patch.scr_global;
    std::uint32_t live = c.read32(global);
    if (live == 0) {
        /* Only take back a remembered screen that is still what we think it is: a live
         * heap block whose vptr is this class's. Anything else was freed or reused by the
         * engine in the meantime and must not be touched. */
        while (!g_scr_stack.empty()) {
            const std::uint32_t cand = g_scr_stack.back();
            g_scr_stack.pop_back();
            if (c.rt->heap.size_of(cand) != 0 && c.read32(cand) == c.img->so_base + sym().patch.scr_vtable) {
                live = cand;
                c.write32(global, live);
                pvz2_log("[screen] destroy found no screen (caller .so+0x%x): destroying the one "
                         "that was orphaned, 0x%08x (%zu nested left)", c.lr() - c.img->so_base,
                         live, g_scr_stack.size());
                break;
            }
            pvz2_log("[screen] dropped remembered screen 0x%08x: no longer a live screen object",
                     cand);
        }
    }
    pvz2native::diagnostics::vlog("screen", "DESTROY reward-screen singleton, caller .so+0x%x "
                                  "(live=0x%08x)", c.lr() - c.img->so_base, live);
    if (credits_is_modern34_reward_active() || credits_is_modern34_won()) {
        pvz2_log("[screen] Modern 34 reward screen dismissed -- arming native credits transition");
        g_pending_md34_credits = true;
    }
    emulate_push(c, {4, 5, 6}, c.lr());
    c.no_return();
}

/* PopAnim and UI state controller null pointer guards */
constexpr std::uint32_t kPopAnimAttachFn   = 0x6b7bfcu;
constexpr std::uint32_t kPopAnimAttachPush = 0xE92D4830u; /* PUSH {r4,r5,fp,lr} */
constexpr std::uint32_t kPopAnimCreateFn   = 0x33fcd0u;
constexpr std::uint32_t kPopAnimCreatePush = 0xE92D41F0u; /* PUSH {r4-r8,lr} */
constexpr std::uint32_t kZenStateGuardFn   = 0x3a0808u;
constexpr std::uint32_t kZenStateGuardWord = 0xE5940000u; /* LDR r0, [r4] */

void popanim_attach_enter(GuestCall &c) {
    const std::uint32_t obj = c.arg(0);
    const std::uint32_t factory = c.arg(1);
    if (factory == 0 || !c.in_bounds(factory + 0xc, 4)) {
        pvz2_log("[popanim] 0x6b7bfc called with null/invalid factory (obj=0x%08x, lr=0x%08x) -- skipping attach",
                 obj, c.lr());
        c.set_result(0);
        return;
    }
    const std::uint32_t create_fn = c.read32(factory + 0xc);
    if (create_fn == 0) {
        pvz2_log("[popanim] 0x6b7bfc factory has null creator fn (obj=0x%08x, lr=0x%08x) -- skipping attach",
                 obj, c.lr());
        c.set_result(0);
        return;
    }
    emulate_push(c, {4, 5, 11}, c.lr());
    c.no_return();
}

void popanim_create_enter(GuestCall &c) {
    const std::uint32_t factory = c.arg(0);
    if (factory == 0 || !c.in_bounds(factory + 0xc, 4)) {
        pvz2_log("[popanim] 0x33fcd0 called with null/invalid factory (lr=0x%08x) -- returning null", c.lr());
        c.set_result(0);
        return;
    }
    const std::uint32_t create_fn = c.read32(factory + 0xc);
    if (create_fn == 0) {
        pvz2_log("[popanim] 0x33fcd0 factory has null creator fn (lr=0x%08x) -- returning null", c.lr());
        c.set_result(0);
        return;
    }
    emulate_push(c, {4, 5, 6, 7, 8}, c.lr());
    c.no_return();
}

void zen_state_guard_enter(GuestCall &c) {
    const std::uint32_t r4_val = c.regs[4];
    if (r4_val == 0 || !c.in_bounds(r4_val, 4)) {
        pvz2_log("[state] 0x3a0808 null controller guard triggered (r4=0x%08x, sp=0x%08x) -- skipping virtual call",
                 r4_val, c.sp());
        const std::uint32_t sp = c.regs[13];
        c.regs[4]  = c.read32(sp + 8);
        c.regs[5]  = c.read32(sp + 12);
        c.regs[11] = c.read32(sp + 16);
        const std::uint32_t ret_pc = c.read32(sp + 20);
        c.regs[13] = sp + 24;
        c.regs[14] = ret_pc;
        c.set_result(0);
        return;
    }
    c.regs[0] = c.read32(r4_val);
    c.no_return();
}

}  // namespace

void game_install_reward_screen_fix(pvz2_elf_image_t *img) {
    if (img == nullptr) return;
    const auto &p = sym().patch;
    install_svc_at(img, p.scr_create, kScrCreatePush, "scr.create", scr_create, "screen create");
    install_svc_at(img, p.scr_destroy, kScrDestroyPush, "scr.destroy", scr_destroy, "screen destroy");

    g_gsm_enter_mainmenu_exit = make_guest_callback("gsm.mainmenu.exit", gsm_enter_mainmenu_exit);
    install_svc_at(img, kGsmTransitionToFn, kGsmTransitionToPush, "gsm.transition", gsm_transition_to_enter,
                   "post-MD34 credits transition");
    install_svc_at(img, kGsmEnterMainMenuFn, kGsmEnterMainMenuPush, "gsm.mainmenu.enter", gsm_enter_mainmenu_enter,
                   "post-MD34 credits setup");
    install_svc_at(img, kMainMenuUpdateFn, kMainMenuUpdatePush, "mainmenu.update", mainmenu_update_enter,
                   "post-MD34 credits update");
    install_svc_at(img, kRewardBtnCreateFn, kRewardBtnCreatePush, "reward.btn.create", reward_btn_create_enter,
                   "MD34 reward view-credits button");
    install_svc_at(img, kCreditsWidgetCtorFn, kCreditsWidgetCtorPush, "credits.ctor", credits_widget_ctor_enter,
                   "credits widget music start");
    install_svc_at(img, kCreditsWidgetUpdFn, kCreditsWidgetUpdPush, "credits.update", credits_widget_update_enter,
                   "credits widget music update");
    install_svc_at(img, kQuestDialogOpenFn, kQuestDialogOpenPush, "quest.dlg.open", quest_dialog_open_enter,
                   "travel log open tap");
    install_svc_at(img, kQuestDialogDtorFn, kQuestDialogDtorPush, "quest.dlg.dtor", quest_dialog_dtor_enter,
                   "travel log close tap");
    install_svc_at(img, kPopAnimAttachFn, kPopAnimAttachPush, "popanim.attach", popanim_attach_enter,
                   "popanim attach null factory guard");
    install_svc_at(img, kPopAnimCreateFn, kPopAnimCreatePush, "popanim.create", popanim_create_enter,
                   "popanim create null factory guard");
    install_svc_at(img, kZenStateGuardFn, kZenStateGuardWord, "zen.state.guard", zen_state_guard_enter,
                   "state controller null virtual dispatch guard");
}

/* --- quest trace (verbose only) --------------------------------------------- *
 *
 * Read-only probes on the quest manager's pick path, so the log says what the engine picked and
 * why: which candidates a slot had, how many it added, and the engine's own "Pick Time /
 * Limiters / + Add N quests from slot X" text. Nothing here changes what the engine does -- each
 * handler logs and then does what the instruction it replaced did. Installed only with
 * [log] verbose=1; the save-file side of the trace is diagnostics/quest_trace.cpp. */
namespace {

constexpr std::uint32_t kQuestPickPush = 0xE92D4FF0u;     /* PUSH {r4-r8,sb,sl,fp,lr} */
constexpr std::uint32_t kQuestSlotPickPush = 0xE92D4BF0u; /* PUSH {r4-r8,sb,fp,lr} */
constexpr std::uint32_t kQuestCandidatePush = 0xE92D47F0u; /* PUSH {r4-r8,sb,sl,lr} */
constexpr std::uint32_t kQuestAddPush = 0xE92D41F0u;      /* PUSH {r4-r8,lr} */

/* QuestMgr field offsets (4.5.2). The active list is a vector of 8-byte weak handles. */
constexpr std::uint32_t kMgrActiveBegin = 0x70;
constexpr std::uint32_t kMgrActiveEnd = 0x74;
constexpr std::uint32_t kMgrPickPending = 0x90;
constexpr std::uint32_t kMgrPickLog = 0x94; /* std::string */

std::uint32_t g_quest_pick_exit = 0;
std::uint32_t g_quest_slot_exit = 0;

struct QuestFrame {
    std::uint32_t lr;
    std::uint32_t mgr;
    std::uint32_t active;
    std::int32_t want;
    bool pending;
};
std::vector<QuestFrame> g_quest_pick_frames, g_quest_slot_frames;

std::uint32_t quest_active_count(GuestCall &c, std::uint32_t mgr) {
    if (!c.in_bounds(mgr + kMgrActiveBegin, 8)) return 0;
    const std::uint32_t b = c.read32(mgr + kMgrActiveBegin), e = c.read32(mgr + kMgrActiveEnd);
    return e >= b ? (e - b) / 8 : 0;
}

namespace dg = pvz2native::diagnostics;

void quest_pick_exit(GuestCall &c) {
    if (g_quest_pick_frames.empty()) return;
    const QuestFrame fr = g_quest_pick_frames.back();
    g_quest_pick_frames.pop_back();
    if (fr.pending) {
        dg::vlog("quest", "PICK done: active quests %u -> %u", fr.active, quest_active_count(c, fr.mgr));
        const std::string text = guest_std_string(c, fr.mgr + kMgrPickLog);
        std::size_t at = 0;
        while (at < text.size()) {
            std::size_t nl = text.find('\n', at);
            if (nl == std::string::npos) nl = text.size();
            if (nl > at) dg::vlog("quest", "pick-log| %s", text.substr(at, nl - at).c_str());
            at = nl + 1;
        }
    }
    c.regs[14] = fr.lr;
}

void quest_pick_enter(GuestCall &c) {
    QuestFrame fr{};
    fr.lr = c.lr();
    fr.mgr = c.arg(0);
    fr.active = quest_active_count(c, fr.mgr);
    fr.pending = c.in_bounds(fr.mgr + kMgrPickPending, 1) && c.read8(fr.mgr + kMgrPickPending) != 0;
    g_quest_pick_frames.push_back(fr);
    if (fr.pending) {
        dg::vlog("quest", "PICK start: mgr=0x%08x active=%u caller=.so+0x%x board='%s' callers:%s", fr.mgr, fr.active,
                 fr.lr - c.img->so_base, dex::current_board_level().c_str(), dg::guess_callers(c, 14).c_str());
    }
    emulate_push(c, {4, 5, 6, 7, 8, 9, 10, 11}, g_quest_pick_exit);
    c.no_return();
}

void quest_slot_exit(GuestCall &c) {
    if (g_quest_slot_frames.empty()) return;
    const QuestFrame fr = g_quest_slot_frames.back();
    g_quest_slot_frames.pop_back();
    const std::int32_t missing = (std::int32_t)c.arg(0);
    dg::vlog("quest", "slot-pick done: wanted %d, still missing %d -> picked %d, active %u -> %u", fr.want, missing,
             fr.want - missing, fr.active, quest_active_count(c, fr.mgr));
    c.regs[14] = fr.lr;
}

void quest_slot_enter(GuestCall &c) {
    QuestFrame fr{};
    fr.lr = c.lr();
    fr.mgr = c.arg(0);
    fr.want = (std::int32_t)c.arg(2);
    fr.active = quest_active_count(c, fr.mgr);
    g_quest_slot_frames.push_back(fr);
    const std::uint32_t slot_info = c.arg(1);
    dg::vlog("quest", "slot-pick start: slotInfo=0x%08x slot-id=%u wants %d, active=%u", slot_info,
             c.in_bounds(slot_info + 4, 4) ? c.read32(slot_info + 4) : 0xFFFFFFFFu, fr.want, fr.active);
    emulate_push(c, {4, 5, 6, 7, 8, 9, 11}, g_quest_slot_exit);
    c.no_return();
}

/* One call per candidate that passed the slot and eligibility filters; arg 2 is its id. */
void quest_candidate_enter(GuestCall &c) {
    dg::vlog("quest", "  candidate '%s' (active=%u)", guest_std_string(c, c.arg(2)).c_str(),
             quest_active_count(c, c.arg(1)));
    emulate_push(c, {4, 5, 6, 7, 8, 9, 10}, c.lr());
    c.no_return();
}

void quest_add_enter(GuestCall &c) {
    const std::uint32_t h = c.arg(2);
    dg::vlog("quest", "  ADD quest to the active list: data handle=%08x:%08x active=%u (the new id shows in the next [quest-save])",
             c.in_bounds(h, 8) ? c.read32(h) : 0u, c.in_bounds(h, 8) ? c.read32(h + 4) : 0u,
             quest_active_count(c, c.arg(1)));
    emulate_push(c, {4, 5, 6, 7, 8}, c.lr());
    c.no_return();
}

}  // namespace

void game_install_quest_trace(pvz2_elf_image_t *img) {
    if (img == nullptr || !pvz2native::diagnostics::verbose()) return;
    const auto &p = sym().patch;
    if (p.quest_pick_fn == 0) return;

    g_quest_pick_exit = make_guest_callback("quest.pick.exit", quest_pick_exit);
    g_quest_slot_exit = make_guest_callback("quest.slot.exit", quest_slot_exit);
    if (g_quest_pick_exit == 0 || g_quest_slot_exit == 0) {
        pvz2_log("[quest] trace skipped -- trampoline table full");
        return;
    }
    install_svc_at(img, p.quest_pick_fn, kQuestPickPush, "quest.pick.enter", quest_pick_enter, "quest pick trace");
    install_svc_at(img, p.quest_slot_pick_fn, kQuestSlotPickPush, "quest.slot.enter", quest_slot_enter,
                   "quest slot-pick trace");
    install_svc_at(img, p.quest_candidate_fn, kQuestCandidatePush, "quest.cand.enter", quest_candidate_enter,
                   "quest candidate trace");
    install_svc_at(img, p.quest_add_fn, kQuestAddPush, "quest.add.enter", quest_add_enter, "quest add trace");
}

/* --- quest pick randomisation ----------------------------------------------- *
 *
 * The engine picks a slot's quests by std::random_shuffle over the eligible candidates and
 * taking the first ones. libc++'s random_shuffle draws from std::__rs_default, a static
 * std::mt19937 built the first time it is used, from the fixed default seed 5489. In a process
 * that has just started, the first shuffle is therefore always the same permutation: this port
 * starts a fresh process on every launch, and the same quest (Generic slot -> "Defeat the
 * Gargantuars" 2) was picked every time. On a phone the process lives for hours and the draws
 * before a pick vary, which hid it.
 *
 * Each shuffle entry reseeds that generator in guest memory from host entropy. The state layout
 * (guard byte, 624 words, index) is the one std::mt19937's constructor and operator() in the .so
 * use; the handler runs in place of the shuffle's PUSH and does what it did. */
namespace {

constexpr std::uint32_t kShufflePush = 0xE92D4FF0u; /* PUSH {r4-r8,sb,sl,fp,lr} */
constexpr std::uint32_t kRsGuard = 0x4;
constexpr std::uint32_t kRsState = 0x8;
constexpr std::uint32_t kRsIndex = 0x9c8;
constexpr std::uint32_t kRsWords = 624;

void quest_shuffle_enter(GuestCall &c) {
    const std::uint32_t base = c.img->so_base + sym().patch.rs_default_state;
    if (c.in_bounds(base, kRsIndex + 4)) {
        static std::mt19937_64 host_rng{[] {
            std::random_device rd;
            return ((std::uint64_t)rd() << 32) ^ rd() ^
                   (std::uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
        }()};
        const std::uint32_t seed = (std::uint32_t)host_rng();
        std::uint32_t w = seed;
        c.write32(base + kRsState, w);
        for (std::uint32_t i = 1; i < kRsWords; ++i) {
            w = 1812433253u * (w ^ (w >> 30)) + i;
            c.write32(base + kRsState + 4 * i, w);
        }
        c.write32(base + kRsIndex, 0);
        c.write8(base + kRsGuard, 1); /* the lazy initialiser must not run over our state */
        pvz2native::diagnostics::vlog("quest", "shuffle: generator reseeded (seed=0x%08x), %u candidate(s)", seed,
                                      c.regs[1] >= c.regs[0] ? (c.regs[1] - c.regs[0]) / 8 : 0u);
    }
    emulate_push(c, {4, 5, 6, 7, 8, 9, 10, 11}, c.lr());
    c.no_return();
}

}  // namespace

void game_install_quest_shuffle_seed(pvz2_elf_image_t *img) {
    if (img == nullptr) return;
    const auto &p = sym().patch;
    if (p.quest_shuffle_fn == 0 || p.rs_default_state == 0) return;
    install_svc_at(img, p.quest_shuffle_fn, kShufflePush, "quest.shuffle.enter", quest_shuffle_enter,
                   "quest shuffle reseed");
}

/* --- engine clock ------------------------------------------------------------ *
 *
 * The engine does not compare dates in UTC. Its clock function returns `UTC + tm_gmtoff`: the player's wall
 * clock written as if it were UTC. The Level of the Day schedule dates (00:00 UTC, one entry per calendar
 * day) are compared with that, so a day starts at the player's own midnight, and the end of the active event
 * is worked out as mktime(gmtime(start)) + days, the matching local midnight. Everything lives in the same
 * "local clock" space, in any time zone.
 *
 * The engine's own clock is a float counter plus an offset that the game server sets. Without a server it
 * reads ~2030, which sent the schedule to far-future events, so the port made the function return plain
 * time(). That dropped the offset: the start was then compared in UTC and the end in local time, and for
 * UTC-3 the next day's event appeared three hours early with 26 h left (in UTC-8, eight hours early). The
 * function is now replaced by one that returns the real time plus the host's offset, which is what the
 * original returned once the server had set its clock. [log] force_date is part of time(), so it carries
 * through. */
namespace {

constexpr std::uint32_t kEngineClockPush = 0xE92D4010u; /* PUSH {r4,lr} */
constexpr std::uint32_t kClockUseDeviceTime = 0x54;     /* byte: the engine's own "use time()" switch */

void engine_clock(GuestCall &c) {
    const std::int64_t utc = (std::int64_t)std::time(nullptr) + pvz2_time_offset_sec();
    const std::uint32_t clock = c.arg(0);
    const bool device_time = c.in_bounds(clock + kClockUseDeviceTime, 1) && c.read8(clock + kClockUseDeviceTime) != 0;
    c.set_result((std::uint32_t)(device_time ? utc : utc + host_time::utc_offset((std::time_t)utc)));
    /* Returns to the caller: the function's body never runs. */
}

}  // namespace

void game_install_engine_clock(pvz2_elf_image_t *img) {
    if (img == nullptr) return;
    install_svc_at(img, sym().patch.engine_clock_fn, kEngineClockPush, "engine.clock", engine_clock,
                   "engine clock (UTC + local offset)");
}

}  // namespace pvz2native
