# Bare-metal Armv8-M Mainline (Cortex-M33, hard-float) cross toolchain,
# matching the Raspberry Pi RP2350's Arm cores (dual Cortex-M33 @150MHz,
# 520KB SRAM - the Pico 2's default core mode; RP2350 can alternatively run
# its two Hazard3 RISC-V cores instead, but Cortex-M33 is what every current
# RP2350 SDK/toolchain targets by default, and is what matters for struct
# layout/memory footprint here, not instruction selection).
#
# Requires (Ubuntu/Debian): gcc-arm-none-eabi, picolibc-arm-none-eabi,
# binutils-arm-none-eabi. Used with -DCMAKE_TOOLCHAIN_FILE=.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)

# gcc-ar/gcc-ranlib (not plain binutils ar/ranlib) so the LTO plugin can index
# symbols inside the slim-LTO object files that -flto produces.
set(CMAKE_AR arm-none-eabi-gcc-ar)
set(CMAKE_RANLIB arm-none-eabi-gcc-ranlib)

# -Os beats -O2/-O3 for size here, same measurement as the RV32IMAFC/BL616
# toolchain (see toolchain-rv32imafc.cmake) - not re-measured per-arch, but
# there is no reason -Os's usual size win would flip on Cortex-M33.
set(CHDR_CORTEX_M33_FLAGS "-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 --specs=picolibc.specs --crt0=semihost --oslib=semihost -Os -flto -ffunction-sections -fdata-sections")
set(CMAKE_C_FLAGS_INIT "${CHDR_CORTEX_M33_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CHDR_CORTEX_M33_FLAGS} -Wl,--gc-sections")

# bare-metal: skip the link-a-full-executable compiler sanity check
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
