#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

#include <stdint.h>

// Hardware Configuration
#define HW_REGS_BASE        0xFF200000
#define HW_REGS_SPAN        0x00200000
#define HW_REGS_MASK        (HW_REGS_SPAN - 1)

// Shared Memory Configuration - ACTUALIZADO según system.h
#define SHARED_MEMORY_NIOS_BASE   0x40000
#define SHARED_MEMORY_SIZE        131072    // 128KB total
#define SHARED_MEMORY_HPS_BASE    0xC0000000

#define SHARED_BUFFER_BASE  SHARED_MEMORY_HPS_BASE
#define SHARED_BUFFER_SIZE  SHARED_MEMORY_SIZE

// NUEVO: Configuración de Streaming por Chunks
#define CONTROL_BUFFER_SIZE     1024        // 1KB para control
#define AUDIO_CHUNK_SIZE        (120 * 1024) // 120KB para datos de audio
#define MAX_CHUNKS_IN_MEMORY    1           // Solo un chunk a la vez

// Layout de memoria compartida
#define CONTROL_OFFSET          0x0000      // Bytes 0-1023: Control
#define AUDIO_DATA_OFFSET       0x0400      // Bytes 1024+: Audio data

// Audio Configuration
#define AUDIO_CODEC_NIOS_BASE   0x8860
#define AUDIO_CODEC_SPAN        16
#define AUDIO_CODEC_IRQ         2

#define AUDIO_SAMPLE_RATE   48000
#define AUDIO_CHANNELS      2
#define AUDIO_BITS_PER_SAMPLE 16

// Sistema de chunks
#define CHUNK_SIZE              AUDIO_CHUNK_SIZE
#define BYTES_PER_SAMPLE        (AUDIO_BITS_PER_SAMPLE / 8 * AUDIO_CHANNELS)
#define SAMPLES_PER_CHUNK       (CHUNK_SIZE / BYTES_PER_SAMPLE)

// Comandos extendidos para streaming
#define CMD_PLAY            0x01
#define CMD_PAUSE           0x02
#define CMD_STOP            0x03
#define CMD_NEXT            0x04
#define CMD_PREV            0x05
#define CMD_LOAD_CHUNK      0x10
#define CMD_CHUNK_READY     0x11
#define CMD_REQUEST_CHUNK   0x12

// Estados extendidos
#define STATUS_READY        0x00
#define STATUS_PLAYING      0x01
#define STATUS_PAUSED       0x02
#define STATUS_LOADING      0x03
#define STATUS_BUFFERING    0x10
#define STATUS_CHUNK_READY  0x11
#define STATUS_END_OF_SONG  0x12

// System Components
#define TIMER_NIOS_BASE     0x8820
#define TIMER_IRQ           0
#define TIMER_FREQ          50000000

#define BUTTONS_NIOS_BASE   0x8800
#define BUTTONS_IRQ         3

#define SEVEN_SEGMENTS_NIOS_BASE  0x8810

// HPS Bridge Mapping
#define AUDIO_CODEC_HPS_BASE    (HW_REGS_BASE + AUDIO_CODEC_NIOS_BASE)
#define TIMER_HPS_BASE          (HW_REGS_BASE + TIMER_NIOS_BASE)
#define BUTTONS_HPS_BASE        (HW_REGS_BASE + BUTTONS_NIOS_BASE)
#define SEVEN_SEGMENTS_HPS_BASE (HW_REGS_BASE + SEVEN_SEGMENTS_NIOS_BASE)

// System Configuration
#define MAX_TRACKS          3
#define MAX_FILENAME_LEN    256

// Audio File Configuration
#define WAV_HEADER_SIZE     44

// Path Configuration
#define SD_MOUNT_POINT      "/media/sd"
#define SONGS_DIRECTORY     "/media/sd/songs"

#endif /* AUDIO_CONFIG_H */