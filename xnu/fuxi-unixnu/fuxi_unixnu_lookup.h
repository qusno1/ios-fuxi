/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Adapted from UniXNU/UniXNU cb0749940c598f2bd7fb113c1a660fcc2fd825ce.
 * See LICENSE.UniXNU. Fuxi's opt-in gate is original code.
 */
#ifndef FUXI_UNIXNU_LOOKUP_H
#define FUXI_UNIXNU_LOOKUP_H
#include <pexpert/pexpert.h>
/* namei succeeded and owns one iocount. A non-directory lower vnode cannot
 * contain the missing upper-layer child (e.g. a framework symlink below an
 * upper directory). Return ENOENT so lookup keeps its existing failure path,
 * releasing exactly that iocount; nameidone remains the caller's job.
 * A boot option gates this observable errno change; default preserves XNU.
 */
static int
fuxi_unixnu_lower_directory(vnode_t vp)
{
 unsigned enabled = 0;
 PE_parse_boot_argn("fuxiuniondir", &enabled, sizeof(enabled));
 if (enabled == 1 && vp->v_type != VDIR) {
  vnode_put(vp);
  return ENOENT;
 }
 return 0;
}
#endif
