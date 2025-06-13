#ifndef SHARED_BUFFER_PROTOCOL_H
#define SHARED_BUFFER_PROTOCOL_H

#include <stdint.h>
#include "audio_config.h"

// Shared Memory Protocol Structure
typedef struct __attribute__((packed)) {
    volatile uint32_t magic;           // 0xABCD2025
    volatile uint32_t version;         // Protocol version
    volatile uint32_t hps_ready;       // HPS has data ready
    volatile uint32_t fpga_ready;      // FPGA can receive data
    
    volatile uint32_t data_size;       // Audio data size in bytes
    volatile uint32_t sample_rate;     // Current sample rate
    volatile uint32_t channels;        // Number of channels
    volatile uint32_t bits_per_sample; // Bits per sample
    
    volatile uint32_t current_track;   // Current track (0-9)
    volatile uint32_t command;         // Command from FPGA to HPS
    volatile uint32_t status;          // System status
    volatile uint32_t playback_pos;    // Playback position
    
    volatile uint32_t read_ptr;        // FPGA read pointer
    volatile uint32_t write_ptr;       // HPS write pointer
    volatile uint32_t buffer_level;    // Buffer fill level
    
    volatile uint32_t last_error;      // Last error code
    
} shared_control_t;

// Commands
#define CMD_NONE               0
#define CMD_LOAD_TRACK         1
#define CMD_NEXT_TRACK         2
#define CMD_PREV_TRACK         3
#define CMD_STOP               4

// Status
#define STATUS_IDLE            0
#define STATUS_LOADING         1
#define STATUS_READY           2
#define STATUS_PLAYING         3
#define STATUS_ERROR           4

// Error Codes
#define ERROR_NONE             0
#define ERROR_FILE_NOT_FOUND   1
#define ERROR_INVALID_FORMAT   2
#define ERROR_READ_FAILED      3

// Protocol Constants
#define PROTOCOL_MAGIC         0xABCD2025
#define PROTOCOL_VERSION       0x00020000

// Memory Access
#define SHARED_CONTROL_PTR     ((shared_control_t*)SHARED_BUFFER_BASE)
#define SHARED_AUDIO_PTR       ((uint8_t*)(SHARED_BUFFER_BASE + sizeof(shared_control_t)))

#endif /* SHARED_BUFFER_PROTOCOL_H */