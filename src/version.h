/* SPDX-License-Identifier: MIT */
/* The release name ("0.0.4-test7") is given by the build (UNARC_RELEASE in
 * scripts/build-abiv11.sh), which also derives UNARC_VR from it; make-package.sh
 * refuses a build made for another name. The $VER version.revision is what
 * Version and installers compare as two numbers, so it is
 * major.(minor * 100 + patch): 0.0.4 is 0.4, a later 0.1.2 would be 0.102.
 * That needs patch below 100, which the build checks. The full name follows
 * the date. The values here are for builds without a name. */
#ifndef UNARC_VERSION_H
#define UNARC_VERSION_H

#ifndef UNARC_RELEASE
#define UNARC_RELEASE "0.0.4-dev"
#endif
#ifndef UNARC_VR
#define UNARC_VR   "0.4"
#endif
#define UNARC_DATE "21.10.2026"

#endif
