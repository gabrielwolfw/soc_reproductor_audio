CC = arm-linux-gnueabihf-gcc
TARGET = hps_audio_loader
SOURCE = hps_audio_loader.c

all:
	$(CC) -O2 -static -o $(TARGET) $(SOURCE)
	@echo "Compiled for ARM"
	@ls -lh $(TARGET)

clean:
	rm -f $(TARGET)
