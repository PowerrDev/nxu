# Firmware table inventory and AML namespace discovery.
I386_C_SOURCES += drivers/video/firmware.c kern/acpi/platform.c kern/acpi/host.c \
	$(wildcard EXTERNAL_HEADERS/uacpi/source/*.c)
I386_CFLAGS += -IEXTERNAL_HEADERS/uacpi/include -DUACPI_REDUCED_HARDWARE=1 -DUACPI_DEFAULT_LOG_LEVEL=2
