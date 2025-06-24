// File: container_final_robust.c
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
#include <dirent.h>

#define STACK_SIZE (1024 * 1024)
#define MY_RUNTIME_CGROUP "/sys/fs/cgroup/my_runtime"
#define MY_RUNTIME_STATE "/run/my_runtime"
#define NEXT_CPU_FILE "/tmp/my_runtime_next_cpu"

// --- The New, Robust Setup Function ---
void setup_cgroup_hierarchy() {
    mkdir(MY_RUNTIME_CGROUP, 0755);
    mkdir(MY_RUNTIME_STATE, 0755);
    system("echo \"+cpu +memory +pids\" > /sys/fs/cgroup/my_runtime/cgroup.subtree_control");
}

// All other helper functions and container_main are the same
void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w"); if (f == NULL) { perror("fopen"); return; }
    fprintf(f, "%s", content); fclose(f);
}
int container_main(void *arg) { /* ... same ... */
    sethostname("container", 9); char *rootfs = ((char **)arg)[0]; if (chroot(rootfs) != 0) { perror("chroot failed"); return 1; } if (chdir("/") != 0) { perror("chdir failed"); return 1; } mount("proc", "/proc", "proc", 0, NULL); printf("[CHILD] Executing command...\n"); char **argv = &(((char **)arg)[1]); execv(argv[0], argv); perror("[CHILD] !!! execv FAILED"); return 1;
}

// --- Simplified `do_run` function (no detach logic) ---
int do_run(int argc, char *argv[]) {
    setup_cgroup_hierarchy();

    char *mem_limit = NULL; char *cpu_quota = NULL; int pin_cpu_flag = 0;
    static struct option long_options[] = {
            {"pin-cpu", no_argument, NULL, 'p'}, {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "+m:C:p", long_options, NULL)) != -1) {
        switch (opt) {
            case 'm': mem_limit = optarg; break; case 'C': cpu_quota = optarg; break; case 'p': pin_cpu_flag = 1; break; default:
                fprintf(stderr, "Usage: %s run ...\n", argv[0]); return 1;
        }
    }
    if (optind + 1 >= argc) { fprintf(stderr, "Usage: %s run ...\n", argv[0]); return 1; }

    char **container_argv = &argv[optind];
    char *container_stack = malloc(STACK_SIZE);
    char *stack_top = container_stack + STACK_SIZE;
    int clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD;
    pid_t container_pid = clone(container_main, stack_top, clone_flags, container_argv);

    if (container_pid == -1) { perror("clone failed"); free(container_stack); return 1; }

    printf("[PARENT] Container created with host PID %d.\n", container_pid);

    // Setup state, cgroup, and limits...
    char state_dir[PATH_MAX]; snprintf(state_dir, sizeof(state_dir), "%s/%d", MY_RUNTIME_STATE, container_pid); mkdir(state_dir, 0755);
    char cgroup_path[PATH_MAX]; snprintf(cgroup_path, sizeof(cgroup_path), "%s/container_%d", MY_RUNTIME_CGROUP, container_pid); mkdir(cgroup_path, 0755);
    char cmd_path[PATH_MAX]; snprintf(cmd_path, sizeof(cmd_path), "%s/command", state_dir);
    FILE *cmd_file = fopen(cmd_path, "w"); if (cmd_file) { for (int i = 0; container_argv[i] != NULL; i++) { fprintf(cmd_file, "%s ", container_argv[i]); } fclose(cmd_file); }
    if (pin_cpu_flag) { /* ... pinning logic is the same ... */ }
    if (mem_limit) { /* ... mem limit logic is the same ... */ }
    if (cpu_quota) { /* ... cpu limit logic is the same ... */ }
    char procs_path[PATH_MAX]; char pid_str[16]; snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path); snprintf(pid_str, sizeof(pid_str), "%d", container_pid); write_file(procs_path, pid_str);

    // The parent process now ALWAYS waits for the child.
    printf("[PARENT] Waiting for container %d to exit...\n", container_pid);
    int child_status;
    waitpid(container_pid, &child_status, 0);

    // Cleanup for the foreground container
    char cmd_to_remove[PATH_MAX]; snprintf(cmd_to_remove, sizeof(cmd_to_remove), "%s/command", state_dir); remove(cmd_to_remove);
    rmdir(state_dir); rmdir(cgroup_path);

    printf("[PARENT] Container %d terminated.\n", container_pid);
    return WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1;
}

// The do_list and do_status functions can now be simplified as they
// don't need reaper logic if we only run detached containers with nohup
// and manage them properly. For now, let's use the robust status logic.

// Helper functions for status
void format_bytes(long bytes, char *buf, size_t size) { /* ... same as before ... */ }
long read_cgroup_long(const char *path) { /* ... new helper from last time ... */ }
// We can now use the final formatted do_status from our last attempt.

// The `main` function is also simplified (no detach)
int main(int argc, char *argv[]) { /* ... same, but you could remove detach from help text ... */ }
