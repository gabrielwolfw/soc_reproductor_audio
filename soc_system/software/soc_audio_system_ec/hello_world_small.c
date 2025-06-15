#include "system.h"
#include "sys/alt_stdio.h"
#include "sys/alt_irq.h"
#include <stdint.h>
#include <unistd.h>
#include "altera_up_avalon_audio.h"

#define SAMPLE_RATE 48000
#define CHUNK_SIZE (120 * 1024)
#define CONTROL_OFFSET 0x0000
#define AUDIO_DATA_OFFSET 0x0400

// *** DEFINIR COMANDOS Y ESTADOS PRIMERO ***
#define CMD_NONE               0
#define CMD_PLAY               1
#define CMD_PAUSE              2
#define CMD_STOP               3
#define CMD_NEXT               4
#define CMD_PREV               5
#define CMD_LOAD_CHUNK         0x10
#define CMD_CHUNK_READY        0x11
#define CMD_REQUEST_CHUNK      0x12

#define STATUS_READY           0
#define STATUS_PLAYING         1
#define STATUS_PAUSED          2
#define STATUS_LOADING         3
#define STATUS_BUFFERING       0x10
#define STATUS_CHUNK_READY     0x11
#define STATUS_END_OF_SONG     0x12
#define STATUS_ERROR           0xFF

// *** ESTRUCTURA DEFINIDA ANTES DE SER USADA ***
typedef struct __attribute__((packed)) {
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

    // Flags de comunicación - CRÍTICOS
    volatile uint32_t chunk_ready;      // 1=HPS cargó chunk, 0=FPGA consumió
    volatile uint32_t request_next;     // 1=FPGA solicita siguiente chunk
    volatile uint32_t buffer_underrun;  // 1=Buffer vacío (error)

    // Sistema de conexión SIMPLE
    volatile uint32_t hps_connected;    // 1=HPS conectado, 0=desconectado
    volatile uint32_t fpga_heartbeat;   // Solo FPGA heartbeat

    // Campos de compatibilidad
    volatile uint32_t magic;           // 0xABCD2025
    volatile uint32_t version;         // Protocol version
    volatile uint32_t hps_ready;       // HPS has data ready
    volatile uint32_t fpga_ready;      // FPGA can receive data
    volatile uint32_t data_size;       // Audio data size
    volatile uint32_t sample_rate;     // Current sample rate
    volatile uint32_t channels;        // Number of channels
    volatile uint32_t bits_per_sample; // Bits per sample
    volatile uint32_t current_track;   // Current track
    volatile uint32_t playback_pos;    // Playback position
    volatile uint32_t read_ptr;        // FPGA read pointer
    volatile uint32_t write_ptr;       // HPS write pointer
    volatile uint32_t buffer_level;    // Buffer fill level

    // Reservado para expansión futura
    volatile uint32_t reserved[8];
} shared_control_t;

// *** AHORA SÍ DECLARAR LAS VARIABLES GLOBALES ***
alt_up_audio_dev *audio_dev = NULL;
volatile shared_control_t *shared_ctrl = (shared_control_t*)0x40000;
volatile uint8_t *shared_data = (uint8_t*)(0x40000 + AUDIO_DATA_OFFSET);

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

// --- Verificar conexión SIMPLE ---
int check_hps_connection(void) {
    return (shared_ctrl->magic == 0xABCD2025) ? 1 : 0;
}

// --- Interrupción Timer (500ms) - SIGNATURA CORREGIDA ---
static void timer_isr(void* context, alt_u32 id) {
    volatile unsigned int* timer_status = (unsigned int*) TIMER_BASE;
    *timer_status = 0; // Limpia TO

    // Incrementar uptime del sistema
    system_uptime_ms += 500;

    // Actualizar heartbeat de FPGA
    shared_ctrl->fpga_heartbeat++;
    shared_ctrl->fpga_ready = 1;

    // Verificar conexión HPS
    shared_ctrl->hps_connected = check_hps_connection();

    if (is_playing) {
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
    }

    // Actualizar posición de reproducción
    shared_ctrl->playback_pos = (elapsed_minutes * 60 + elapsed_seconds) * 1000 + elapsed_ms;
}

// --- Solicitar siguiente chunk ---
void request_next_chunk(void) {
    if (!shared_ctrl->hps_connected) {
        return;
    }

    shared_ctrl->request_next = 1;
    shared_ctrl->chunk_ready = 0;
    audio_read_ptr = 0;

    alt_printf("Requesting next chunk %d/%d\n",
              shared_ctrl->current_chunk + 2, shared_ctrl->total_chunks);
}

// --- Procesar datos de audio desde SHARED_MEMORY ---
void process_audio_data(void) {
    // Verificar que hay un chunk listo
    if (shared_ctrl->chunk_ready == 0 || shared_ctrl->chunk_size == 0) {
        return;
    }

    // Verificar espacio disponible en FIFO
    int write_space_left = alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_LEFT);
    int write_space_right = alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_RIGHT);

    if (write_space_left > 0 && write_space_right > 0) {
        int samples_to_write = (write_space_left < 16) ? write_space_left : 16;

        for (int i = 0; i < samples_to_write; i++) {
            // Verificar si llegamos al final del chunk actual
            if (audio_read_ptr >= shared_ctrl->chunk_size) {
                request_next_chunk();
                break;
            }

            // Verificar que no nos pasemos del tamaño del chunk
            if (audio_read_ptr + 4 > shared_ctrl->chunk_size) {
                request_next_chunk();
                break;
            }

            // Leer muestra de 16 bits stereo desde SHARED_MEMORY
            uint16_t left_sample = *(uint16_t*)(shared_data + audio_read_ptr);
            uint16_t right_sample = *(uint16_t*)(shared_data + audio_read_ptr + 2);

            // Convertir a 32-bit para el codec (sign extend)
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

        // Actualizar punteros en memoria compartida
        shared_ctrl->read_ptr = audio_read_ptr;

        // Calcular progreso del chunk actual
        if (shared_ctrl->chunk_size > 0) {
            shared_ctrl->buffer_level = (audio_read_ptr * 100) / shared_ctrl->chunk_size;
        }
    }
}

// --- Interrupción Audio - SIGNATURA CORREGIDA ---
static void audio_isr(void* context, alt_u32 id) {
    if (!is_playing || audio_dev == NULL || !shared_ctrl->hps_connected)
        return;

    process_audio_data();
}

// --- Enviar comando al HPS ---
void send_command_to_hps(uint32_t cmd) {
    if (!shared_ctrl->hps_connected) {
        alt_printf("HPS not connected\n");
        return;
    }

    shared_ctrl->command = cmd;
    alt_printf("Command sent: %d\n", cmd);
}

// --- 7 segmentos MMSS ---
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

// --- Botones: Play/Pause, Next, Prev ---
void handle_buttons(void) {
    static int prev_button_state = 0x7;
    volatile unsigned int * button_ptr = (unsigned int *) BUTTONS_BASE;
    int button_state = *button_ptr;
    int button_pressed = (~button_state) & prev_button_state;

    if (button_pressed & 0x1) { // KEY0: Play/Pause
        if (!shared_ctrl->hps_connected) {
            alt_printf("HPS not connected\n");
            return;
        }

        if (is_playing) {
            is_playing = 0;
            shared_ctrl->command = CMD_PAUSE;
            shared_ctrl->status = STATUS_PAUSED;
            alt_printf("Paused\n");
        } else {
            is_playing = 1;
            shared_ctrl->command = CMD_PLAY;
            alt_printf("Playing song %d\n", shared_ctrl->song_id + 1);
        }
    }

    if (button_pressed & 0x2) { // KEY1: Next
        if (!shared_ctrl->hps_connected) {
            alt_printf("HPS not connected\n");
            return;
        }

        send_command_to_hps(CMD_NEXT);
        elapsed_ms = 0;
        elapsed_seconds = 0;
        elapsed_minutes = 0;
        audio_read_ptr = 0;
        update_seven_segment_display();
        alt_printf("Next track\n");
    }

    if (button_pressed & 0x4) { // KEY2: Previous
        if (!shared_ctrl->hps_connected) {
            alt_printf("HPS not connected\n");
            return;
        }

        send_command_to_hps(CMD_PREV);
        elapsed_ms = 0;
        elapsed_seconds = 0;
        elapsed_minutes = 0;
        audio_read_ptr = 0;
        update_seven_segment_display();
        alt_printf("Previous track\n");
    }

    prev_button_state = button_state;
}

int main(void) {
    alt_putstr("=== FPGA Audio Player - On-Chip Memory v2.3 ===\n");
    alt_printf("Shared memory: 0x%x, Size: %d bytes\n", 0x40000, sizeof(shared_control_t));

    // Inicializar audio
    audio_dev = alt_up_audio_open_dev(AUDIO_NAME);
    if (audio_dev == NULL) {
        alt_printf("ERROR: Cannot open audio device\n");
        return -1;
    }
    alt_printf("Audio device OK\n");

    // Limpiar memoria compartida
    for (int i = 0; i < sizeof(shared_control_t)/4; i++) {
        ((volatile uint32_t*)shared_ctrl)[i] = 0;
    }

    // Inicializar memoria compartida
    shared_ctrl->magic = 0xABCD2025;
    shared_ctrl->version = 0x00020003;
    shared_ctrl->fpga_ready = 1;
    shared_ctrl->sample_rate = SAMPLE_RATE;
    shared_ctrl->channels = 2;
    shared_ctrl->bits_per_sample = 16;
    shared_ctrl->song_id = 0;
    shared_ctrl->command = CMD_NONE;
    shared_ctrl->status = STATUS_READY;

    // Registrar interrupciones - SIGNATURAS CORREGIDAS
    alt_irq_register(TIMER_IRQ, NULL, timer_isr);
    alt_irq_register(AUDIO_IRQ, NULL, audio_isr);

    // Configurar Timer para 500ms
    volatile unsigned int* timer_control = (unsigned int*)(TIMER_BASE + 4);
    *timer_control = 0x7;

    // Inicialización
    is_playing = 0;
    elapsed_ms = 0;
    elapsed_seconds = 0;
    elapsed_minutes = 0;
    audio_read_ptr = 0;
    system_uptime_ms = 0;

    update_seven_segment_display();
    alt_putstr("Controls: KEY0=Play/Pause | KEY1=Next | KEY2=Previous\n");
    alt_putstr("Waiting for HPS...\n\n");

    // Loop principal
    uint32_t loop_counter = 0;
    uint32_t last_connected = 0;

    while (1) {
        handle_buttons();

        // Procesar audio
        if (is_playing && shared_ctrl->hps_connected) {
            process_audio_data();
        }

        // Debug cada 10 segundos
        if ((loop_counter % 100000) == 0) {
            alt_printf("=== STATUS ===\n");
            alt_printf("HPS Connected: %d\n", shared_ctrl->hps_connected);
            alt_printf("Magic: 0x%x\n", shared_ctrl->magic);
            alt_printf("Chunk Ready: %d\n", shared_ctrl->chunk_ready);
            alt_printf("Chunk Size: %d\n", shared_ctrl->chunk_size);
            alt_printf("Playing: %d\n", is_playing);
            alt_printf("Shared Memory: 0x%x\n", 0x40000);
            alt_printf("==============\n");
        }

        // Detectar cambios de conexión
        if (shared_ctrl->hps_connected != last_connected) {
            if (shared_ctrl->hps_connected) {
                alt_printf("*** HPS CONNECTED ***\n");
            } else {
                alt_printf("*** HPS DISCONNECTED ***\n");
                is_playing = 0;
            }
            last_connected = shared_ctrl->hps_connected;
        }

        loop_counter++;
        for (volatile int i = 0; i < 500; i++); // Small delay
    }

    return 0;
}
