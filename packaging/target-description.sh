#!/bin/bash
set -eu
UNARC_ABI=${1:?ABI required}
if [ "$UNARC_ABI" = raspi ]; then
  printf '%s\n' "This release is for native AROS Raspberry Pi AArch64 ABIv1." \
    "Use the x86_64 packages for PC systems. Hosted AROS is a separate target."
elif [ "$UNARC_ABI" = v1 ]; then
  printf '%s\n' "This release is for mainline AROS (ABIv1) on x86_64. It does not run" \
    "on ABIv11 systems: use the -aros-x86_64-abiv11-pc package there."
else
  printf '%s\n' "This release is for AROS ABIv11 on x86_64 using the 2026.09" \
    "base. It does not run on mainline AROS (ABIv1): use the -aros-x86_64-abiv1-pc package" "there."
fi
