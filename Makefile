TARGET=crow
EXECUTABLE=$(TARGET).elf

GIT_VERSION := $(shell git describe --tags)

CUBE=submodules/STM32CubeF7_Drivers
HALS=$(CUBE)/STM32F7xx_HAL_Driver/Src
USBD=submodules/STM32CubeF7_USB/STM32_USB_Device_Library
WRLIB=submodules/wrLib
WRDSP=submodules/wrDsp
LUAS=submodules/lua/src
BOOTLOADER=submodules/dfu-stm32f7
BUILD_DIR := build
PRJ_DIR=crow
LUAC_CROSS=util/luacc

CC=arm-none-eabi-gcc
LD=arm-none-eabi-gcc
AR=arm-none-eabi-ar
AS=arm-none-eabi-as
CP=arm-none-eabi-objcopy
OBJDUMP=arm-none-eabi-objdump

FENNEL=fennel

# BIN=$(CP) -O ihex 
BIN = $(TARGET).bin

DEFS = -DUSE_STDPERIPH_DRIVER -DSTM32F7XX -DARM_MATH_CM7 -DHSE_VALUE=8000000
DEFS += -DSTM32F722xx -DUSE_HAL_DRIVER
STARTUP = $(CUBE)/CMSIS/Device/ST/STM32F7xx/Source/Templates/gcc/startup_stm32f722xx.s

MCFLAGS = -mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-sp-d16

STM32_INCLUDES = \
	-I$(WRLIB)/ \
	-I$(WRDSP)/ \
	-I$(CUBE)/CMSIS/Device/ST/STM32F7xx/Include/ \
	-I$(CUBE)/CMSIS/Include/ \
	-I$(CUBE)/STM32F7xx_HAL_Driver/Inc/ \
	-I/usr/local/include/ \
	-Iusbd/ \
	-I$(USBD)/Class/CDC/Inc/ \
	-I$(USBD)/Core/Inc/ \

OPTIMIZE       = -O2

CFLAGS += -std=c99
CFLAGS += -Wall
CFLAGS += -Wno-unused-function -Wno-unused-value
CFLAGS += $(MCFLAGS)
CFLAGS += $(OPTIMIZE)
CFLAGS += $(DEFS) -I. -I./ $(STM32_INCLUDES)
CFLAGS += -fsingle-precision-constant -Wdouble-promotion
CFLAGS += -DLUA_32BITS
CFLAGS += -fno-common
CFLAGS += -DVERSION=\"$(GIT_VERSION)\"
CFLAGS += -ffunction-sections -fdata-sections # provides majority of LTO binary size reduction
CFLAGS += -DLUA_COMPAT_MATHLIB
# crow's own lua libraries are bytecode in flash (.rodata). run them from there
# rather than copying their instructions into the lua heap. the range tells the
# loader which chunks really are in flash: anything else is copied as usual.
CFLAGS += -DBYTECODE_IN_FLASH
CFLAGS += -DBYTECODE_FLASH_START=0x08000000 -DBYTECODE_FLASH_END=0x08080000

# debugger: choose between uart (=0) & swtrace(=1). latter requires hardware mod
TRACE ?= 0
ifeq ($(TRACE), 1)
	CFLAGS += -DTRACE
endif

# telemetry: sys_telemetry() & the measurement hooks behind tests/hw. (=0) removes them
TELEMETRY ?= 1
ifeq ($(TELEMETRY), 1)
	CFLAGS += -DTELEMETRY
endif

# release: if (=1), disable all debug prints
R ?= 0
ifeq ($(R), 1)
	CFLAGS += -DRELEASE
	#CFLAGS += -flto # broken in debug mode. provides a small LTO binary size reduction
endif

LDFLAGS = -Wl,-T,stm32_flash.ld,-flto,-gc-sections
LIBS = -lm -lc -lnosys

SRC = main.c \
	stm32f7xx_it.c \
	system_stm32f7xx.c \
	$(HALS)/stm32f7xx_hal.c \
	$(HALS)/stm32f7xx_hal_cortex.c \
	$(HALS)/stm32f7xx_hal_rcc.c \
	$(HALS)/stm32f7xx_hal_rcc_ex.c \
	$(HALS)/stm32f7xx_hal_flash.c \
	$(HALS)/stm32f7xx_hal_flash_ex.c \
	$(HALS)/stm32f7xx_hal_gpio.c \
	$(HALS)/stm32f7xx_hal_i2c.c \
	$(HALS)/stm32f7xx_hal_i2s.c \
	$(HALS)/stm32f7xx_hal_dma.c \
	$(HALS)/stm32f7xx_hal_dma2d.c \
	$(HALS)/stm32f7xx_hal_pcd.c \
	$(HALS)/stm32f7xx_hal_pcd_ex.c \
	$(HALS)/stm32f7xx_hal_pwr.c \
	$(HALS)/stm32f7xx_hal_pwr_ex.c \
	$(HALS)/stm32f7xx_hal_rng.c \
	$(HALS)/stm32f7xx_hal_spi.c \
	$(HALS)/stm32f7xx_hal_tim.c \
	$(HALS)/stm32f7xx_hal_tim_ex.c \
	$(HALS)/stm32f7xx_hal_uart.c \
	$(HALS)/stm32f7xx_hal_usart.c \
	$(HALS)/stm32f7xx_ll_usb.c \
	$(wildcard lib/*.c) \
	$(wildcard ll/*.c) \
	$(wildcard usbd/*.c) \
	$(USBD)/Core/Src/usbd_core.c \
	$(USBD)/Core/Src/usbd_ctlreq.c \
	$(USBD)/Core/Src/usbd_ioreq.c \
	$(USBD)/Class/CDC/Src/usbd_cdc.c \
	$(WRLIB)/str_buffer.c \
	$(WRLIB)/wrConvert.c \
	$(WRLIB)/wrMath.c \
	$(WRLIB)/wrMeters.c \
	$(WRLIB)/wrQueue.c \
	$(WRDSP)/wrBlocks.c \
	$(WRDSP)/wrFilter.c \


# lua tests
LTESTS = $(wildcard tests/*.lua) \


# recipes!
all: $(TARGET).hex $(BIN)


### pre-process only files

# fennel script conversion to lua
FNL_SRC = $(wildcard lua/*.fnl) \

FNL_PP = $(FNL_SRC:%.fnl=%.lua)

# i2c descriptors
II_SRCD = lua/ii
II_SRC = $(wildcard $(II_SRCD)/*.lua)

$(BUILD_DIR)/ii_c_layer.h: $(II_SRC) util/ii_c_layer.lua | $(BUILD_DIR)
	@lua util/ii_c_layer.lua $(II_SRCD) $@
	@echo "ii-c-layer $@"

$(BUILD_DIR)/ii_mod_gen.h: $(II_SRC) util/ii_mod_gen.lua | $(BUILD_DIR)
	@lua util/ii_mod_gen.lua $(II_SRCD) $@
	@echo "ii-mod-gen $@"


### destination sources

# lua srcs: these get converted to bytecode strings wrapped in c-headers
LUA_SRC += lua/asl.lua
LUA_SRC += lua/asllib.lua
LUA_SRC += lua/calibrate.lua
LUA_SRC += lua/clock.lua
LUA_SRC += lua/crowlib.lua
LUA_SRC += lua/First.lua
LUA_SRC += lua/ii.lua
LUA_SRC += lua/input.lua
LUA_SRC += lua/metro.lua
LUA_SRC += lua/output.lua
LUA_SRC += lua/public.lua
LUA_SRC += lua/quote.lua
LUA_SRC += lua/sequins.lua
LUA_SRC += lua/timeline.lua
LUA_SRC += lua/hotswap.lua

LUA_PP = $(LUA_SRC:%.lua=%.lua.h)
LUA_PP: $(LUA_SRC)
	@echo "pre-compiling lua sources to bytecode wrapped in c headers"

LUACORE_OBJS=	lapi.o lcode.o lctype.o ldebug.o ldo.o ldump.o lfunc.o lgc.o llex.o \
		lmem.o lobject.o lopcodes.o lparser.o lstate.o lstring.o ltable.o \
		ltm.o lundump.o lvm.o lzio.o
LUALIB_OBJS=	lauxlib.o lbaselib.o lbitlib.o lcorolib.o ldblib.o liolib.o \
		lmathlib.o loslib.o lstrlib.o ltablib.o lutf8lib.o loadlib.o linit.o


# build the objects from c source
OBJDIR = .
OBJS = $(SRC:%.c=$(OBJDIR)/%.o)
OBJS += $(addprefix $(LUAS)/,$(LUACORE_OBJS) $(LUALIB_OBJS) )
OBJS += Startup.o

$(OBJS): $(LUA_PP)

# specific objects that require built dependencies (ii)
$(OBJDIR)/lib/l_bootstrap.o: $(LUA_PP) #$(BUILD_DIR)/ii_lualink.h
$(OBJDIR)/lib/ii.o: $(BUILD_DIR)/ii_c_layer.h
$(OBJDIR)/lib/l_ii_mod.o: $(BUILD_DIR)/ii_mod_gen.h

# generate the build directory
$(BUILD_DIR):
	@echo build_dir $(BUILD_DIR)/
	@mkdir -p $(BUILD_DIR)


# C dependencies echoed into Makefile
DEP = $(OBJS:.o=.d)  # one dependency file for each source


# OS dependent size printing
UNAME := $(shell uname)

GETSIZE = stat

ifeq ($(UNAME), Darwin)
	GETSIZE = stat -x
endif


.PHONY: tests
tests:
	@for t in $(LTESTS); do \
		lua $$t; \
	done

# include all DEP files in the makefile
# will rebuild elements if dependent C headers are changed
-include $(DEP)

$(TARGET).hex: $(EXECUTABLE)
	@$(CP) -O ihex $^ $@

$(EXECUTABLE): $(OBJS)
	@$(LD) -g $(MCFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) -o $@
	@echo "linked:       $@"
	@$(OBJDUMP) --disassemble $@ > $@.lst
	@echo "disassembly:  $@.lst"

$(BIN): $(EXECUTABLE)
	@$(CP) -O binary $< $@
	@echo "binary:       $@"
	@$(OBJDUMP) -x --syms $< > $(addsuffix .dmp, $(basename $<))
	@echo "symbol table: $@.dmp"
	@echo "Release: "$(R)
	@$(GETSIZE) $(BIN) | grep 'Size'
	@echo "        ^ must be less than 384kB (384,000)"
	# 512kb -64kb(bootloader) -128kb(scripts)

flash: $(BIN)
	st-flash write $(BIN) 0x08020000

debug:
	make flash TRACE=1
	stlink-trace -c 216

dfu: $(BIN)
	sudo dfu-util -a 0 -s 0x08020000 -R -D $(BIN) -d ,0483:df11

dfureset:
	@stty -F /dev/ttyACM0 raw speed 115200
	@echo '^^b' > /dev/ttyACM0
	@sleep 1

pydfu: $(TARGET).dfu $(BIN)
	@python3 util/pydfu.py -u $<

$(TARGET).dfu: $(BIN)
	python3 util/dfu.py -D 0x0483:0xDF11 -b 0x08020000:$^ $@

boot:
	cd $(BOOTLOADER) && \
	make R=1 flash

zip: $(BIN) $(TARGET).dfu
	mkdir -p $(TARGET)-$(GIT_VERSION)
	cp util/osx_linux-update_firmware.command $(TARGET)-$(GIT_VERSION)/
	cp util/osx_linux-erase_userscript.command $(TARGET)-$(GIT_VERSION)/
	cp util/windows-update_firmware.bat $(TARGET)-$(GIT_VERSION)/
	cp util/windows-erase_userscript.bat $(TARGET)-$(GIT_VERSION)/
	cp util/blank.bin $(TARGET)-$(GIT_VERSION)/
	cp $(BIN) $(TARGET)-$(GIT_VERSION)/
	cp $(TARGET).dfu $(TARGET)-$(GIT_VERSION)/
	zip -r $(TARGET)-$(GIT_VERSION).zip $(TARGET)-$(GIT_VERSION)/

%.o: %.c
	@$(CC) -ggdb $(CFLAGS) -c $< -o $@
	@echo $@

%.d: %.c
	@$(CC) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@

%.s: %.c
	@$(CC) -ggdb $(CFLAGS) -S $< -o $@

%.lua: %.fnl
	@echo f2l $< "->" $@
	@$(FENNEL) --compile $< > $@

# a bunch of gnarly make-functions to massage the intermediate stage filenames
# everything goes into /build now, and we have to save output of LUAC_CROSS into
# named files for (xxd -i) to build include files with valid names
# could be avoided by a more complicated pass in (sed), but this was easier
# 1. cross-compile all .lua files into .lc bytecode for stm32-arm-cortex-m7 format
# 2. wrap the .lc binary files into .h headers with auto-generated names
# 3. add const qualifiers to headers: the length for C99 struct initializers, and
#    the bytecode itself so it is linked into flash instead of being copied to ram

%.lua.h: %.lua $(BUILD_DIR)
	@echo l2h $< "->" $(addprefix $(BUILD_DIR)/, $(notdir $(subst .lua.h,.h,$@)))
	@$(LUAC_CROSS) -s -o $(addprefix $(BUILD_DIR)/, $(notdir $(subst .lua,.lc,$<))) $<
	@xxd -i $(addprefix $(BUILD_DIR)/, $(notdir $(subst .lua,.lc,$<))) $(addprefix $(BUILD_DIR)/, $(notdir $(subst .lua.h,.h,$@)))
	@sed -i 's/unsigned int/const unsigned int/g' $(addprefix $(BUILD_DIR)/, $(notdir $(subst .lua.h,.h,$@)))
	@sed -i 's/^unsigned char/const unsigned char/g' $(addprefix $(BUILD_DIR)/, $(notdir $(subst .lua.h,.h,$@)))

Startup.o: $(STARTUP)
	@$(CC) $(CFLAGS) -c $< -o $@
	@echo $@

wav: fsk-wav

fsk-wav: $(BIN)
	export PYTHONPATH=$$PYTHONPATH:'.' && \
	cd .. && python stm-audio-bootloader/fsk/encoder.py \
		-s 48000 -b 16 -n 8 -z 4 -p 256 -g 16384 -k 1800 \
		$(PRJ_DIR)/$(BIN)

erase:
	st-flash erase

norns:
	lua util/ii_norns_actions.lua lua/ii/ $(NORNS_DIR)/lua/core/crow/ii_actions.lua
	lua util/ii_norns_events.lua lua/ii/ $(NORNS_DIR)/lua/core/crow/ii_events.lua

.PHONY: clean
clean:
	@rm -rf Startup.lst $(TARGET).elf.lst $(OBJS) $(AUTOGEN) \
	$(TARGET).bin  $(TARGET).out  $(TARGET).hex $(TARGET).dfu \
	$(TARGET).map  $(TARGET).dmp  $(EXECUTABLE) $(DEP) \
	$(BUILD_DIR) lua/*.lua.h \
	$(TARGET)-$(GIT_VERSION)/  *.zip \

splint:
	splint -I. -I./ $(STM32_INCLUDES) *.c
