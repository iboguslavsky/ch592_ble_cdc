CROSS_COMPILE ?= /home/igor/esp32/tools/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-
CC = $(CROSS_COMPILE)gcc
AS = $(CROSS_COMPILE)gcc
OBJCOPY = $(CROSS_COMPILE)objcopy
OBJDUMP = $(CROSS_COMPILE)objdump
SIZE = $(CROSS_COMPILE)size

TARGET = ch592_ble_cdc
BUILD_DIR = build

SRCS_C = \
	APP/bridge_main.c \
	APP/bridge.c \
	APP/usb_cdc.c \
	Profile/devinfoservice.c \
	Profile/ble_uart_service.c \
	HAL/MCU.c \
	HAL/RTC.c \
	HAL/SLEEP.c \
	StdPeriphDriver/CH59x_sys.c \
	StdPeriphDriver/CH59x_clk.c \
	StdPeriphDriver/CH59x_flash.c \
	StdPeriphDriver/CH59x_gpio.c \
	StdPeriphDriver/CH59x_pwr.c \
	StdPeriphDriver/CH59x_adc.c

SRCS_ASM = \
	Startup/startup_CH592.S \
	LIB/ble_task_scheduler.S

INCLUDES = \
	-IAPP/include \
	-IHAL/include \
	-IProfile/include \
	-ILIB \
	-IStdPeriphDriver/inc \
	-IRVMSIS

CFLAGS = -march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore \
         -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common \
         -std=gnu99 -DDEBUG=Debug_USB -DHAL_SLEEP=FALSE -DCLK_OSC32K=1 -DDCDC_ENABLE=FALSE -DINT_SOFT \
         -DBLE_BUFF_MAX_LEN=251 -DBLE_BUFF_NUM=16 -DBLE_TX_NUM_EVENT=10 $(INCLUDES)

ASFLAGS = -march=rv32imac_zicsr_zifencei -mabi=ilp32 $(INCLUDES) -x assembler-with-cpp

LDFLAGS = -march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medany \
          -T Ld/Link.ld -nostartfiles -Wl,--gc-sections \
          -Wl,-Map=$(BUILD_DIR)/$(TARGET).map \
          --specs=nano.specs --specs=nosys.specs \
          -LStdPeriphDriver -LLIB -lISP592 -lCH59xBLE

OBJS = $(addprefix $(BUILD_DIR)/, $(SRCS_C:.c=.o)) $(addprefix $(BUILD_DIR)/, $(SRCS_ASM:.S=.o))

all: $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD_DIR)/$(TARGET).hex: $(BUILD_DIR)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	$(OBJCOPY) -O binary -S $< $@

flash: $(BUILD_DIR)/$(TARGET).bin
	./flash.sh

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean flash
