# Bare-metal RV32IMAFC/ilp32f cross toolchain, matching the Bouffalo Lab BL616
# (RV32IMAFCP @ 320MHz, 480KB SRAM) base ISA/ABI. The vendor's "P" (packed-SIMD)
# and custom xtheade/zpsfoperand extensions are dropped since they don't affect
# struct layout or memory footprint - only the RAM/heap numbers matter here, not
# instruction selection.
#
# Also matches the RISC-V cores on Espressif's ESP32-P4 (dual-core RV32IMAFC
# up to ~360-400MHz, 768KB HP L2MEM + 32KB LP SRAM) for the same reason - its
# Zc/XespV/XespLoop extensions are additive and don't change struct layout or
# the base ilp32f ABI either. See contrib/esp32p4/ and
# .github/workflows/esp32p4-build.yml.
#
# Requires (Ubuntu/Debian): gcc-riscv64-unknown-elf, picolibc-riscv64-unknown-elf,
# binutils-riscv64-unknown-elf. Used with -DCMAKE_TOOLCHAIN_FILE=.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)

set(CMAKE_C_COMPILER riscv64-unknown-elf-gcc)
set(CMAKE_ASM_COMPILER riscv64-unknown-elf-gcc)

# gcc-ar/gcc-ranlib (not plain binutils ar/ranlib) so the LTO plugin can index
# symbols inside the slim-LTO object files that -flto produces.
set(CMAKE_AR riscv64-unknown-elf-gcc-ar)
set(CMAKE_RANLIB riscv64-unknown-elf-gcc-ranlib)

# -Os beats -O2/-O3 for size here (measured: -O2 +18%, -O3 +31% vs -Os on the
# full 9-codec build); -flto adds another ~3% via cross-TU dead-code elim,
# confirmed CRC-correct under qemu-system-riscv32 - see the libchdr PR/issue
# that measured this for the numbers.
set(CHDR_RV32_FLAGS "-march=rv32imafc -mabi=ilp32f --specs=picolibc.specs --crt0=semihost --oslib=semihost -Os -flto -ffunction-sections -fdata-sections")
set(CMAKE_C_FLAGS_INIT "${CHDR_RV32_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CHDR_RV32_FLAGS} -Wl,--gc-sections")

# bare-metal: skip the link-a-full-executable compiler sanity check
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
