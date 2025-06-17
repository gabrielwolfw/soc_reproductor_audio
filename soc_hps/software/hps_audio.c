#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include "hps_0.h" // El archivo de cabecera que generaste

// --- Constantes Físicas ---

// Dirección base del puente Lightweight HPS-to-FPGA en el mapa de memoria del HPS.
#define HW_REGS_BASE            0xFF200000
// Tamaño de la ventana de memoria que vamos a mapear.
#define HW_REGS_SPAN            0x00200000
#define HW_REGS_MASK            (HW_REGS_SPAN - 1)

// --- Prototipos de Funciones ---
void display_number(volatile unsigned int *seven_seg_ptr, int number);

// Patrones para los dígitos 0-9 en un display de 7 segmentos de cátodo común.
// El bit 7 es el punto decimal (no lo usamos), los bits 6-0 son los segmentos g-a.
// Un '0' en un bit enciende el segmento.
const unsigned char seven_seg_patterns[10] = {
    0x40, // 0
    0x79, // 1
    0x24, // 2
    0x30, // 3
    0x19, // 4
    0x12, // 5
    0x02, // 6
    0x78, // 7
    0x00, // 8
    0x10  // 9
};

// Función para mostrar un número de hasta 4 dígitos en los displays.
void display_number(volatile unsigned int *seven_seg_ptr, int number) {
    // Asegurarnos de que el número esté en el rango 0-9999.
    if (number < 0 || number > 9999) {
        return;
    }

    // Separar el número en sus 4 dígitos.
    int digit1 = number % 10;
    int digit2 = (number / 10) % 10;
    int digit3 = (number / 100) % 10;
    int digit4 = (number / 1000) % 10;

    // Combinar los patrones de los 4 dígitos en un solo valor de 28 bits.
    // El PIO tiene 28 bits de ancho (7 bits por cada uno de los 4 displays).
    // HEX3 (más a la izquierda) ... HEX0 (más a la derecha)
    unsigned int display_value = (seven_seg_patterns[digit4] << 21) |
                                 (seven_seg_patterns[digit3] << 14) |
                                 (seven_seg_patterns[digit2] << 7)  |
                                 (seven_seg_patterns[digit1]);

    // Escribir el valor en el registro del PIO de 7 segmentos.
    *seven_seg_ptr = display_value;
}


int main() {
    // Variables para el mapeo de memoria.
    void *virtual_base;
    int fd;
    
    // Puntero volátil al PIO de 7 segmentos. 'volatile' evita que el compilador
    // optimice los accesos a memoria, lo cual es crucial para el hardware.
    volatile unsigned int *seven_seg_pio_ptr = NULL;

    printf("=== Demo de Control de 7 Segmentos desde HPS/Linux ===\n");

    // --- Mapeo de Memoria ---
    // Abrir /dev/mem para obtener acceso a la memoria física.
    if ((fd = open("/dev/mem", (O_RDWR | O_SYNC))) == -1) {
        printf("ERROR: No se pudo abrir /dev/mem...\n");
        return 1;
    }

    // Mapear la región del puente Lightweight HPS-to-FPGA en el espacio virtual del programa.
    virtual_base = mmap(NULL, HW_REGS_SPAN, (PROT_READ | PROT_WRITE), MAP_SHARED, fd, HW_REGS_BASE);

    if (virtual_base == MAP_FAILED) {
        printf("ERROR: mmap() falló...\n");
        close(fd);
        return 1;
    }
    
    // Calcular la dirección virtual del PIO de 7 segmentos.
    // Se suma el offset del periférico (desde hps_0.h) a la base virtual.
    seven_seg_pio_ptr = (unsigned int *)(virtual_base + ((unsigned long)(SEVEN_SEGMENTS_BASE) & (unsigned long)(HW_REGS_MASK)));

    printf("PIO de 7 Segmentos mapeado en la dirección virtual: %p\n", seven_seg_pio_ptr);

    // --- Bucle Principal ---
    // Este bucle contará de 0 a 9999 y lo mostrará en los displays.
    int counter = 0;
    while (1) {
        display_number(seven_seg_pio_ptr, counter);
        
        // Incrementar el contador.
        counter++;
        if (counter > 9999) {
            counter = 0;
        }
        
        // Esperar un corto tiempo para que el contador no sea demasiado rápido.
        usleep(10000); // 10 milisegundos de espera.
    }

    // --- Limpieza (aunque el bucle es infinito, es una buena práctica) ---
    // Apagar los displays al salir.
    *seven_seg_pio_ptr = 0xFFFFFFFF;
    
    // Desmapear la memoria y cerrar el archivo.
    if (munmap(virtual_base, HW_REGS_SPAN) != 0) {
        printf("ERROR: munmap() falló...\n");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}