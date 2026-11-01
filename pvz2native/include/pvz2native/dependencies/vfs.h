#ifndef PVZ2NATIVE_DEPENDENCIES_VFS_H
#define PVZ2NATIVE_DEPENDENCIES_VFS_H

#include <cstdint>
#include <string>

#include <pvz2native/runtime/guest_runtime.h>

namespace pvz2native {

/* The guest filesystem view.
 *
 * SexyAppFramework's resource layer tags every lookup with a scheme prefix
 * ("ASSET:<path>") and assembles paths with Android separators before they
 * reach libc. On a device the AAssetManager layer consumes all of that; we have
 * no such layer, so translation happens here, in one place, for every libc
 * entry point that takes a path (fopen, open, stat, access, mkdir, unlink).
 *
 * Both the libc modules and the DEX hooks need these paths -- the hooks hand
 * the engine the .obb location it then opens through libc -- so they live here
 * rather than in either one.
 */
namespace vfs {

/* The real expansion file on this machine, and the same file as the engine
 * must SEE it. ResStreamsManager::LoadRSB (sub_867740) only uses a path
 * verbatim when it starts with '/'; anything else it treats as a relative
 * resource name and routes through the Java asset API. On a device the .obb is
 * /storage/.../main.7.<pkg>.obb -- absolute -- so it always takes the verbatim
 * branch. A Windows "E:/..." path does not, hence the leading slash, which
 * translate() strips back off on the way to the host. */
const char *obb_host_path();
const char *obb_guest_path();

/* Guest path -> host path. Any path whose basename is "main.rsb" is redirected
 * to the .obb (the engine assembles that name itself from its resource folder),
 * and reaching either form sets rt->rsb_touched. */
std::string translate(GuestRuntime *rt, std::string guest_path);

/* True when `host_path` (as returned by translate) is the .obb. Compares with
 * both separators folded and case ignored, and also accepts any "*.obb" name,
 * because translate() hands back the engine's own spelling of the path rather
 * than obb_host_path() verbatim. */
bool is_obb_path(const std::string &host_path);

/* Discord Rich Presence tap: reports which resource group (and, when the read
 * starts exactly on one, which file) of the .obb the engine is reading at host
 * offset `pos`. Cheap no-op unless presence is on; call it only for streams
 * known to be the .obb. */
void note_obb_read(GuestRuntime *rt, long long pos);

/* bionic/ARM open(2) flag bits -> host _O_* values. They differ for
 * O_CREAT/O_EXCL/O_TRUNC/O_APPEND; binary mode is always forced. */
int translate_open_flags(std::uint32_t guest_flags);

}  // namespace vfs
}  // namespace pvz2native

#endif
