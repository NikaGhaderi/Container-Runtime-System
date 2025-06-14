// File: container_debug.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/mount.h>

#define STACK_SIZE (1024 * 1024)

int container_main(void *arg) {
    // This function now returns its exit code instead of calling exit()
    printf("[CHILD] --> Process started. PID: %ld\n", (long)getpid());

    if (sethostname("container", 9) != 0) {
        perror("[CHILD] sethostname failed");
        return 1;
    }
    printf("[CHILD] --> Hostname set.\n");

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        perror("[CHILD] Failed to make root mount private");
        return 1;
    }
    printf("[CHILD] --> Root mount is now private.\n");

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("[CHILD] Failed to mount /proc");
        return 1;
    }
    printf("[CHILD] --> /proc mounted.\n");
    
    printf("[CHILD] --> Ready to execute: %s\n", ((char **)arg)[0]);
    
    char **argv = (char **)arg;
    execv(argv[0], argv);
    
    // If we get here, execv FAILED.
    perror("[CHILD] !!! execv FAILED");
    return 1; // Return an error code
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args...]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    printf("[PARENT] --> Starting container...\n");

    char *container_stack = malloc(STACK_SIZE);
    if (!container_stack) {
        perror("[PARENT] malloc failed");
        exit(EXIT_FAILURE);
    }
    char *stack_top = container_stack + STACK_SIZE;

    int clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD;

    pid_t container_pid = clone(container_main, stack_top, clone_flags, &argv[1]);

    if (container_pid == -1) {
        perror("[PARENT] clone failed");
        free(container_stack);
        exit(EXIT_FAILURE);
    }

    printf("[PARENT] --> Container created with host PID %d.\n", container_pid);
    printf("[PARENT] --> I will now wait. Check for me and my child in 'pstree -p'.\n");

    // We will now wait for the child in a more controlled way
    int child_status;
    waitpid(container_pid, &child_status, 0);

    // WIFEXITED checks if the child terminated normally. WEXITSTATUS gets the exit code.
    int exit_code = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1;
    printf("[PARENT] --> Container terminated with exit code: %d\n", exit_code);

    free(container_stack);
    return 0;
}