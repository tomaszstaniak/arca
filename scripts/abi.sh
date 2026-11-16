# Source from build scripts: . "$(dirname "$0")/abi.sh" <target>
# Each target has a separate SDK and build directory.
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
UNARC_ABI=${1:-abiv11}
ARCH=x86_64 CROSS_PREFIX=x86_64-aros
case $UNARC_ABI in
  abiv11)
    : "${UNARC_TOOLCHAIN_ABIV11:?set in local.env}" "${UNARC_SDK_ABIV11:?set in local.env}"
    TC=$UNARC_TOOLCHAIN_ABIV11 SDK=$UNARC_SDK_ABIV11
    ABI_TEXT="AROS ABIv11 x86_64" PKG_SUFFIX=aros-x86_64-abiv11-pc ;;
  v1)
    : "${UNARC_TOOLCHAIN_V1:?set in local.env}" "${UNARC_SDK_V1:?set in local.env}"
    TC=$UNARC_TOOLCHAIN_V1 SDK=$UNARC_SDK_V1
    ABI_TEXT="mainline AROS (ABIv1) x86_64" PKG_SUFFIX=aros-x86_64-abiv1-pc ;;
  raspi)
    : "${UNARC_TOOLCHAIN_RASPI:?set in local.env}" "${UNARC_SDK_RASPI:?set in local.env}"
    TC=$UNARC_TOOLCHAIN_RASPI SDK=$UNARC_SDK_RASPI
    ARCH=aarch64 CROSS_PREFIX=aarch64-aros
    ABI_TEXT="AROS Raspberry Pi AArch64 ABIv1" PKG_SUFFIX=aros-aarch64-abiv1-raspi
    export RPI_SDK="$SDK" ;;
  *) echo "unknown target \"$UNARC_ABI\": abiv11, v1 or raspi" >&2; exit 2 ;;
esac
R=$UNARC_ROOT
B=$R/build/$UNARC_ABI-$ARCH
export UNARC_TC=$TC UNARC_SDK=$SDK UNARC_ARCH=$ARCH UNARC_CROSS_PREFIX=$CROSS_PREFIX
