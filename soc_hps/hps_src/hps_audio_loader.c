#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include "shared_buffer_protocol.h"
#include "audio_config.h"

// Variables Globales
static shared_control_t *shared_ctrl = NULL;
static uint8_t *shared_audio = NULL;
static int mem_fd = -1;
static volatile int running = 1;

// Playlist fija de 3 canciones
const char* playlist[3] = {
    "/media/sd/songs/song1.wav",
    "/media/sd/songs/song2.wav", 
    "/media/sd/songs/song3.wav"
};

void signal_handler(int sig) {
    printf("Shutting down...\n");
    running = 0;
}

int init_shared_memory(void) {
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        perror("Cannot open /dev/mem");
        return -1;
    }
    
    void *mapped = mmap(NULL, SHARED_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                       MAP_SHARED, mem_fd, SHARED_BUFFER_BASE);
    if (mapped == MAP_FAILED) {
        perror("mmap failed");
        close(mem_fd);
        return -1;
    }
    
    shared_ctrl = (shared_control_t*)mapped;
    shared_audio = (uint8_t*)mapped + sizeof(shared_control_t);
    
    // Inicializar control
    memset((void*)shared_ctrl, 0, sizeof(shared_control_t));
    shared_ctrl->magic = PROTOCOL_MAGIC;
    shared_ctrl->version = PROTOCOL_VERSION;
    shared_ctrl->sample_rate = AUDIO_SAMPLE_RATE;
    shared_ctrl->channels = AUDIO_CHANNELS;
    shared_ctrl->bits_per_sample = AUDIO_BITS_PER_SAMPLE;
    
    return 0;
}

int load_audio_file(const char* filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        printf("Cannot open: %s\n", filename);
        shared_ctrl->status = STATUS_ERROR;
        return -1;
    }
    
    printf("Loading: %s\n", filename);
    shared_ctrl->status = STATUS_LOADING;
    
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (file_size > AUDIO_BUFFER_SIZE) {
        printf("File too large: %ld bytes\n", file_size);
        fclose(file);
        shared_ctrl->status = STATUS_ERROR;
        return -1;
    }
    
    size_t bytes_read = fread(shared_audio, 1, file_size, file);
    fclose(file);
    
    if (bytes_read != file_size) {
        printf("Read error\n");
        shared_ctrl->status = STATUS_ERROR;
        return -1;
    }
    
    shared_ctrl->data_size = bytes_read;
    shared_ctrl->hps_ready = 1;
    shared_ctrl->status = STATUS_READY;
    
    printf("Loaded %zu bytes\n", bytes_read);
    return 0;
}

void process_commands(void) {
    static uint32_t last_command = CMD_NONE;
    
    if (shared_ctrl->command != last_command && shared_ctrl->command != CMD_NONE) {
        switch (shared_ctrl->command) {
            case CMD_NEXT_TRACK:
                shared_ctrl->current_track = (shared_ctrl->current_track + 1) % 3;
                load_audio_file(playlist[shared_ctrl->current_track]);
                break;
            case CMD_PREV_TRACK:
                shared_ctrl->current_track = (shared_ctrl->current_track + 2) % 3;
                load_audio_file(playlist[shared_ctrl->current_track]);
                break;
            case CMD_LOAD_TRACK:
                load_audio_file(playlist[shared_ctrl->current_track]);
                break;
        }
        
        shared_ctrl->command = CMD_NONE;
        last_command = shared_ctrl->command;
    }
}

int main(void) {
    printf("HPS Audio Loader - 3 Songs\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    if (init_shared_memory() < 0) {
        return EXIT_FAILURE;
    }
    
    // Verificar las 3 canciones
    printf("Checking songs...\n");
    int valid_files = 0;
    for (int i = 0; i < 3; i++) {
        if (access(playlist[i], R_OK) == 0) {
            printf("Found: song%d.wav\n", i + 1);
            valid_files++;
        } else {
            printf("Missing: song%d.wav\n", i + 1);
        }
    }
    
    if (valid_files > 0) {
        // Cargar primera canción disponible
        for (int i = 0; i < 3; i++) {
            if (access(playlist[i], R_OK) == 0) {
                shared_ctrl->current_track = i;
                load_audio_file(playlist[i]);
                break;
            }
        }
    } else {
        printf("No songs found in /media/sd/songs/\n");
    }
    
    printf("Audio Loader Ready\n");
    
    // Loop principal
    while (running) {
        process_commands();
        usleep(50000);
    }
    
    // Cleanup
    if (shared_ctrl) {
        shared_ctrl->hps_ready = 0;
        munmap(shared_ctrl, SHARED_BUFFER_SIZE);
    }
    if (mem_fd >= 0) close(mem_fd);
    
    return 0;
}