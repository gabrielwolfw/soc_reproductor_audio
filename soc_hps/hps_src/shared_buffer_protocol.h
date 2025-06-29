#ifndef SHARED_BUFFER_PROTOCOL_H
#define SHARED_BUFFER_PROTOCOL_H

#include <stdint.h>
#include "audio_config.h"

// Estructura de control para streaming
typedef struct {
    // Control básico
    volatile uint32_t command;          // Comando actual
    volatile uint32_t status;           // Estado del sistema
    volatile uint32_t song_id;          // ID de canción actual (0-2)
    
    // Control de chunks
    volatile uint32_t current_chunk;    // Chunk actual en reproducción
    volatile uint32_t total_chunks;     // Total de chunks de la canción
    volatile uint32_t chunk_size;       // Tamaño del chunk actual en bytes
    volatile uint32_t chunk_samples;    // Samples en el chunk actual
    
    // Información de la canción
    volatile uint32_t song_total_size;  // Tamaño total de la canción
    volatile uint32_t song_position;    // Posición actual en bytes
    volatile uint32_t song_duration;    // Duración en samples
    
    // Flags de comunicación
    volatile uint32_t chunk_ready;      // 1=HPS cargó chunk, 0=FPGA consumió
    volatile uint32_t request_next;     // 1=FPGA solicita siguiente chunk
    volatile uint32_t buffer_underrun;  // 1=Buffer vacío (error)
    
    // Información de canciones precargada
    volatile uint32_t song_sizes[MAX_TRACKS];    // Tamaños de las 3 canciones
    volatile uint32_t song_chunks[MAX_TRACKS];   // Chunks de cada canción
    
    // Debugging/status
    volatile uint32_t chunks_loaded;    // Total de chunks cargados
    volatile uint32_t last_error;       // Último código de error
    
    // Reservado para expansión futura
    volatile uint32_t reserved[8];
    
} __attribute__((packed)) audio_control_t;

// Verificación manual del tamaño (reemplaza static_assert)
#define CONTROL_SIZE_CHECK() \
    do { \
        if (sizeof(audio_control_t) > CONTROL_BUFFER_SIZE) { \
            printf("ERROR: Control structure too large (%zu > %d)\n", \
                   sizeof(audio_control_t), CONTROL_BUFFER_SIZE); \
            return -1; \
        } \
    } while(0)

// Funciones helper para acceso a memoria compartida
static inline audio_control_t* get_control_buffer(void* shared_mem) {
    return (audio_control_t*)((uint8_t*)shared_mem + CONTROL_OFFSET);
}

static inline void* get_audio_buffer(void* shared_mem) {
    return (void*)((uint8_t*)shared_mem + AUDIO_DATA_OFFSET);
}

// Macros para verificación de chunks
#define IS_VALID_SONG_ID(id)    ((id) < MAX_TRACKS)

#endif /* SHARED_BUFFER_PROTOCOL_H */