#include <stdio.h>
#include <unistd.h>

int main() {
    printf("Hello from inside the container!\n");
    printf("My PID here is: %d\n", getpid());
    printf("Sleeping for 10 seconds...\n");
    sleep(10);
    printf("Exiting now.\n");
    return 0;
}