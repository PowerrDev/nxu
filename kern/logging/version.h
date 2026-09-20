#ifndef NXU_KERN_VERSION_H
#define NXU_KERN_VERSION_H

/*
 * NXU_VERSION and NXU_BUILD are derived from the commit count by
 * tools/version.sh; refresh them with `make version` rather than by hand.
 * 187 commits: generation 2, position 87 -> 0.2.87-dev, build 2I7A.
 */
#define NXU_VERSION "0.2.87-dev"
#define NXU_BUILD "NXU-2I7A"
#define NXU_KERNEL_NAME "NXU"
#define NXU_KERNEL_VERSION "NXU Kernel Version " NXU_VERSION " (" NXU_BUILD ")"

#endif
