#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <errno.h> 

// Direcciones base del bridge (mismas del ejemplo funcional)
#define HW_REGS_BASE ( 0xff200000 )
#define HW_REGS_SPAN ( 0x00200000 )  // 2MB
#define HW_REGS_MASK ( HW_REGS_SPAN - 1 )

// TU NUEVO ADDRESS MAP (128 KB)
#define SHARED_MEMORY_OFFSET  0x80000     // Tu base: 0x0008_0000
#define CONTROL_OFFSET        0x0000      // Estructura al inicio
#define AUDIO_DATA_OFFSET     0x0400      // Audio después de 1KB de control

// Configuración optimizada para 128 KB
#define MEMORY_SIZE           0x20000     // 128 KB
#define AUDIO_CHUNK_SIZE      (30 * 1024) // 30 KB chunks (más pequeños)
#define CONTROL_SIZE          1024        // 1 KB para control
#define MAX_AUDIO_SIZE        (120 * 1024) // Máximo 120 KB para audio

// Comandos
#define CMD_NONE    0
#define CMD_PLAY    1
#define CMD_PAUSE   2
#define CMD_STOP    3
#define CMD_NEXT    4
#define CMD_PREV    5

// Estados
#define STATUS_READY    0
#define STATUS_PLAYING  1
#define STATUS_PAUSED   2

// Estructura compacta y optimizada
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

// Variables globales
int fd = -1;
void *virtual_base = NULL;
volatile compact_shared_control_t *shared_ctrl = NULL;
volatile uint8_t *shared_audio = NULL;

#define MAX_TRACKS 3

typedef struct {
    char filename[256];
    uint32_t file_size;
    uint32_t num_chunks;
    uint32_t duration_sec;
    FILE* file_handle;
} song_info_t;

song_info_t songs[MAX_TRACKS];
int current_song = 0;
int current_chunk = 0;

void cleanup_and_exit(int sig) {
    printf("\nLimpiando recursos...\n");
    
    if (shared_ctrl) {
        shared_ctrl->hps_connected = 0;
    }
    
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (songs[i].file_handle) {
            fclose(songs[i].file_handle);
        }
    }
    
    if (virtual_base != NULL) {
        munmap(virtual_base, HW_REGS_SPAN);
    }
    
    if (fd != -1) {
        close(fd);
    }
    
    exit(0);
}

int verify_memory_layout() {
    printf("=== Verificando Layout de Memoria (128 KB) ===\n");
    printf("Configuración optimizada:\n");
    printf("  Base memory: 0x%08x\n", SHARED_MEMORY_OFFSET);
    printf("  Memory size: %d KB (0x%x bytes)\n", MEMORY_SIZE/1024, MEMORY_SIZE);
    printf("  Control offset: 0x%08x\n", SHARED_MEMORY_OFFSET + CONTROL_OFFSET);
    printf("  Audio offset: 0x%08x\n", SHARED_MEMORY_OFFSET + AUDIO_DATA_OFFSET);
    printf("  Control size: %zu bytes\n", sizeof(compact_shared_control_t));
    printf("  Max audio size: %d KB\n", MAX_AUDIO_SIZE/1024);
    printf("  Audio chunk size: %d KB\n", AUDIO_CHUNK_SIZE/1024);
    
    // Verificar que todo cabe
    uint32_t control_end = sizeof(compact_shared_control_t);
    uint32_t audio_start = AUDIO_DATA_OFFSET;
    uint32_t audio_end = audio_start + MAX_AUDIO_SIZE;
    
    printf("Layout detallado:\n");
    printf("  Control: 0x0000 - 0x%04x (%zu bytes)\n", control_end, sizeof(compact_shared_control_t));
    printf("  Gap: 0x%04x - 0x%04x (%d bytes)\n", control_end, audio_start, audio_start - control_end);
    printf("  Audio: 0x%04x - 0x%04x (%d bytes)\n", audio_start, audio_end, MAX_AUDIO_SIZE);
    printf("  Total usado: %d bytes de %d disponibles\n", audio_end, MEMORY_SIZE);
    
    if (audio_end > MEMORY_SIZE) {
        printf("ERROR: No cabe en 128 KB!\n");
        printf("  Necesitas: %d bytes\n", audio_end);
        printf("  Tienes: %d bytes\n", MEMORY_SIZE);
        return -1;
    }
    
    if (control_end > audio_start) {
        printf("ERROR: Control structure se superpone con audio!\n");
        return -1;
    }
    
    printf("✓ Layout de memoria verificado - todo cabe en 128 KB\n");
    return 0;
}

int map_shared_memory() {
    printf("=== Mapeando Memoria Compartida (128 KB) ===\n");
    
    if (verify_memory_layout() != 0) {
        return -1;
    }
    
    // Abrir /dev/mem
    if ((fd = open("/dev/mem", (O_RDWR | O_SYNC))) == -1) {
        printf("ERROR: No se pudo abrir /dev/mem: %s\n", strerror(errno));
        return -1;
    }
    printf("✓ /dev/mem abierto\n");
    
    // Mapear memoria
    virtual_base = mmap(NULL, HW_REGS_SPAN, (PROT_READ | PROT_WRITE),
                       MAP_SHARED, fd, HW_REGS_BASE);
    if (virtual_base == MAP_FAILED) {
        printf("ERROR: mmap() falló: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    printf("✓ Memoria mapeada en: %p\n", virtual_base);
    
    // Calcular punteros
    shared_ctrl = (compact_shared_control_t *)(virtual_base + 
                  ((SHARED_MEMORY_OFFSET + CONTROL_OFFSET) & (HW_REGS_MASK)));
    shared_audio = (uint8_t *)(virtual_base + 
                   ((SHARED_MEMORY_OFFSET + AUDIO_DATA_OFFSET) & (HW_REGS_MASK)));
    
    printf("Layout mapeado:\n");
    printf("  Base virtual: %p\n", virtual_base);
    printf("  Control en: %p\n", (void*)shared_ctrl);
    printf("  Audio en: %p\n", (void*)shared_audio);
    printf("  Estructura: %zu bytes\n", sizeof(compact_shared_control_t));
    
    // Test de acceso
    printf("Probando acceso...\n");
    memset((void*)shared_ctrl, 0, sizeof(compact_shared_control_t));
    shared_ctrl->magic = 0xABCD2025;
    
    if (shared_ctrl->magic == 0xABCD2025) {
        printf("✓ Acceso verificado\n");
        return 0;
    } else {
        printf("✗ Test falló (magic = 0x%08x)\n", shared_ctrl->magic);
        return -1;
    }
}

int load_songs() {
    printf("=== Cargando Canciones ===\n");
    
    const char* song_paths[MAX_TRACKS] = {
        "/media/sd/songs/song1.wav",
        "/media/sd/songs/song2.wav", 
        "/media/sd/songs/song3.wav"
    };
    
    int loaded = 0;
    
    for (int i = 0; i < MAX_TRACKS; i++) {
        songs[i].file_handle = fopen(song_paths[i], "rb");
        if (songs[i].file_handle) {
            fseek(songs[i].file_handle, 0, SEEK_END);
            songs[i].file_size = ftell(songs[i].file_handle);
            fseek(songs[i].file_handle, 0, SEEK_SET);
            
            // Calcular chunks de 30KB
            songs[i].num_chunks = (songs[i].file_size + AUDIO_CHUNK_SIZE - 1) / AUDIO_CHUNK_SIZE;
            songs[i].duration_sec = songs[i].file_size / (48000 * 2 * 2);
            strcpy(songs[i].filename, song_paths[i]);
            
            printf("✓ Canción %d: %s\n", i+1, songs[i].filename);
            printf("    %.1f MB, %d chunks de %d KB\n", 
                   songs[i].file_size/1024.0/1024.0, 
                   songs[i].num_chunks, AUDIO_CHUNK_SIZE/1024);
            printf("    Duración: ~%d segundos\n", songs[i].duration_sec);
            
            loaded++;
        } else {
            printf("⚠ No se pudo abrir: %s\n", song_paths[i]);
        }
    }
    
    printf("Cargadas %d/%d canciones\n\n", loaded, MAX_TRACKS);
    return loaded > 0 ? 0 : -1;
}

int load_chunk(int song_idx, int chunk_idx) {
    if (song_idx >= MAX_TRACKS || !songs[song_idx].file_handle) {
        return -1;
    }
    
    FILE* file = songs[song_idx].file_handle;
    long offset = chunk_idx * AUDIO_CHUNK_SIZE;
    
    if (chunk_idx >= songs[song_idx].num_chunks) {
        printf("ERROR: Chunk %d excede total %d\n", chunk_idx, songs[song_idx].num_chunks);
        return -1;
    }
    
    if (fseek(file, offset, SEEK_SET) != 0) {
        printf("ERROR: Seek falló para chunk %d\n", chunk_idx);
        return -1;
    }
    
    size_t bytes_read = fread((void*)shared_audio, 1, AUDIO_CHUNK_SIZE, file);
    
    if (bytes_read > 0) {
        shared_ctrl->chunk_size = bytes_read;
        shared_ctrl->current_chunk = chunk_idx;
        shared_ctrl->song_position = offset + bytes_read;
        shared_ctrl->chunk_ready = 1;
        shared_ctrl->request_next = 0;
        shared_ctrl->chunks_loaded++;
        
        // Calcular progreso
        shared_ctrl->buffer_level = (chunk_idx * 100) / songs[song_idx].num_chunks;
        
        printf("Chunk %d/%d cargado (%zu bytes, %d%% completado)\n", 
               chunk_idx + 1, songs[song_idx].num_chunks, 
               bytes_read, shared_ctrl->buffer_level);
        return 0;
    }
    
    printf("ERROR: Lectura falló\n");
    return -1;
}

int main() {
    printf("=== HPS Audio Loader - 128 KB Optimizado ===\n");
    printf("Memoria: 0x%08x - 0x%08x (128 KB)\n", 
           SHARED_MEMORY_OFFSET, SHARED_MEMORY_OFFSET + MEMORY_SIZE - 1);
    printf("Chunks de audio: %d KB\n", AUDIO_CHUNK_SIZE/1024);
    printf("Estructura: %zu bytes\n", sizeof(compact_shared_control_t));
    printf("Usuario: %s\n", getenv("USER") ? getenv("USER") : "unknown");
    printf("Compilado: %s %s\n\n", __DATE__, __TIME__);
    
    if (getuid() != 0) {
        printf("ERROR: Ejecutar como root (sudo)\n");
        return 1;
    }
    
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);
    
    // Mapear memoria
    if (map_shared_memory() != 0) {
        printf("FATAL: Falló mapeo de memoria\n");
        return 1;
    }
    
    // Cargar canciones
    if (load_songs() != 0) {
        printf("ADVERTENCIA: Sin canciones, modo test\n");
    }
    
    // Inicializar sistema
    printf("=== Inicializando Sistema ===\n");
    
    shared_ctrl->magic = 0xABCD2025;
    shared_ctrl->command = CMD_NONE;
    shared_ctrl->status = STATUS_READY;
    shared_ctrl->song_id = 0;
    shared_ctrl->hps_connected = 1;
    shared_ctrl->sample_rate = 48000;
    shared_ctrl->channels = 2;
    shared_ctrl->error_flags = 0;
    shared_ctrl->chunks_loaded = 0;
    shared_ctrl->bytes_played = 0;
    
    if (songs[0].file_handle) {
        shared_ctrl->total_chunks = songs[0].num_chunks;
        shared_ctrl->song_total_size = songs[0].file_size;
        shared_ctrl->duration_sec = songs[0].duration_sec;
        
        if (load_chunk(0, 0) == 0) {
            printf("✓ Primer chunk cargado\n");
        }
    }
    
    printf("\n=== Estado Inicial ===\n");
    printf("Magic: 0x%08x\n", shared_ctrl->magic);
    printf("HPS Conectado: %d\n", shared_ctrl->hps_connected);
    printf("Canción: %d, Chunks: %d\n", shared_ctrl->song_id, shared_ctrl->total_chunks);
    printf("Chunk listo: %d (%d bytes)\n", shared_ctrl->chunk_ready, shared_ctrl->chunk_size);
    
    printf("\n=== Loop Principal ===\n");
    printf("Esperando FPGA...\n");
    printf("Controles: KEY0=Play/Pause, KEY1=Siguiente, KEY2=Anterior\n\n");
    
    uint32_t loop_counter = 0;
    uint32_t last_heartbeat = 0;
    
    while (1) {
        shared_ctrl->hps_connected = 1;
        
        // Heartbeat
        if (shared_ctrl->fpga_heartbeat != last_heartbeat) {
            printf("[%06d] FPGA activo: %u\n", loop_counter, shared_ctrl->fpga_heartbeat);
            last_heartbeat = shared_ctrl->fpga_heartbeat;
        }
        
        // Chunk requests
        if (shared_ctrl->request_next && !shared_ctrl->chunk_ready && songs[current_song].file_handle) {
            printf("[%06d] Cargando siguiente chunk...\n", loop_counter);
            current_chunk++;
            
            if (current_chunk >= songs[current_song].num_chunks) {
                printf("Fin de canción, siguiente...\n");
                current_chunk = 0;
                current_song = (current_song + 1) % MAX_TRACKS;
                
                while (!songs[current_song].file_handle && current_song < MAX_TRACKS) {
                    current_song++;
                }
                if (current_song >= MAX_TRACKS) current_song = 0;
                
                shared_ctrl->song_id = current_song;
                shared_ctrl->total_chunks = songs[current_song].num_chunks;
                shared_ctrl->song_total_size = songs[current_song].file_size;
                shared_ctrl->duration_sec = songs[current_song].duration_sec;
                printf("Canción %d\n", current_song + 1);
            }
            
            if (load_chunk(current_song, current_chunk) != 0) {
                shared_ctrl->error_flags |= 0x01;
            }
        }
        
        // Commands
        switch (shared_ctrl->command) {
            case CMD_NEXT:
                if (shared_ctrl->command != CMD_NONE) {
                    printf("[%06d] Comando: SIGUIENTE\n", loop_counter);
                    current_song = (current_song + 1) % MAX_TRACKS;
                    while (!songs[current_song].file_handle && current_song < MAX_TRACKS) current_song++;
                    if (current_song >= MAX_TRACKS) current_song = 0;
                    
                    current_chunk = 0;
                    shared_ctrl->song_id = current_song;
                    shared_ctrl->total_chunks = songs[current_song].num_chunks;
                    shared_ctrl->song_total_size = songs[current_song].file_size;
                    shared_ctrl->duration_sec = songs[current_song].duration_sec;
                    load_chunk(current_song, current_chunk);
                    shared_ctrl->command = CMD_NONE;
                    printf("Cambiado a canción %d\n", current_song + 1);
                }
                break;
                
            case CMD_PREV:
                if (shared_ctrl->command != CMD_NONE) {
                    printf("[%06d] Comando: ANTERIOR\n", loop_counter);
                    current_song = (current_song - 1 + MAX_TRACKS) % MAX_TRACKS;
                    while (!songs[current_song].file_handle && current_song >= 0) current_song--;
                    if (current_song < 0) current_song = MAX_TRACKS - 1;
                    
                    current_chunk = 0;
                    shared_ctrl->song_id = current_song;
                    shared_ctrl->total_chunks = songs[current_song].num_chunks;
                    shared_ctrl->song_total_size = songs[current_song].file_size;
                    shared_ctrl->duration_sec = songs[current_song].duration_sec;
                    load_chunk(current_song, current_chunk);
                    shared_ctrl->command = CMD_NONE;
                    printf("Cambiado a canción %d\n", current_song + 1);
                }
                break;
                
            case CMD_PLAY:
                if (shared_ctrl->command != CMD_NONE) {
                    printf("[%06d] Comando: PLAY\n", loop_counter);
                    shared_ctrl->status = STATUS_PLAYING;
                    shared_ctrl->command = CMD_NONE;
                }
                break;
                
            case CMD_PAUSE:
                if (shared_ctrl->command != CMD_NONE) {
                    printf("[%06d] Comando: PAUSE\n", loop_counter);
                    shared_ctrl->status = STATUS_PAUSED;
                    shared_ctrl->command = CMD_NONE;
                }
                break;
                
            case CMD_STOP:
                if (shared_ctrl->command != CMD_NONE) {
                    printf("[%06d] Comando: STOP\n", loop_counter);
                    shared_ctrl->status = STATUS_READY;
                    current_chunk = 0;
                    load_chunk(current_song, current_chunk);
                    shared_ctrl->command = CMD_NONE;
                }
                break;
        }
        
        // Status cada 5 segundos
        if ((loop_counter % 500) == 0 && loop_counter > 0) {
            printf("[%06d] Estado: Cmd=%d, Status=%d, Canción=%d, Chunk=%d/%d (%d%%), Listo=%d\n",
                   loop_counter, shared_ctrl->command, shared_ctrl->status,
                   shared_ctrl->song_id, shared_ctrl->current_chunk + 1, 
                   shared_ctrl->total_chunks, shared_ctrl->buffer_level,
                   shared_ctrl->chunk_ready);
        }
        
        loop_counter++;
        usleep(10000); // 10ms
    }
    
    return 0;
}