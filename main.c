// File: container_final_with_lifecycle.c
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
#include <time.h>

// --- All helper functions are included ---
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

// --- MODIFIED do_run: NO cleanup logic at the end ---
int do_run(int argc, char *argv[]) {
    setup_cgroup_hierarchy();
    char *mem_limit = NULL; char *cpu_quota = NULL; int pin_cpu_flag = 0; int detach_flag = 0;

    static struct option long_options[] = {
        {"mem", required_argument, 0, 'm'}, {"cpu", required_argument, 0, 'C'},
        {"pin-cpu", no_argument, NULL, 'p'}, {"detach", no_argument, NULL, 'd'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "+m:C:pd", long_options, NULL)) != -1) {
        switch (opt) {
            case 'm': mem_limit = optarg; break; case 'C': cpu_quota = optarg; break;
            case 'p': pin_cpu_flag = 1; break; case 'd': detach_flag = 1; break;
            default: return 1;
        }
    }
    if (optind + 1 >= argc) { fprintf(stderr, "Usage: %s run [opts] <image> <cmd>...\n", argv[0]); return 1; }
    char* image_name = argv[optind];
    char** container_cmd_argv = &argv[optind + 1];

    if (detach_flag) {
        if (fork() != 0) { exit(0); }
        setsid();
        freopen("/dev/null", "r", stdin); freopen("/dev/null", "w", stdout); freopen("/dev/null", "w", stderr);
    }

    // OverlayFS Setup
    char lowerdir[PATH_MAX], upperdir[PATH_MAX], workdir[PATH_MAX], merged[PATH_MAX];
    srand(time(NULL) ^ getpid());
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
    if (mount("overlay", merged, "overlay", 0, mount_opts) != 0) { perror("Overlay mount failed"); return 1; }

    struct container_args args;
    args.merged_path = merged;
    args.argv = container_cmd_argv;
    char *container_stack = malloc(STACK_SIZE);
    char *stack_top = container_stack + STACK_SIZE;
    pid_t container_pid = clone(container_main, stack_top, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, &args);

    if (container_pid == -1) { perror("clone"); return 1; }
    
    char state_dir[PATH_MAX]; snprintf(state_dir, sizeof(state_dir), "%s/%d", MY_RUNTIME_STATE, container_pid); mkdir(state_dir, 0755);
    char cgroup_path[PATH_MAX]; snprintf(cgroup_path, sizeof(cgroup_path), "%s/container_%d", MY_RUNTIME_CGROUP, container_pid);
    if(mkdir(cgroup_path, 0755) != 0 && errno != EEXIST) { perror("Failed to create container cgroup"); }

    char cmd_path[PATH_MAX]; snprintf(cmd_path, sizeof(cmd_path), "%s/command", state_dir);
    FILE *cmd_file = fopen(cmd_path, "w");
    if (cmd_file) {
        for (int i = 0; container_cmd_argv[i] != NULL; i++) { fprintf(cmd_file, "%s ", container_cmd_argv[i]); }
        fclose(cmd_file);
    }
    
    // Apply resource limits logic...
    if (cpu_quota) { /* ... */ } // This logic remains the same
    
    char procs_path[PATH_MAX]; snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path);
    char pid_str[16]; snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
    write_file(procs_path, pid_str);

    if (detach_flag) {
        printf("Container started with PID %d\n", container_pid);
        return 0;
    }

    printf("Container started with PID %d. Press Ctrl+C to stop.\n", container_pid);
    waitpid(container_pid, NULL, 0);
    printf("Container %d has exited.\n", container_pid);
    // THE CLEANUP LOGIC HAS BEEN REMOVED FROM HERE
    return 0;
}

// --- list, status, freeze, thaw functions are unchanged ---
int do_list(int argc, char *argv[]) { /* ... */ }
int do_status(int argc, char *argv[]) { /* ... */ }
int do_freeze(int argc, char *argv[]) { /* ... */ }
int do_thaw(int argc, char *argv[]) { /* ... */ }

// --- The NEW stop and rm commands ---
int do_stop(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s stop <container_pid>\n", argv[0]); return 1; }
    pid_t pid = atoi(argv[1]);
    if (kill(pid, SIGKILL) != 0) { perror("kill"); return 1; }
    printf("Container %d stopped.\n", pid);
    return 0;
}

int do_rm(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s rm <container_pid>\n", argv[0]); return 1; }
    char *pid_str = argv[1];
    char proc_path[PATH_MAX]; snprintf(proc_path, sizeof(proc_path), "/proc/%s", pid_str);
    if (access(proc_path, F_OK) == 0) {
        fprintf(stderr, "Error: Cannot remove a running container. Stop it first.\n");
        return 1;
    }
    printf("Removing container %s...\n", pid_str);
    char state_dir[PATH_MAX]; snprintf(state_dir, sizeof(state_dir), "%s/%s", MY_RUNTIME_STATE, pid_str);
    char cgroup_dir[PATH_MAX]; snprintf(cgroup_dir, sizeof(cgroup_dir), "%s/container_%s", MY_RUNTIME_CGROUP, pid_str);
    
    // We cannot easily clean up the overlayfs layers for a stopped container,
    // as we don't save the random ID. This is a known limitation for this project.
    
    char cmd_to_remove[PATH_MAX]; snprintf(cmd_to_remove, sizeof(cmd_to_remove), "%s/command", state_dir);
    remove(cmd_to_remove);
    rmdir(state_dir);
    rmdir(cgroup_dir);
    printf("Container %s removed.\n", pid_str);
    return 0;
}

// --- The FINAL main dispatcher function ---
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args...]\nCommands: run, list, status, freeze, thaw, stop, rm\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "run") == 0) {
        return do_run(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "list") == 0) {
        return do_list(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "status") == 0) {
        // return do_status(argc - 1, &argv[1]); // You can re-enable this
    } else if (strcmp(argv[1], "freeze") == 0) {
        return do_freeze(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "thaw") == 0) {
        return do_thaw(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "stop") == 0) {
        return do_stop(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "rm") == 0) {
        return do_rm(argc - 1, &argv[1]);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
    return 0;
}