#include <stdio.h>
#include <string.h>

int main() {
    char line[256];
    printf("Reader program started (PID: %d)\n", getpid());
    printf("Reading from stdin:\n");
    
    while (fgets(line, sizeof(line), stdin) != NULL) {
        printf("READ: %s", line);
    }
    
    printf("Reader finished.\n");
    return 0;
}
