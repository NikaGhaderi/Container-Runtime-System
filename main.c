// File: container_runtime_complete.c
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
#include <time.h> // For srand

// --- All helper functions from our stable version ---
#define STACK_SIZE (1024 * 1024)
#define MY_RUNTIME_CGROUP "/sys/fs/cgroup/my_runtime"
#define MY_RUNTIME_STATE "/run/my_runtime"
#define NEXT_CPU_FILE "/tmp/my_runtime_next_cpu"

void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f == NULL) { perror("fopen for write"); return; }
    fprintf(f, "%s", content);
    fclose(f);
}

void setup_cgroup_hierarchy() {
    mkdir(MY_RUNTIME_CGROUP, 0755);
    mkdir(MY_RUNTIME_STATE, 0755);
    system("echo \"+cpu +memory +pids\" > /sys/fs/cgroup/my_runtime/cgroup.subtree_control 2>/dev/null || true");
}

struct container_args {
    char* merged_path;
    char** argv;
};

int container_main(void *arg) {
    struct container_args* args = (struct container_args*)arg;
    sethostname("container", 9);
    if (chroot(args->merged_path) != 0) { perror("chroot failed"); return 1; }
    if (chdir("/") != 0) { perror("chdir failed"); return 1; }
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) { perror("mount proc failed"); }
    execv(args->argv[0], args->argv);
    perror("execv failed");
    return 1;
}


// --- The complete 'do_run' function with all features ---
int do_run(int argc, char *argv[]) {
    setup_cgroup_hierarchy();

    char *mem_limit = NULL;
    char *cpu_quota = NULL;

    static struct option long_options[] = {
        {"mem", required_argument, 0, 'm'},
        {"cpu", required_argument, 0, 'C'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "+m:C:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'm': mem_limit = optarg; break;
            case 'C': cpu_quota = optarg; break;
            default: return 1;
        }
    }
    if (optind + 1 >= argc) {
        fprintf(stderr, "Usage: %s run [options] <image> <cmd>...\n", argv[0]);
        return 1;
    }
    char* image_name = argv[optind];
    char** container_cmd_argv = &argv[optind + 1];

    // --- OverlayFS Setup ---
    char lowerdir[PATH_MAX], upperdir[PATH_MAX], workdir[PATH_MAX], merged[PATH_MAX];
    srand(time(NULL));
    int random_id = rand() % 10000;
    snprintf(lowerdir, sizeof(lowerdir), "%s", image_name);
    snprintf(upperdir, sizeof(upperdir), "overlay_layers/%d/upper", random_id);
    snprintf(workdir, sizeof(workdir), "overlay_layers/%d/work", random_id);
    snprintf(merged, sizeof(merged), "overlay_layers/%d/merged", random_id);
    char command[PATH_MAX * 2];
    sprintf(command, "mkdir -p %s %s %s", upperdir, workdir, merged);
    if (system(command) != 0) { return 1; }
    
    char mount_opts[PATH_MAX * 3];
    snprintf(mount_opts, sizeof(mount_opts), "lowerdir=%s,upperdir=%s,workdir=%s", lowerdir, upperdir, workdir);
    if (mount("overlay", merged, "overlay", 0, mount_opts) != 0) {
        perror("Overlay mount failed");
        return 1;
    }

    // --- Clone Setup ---
    struct container_args args;
    args.merged_path = merged;
    args.argv = container_cmd_argv;
    char *container_stack = malloc(STACK_SIZE);
    char *stack_top = container_stack + STACK_SIZE;
    pid_t container_pid = clone(container_main, stack_top, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, &args);

    if (container_pid == -1) { perror("clone"); return 1; }

    // --- Cgroup Setup (for the new container) ---
    char cgroup_path[PATH_MAX];
    snprintf(cgroup_path, sizeof(cgroup_path), "%s/container_%d", MY_RUNTIME_CGROUP, container_pid);
    mkdir(cgroup_path, 0755);

    if (mem_limit) {
        char mem_path[PATH_MAX];
        snprintf(mem_path, sizeof(mem_path), "%s/memory.max", cgroup_path);
        write_file(mem_path, mem_limit);
    }
    if (cpu_quota) {
        char cpu_path[PATH_MAX], cpu_content[64];
        snprintf(cpu_path, sizeof(cpu_path), "%s/cpu.max", cgroup_path);
        snprintf(cpu_content, sizeof(cpu_content), "%s 100000", cpu_quota);
        write_file(cpu_path, cpu_content);
    }
    char procs_path[PATH_MAX], pid_str[16];
    snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path);
    snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
    write_file(procs_path, pid_str);

    printf("Container started with PID %d. Press Ctrl+C to stop.\n", container_pid);
    waitpid(container_pid, NULL, 0);
    printf("Container %d terminated.\n", container_pid);

    // --- Full Cleanup ---
    printf("[PARENT] Cleaning up mounts and directories...\n");
    char proc_path[PATH_MAX];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", merged);
    umount(proc_path);
    umount(merged);
    rmdir(cgroup_path);
    sprintf(command, "rm -rf overlay_layers/%d", random_id);
    system(command);
    
    return 0;
}

// A simplified main that only knows "run" for this final test
int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "run") == 0) {
        return do_run(argc - 1, &argv[1]);
    } else {
        fprintf(stderr, "Usage: %s run [options] <image> <command>...\n", argv[0]);
        return 1;
    }
    return 0;
}