// File: container_step3_chroot.c
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
    printf("[CHILD] --> Process started.\n");

    sethostname("container", 9);
    
    // Path to the new rootfs is passed as the first argument
    char *rootfs = ((char **)arg)[0];

    // Chroot: Change the root directory
    if (chroot(rootfs) != 0) {
        perror("chroot failed");
        return 1;
    }
    printf("[CHILD] --> Root directory changed to: %s\n", rootfs);

    // Chdir: Change the current working directory to the new root
    if (chdir("/") != 0) {
        perror("chdir failed");
        return 1;
    }

    // Remount /proc. It MUST be done AFTER chroot.
    mount("proc", "/proc", "proc", 0, NULL);

    // We execute the command passed AFTER the rootfs path
    char **argv = &(((char **)arg)[1]);
    execv(argv[0], argv);
    
    perror("[CHILD] !!! execv FAILED");
    return 1;
}

int main(int argc, char *argv[]) {
    // We now need at least 3 arguments: ./program <rootfs_path> <command>
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <rootfs_path> <command> [args...]\n", argv[0]);
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
    
    // We pass all arguments from index 1 onwards to the child
    pid_t container_pid = clone(container_main, stack_top, clone_flags, &argv[1]);

    if (container_pid == -1) {
        perror("[PARENT] clone failed");
        free(container_stack);
        exit(EXIT_FAILURE);
    }

    printf("[PARENT] --> Container created with host PID %d.\n", container_pid);

    int child_status;
    waitpid(container_pid, &child_status, 0);

    int exit_code = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1;
    printf("[PARENT] --> Container terminated with exit code: %d\n", exit_code);

    free(container_stack);
    return 0;
}