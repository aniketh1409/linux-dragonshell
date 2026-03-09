#include <stdio.h>
#include <unistd.h>

int main() {
    printf("Writer program (PID: %d)\n", getpid());
    printf("Line 1: Hello World\n");
    printf("Line 2: Testing pipes\n");
    printf("Line 3: DragonShell rocks!\n");
    printf("Line 4: Final line\n");
    return 0;
}
