#!/bin/sh
# Command-line build of the STM32 firmware, same flags as the CubeIDE project (GCC -O2).
# Needs arm-none-eabi-gcc in PATH (or ARM_GCC_BIN=<toolchain bin dir>).
# Output: build/firmware/firmware.elf (build/ is ignored by git). Warnings are errors.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
fw="$root/firmware"
out="$root/build/firmware"
[ -n "${ARM_GCC_BIN:-}" ] && PATH="$ARM_GCC_BIN:$PATH"
mkdir -p "$out"

cpu="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard"
cflags="$cpu -std=gnu11 -O2 -g3 -DUSE_HAL_DRIVER -DSTM32L476xx -ffunction-sections -fdata-sections -Wall -Werror --specs=nano.specs
 -I$fw/Core/Inc -I$fw/Drivers/STM32L4xx_HAL_Driver/Inc -I$fw/Drivers/STM32L4xx_HAL_Driver/Inc/Legacy
 -I$fw/Drivers/CMSIS/Device/ST/STM32L4xx/Include -I$fw/Drivers/CMSIS/Include
 -I$fw/Middlewares/Third_Party/FreeRTOS/Source/include -I$fw/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
 -I$fw/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F"

objs=""
for src in $(cd "$fw" && find Core Drivers Middlewares -name '*.c' -o -name '*.s' | sort); do
  obj="$out/$(echo "$src" | tr '/' '_').o"
  case "$src" in
    *.s) arm-none-eabi-gcc $cpu -x assembler-with-cpp -c "$fw/$src" -o "$obj" ;;
    *)   arm-none-eabi-gcc $cflags -c "$fw/$src" -o "$obj" ;;
  esac
  objs="$objs $obj"
done

arm-none-eabi-gcc $cpu --specs=nosys.specs --specs=nano.specs -T"$fw/STM32L476RGTX_FLASH.ld" \
  -Wl,--gc-sections -Wl,-Map="$out/firmware.map" -static -Wl,--start-group -lc -lm -Wl,--end-group \
  $objs -o "$out/firmware.elf"
arm-none-eabi-size "$out/firmware.elf"
