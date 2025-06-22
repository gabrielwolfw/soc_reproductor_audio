#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    int fd = open("/dev/fpga_cmd", O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    char cmd;
    while (1) {
        ssize_t n = read(fd, &cmd, 1);
        if (n == 1) {
            switch (cmd) {
                case '1': printf("Play\n"); break;
                case '2': printf("Pause\n"); break;
                case '3': printf("Next\n"); break;
                case '4': printf("Prev\n"); break;
                default:  printf("Comando desconocido: %d\n", cmd);
            }
            fflush(stdout);
        }
    }
    close(fd);
    return 0;
}