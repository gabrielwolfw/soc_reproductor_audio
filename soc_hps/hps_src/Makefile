# Makefile para HPS Audio Loader - DE1-SoC

# Configuración del toolchain para DE1-SoC
CROSS_COMPILE = arm-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

# Flags de compilación para HPS
CFLAGS = -Wall -O2 -std=c99 -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard
LDFLAGS = -static

# Archivos
TARGET = hps_audio_loader
SOURCES = hps_audio_loader.c
HEADERS = shared_buffer_protocol.h audio_config.h

# Target principal
all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
    $(CC) $(CFLAGS) -o $(TARGET) $(SOURCES) $(LDFLAGS)
    @echo "Compilation successful for ARM Cortex-A9"
    @ls -lh $(TARGET)

# Target debug
debug: CFLAGS += -g -DDEBUG
debug: LDFLAGS = 
debug: $(TARGET)

# Target optimizado
release: $(TARGET)
    $(STRIP) $(TARGET)
    @echo "Binary stripped for release"

# Verificar toolchain
check:
    @which $(CC) > /dev/null || (echo "ERROR: $(CC) not found"; exit 1)
    @$(CC) --version | head -1

# Limpiar
clean:
    rm -f $(TARGET)

.PHONY: all debug release check clean