// This is a conceptual C code snippet
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

// Function that will be executed by the container process
int container_main(void *arg) {
    printf("Inside the container!\n");
    // Set a new hostname for this container
    sethostname("my-container", 12);
    // Execute the user's command, e.g., /bin/sh
    char **argv = (char **)arg;
    execv(argv[0], argv);
    printf("Oops, execv failed!\n");
    return 1;
}

int main(int argc, char *argv[]) {
    // Command to run inside container is passed as arguments
    // e.g., ./my-runner /bin/sh
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command>\n", argv[0]);
        exit(1);
    }
    
    char *stack = malloc(4096); // Allocate stack for the new process
    if (!stack) exit(1);
    char *stack_top = stack + 4096;
    
    // Flags to create new namespaces
    int flags = CLONE_NEWPID |  // New PID namespace
                CLONE_NEWNS |   // New Mount namespace
                CLONE_NEWUTS |  // New UTS (hostname) namespace
                CLONE_NEWUSER;  // New User namespace

    // clone() creates the new process
    pid_t pid = clone(container_main, stack_top, flags | SIGCHLD, &argv[1]);
    
    if (pid == -1) {
        perror("clone");
        exit(1);
    }
    
    // The parent process waits for the container to exit
    waitpid(pid, NULL, 0);
    
    free(stack);
    return 0;
}