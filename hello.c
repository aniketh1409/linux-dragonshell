#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    printf("Hello from hello program!\n");
    printf("PID: %d\n", getpid());
    
    if (argc > 1) {
        printf("Arguments received:\n");
        for (int i = 1; i < argc; i++) {
            printf("  arg[%d]: %s\n", i, argv[i]);
        }
    }
    
    return 0;
}
