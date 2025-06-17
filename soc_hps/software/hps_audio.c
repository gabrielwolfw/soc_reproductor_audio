#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>

#define HW_REGS_BASE 0xFF200000
#define HW_REGS_SPAN 0x00200000
#define AUDIO_BASE_OFFSET 0x8860

int main()
{
    int fd;
    void *virtual_base;
    volatile int *audio_ptr;
    unsigned int fifospace;
    
    printf("Testing audio loopback...\n");
    
    // Mapear memoria
    if ((fd = open("/dev/mem", O_RDWR | O_SYNC)) == -1) {
        printf("ERROR: could not open /dev/mem\n");
        return 1;
    }
    
    virtual_base = mmap(NULL, HW_REGS_SPAN, PROT_READ | PROT_WRITE, 
                       MAP_SHARED, fd, HW_REGS_BASE);
    
    if (virtual_base == MAP_FAILED) {
        printf("ERROR: mmap() failed\n");
        close(fd);
        return 1;
    }
    
    // Tu código de prueba exacto
    audio_ptr = (volatile int *)((char*)virtual_base + AUDIO_BASE_OFFSET);
    
    printf("Audio test running... (Ctrl+C to stop)\n");
    printf("Speak into the microphone, you should hear it in headphones/speakers\n");
    
    while (1)
    {
        fifospace = *(audio_ptr+1); // read the audio port fifospace register
        if ((fifospace & 0x000000FF) > 0 &&      // Available sample right
            (fifospace & 0x00FF0000) > 0 &&      // Available write space right
            (fifospace & 0xFF000000) > 0)        // Available write space left
        {
            int sample = *(audio_ptr + 3);  // read right channel only
            *(audio_ptr + 2) = sample;      // Write to both channels
            *(audio_ptr + 3) = sample;
        }
    }
    
    munmap(virtual_base, HW_REGS_SPAN);
    close(fd);
    return 0;
}