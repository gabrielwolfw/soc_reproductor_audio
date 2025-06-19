TARGET = hps_audio
C_SOURCE = hps_audio.c
CROSS_COMPILE = arm-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc

# Añadir -lm al final para enlazar la librería matemática (para sin())
CFLAGS = -g -Wall -I.
LDFLAGS = -Wall -lm

all: $(TARGET)

$(TARGET): $(C_SOURCE:.c=.o)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

.PHONY: clean
clean:
	rm -f $(TARGET) *.o