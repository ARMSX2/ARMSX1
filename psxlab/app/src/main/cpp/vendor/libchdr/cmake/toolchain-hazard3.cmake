# Bare-metal RV32IMAC_Zicsr_Zifencei/ilp32 cross toolchain, matching the
# Raspberry Pi RP2350's Hazard3 RISC-V cores (dual Hazard3 @150MHz, 520KB
# SRAM - the RP2350's alternate core mode to its Cortex-M33 pair; see
# cmake/toolchain-cortex-m33.cmake). Hazard3 has no F/D extension - no
# hardware FPU - so this is soft-float ilp32, not ilp32f/ilp32d: a
# meaningfully different ABI from every other RV32 target in this repo
# (BL616/ESP32-P4's rv32imafc/ilp32f), which is the whole reason this needs
# its own toolchain file and RAM-budget measurement rather than reusing
# toolchain-rv32imafc.cmake. Hazard3's Zba/Zbb/Zbs/Zbkb/Zcb/Zcmp extensions
# are dropped for the same reason toolchain-rv32imafc.cmake drops BL616's
# vendor extensions: they don't affect struct layout or memory footprint,
# only instruction selection.
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

# -Os beats -O2/-O3 for size here, same measurement as
# toolchain-rv32imafc.cmake - not re-measured per-ABI, but there is no
# reason -Os's usual size win would flip on soft-float rv32imac.
set(CHDR_HAZARD3_FLAGS "-march=rv32imac_zicsr_zifencei -mabi=ilp32 --specs=picolibc.specs --crt0=semihost --oslib=semihost -Os -flto -ffunction-sections -fdata-sections")
set(CMAKE_C_FLAGS_INIT "${CHDR_HAZARD3_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CHDR_HAZARD3_FLAGS} -Wl,--gc-sections")

# bare-metal: skip the link-a-full-executable compiler sanity check
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
