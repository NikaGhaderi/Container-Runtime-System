// File: container_step4_final.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>     // For getopt, access, chdir
#include <sys/mount.h>
#include <sys/stat.h>   // For mkdir
#include <string.h>
#include <errno.h>      // For errno

#define STACK_SIZE (1024 * 1024)
#define MY_RUNTIME_CGROUP "/sys/fs/cgroup/my_runtime"

// Helper function to write a string to a file
void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "Failed to open %s: ", path);
        perror("");
        return;
    }
    fprintf(f, "%s", content);
    fclose(f);
}

// NEW function to handle one-time setup of our main cgroup
void setup_cgroup_hierarchy() {
    // Check if our main cgroup directory already exists
    if (access(MY_RUNTIME_CGROUP, F_OK) == 0) {
        printf("[SETUP] --> Cgroup hierarchy at %s already exists.\n", MY_RUNTIME_CGROUP);
        return;
    }

    printf("[SETUP] --> Creating cgroup hierarchy at %s...\n", MY_RUNTIME_CGROUP);
    if (mkdir(MY_RUNTIME_CGROUP, 0755) != 0) {
        if (errno != EEXIST) { // It's okay if it already exists
            perror("mkdir my_runtime failed");
            // In a real app, you might want to exit here. For now, we'll continue.
            return;
        }
    }

    // Delegate controllers to children of our main cgroup
    char subtree_control_path[256];
    snprintf(subtree_control_path, sizeof(subtree_control_path), "%s/cgroup.subtree_control", MY_RUNTIME_CGROUP);
    write_file(subtree_control_path, "+cpu +memory +pids");
    printf("[SETUP] --> Delegated controllers.\n");
}


int container_main(void *arg) {
    // This function remains unchanged
    printf("[CHILD] --> Process started.\n");
    sethostname("container", 9);
    char *rootfs = ((char **)arg)[0];
    chroot(rootfs);
    chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);
    char **argv = &(((char **)arg)[1]);
    execv(argv[0], argv);
    perror("[CHILD] !!! execv FAILED");
    return 1;
}

int main(int argc, char *argv[]) {
    // --- One-Time Setup ---
    setup_cgroup_hierarchy();
    // --- End of Setup ---
    
    char *mem_limit = NULL;
    char *cpu_quota = NULL;
    int opt;

    // The rest of the main function is the same as the previous "fixed" version
    while ((opt = getopt(argc, argv, "m:c:")) != -1) { /* ... getopt logic ... */ 
        switch (opt) {
            case 'm':
                mem_limit = optarg;
                break;
            case 'c':
                cpu_quota = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s [-m mem_limit] [-c cpu_quota] <rootfs> <cmd> [args]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    if (optind + 1 >= argc) { /* ... usage check ... */
        fprintf(stderr, "Usage: %s [-m mem_limit] [-c cpu_quota] <rootfs> <cmd> [args]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    char **container_argv = &argv[optind];
    char *container_rootfs = container_argv[0];
    printf("[PARENT] --> Starting container...\n");
    char *container_stack = malloc(STACK_SIZE);
    char *stack_top = container_stack + STACK_SIZE;
    int clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD;
    pid_t container_pid = clone(container_main, stack_top, clone_flags, container_argv);

    if (container_pid == -1) { /* ... error handling ... */
        perror("[PARENT] clone failed");
        free(container_stack);
        exit(EXIT_FAILURE);
    }
    printf("[PARENT] --> Container created with host PID %d.\n", container_pid);

    char cgroup_path[256];
    snprintf(cgroup_path, sizeof(cgroup_path), "%s/container_%d", MY_RUNTIME_CGROUP, container_pid);
    
    if (mkdir(cgroup_path, 0755) != 0) {
        perror("mkdir cgroup failed");
    } else {
        printf("[PARENT] --> Cgroup created at %s\n", cgroup_path);

        char procs_path[256];
        char pid_str[16];
        snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path);
        snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
        write_file(procs_path, pid_str);

        if (mem_limit) { /* ... set mem limit ... */
            char mem_path[256];
            snprintf(mem_path, sizeof(mem_path), "%s/memory.max", cgroup_path);
            write_file(mem_path, mem_limit);
            printf("[PARENT] --> Set memory limit to %s\n", mem_limit);
        }

        if (cpu_quota) { /* ... set cpu limit ... */
            char cpu_path[256];
            char cpu_content[64];
            snprintf(cpu_path, sizeof(cpu_path), "%s/cpu.max", cgroup_path);
            snprintf(cpu_content, sizeof(cpu_content), "%s 100000", cpu_quota);
            write_file(cpu_path, cpu_content);
            printf("[PARENT] --> Set CPU quota to %s\n", cpu_quota);
        }
    }
    
    int child_status;
    waitpid(container_pid, &child_status, 0);

    if (rmdir(cgroup_path) != 0) {
        // perror("rmdir cgroup failed");
    } else {
        printf("[PARENT] --> Cgroup cleaned up.\n");
    }

    int exit_code = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1;
    printf("[PARENT] --> Container terminated with exit code: %d\n", exit_code);

    free(container_stack);
    return 0;
}