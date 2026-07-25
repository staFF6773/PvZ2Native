/* Guest-code patches -- see include/pvz2native/game/patches.h for why. */

#include <pvz2native/game/patches.h>

#include <pvz2native/config.h>
#include <pvz2native/game/symbols.h>

#include <cstdio>
#include <cstring>

namespace pvz2native {
namespace {

/* ARM (never Thumb here -- the engine's ARM32 code in both mapped builds is
 * A32): cond 1011 imm24 is BL. Masking the condition off means a conditional
 * BL still matches, and B (1010) still does not. */
constexpr std::uint32_t kOpcodeMask = 0x0F000000u;
constexpr std::uint32_t kOpcodeBl   = 0x0B000000u;

/* MOV R0, #1 -- the value the patched-out call used to return for "online". */
constexpr std::uint32_t kMovR0One = 0xE3A00001u;

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

/* Rewrites one `BL <online?>` into `MOV R0, #1`, i.e. makes that one call site
 * read "connected" without changing what the callee returns to anyone else.
 * Refuses -- loudly -- if the instruction is not the BL the table promised,
 * because writing over something else would corrupt the engine silently. */
bool force_online_call(pvz2_elf_image_t *img, std::uint32_t offset) {
    if (!in_image(img, offset)) {
        std::printf("pvz2: [patch] purchase gate 0x%x is outside the image -- skipped\n", offset);
        return false;
    }
    const std::uint32_t word = read_word(img, offset);
    if ((word & kOpcodeMask) != kOpcodeBl) {
        std::printf("pvz2: [patch] purchase gate 0x%x is not a BL (found 0x%08x) -- skipped; the "
                    "address in src/game/symbols.cpp is wrong for this build\n",
                    offset, word);
        return false;
    }
    write_word(img, offset, kMovR0One);
    return true;
}

/* Replaces one instruction, but only if it is exactly the one the table
 * promised -- a mismatch means the address belongs to another build and writing
 * over it would corrupt the engine silently, so it is reported and skipped. */
bool rewrite_word(pvz2_elf_image_t *img, std::uint32_t offset, std::uint32_t expect,
                  std::uint32_t replace, const char *what) {
    if (offset == 0) return false;
    if (!in_image(img, offset)) {
        std::printf("pvz2: [patch] %s (0x%x) is outside the image -- skipped\n", what, offset);
        return false;
    }
    const std::uint32_t word = read_word(img, offset);
    if (word != expect) {
        std::printf("pvz2: [patch] %s (0x%x) is 0x%08x, expected 0x%08x -- skipped; the address in "
                    "src/game/symbols.cpp is wrong for this build\n",
                    what, offset, word, expect);
        return false;
    }
    write_word(img, offset, replace);
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
    std::printf("pvz2: [patch] only half of the receipt patch applied -- purchases will %s\n",
                finish_now ? "report \"Unable to contact store\""
                           : "hang waiting for server validation");
    return false;
}

}  // namespace

void game_apply_patches(pvz2_elf_image_t *img) {
    if (img == nullptr) return;

    /* With the emulated store off the engine is meant to find no store at all,
     * and "no connection" is a perfectly good reason for it to say so. */
    if (pvz2_config()->emulate_iap == 0) return;

    const std::uint32_t *gates = sym().patch.purchase_online_gate;
    const std::size_t count = sizeof(sym().patch.purchase_online_gate) / sizeof(gates[0]);
    unsigned patched = 0, refused = 0;
    for (std::size_t i = 0; i < count && gates[i] != 0; ++i) {
        if (force_online_call(img, gates[i])) {
            ++patched;
        } else {
            ++refused;
        }
    }

    if (patched != 0) {
        std::printf("pvz2: [patch] purchase broker: %u connectivity check(s) forced to \"online\" "
                    "so the emulated store is reachable offline\n", patched);
    }
    if (refused != 0) {
        std::printf("pvz2: [patch] %u purchase gate(s) left alone -- buying will report "
                    "\"Service Unavailable\"\n", refused);
    }

    if (accept_receipt_locally(img)) {
        std::printf("pvz2: [patch] purchase receipts are accepted locally, exactly as a validation "
                    "server answering \"passed\"\n");
    }
}

}  // namespace pvz2native
