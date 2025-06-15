#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>

// Direcciones correctas para lwhps2fpga bridge
#define HW_REGS_BASE    ( 0xff200000 )
#define HW_REGS_SPAN    ( 0x00200000 )
#define HW_REGS_MASK    ( HW_REGS_SPAN - 1 )

// Offsets en el bridge para comunicación FPGA
#define SHARED_CTRL_OFFSET    0x0000      // Control structure offset
#define SHARED_AUDIO_OFFSET   0x1000      // Audio data offset

// Estructura de control (debe coincidir con FPGA)
typedef struct __attribute__((packed)) {
    volatile uint32_t magic;           // 0xABCD2025
    volatile uint32_t version;         // Protocol version
    volatile uint32_t command;         // Commands
    volatile uint32_t status;          // Status
    volatile uint32_t song_id;         // Current song 0-2
    volatile uint32_t chunk_ready;     // 1=data ready, 0=consumed
    volatile uint32_t chunk_size;      // Size of current chunk
    volatile uint32_t request_next;    // 1=request next chunk
} shared_control_t;

// Commands & Status
#define CMD_PLAY    1
#define CMD_PAUSE   2
#define CMD_STOP    3
#define CMD_NEXT    4
#define CMD_PREV    5

#define STATUS_READY    0
#define STATUS_PLAYING  1
#define STATUS_PAUSED   2

// Global variables
int fd_mem = -1;
void* virtual_base = NULL;
volatile shared_control_t* shared_ctrl = NULL;
volatile uint8_t* shared_audio = NULL;

// Audio configuration
#define AUDIO_CHUNK_SIZE    (120 * 1024)   // 120KB chunks
#define MAX_TRACKS          3

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
    printf("\nCleaning up...\n");
    
    if (shared_ctrl) {
        shared_ctrl->command = CMD_STOP;
        shared_ctrl->status = STATUS_READY;
    }
    
    // Close file handles
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (songs[i].file_handle) {
            fclose(songs[i].file_handle);
        }
    }
    
    // Unmap memory
    if (virtual_base != NULL) {
        munmap(virtual_base, HW_REGS_SPAN);
    }
    
    // Close /dev/mem
    if (fd_mem != -1) {
        close(fd_mem);
    }
    
    exit(0);
}

int map_memory() {
    // Open /dev/mem
    fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_mem == -1) {
        printf("ERROR: Cannot open /dev/mem\n");
        return -1;
    }
    
    // Map lwhps2fpga bridge
    virtual_base = mmap(NULL, HW_REGS_SPAN, PROT_READ | PROT_WRITE, 
                       MAP_SHARED, fd_mem, HW_REGS_BASE);
    
    if (virtual_base == MAP_FAILED) {
        printf("ERROR: Cannot map lwhps2fpga bridge\n");
        close(fd_mem);
        return -1;
    }
    
    printf("lwhps2fpga bridge mapped at virtual address: %p\n", virtual_base);
    
    // Calculate pointers to shared memory regions
    shared_ctrl = (shared_control_t*)((char*)virtual_base + SHARED_CTRL_OFFSET);
    shared_audio = (uint8_t*)((char*)virtual_base + SHARED_AUDIO_OFFSET);
    
    printf("Shared control at: %p\n", (void*)shared_ctrl);
    printf("Shared audio at: %p\n", (void*)shared_audio);
    
    return 0;
}

int load_song_info() {
    printf("Loading song information...\n");
    
    const char* song_paths[MAX_TRACKS] = {
        "/media/sd/songs/song1.wav",
        "/media/sd/songs/song2.wav", 
        "/media/sd/songs/song3.wav"
    };
    
    int loaded_count = 0;
    
    for (int i = 0; i < MAX_TRACKS; i++) {
        FILE* file = fopen(song_paths[i], "rb");
        if (file) {
            // Get file size
            fseek(file, 0, SEEK_END);
            songs[i].file_size = ftell(file);
            fseek(file, 0, SEEK_SET);
            
            // Calculate chunks and duration
            songs[i].num_chunks = (songs[i].file_size + AUDIO_CHUNK_SIZE - 1) / AUDIO_CHUNK_SIZE;
            songs[i].duration_sec = songs[i].file_size / (48000 * 2 * 2); // Approximate
            
            strcpy(songs[i].filename, song_paths[i]);
            songs[i].file_handle = file;
            
            printf("Song %d: %s\n", i+1, songs[i].filename);
            printf("  Size: %.2f MB (%u bytes)\n", songs[i].file_size/1024.0/1024.0, songs[i].file_size);
            printf("  Chunks: %u (%.2f KB each)\n", songs[i].num_chunks, AUDIO_CHUNK_SIZE/1024.0);
            printf("  Duration: ~%.1f seconds\n", (double)songs[i].duration_sec);
            
            loaded_count++;
        } else {
            printf("WARNING: Cannot open %s\n", song_paths[i]);
            songs[i].file_handle = NULL;
        }
    }
    
    printf("\nSuccessfully loaded %d/%d songs\n", loaded_count, MAX_TRACKS);
    return loaded_count > 0 ? 0 : -1;
}

int load_chunk(int song_idx, int chunk_idx) {
    if (song_idx >= MAX_TRACKS || !songs[song_idx].file_handle) {
        return -1;
    }
    
    FILE* file = songs[song_idx].file_handle;
    long offset = chunk_idx * AUDIO_CHUNK_SIZE;
    
    if (fseek(file, offset, SEEK_SET) != 0) {
        return -1;
    }
    
    size_t bytes_read = fread((void*)shared_audio, 1, AUDIO_CHUNK_SIZE, file);
    
    if (bytes_read > 0) {
        shared_ctrl->chunk_size = bytes_read;
        shared_ctrl->chunk_ready = 1;
        
        uint32_t samples = bytes_read / 4; // 16-bit stereo
        printf("Loaded chunk %d/%d of song %d (%zu bytes, %u samples)\n", 
               chunk_idx + 1, songs[song_idx].num_chunks, song_idx + 1, bytes_read, samples);
        return 0;
    }
    
    return -1;
}

int main() {
    printf("=== HPS Audio Streaming Loader ===\n");
    printf("Fixed version using lwhps2fpga bridge\n\n");
    
    // Setup signal handler
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);
    
    // Map memory
    printf("Mapping lwhps2fpga bridge...\n");
    if (map_memory() != 0) {
        return -1;
    }
    
    // Load song information
    if (load_song_info() != 0) {
        cleanup_and_exit(1);
    }
    
    // Initialize control structure
    printf("Control structure initialized\n");
    shared_ctrl->magic = 0xABCD2025;
    shared_ctrl->version = 0x00020001;
    shared_ctrl->command = CMD_PLAY;
    shared_ctrl->status = STATUS_READY;
    shared_ctrl->song_id = 0;
    shared_ctrl->chunk_ready = 0;
    shared_ctrl->request_next = 0;
    
    // Pre-load first chunk
    printf("Pre-loading first chunk...\n");
    if (load_chunk(0, 0) == 0) {
        printf("Ready to play!\n\n");
    }
    
    printf("=== Starting audio streaming loop ===\n");
    printf("Commands: PLAY(1) PAUSE(2) STOP(3) NEXT(4) PREV(5)\n");
    printf("Use FPGA buttons/switches to control playback\n");
    
    // Main communication loop
    while (1) {
        // Check for next chunk request from FPGA
        if (shared_ctrl->request_next && !shared_ctrl->chunk_ready) {
            current_chunk++;
            
            // Check if we need to advance to next song or loop
            if (current_chunk >= songs[current_song].num_chunks) {
                current_chunk = 0;
                current_song = (current_song + 1) % MAX_TRACKS;
                printf("Advancing to song %d\n", current_song + 1);
            }
            
            // Load next chunk
            if (load_chunk(current_song, current_chunk) == 0) {
                shared_ctrl->request_next = 0;
            }
        }
        
        // Small delay to prevent busy waiting
        usleep(10000); // 10ms
    }
    
    cleanup_and_exit(0);
    return 0;
}