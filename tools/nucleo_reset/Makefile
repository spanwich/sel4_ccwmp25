CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
CFLAGS  = -mcpu=cortex-m7 -mthumb -Os -Wall -Wextra -ffreestanding -nostdlib

all: nucleo_reset.bin

nucleo_reset.elf: main.c link.ld
	$(CC) $(CFLAGS) -T link.ld -o $@ main.c

nucleo_reset.bin: nucleo_reset.elf
	$(OBJCOPY) -O binary $< $@

# st-flash 1.8 fails on the F7 flash loader; use OpenOCD
flash: nucleo_reset.elf
	openocd -f board/st_nucleo_f7.cfg -c "program $< verify reset exit"

clean:
	rm -f nucleo_reset.elf nucleo_reset.bin

.PHONY: all flash clean
