// File: container_step5_final.c
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
#include <getopt.h>

#define STACK_SIZE (1024 * 1024)
#define MY_RUNTIME_CGROUP "/sys/fs/cgroup/my_runtime"
#define MY_RUNTIME_STATE "/run/my_runtime"
#define NEXT_CPU_FILE "/tmp/my_runtime_next_cpu"

// --- Helper Functions (write_file, setup_cgroup_hierarchy, container_main) ---
void write_file(const char *path, const char *content) { /* ... same ... */
    FILE *f = fopen(path, "w"); if (f == NULL) { fprintf(stderr, "Failed to open %s: ", path); perror(""); return; } fprintf(f, "%s", content); fclose(f);
}
void setup_cgroup_hierarchy() { /* ... same ... */
    if (access(MY_RUNTIME_CGROUP, F_OK) == 0) return; if (mkdir(MY_RUNTIME_CGROUP, 0755) != 0 && errno != EEXIST) { perror("mkdir my_runtime failed"); return; } char subtree_control_path[PATH_MAX]; snprintf(subtree_control_path, sizeof(subtree_control_path), "%s/cgroup.subtree_control", MY_RUNTIME_CGROUP); write_file(subtree_control_path, "+cpu +memory +pids");
}
int container_main(void *arg) { /* ... same ... */
    printf("[CHILD] --> Process started.\n"); sethostname("container", 9); char *rootfs = ((char **)arg)[0]; if (chroot(rootfs) != 0) { perror("chroot failed"); return 1; } printf("[CHILD] --> Root directory changed.\n"); if (chdir("/") != 0) { perror("chdir failed"); return 1; } mount("proc", "/proc", "proc", 0, NULL); char **argv = &(((char **)arg)[1]); execv(argv[0], argv); perror("[CHILD] !!! execv FAILED"); return 1;
}

int do_run(int argc, char *argv[]) {
    setup_cgroup_hierarchy();
    if (mkdir(MY_RUNTIME_STATE, 0755) != 0 && errno != EEXIST) {
        perror("mkdir runtime state dir failed"); return 1;
    }
    
    char *mem_limit = NULL;
    char *cpu_quota = NULL;
    int pin_cpu_flag = 0;
    
    // --- FIX 1: Correctly define long_options ---
    // We set .flag to NULL and provide a character for .val
    static struct option long_options[] = {
        {"pin-cpu", no_argument, NULL, 'p'},
        {0, 0, 0, 0}
    };
    int option_index = 0;
    int opt;

    // --- FIX 2: Add 'p' to the optstring ---
    while ((opt = getopt_long(argc, argv, "+m:C:p", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'm': mem_limit = optarg; break;
            case 'C': cpu_quota = optarg; break;
            case 'p': pin_cpu_flag = 1; break; // Set our flag when 'p' is seen
            default:
                fprintf(stderr, "Usage: %s run [-m mem] [-C quota] [--pin-cpu] <rootfs> <cmd>\n", argv[0]);
                return 1;
        }
    }

    if (optind + 1 >= argc) {
        fprintf(stderr, "Usage: %s run [-m mem] [-C quota] [--pin-cpu] <rootfs> <cmd>\n", argv[0]);
        return 1;
    }
    
    char **container_argv = &argv[optind];
    
    printf("[PARENT] --> Starting container...\n");
    char *container_stack = malloc(STACK_SIZE);
    char *stack_top = container_stack + STACK_SIZE;
    int clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD;
    pid_t container_pid = clone(container_main, stack_top, clone_flags, container_argv);

    if (container_pid == -1) {
        perror("clone failed"); free(container_stack); return 1;
    }
    printf("[PARENT] --> Container created with host PID %d.\n", container_pid);

    char state_dir[PATH_MAX];
    snprintf(state_dir, sizeof(state_dir), "%s/%d", MY_RUNTIME_STATE, container_pid);
    mkdir(state_dir, 0755);

    char cgroup_path[PATH_MAX];
    snprintf(cgroup_path, sizeof(cgroup_path), "%s/container_%d", MY_RUNTIME_CGROUP, container_pid);
    mkdir(cgroup_path, 0755);

    if (pin_cpu_flag) {
        FILE *f = fopen(NEXT_CPU_FILE, "r+");
        int next_cpu = 0;
        if (f) { fscanf(f, "%d", &next_cpu); } else { f = fopen(NEXT_CPU_FILE, "w"); }
        long num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
        int target_cpu = next_cpu % num_cpus;
        printf("[PARENT] --> Pinning container to CPU %d\n", target_cpu);
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(target_cpu, &cpuset);
        sched_setaffinity(container_pid, sizeof(cpu_set_t), &cpuset);
        struct sched_param param = { .sched_priority = 50 };
        sched_setscheduler(container_pid, SCHED_RR, &param);
        fseek(f, 0, SEEK_SET);
        fprintf(f, "%d", target_cpu + 1);
        fclose(f);
    }

    // Configure-Then-Add Logic
    if (mem_limit) {
        char mem_path[PATH_MAX]; snprintf(mem_path, sizeof(mem_path), "%s/memory.max", cgroup_path); write_file(mem_path, mem_limit);
        char swap_path[PATH_MAX]; snprintf(swap_path, sizeof(swap_path), "%s/memory.swap.max", cgroup_path); write_file(swap_path, "0");
    }
    if (cpu_quota) {
        char cpu_path[PATH_MAX]; char cpu_content[64]; snprintf(cpu_path, sizeof(cpu_path), "%s/cpu.max", cgroup_path); snprintf(cpu_content, sizeof(cpu_content), "%s 100000", cpu_quota); write_file(cpu_path, cpu_content);
    }
    char procs_path[PATH_MAX];
    snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path);
    write_file(procs_path, "self");
    
    int child_status;
    waitpid(container_pid, &child_status, 0);

    rmdir(state_dir);
    rmdir(cgroup_path);
    
    printf("[PARENT] --> Container terminated.\n");
    return WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1;
}

// Placeholder `do_list` and `do_status` functions remain the same
int do_list(int argc, char *argv[]) {
    printf("Listing running containers...\n"); printf("CONTAINER_PID\tCOMMAND\n"); return 0;
}
int do_status(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s status <container_pid>\n", argv[0]); return 1; }
    printf("Getting status for container %s...\n", argv[1]); return 0;
}

// `main` dispatcher function remains the same
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args...]\nCommands: run, list, status\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "run") == 0) {
        return do_run(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "list") == 0) {
        return do_list(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "status") == 0) {
        return do_status(argc - 1, &argv[1]);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
    return 0;
}