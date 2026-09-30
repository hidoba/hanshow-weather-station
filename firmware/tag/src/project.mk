OBJS += \
$(OUT_PATH)/epd_spi.o \
$(OUT_PATH)/epd.o \
$(OUT_PATH)/ota.o \
$(OUT_PATH)/main.o

$(OUT_PATH)/%.o: ./src/%.c
	@echo 'Building file: $<'
	@$(TC32_COMPILER_PATH)tc32-elf-gcc $(GCC_FLAGS) $(INCLUDE_PATHS) -c -o"$@" "$<"
