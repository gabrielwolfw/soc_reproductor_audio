#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

#include <stdint.h>

// Hardware Configuration
#define HW_REGS_BASE        0xFF200000
#define HW_REGS_SPAN        0x00200000
#define HW_REGS_MASK        (HW_REGS_SPAN - 1)

// Shared Memory Configuration
#define SHARED_MEMORY_NIOS_BASE   0x40000
#define SHARED_MEMORY_SIZE        131072
#define SHARED_MEMORY_HPS_BASE    0xC0000000

#define SHARED_BUFFER_BASE  SHARED_MEMORY_HPS_BASE
#define SHARED_BUFFER_SIZE  SHARED_MEMORY_SIZE

// Audio Configuration
#define AUDIO_CODEC_NIOS_BASE   0x8860
#define AUDIO_CODEC_SPAN        16
#define AUDIO_CODEC_IRQ         2

#define AUDIO_SAMPLE_RATE   48000
#define AUDIO_CHANNELS      2
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_BUFFER_SIZE   (120 * 1024)
#define CONTROL_BUFFER_SIZE (8 * 1024)

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
#define MAX_TRACKS          10
#define MAX_FILENAME_LEN    256

// Memory Layout
#define CONTROL_OFFSET      0x0000
#define AUDIO_DATA_OFFSET   0x2000

// Audio File Configuration
#define WAV_HEADER_SIZE     44

// Path Configuration
#define SD_MOUNT_POINT      "/media/sd"
#define SONGS_DIRECTORY     "/media/sd/songs"

#endif /* AUDIO_CONFIG_H */