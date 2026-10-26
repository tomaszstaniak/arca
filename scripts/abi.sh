# Source from the build scripts: . "$(dirname "$0")/abi.sh" <abi>
# One toolchain, SDK and build directory per ABI; nothing is shared between
# them (binaries of one ABI do not run on the other).
#   abiv11  AROS ABIv11 x86_64 (AROS One, the 2026.09 base): UNARC_TOOLCHAIN_ABIV11, UNARC_SDK_ABIV11
#   v1      mainline AROS (ABIv1) x86_64: UNARC_TOOLCHAIN_V1, UNARC_SDK_V1
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
UNARC_ABI=${1:-abiv11}
case $UNARC_ABI in
  abiv11)
    : "${UNARC_TOOLCHAIN_ABIV11:?set in local.env}" "${UNARC_SDK_ABIV11:?set in local.env}"
    TC=$UNARC_TOOLCHAIN_ABIV11 SDK=$UNARC_SDK_ABIV11
    ABI_TEXT="AROS ABIv11 x86_64" PKG_SUFFIX=x86_64-abiv11 ;;
  v1)
    : "${UNARC_TOOLCHAIN_V1:?set in local.env}" "${UNARC_SDK_V1:?set in local.env}"
    TC=$UNARC_TOOLCHAIN_V1 SDK=$UNARC_SDK_V1
    ABI_TEXT="mainline AROS (ABIv1) x86_64" PKG_SUFFIX=x86_64-v1 ;;
  *) echo "unknown ABI \"$UNARC_ABI\": abiv11 or v1" >&2; exit 2 ;;
esac
R=$UNARC_ROOT
B=$R/build/$UNARC_ABI-x86_64
export UNARC_TC=$TC UNARC_SDK=$SDK
