// File: container_minimal_test.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#define STACK_SIZE (1024 * 1024)
#define MY_RUNTIME_CGROUP "/sys/fs/cgroup/my_runtime"

// A helper function with proper error checking
void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "CRITICAL: Failed to open %s: ", path);
        perror("");
        exit(1); // Exit on failure
    }
    fprintf(f, "%s", content);
    fclose(f);
}

// The new, robust setup function
void setup_cgroup_hierarchy() {
    if (mkdir(MY_RUNTIME_CGROUP, 0755) != 0 && errno != EEXIST) {
        perror("CRITICAL: mkdir cgroup failed");
        exit(1);
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/cgroup.subtree_control", MY_RUNTIME_CGROUP);

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        printf("[SETUP] NOTICE: Could not open %s for writing. This is expected on subsequent runs.\n", path);
    } else {
        if (fprintf(f, "+cpu +memory +pids") < 0) {
            fprintf(stderr, "[SETUP] NOTICE: Failed to write to %s.\n", path);
        } else {
            printf("[SETUP] Delegated controllers (+cpu +memory +pids) successfully.\n");
        }
        fclose(f);
    }

    printf("[SETUP] Cgroup hierarchy is ready.\n");
}


int container_main(void *arg) {
    printf("[CHILD] Inside container, PID: %d\n", getpid());
    sethostname("container", 9);
    char *rootfs = ((char **)arg)[0];
    if (chroot(rootfs) != 0) { perror("[CHILD] chroot failed"); return 1; }
    if (chdir("/") != 0) { perror("[CHILD] chdir failed"); return 1; }
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) { perror("[CHILD] mount proc failed"); return 1; }
    char **argv = &(((char **)arg)[1]);
    printf("[CHILD] Executing: %s\n", argv[0]);
    execv(argv[0], argv);
    perror("[CHILD] !!! execv FAILED");
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <rootfs> <cmd> [args...]\n", argv[0]);
        return 1;
    }
//    setup_cgroup_hierarchy();
    char **container_argv = &argv[1];
    char *container_stack = malloc(STACK_SIZE);
    if (!container_stack) { perror("CRITICAL: Malloc failed"); return 1; }
    char *stack_top = container_stack + STACK_SIZE;
    printf("[PARENT] Launching container...\n");
    pid_t container_pid = clone(container_main, stack_top, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, container_argv);
    if (container_pid == -1) {
        perror("CRITICAL: clone failed");
        free(container_stack);
        return 1;
    }
    printf("[PARENT] Container process created with host PID %d.\n", container_pid);
    char cgroup_path[PATH_MAX];
    snprintf(cgroup_path, sizeof(cgroup_path), "%s/container_%d", MY_RUNTIME_CGROUP, container_pid);
    if (mkdir(cgroup_path, 0755) != 0) { perror("[PARENT] mkdir container cgroup failed"); }
    char procs_path[PATH_MAX];
    snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path);
    char pid_str[16];
    snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
    write_file(procs_path, pid_str);
    printf("[PARENT] Container placed in cgroup. Waiting for it to exit...\n");
    waitpid(container_pid, NULL, 0);
    printf("[PARENT] Container terminated. Cleaning up...\n");
    rmdir(cgroup_path);
    free(container_stack);
    return 0;
}
