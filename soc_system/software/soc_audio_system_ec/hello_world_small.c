#include "system.h"
#include "sys/alt_stdio.h"
#include "sys/alt_irq.h"
#include <stdint.h>
#include <unistd.h>
#include "altera_up_avalon_audio.h"

#define SAMPLE_RATE 48000
#define AUDIO_CHUNK_SIZE (30 * 1024)  // 30 KB chunks
#define CONTROL_OFFSET 0x0000
#define AUDIO_DATA_OFFSET 0x0400

// *** COMANDOS Y ESTADOS ***
#define CMD_NONE    0
#define CMD_PLAY    1
#define CMD_PAUSE   2
#define CMD_STOP    3
#define CMD_NEXT    4
#define CMD_PREV    5

#define STATUS_READY    0
#define STATUS_PLAYING  1
#define STATUS_PAUSED   2

// *** ESTRUCTURA EXACTAMENTE IGUAL QUE HPS ***
typedef struct __attribute__((packed)) {
    // Identificación y control básico (16 bytes)
    volatile uint32_t magic;           // 0xABCD2025
    volatile uint32_t command;         // HPS → NIOS comandos
    volatile uint32_t status;          // NIOS → HPS estado
    volatile uint32_t song_id;         // Canción actual (0-2)
    
    // Control de chunks (16 bytes)
    volatile uint32_t chunk_ready;     // 1=HPS cargó, 0=NIOS consumió
    volatile uint32_t chunk_size;      // Tamaño actual en bytes
    volatile uint32_t request_next;    // 1=NIOS solicita siguiente
    volatile uint32_t current_chunk;   // Número de chunk actual
    
    // Información de canción (16 bytes)
    volatile uint32_t total_chunks;    // Total chunks de la canción
    volatile uint32_t song_total_size; // Tamaño total del archivo
    volatile uint32_t song_position;   // Posición actual en bytes
    volatile uint32_t duration_sec;    // Duración en segundos
    
    // Sistema y comunicación (16 bytes)
    volatile uint32_t hps_connected;   // 1=HPS activo
    volatile uint32_t fpga_heartbeat;  // Contador de NIOS
    volatile uint32_t sample_rate;     // 48000 Hz
    volatile uint32_t channels;        // 2 (estéreo)
    
    // Estado y debugging (16 bytes)
    volatile uint32_t buffer_level;    // Nivel de buffer (0-100%)
    volatile uint32_t error_flags;     // Flags de error
    volatile uint32_t bytes_played;    // Bytes reproducidos total
    volatile uint32_t chunks_loaded;   // Total de chunks cargados
    
    // Reservado para expansión (176 bytes = 256 bytes total)
    volatile uint32_t reserved[44];
} compact_shared_control_t;

// *** VARIABLES GLOBALES ***
alt_up_audio_dev *audio_dev = NULL;

// USAR DIRECCIONES DE TU SYSTEM.H
volatile compact_shared_control_t *shared_ctrl = (compact_shared_control_t*)SHARED_MEMORY_BASE;
volatile uint8_t *shared_data = (uint8_t*)(SHARED_MEMORY_BASE + AUDIO_DATA_OFFSET);

volatile int is_playing = 0;
volatile int elapsed_ms = 0, elapsed_seconds = 0, elapsed_minutes = 0;
volatile uint32_t audio_read_ptr = 0;
volatile uint32_t system_uptime_ms = 0;

// 7 segmentos: patrones para 0-9
const unsigned char seven_seg_patterns[10] = {
    0x40, 0x79, 0x24, 0x30, 0x19,
    0x12, 0x02, 0x78, 0x00, 0x10
};

// *** PROTOTIPOS DE FUNCIONES ***
void update_seven_segment_display(void);
void handle_buttons(void);
void process_audio_data(void);
void send_command_to_hps(uint32_t cmd);
void request_next_chunk(void);
int check_hps_connection(void);

// --- Verificar conexión HPS ---
int check_hps_connection(void) {
    return (shared_ctrl->magic == 0xABCD2025 && shared_ctrl->hps_connected == 1) ? 1 : 0;
}

// --- Interrupción Timer (500ms) - USAR TU TIMER_IRQ ---
static void timer_isr(void* context, alt_u32 id) {
    volatile unsigned int* timer_status = (unsigned int*) TIMER_BASE;
    *timer_status = 0; // Limpia TO

    // Incrementar uptime del sistema
    system_uptime_ms += 500;

    // Actualizar heartbeat de FPGA
    shared_ctrl->fpga_heartbeat++;

    if (is_playing && check_hps_connection()) {
        elapsed_ms += 500;
        if (elapsed_ms >= 1000) {
            elapsed_ms = 0;
            elapsed_seconds++;
            if (elapsed_seconds >= 60) {
                elapsed_seconds = 0;
                elapsed_minutes++;
                if (elapsed_minutes >= 100)
                    elapsed_minutes = 0;
            }
        }
        update_seven_segment_display();
        
        // Actualizar bytes reproducidos
        shared_ctrl->bytes_played += (audio_read_ptr > 0) ? 4 : 0;
    }

    // Actualizar posición de reproducción
    shared_ctrl->song_position = (elapsed_minutes * 60 + elapsed_seconds) * SAMPLE_RATE * 4;
}

// --- Solicitar siguiente chunk ---
void request_next_chunk(void) {
    if (!check_hps_connection()) {
        alt_printf("HPS desconectado - no se puede solicitar chunk\n");
        shared_ctrl->error_flags |= 0x02;
        return;
    }

    shared_ctrl->request_next = 1;
    shared_ctrl->chunk_ready = 0;
    audio_read_ptr = 0;

    alt_printf("Solicitando chunk %d/%d\n",
              shared_ctrl->current_chunk + 1, shared_ctrl->total_chunks);
}

// --- Procesar datos de audio ---
void process_audio_data(void) {
    if (!check_hps_connection()) {
        shared_ctrl->error_flags |= 0x02;
        return;
    }

    if (shared_ctrl->chunk_ready == 0 || shared_ctrl->chunk_size == 0) {
        if (shared_ctrl->chunk_size == 0 && shared_ctrl->total_chunks > 0 && shared_ctrl->request_next == 0) {
            request_next_chunk();
        }
        shared_ctrl->buffer_level = 0;
        return;
    }

    // Verificar espacio en FIFO
    int write_space_left = alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_LEFT);
    int write_space_right = alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_RIGHT);

    if (write_space_left > 0 && write_space_right > 0) {
        int samples_to_write = (write_space_left < 8) ? write_space_left : 8;

        for (int i = 0; i < samples_to_write; i++) {
            if (audio_read_ptr >= shared_ctrl->chunk_size) {
                alt_printf("Chunk %d completado (%d bytes)\n", 
                          shared_ctrl->current_chunk, audio_read_ptr);
                request_next_chunk();
                break;
            }

            if (audio_read_ptr + 4 > shared_ctrl->chunk_size) {
                request_next_chunk();
                break;
            }

            // Leer muestra stereo de 16 bits
            uint16_t left_sample = *(uint16_t*)(shared_data + audio_read_ptr);
            uint16_t right_sample = *(uint16_t*)(shared_data + audio_read_ptr + 2);

            // Convertir a 32-bit para codec
            int32_t left_32 = (int32_t)((int16_t)left_sample) << 8;
            int32_t right_32 = (int32_t)((int16_t)right_sample) << 8;

            // Escribir al codec
            if (alt_up_audio_write_fifo(audio_dev, (unsigned int*)&left_32, 1, ALT_UP_AUDIO_LEFT) == 0 &&
                alt_up_audio_write_fifo(audio_dev, (unsigned int*)&right_32, 1, ALT_UP_AUDIO_RIGHT) == 0) {
                audio_read_ptr += 4;
            } else {
                break;
            }
        }

        // Actualizar estadísticas
        if (shared_ctrl->chunk_size > 0) {
            shared_ctrl->buffer_level = (audio_read_ptr * 100) / shared_ctrl->chunk_size;
        }
        
        shared_ctrl->error_flags &= ~0x01; // Limpiar buffer underrun
    }
}

// --- Interrupción Audio - USAR TU AUDIO_IRQ ---
static void audio_isr(void* context, alt_u32 id) {
    if (!is_playing || audio_dev == NULL)
        return;

    process_audio_data();
}

// --- Enviar comando al HPS ---
void send_command_to_hps(uint32_t cmd) {
    if (!check_hps_connection()) {
        alt_printf("HPS no conectado\n");
        return;
    }

    shared_ctrl->command = cmd;
    alt_printf("Comando enviado: %d\n", cmd);
}

// --- Display 7 segmentos ---
void update_seven_segment_display(void) {
    volatile unsigned int * sevenseg = (unsigned int *) SEVEN_SEGMENTS_BASE;
    unsigned char min_tens = elapsed_minutes / 10;
    unsigned char min_ones = elapsed_minutes % 10;
    unsigned char sec_tens = elapsed_seconds / 10;
    unsigned char sec_ones = elapsed_seconds % 10;

    unsigned int display_value =
        ((unsigned int)seven_seg_patterns[min_tens] << 21) |
        ((unsigned int)seven_seg_patterns[min_ones] << 14) |
        ((unsigned int)seven_seg_patterns[sec_tens] << 7)  |
        ((unsigned int)seven_seg_patterns[sec_ones]);

    *sevenseg = display_value;
}

// --- Manejar botones - USAR TU BUTTONS_BASE ---
void handle_buttons(void) {
    static int prev_button_state = 0x7;
    volatile unsigned int * button_ptr = (unsigned int *) BUTTONS_BASE;
    int button_state = *button_ptr;
    int button_pressed = (~button_state) & prev_button_state;

    if (button_pressed & 0x1) { // KEY0: Play/Pause
        if (!check_hps_connection()) {
            alt_printf("HPS no conectado\n");
            return;
        }

        if (is_playing) {
            is_playing = 0;
            shared_ctrl->status = STATUS_PAUSED;
            send_command_to_hps(CMD_PAUSE);
            alt_printf("*** PAUSADO ***\n");
        } else {
            is_playing = 1;
            shared_ctrl->status = STATUS_PLAYING;
            send_command_to_hps(CMD_PLAY);
            alt_printf("*** REPRODUCIENDO canción %d ***\n", shared_ctrl->song_id + 1);
        }
    }

    if (button_pressed & 0x2) { // KEY1: Next
        if (!check_hps_connection()) {
            alt_printf("HPS no conectado\n");
            return;
        }

        send_command_to_hps(CMD_NEXT);
        elapsed_ms = 0;
        elapsed_seconds = 0;
        elapsed_minutes = 0;
        audio_read_ptr = 0;
        update_seven_segment_display();
        alt_printf("*** SIGUIENTE ***\n");
    }

    if (button_pressed & 0x4) { // KEY2: Previous
        if (!check_hps_connection()) {
            alt_printf("HPS no conectado\n");
            return;
        }

        send_command_to_hps(CMD_PREV);
        elapsed_ms = 0;
        elapsed_seconds = 0;
        elapsed_minutes = 0;
        audio_read_ptr = 0;
        update_seven_segment_display();
        alt_printf("*** ANTERIOR ***\n");
    }

    prev_button_state = button_state;
}

int main(void) {
    alt_putstr("=== FPGA Audio Player - System.h v4.1 ===\n");
    alt_printf("Shared Memory Base: 0x%x\n", SHARED_MEMORY_BASE);
    alt_printf("Shared Memory Size: %d bytes (%d KB)\n", SHARED_MEMORY_SIZE_VALUE, SHARED_MEMORY_SIZE_VALUE/1024);
    alt_printf("Control Structure: %d bytes\n", sizeof(compact_shared_control_t));
    alt_printf("Audio Offset: 0x%x\n", AUDIO_DATA_OFFSET);
    alt_printf("Audio Chunk Size: %d KB\n", AUDIO_CHUNK_SIZE/1024);
    alt_printf("Timer Period: %d ms\n", TIMER_PERIOD);

    // Verificar que la estructura cabe
    if (sizeof(compact_shared_control_t) > AUDIO_DATA_OFFSET) {
        alt_printf("ERROR: Estructura demasiado grande (%d > %d bytes)\n", 
                  sizeof(compact_shared_control_t), AUDIO_DATA_OFFSET);
        return -1;
    }

    // Inicializar audio usando el nombre correcto del system.h
    audio_dev = alt_up_audio_open_dev(AUDIO_NAME);
    if (audio_dev == NULL) {
        alt_printf("ERROR: No se pudo abrir %s\n", AUDIO_NAME);
        return -1;
    }
    alt_printf("✓ Audio device: %s OK\n", AUDIO_NAME);

    // Limpiar memoria compartida
    for (int i = 0; i < sizeof(compact_shared_control_t)/4; i++) {
        ((volatile uint32_t*)shared_ctrl)[i] = 0;
    }

    // Inicializar estructura
    shared_ctrl->magic = 0xABCD2025;
    shared_ctrl->command = CMD_NONE;
    shared_ctrl->status = STATUS_READY;
    shared_ctrl->song_id = 0;
    shared_ctrl->chunk_ready = 0;
    shared_ctrl->chunk_size = 0;
    shared_ctrl->request_next = 0;
    shared_ctrl->current_chunk = 0;
    shared_ctrl->total_chunks = 0;
    shared_ctrl->song_total_size = 0;
    shared_ctrl->song_position = 0;
    shared_ctrl->duration_sec = 0;
    shared_ctrl->hps_connected = 0;
    shared_ctrl->fpga_heartbeat = 0;
    shared_ctrl->sample_rate = SAMPLE_RATE;
    shared_ctrl->channels = 2;
    shared_ctrl->buffer_level = 0;
    shared_ctrl->error_flags = 0;
    shared_ctrl->bytes_played = 0;
    shared_ctrl->chunks_loaded = 0;

    alt_printf("✓ Estructura inicializada:\n");
    alt_printf("  Magic: 0x%x\n", shared_ctrl->magic);
    alt_printf("  Sample Rate: %d Hz\n", shared_ctrl->sample_rate);
    alt_printf("  Channels: %d\n", shared_ctrl->channels);

    // Registrar interrupciones usando valores de system.h
    if (alt_irq_register(TIMER_IRQ, NULL, timer_isr) != 0) {
        alt_printf("ERROR: Timer IRQ %d registro falló\n", TIMER_IRQ);
        return -1;
    }
    
    if (alt_irq_register(AUDIO_IRQ, NULL, audio_isr) != 0) {
        alt_printf("ERROR: Audio IRQ %d registro falló\n", AUDIO_IRQ);
        return -1;
    }
    alt_printf("✓ IRQs registradas: Timer=%d, Audio=%d\n", TIMER_IRQ, AUDIO_IRQ);

    // Configurar Timer (ya está configurado para 500ms según system.h)
    volatile unsigned int* timer_control = (unsigned int*)(TIMER_BASE + 4);
    *timer_control = 0x7; // Start, continuous, interrupt enable
    alt_printf("✓ Timer configurado: Base=0x%x, Period=%dms\n", TIMER_BASE, TIMER_PERIOD);

    // Inicializar variables
    is_playing = 0;
    elapsed_ms = 0;
    elapsed_seconds = 0;
    elapsed_minutes = 0;
    audio_read_ptr = 0;
    system_uptime_ms = 0;

    update_seven_segment_display();
    
    alt_putstr("\n=== SISTEMA LISTO ===\n");
    alt_printf("CPU: %s @ %d Hz\n", ALT_CPU_NAME, ALT_CPU_FREQ);
    alt_printf("Data Width: %d bits\n", ALT_CPU_DATA_ADDR_WIDTH);
    alt_putstr("Controles:\n");
    alt_putstr("  KEY0 = Play/Pause\n");
    alt_putstr("  KEY1 = Siguiente\n");
    alt_putstr("  KEY2 = Anterior\n");
    alt_putstr("Esperando HPS...\n\n");

    // Loop principal
    uint32_t loop_counter = 0;
    uint32_t last_connected = 0;
    uint32_t last_chunk_ready = 0;

    while (1) {
        handle_buttons();

        if (is_playing && check_hps_connection()) {
            process_audio_data();
        }

        // Debug cada 10 segundos
        if ((loop_counter % 100000) == 0) {
            alt_printf("=== ESTADO (loop %d) ===\n", loop_counter);
            alt_printf("HPS: %d | Magic: 0x%x | Estado: %d\n", 
                      shared_ctrl->hps_connected, shared_ctrl->magic, shared_ctrl->status);
            alt_printf("Canción: %d | Chunk: %d/%d | Listo: %d\n", 
                      shared_ctrl->song_id, shared_ctrl->current_chunk, 
                      shared_ctrl->total_chunks, shared_ctrl->chunk_ready);
            alt_printf("Tamaño: %d bytes | Ptr: %d | Nivel: %d%%\n", 
                      shared_ctrl->chunk_size, audio_read_ptr, shared_ctrl->buffer_level);
            alt_printf("Heartbeat: %d | Reproduciendo: %d | %02d:%02d\n", 
                      shared_ctrl->fpga_heartbeat, is_playing, elapsed_minutes, elapsed_seconds);
            alt_printf("Errores: 0x%x | Bytes: %d\n", 
                      shared_ctrl->error_flags, shared_ctrl->bytes_played);
            alt_printf("========================\n");
        }

        // Detectar conexión HPS
        uint32_t current_connected = check_hps_connection();
        if (current_connected != last_connected) {
            if (current_connected) {
                alt_printf("*** HPS CONECTADO ***\n");
                alt_printf("Canción: %d, Chunks: %d, Tamaño: %d\n", 
                          shared_ctrl->song_id, shared_ctrl->total_chunks, shared_ctrl->song_total_size);
                
                audio_read_ptr = 0;
                elapsed_ms = 0;
                elapsed_seconds = 0;
                elapsed_minutes = 0;
                shared_ctrl->error_flags = 0;
                update_seven_segment_display();
            } else {
                alt_printf("*** HPS DESCONECTADO ***\n");
                is_playing = 0;
                shared_ctrl->status = STATUS_READY;
                shared_ctrl->error_flags |= 0x02;
            }
            last_connected = current_connected;
        }

        // Detectar nuevo chunk
        if (shared_ctrl->chunk_ready != last_chunk_ready) {
            if (shared_ctrl->chunk_ready) {
                alt_printf("*** CHUNK NUEVO: %d (%d bytes) ***\n", 
                          shared_ctrl->current_chunk, shared_ctrl->chunk_size);
                audio_read_ptr = 0;
            }
            last_chunk_ready = shared_ctrl->chunk_ready;
        }

        loop_counter++;
        for (volatile int i = 0; i < 500; i++); // Pequeño delay
    }

    return 0;
}