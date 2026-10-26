#!/bin/bash
# Mainline v1 only: libgcc's unwind-dw2.o rebuilt without the AROS patch's
# MD_FALLBACK_FRAME_STATE_FOR guard, into build/v1-x86_64/unwind-fix/.
# Mainline's gcc-10.5.0-aros.diff wraps __builtin_init_dwarf_reg_size_table in
# that guard, which x86_64 AROS never defines, so no C++ throw is ever caught
# there (workspace notes, "Mainline GCC 10.5: no C++ throw is caught").
# unarc-7z (7-Zip) reports data errors by throwing; it links this object
# before libgcc. Needs the configured mainline GCC build tree (read only);
# the recipe is aros-boxerapp's experiments/cxx-exceptions/build-unwind-fix.sh.
set -euo pipefail
. "$(dirname "$0")/abi.sh" v1
G0=${UNARC_V1_HOST_BUILD:?set in local.env: the mainline build tree with gen/host/tools/crosstools/gnu/gcc}
G=$G0/gen/host/tools/crosstools/gnu/gcc
P=$G0/Ports/host/gcc/gcc-10.5.0/libgcc
out=$B/unwind-fix
mkdir -p "$out"
perl -0pe 's/#ifdef MD_FALLBACK_FRAME_STATE_FOR\n(  __builtin_init_dwarf_reg_size_table \(dwarf_reg_size_table\);\n)#endif\n/$1/' \
  "$P/unwind-dw2.c" > "$out/unwind-dw2.c"
! grep -B1 '__builtin_init_dwarf_reg_size_table (dwarf' "$out/unwind-dw2.c" | grep -q MD_FALLBACK \
  || { echo "guard not removed" >&2; exit 1; }
cd "$G/x86_64-aros/libgcc"
"$G/gcc/xgcc" -B"$G/gcc/" -B"$TC/x86_64-aros/bin/" -B"$TC/x86_64-aros/lib/" \
  -isystem "$TC/x86_64-aros/include" -isystem "$TC/x86_64-aros/sys-include" \
  -mcmodel=large -O2 -DIN_GCC -DCROSS_DIRECTORY_STRUCTURE -isystem ./include \
  -DIN_LIBGCC2 -fbuilding-libgcc -fno-stack-protector -I. -I../.././gcc \
  -I"$P" -I"$P/../gcc" -I"$P/../include" -fexceptions -fvisibility=hidden -DHIDE_EXPORTS \
  -c "$out/unwind-dw2.c" -o "$out/unwind-dw2.o"
# The stock object has no dwarf_reg_size_table (its init is a bare ret).
"$TC/x86_64-aros-nm" "$out/unwind-dw2.o" | grep -q ' dwarf_reg_size_table$' \
  || { echo "dwarf_reg_size_table missing: fix not effective" >&2; exit 1; }
echo "built $out/unwind-dw2.o"
