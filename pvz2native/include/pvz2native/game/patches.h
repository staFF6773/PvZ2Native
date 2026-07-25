#ifndef PVZ2NATIVE_GAME_PATCHES_H
#define PVZ2NATIVE_GAME_PATCHES_H

#include <pvz2native/elf32/elf32_loader.h>

namespace pvz2native {

/* Instructions the port rewrites in the loaded image before any guest code runs.
 *
 * This is a last resort and there is exactly one of them, for a reason the shim
 * layers cannot express. The engine asks Android whether the device is online
 * (AndroidHttpProxy.GetNetworkStatus) and uses the SAME answer for two unrelated
 * decisions:
 *
 *   - "should I wait for downloads?" -- the loading screen. Answer "online" and
 *     the boot never finishes, because this port has no network stack and the
 *     downloads it then waits for can never arrive.
 *   - "may a purchase start at all?" -- the purchase broker checks it BEFORE
 *     touching the store driver, and on "offline" it shows its
 *     "Service Unavailable" dialog and stops there.
 *
 * One JNI answer cannot satisfy both, and the store is emulated locally (see
 * dex/hooks/purchase_driver.cpp) so it needs no connection to work. Rewriting
 * the broker's own connectivity call is what separates the two: the engine still
 * hears "offline" everywhere else -- the boot is untouched -- while the purchase
 * path stops refusing.
 *
 * Each patched site is verified to be the instruction we expect before anything
 * is written, and the addresses are per-version and live in game/symbols.cpp
 * like every other .so address.
 */

/* Applies the patches for the detected version. Call once, after
 * game_symbols_detect and before the first guest instruction executes (dynarmic
 * caches translated blocks, so a later write would be ignored by any code
 * already run). Silent when the version maps no patches or when the emulated
 * store is off; loud about anything it declines to patch. */
void game_apply_patches(pvz2_elf_image_t *img);

}  // namespace pvz2native

#endif
