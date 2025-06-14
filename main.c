// File: container_step2.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/mount.h>

#define STACK_SIZE (1024 * 1024) // 1 MB stack

// Function to be executed by the container
int container_main(void *arg) {
    printf("--> In container_main: child process started.\n");

    // Set a new hostname
    printf(in
    "--> Setting hostname to 'container'...\n");
    sethostname("container", 9);

    // Remount /proc to make the new PID namespace work correctly
    printf("--> Mounting /proc...\n");
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount");
        // We exit because ps, top, etc. won't work without /proc
        exit(EXIT_FAILURE);
    }

    printf("--> Ready to execute user command.\n");
    
    char **argv = (char **)arg;
    execv(argv[0], argv);
    
    // execv only returns on error
    perror("execv failed");
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args...]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    printf("--> Parent: starting container...\n");

    char *container_stack = malloc(STACK_SIZE);
    if (container_stack == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    char *stack_top = container_stack + STACK_SIZE;

    // Flags for clone()
    int clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD;

    pid_t container_pid = clone(container_main, stack_top, clone_flags, &argv[1]);

    if (container_pid == -1) {
        perror("clone");
        free(container_stack);
        exit(EXIT_FAILURE);
    }

    printf("--> Parent: container created with host PID %d.\n", container_pid);

    waitpid(container_pid, NULL, 0);

    printf("--> Parent: container has terminated.\n");

    free(container_stack);
    return 0;
}