// File: container_final_stable.c
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

#define STACK_SIZE (1024 * 1024)
#define MY_RUNTIME_CGROUP "/sys/fs/cgroup/my_runtime"
#define MY_RUNTIME_STATE "/run/my_runtime"

// --- Helper Functions with Error Checking ---

void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "ERROR: Failed to open %s for writing: ", path);
        perror("");
        // In a real-world scenario, you might handle this more gracefully
    } else {
        fprintf(f, "%s", content);
        fclose(f);
    }
}

void setup_cgroup_hierarchy() {
    mkdir(MY_RUNTIME_CGROUP, 0755);
    mkdir(MY_RUNTIME_STATE, 0755);
    if (system("echo \"+cpu +memory +pids\" > /sys/fs/cgroup/my_runtime/cgroup.subtree_control 2>/dev/null") != 0) {
        // This is not a fatal error, as it's expected to fail on subsequent runs.
    }
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

// --- CLI Command Functions ---

int do_run(int argc, char *argv[]) {
    printf("[run] Starting...\n");
    setup_cgroup_hierarchy();
    
    char *mem_limit = NULL; char *cpu_quota = NULL;
    static struct option long_options[] = {
        {"mem", required_argument, 0, 'm'}, {"cpu", required_argument, 0, 'C'}, {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "+m:C:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'm': mem_limit = optarg; break; case 'C': cpu_quota = optarg; break;
            default: return 1;
        }
    }
    if (optind + 1 >= argc) { fprintf(stderr, "Usage: %s run [opts] <image> <cmd>...\n", argv[0]); return 1; }
    char* image_name = argv[optind];
    char** container_cmd_argv = &argv[optind + 1];

    printf("[run] Setting up overlay directories...\n");
    char lowerdir[PATH_MAX], upperdir[PATH_MAX], workdir[PATH_MAX], merged[PATH_MAX];
    srand(time(NULL) ^ getpid()); int random_id = rand() % 10000;
    snprintf(lowerdir, sizeof(lowerdir), "%s", image_name);
    snprintf(upperdir, sizeof(upperdir), "overlay_layers/%d/upper", random_id);
    snprintf(workdir, sizeof(workdir), "overlay_layers/%d/work", random_id);
    snprintf(merged, sizeof(merged), "overlay_layers/%d/merged", random_id);
    char command[PATH_MAX * 2];
    sprintf(command, "mkdir -p %s %s %s", upperdir, workdir, merged);
    if (system(command) != 0) { fprintf(stderr, "ERROR: Failed to create overlay directories\n"); return 1; }

    printf("[run] Mounting overlayfs...\n");
    char mount_opts[PATH_MAX * 3];
    snprintf(mount_opts, sizeof(mount_opts), "lowerdir=%s,upperdir=%s,workdir=%s", lowerdir, upperdir, workdir);
    if (mount("overlay", merged, "overlay", 0, mount_opts) != 0) { perror("ERROR: Overlay mount failed"); return 1; }

    struct container_args args; args.merged_path = merged; args.argv = container_cmd_argv;
    char *container_stack = malloc(STACK_SIZE);
    char *stack_top = container_stack + STACK_SIZE;
    
    printf("[run] Cloning container process...\n");
    pid_t container_pid = clone(container_main, stack_top, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, &args);

    if (container_pid == -1) { perror("ERROR: clone failed"); return 1; }
    
    printf("[run] Setting up cgroup and state for PID %d...\n", container_pid);
    char state_dir[PATH_MAX]; snprintf(state_dir, sizeof(state_dir), "%s/%d", MY_RUNTIME_STATE, container_pid); mkdir(state_dir, 0755);
    char cgroup_path[PATH_MAX]; snprintf(cgroup_path, sizeof(cgroup_path), "%s/container_%d", MY_RUNTIME_CGROUP, container_pid);
    if(mkdir(cgroup_path, 0755) != 0 && errno != EEXIST) { perror("ERROR: Failed to create container cgroup"); }

    char cmd_path[PATH_MAX]; snprintf(cmd_path, sizeof(cmd_path), "%s/command", state_dir);
    FILE *cmd_file = fopen(cmd_path, "w");
    if (cmd_file) {
        for (int i = 0; container_cmd_argv[i] != NULL; i++) { fprintf(cmd_file, "%s ", container_cmd_argv[i]); }
        fclose(cmd_file);
    }
    
    if (pin_cpu_flag) {
        FILE *f = fopen(NEXT_CPU_FILE, "r+"); int next_cpu = 0; if (f) { fscanf(f, "%d", &next_cpu); } else { f = fopen(NEXT_CPU_FILE, "w"); } long num_cpus = sysconf(_SC_NPROCESSORS_ONLN); int target_cpu = next_cpu % num_cpus; cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(target_cpu, &cpuset); sched_setaffinity(container_pid, sizeof(cpu_set_t), &cpuset); struct sched_param param = { .sched_priority = 50 }; sched_setscheduler(container_pid, SCHED_RR, &param); fseek(f, 0, SEEK_SET); fprintf(f, "%d", target_cpu + 1); fclose(f);
    }
    if (mem_limit) {
        char mem_path[PATH_MAX]; snprintf(mem_path, sizeof(mem_path), "%s/memory.max", cgroup_path); write_file(mem_path, mem_limit);
        char swap_path[PATH_MAX]; snprintf(swap_path, sizeof(swap_path), "%s/memory.swap.max", cgroup_path); write_file(swap_path, "0");
    }
    if (cpu_quota) {
        char cpu_path[PATH_MAX]; char cpu_content[64]; snprintf(cpu_path, sizeof(cpu_path), "%s/cpu.max", cgroup_path);
        snprintf(cpu_content, sizeof(cpu_content), "%s 100000", cpu_quota);
        write_file(cpu_path, cpu_content);
    }
    
    char procs_path[PATH_MAX]; snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path);
    char pid_str[16]; snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
    write_file(procs_path, pid_str);

    printf("[run] Handing off to container. Waiting for exit...\n");
    waitpid(container_pid, NULL, 0);
    printf("[run] Container %d has exited.\n", container_pid);
    
    // The 'rm' command is responsible for cleanup.
    return 0;
}

int do_list(int argc, char *argv[]) {
    DIR *d = opendir(MY_RUNTIME_STATE);
    if (d == NULL) {
        if (errno == ENOENT) { printf("No running containers.\n"); return 0; }
        perror("opendir"); return 1;
    }
    printf("%-15s\t%-10s\t%s\n", "CONTAINER PID", "STATUS", "COMMAND");
    struct dirent *dir_entry;
    while ((dir_entry = readdir(d)) != NULL) {
        if (dir_entry->d_type != DT_DIR || strcmp(dir_entry->d_name, ".") == 0 || strcmp(dir_entry->d_name, "..") == 0) continue;
        char proc_path[PATH_MAX]; snprintf(proc_path, sizeof(proc_path), "/proc/%s", dir_entry->d_name);
        const char* status = (access(proc_path, F_OK) == 0) ? "Running" : "Stopped";
        char cmd_path[PATH_MAX], cmd_buf[1024] = {0};
        snprintf(cmd_path, sizeof(cmd_path), "%s/%s/command", MY_RUNTIME_STATE, dir_entry->d_name);
        FILE *cmd_file = fopen(cmd_path, "r");
        if (cmd_file) {
            fgets(cmd_buf, sizeof(cmd_buf) - 1, cmd_file);
            cmd_buf[strcspn(cmd_buf, "\n")] = 0;
            fclose(cmd_file);
        }
        printf("%-15s\t%-10s\t%s\n", dir_entry->d_name, status, cmd_buf);
    }
    closedir(d);
    return 0;
}

int do_status(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s status <container_pid>\n", argv[0]); return 1; }
    char *pid_str = argv[1];
    char path_buffer[PATH_MAX], format_buffer[64];
    char state_dir[PATH_MAX]; snprintf(state_dir, sizeof(state_dir), "%s/%s", MY_RUNTIME_STATE, pid_str);
    if (access(state_dir, F_OK) != 0) { fprintf(stderr, "Error: No container with PID %s found.\n", pid_str); return 1; }
    printf("--- Status for Container PID %s ---\n", pid_str);
    snprintf(path_buffer, sizeof(path_buffer), "%s/command", state_dir);
    FILE *f = fopen(path_buffer, "r");
    if (f) {
        char cmd_buf[1024] = {0}; fgets(cmd_buf, sizeof(cmd_buf)-1, f);
        cmd_buf[strcspn(cmd_buf, "\n")] = 0; printf("%-25s: %s\n", "Command", cmd_buf); fclose(f);
    }
    char cgroup_path[PATH_MAX]; snprintf(cgroup_path, sizeof(cgroup_path), "%s/container_%s", MY_RUNTIME_CGROUP, pid_str);
    printf("\n--- Resources ---\n");
    snprintf(path_buffer, sizeof(path_buffer), "%s/memory.current", cgroup_path);
    long mem_current = read_cgroup_long(path_buffer);
    format_bytes(mem_current, format_buffer, sizeof(format_buffer));
    printf("%-25s: %s\n", "Memory Usage", format_buffer);
    snprintf(path_buffer, sizeof(path_buffer), "%s/cpu.stat", cgroup_path);
    long cpu_micros = find_cgroup_value(path_buffer, "usage_usec");
    if (cpu_micros >= 0) { printf("%-25s: %.2f seconds\n", "Total CPU Time", (double)cpu_micros / 1000000.0); }
    snprintf(path_buffer, sizeof(path_buffer), "%s/pids.current", cgroup_path);
    long pids_current = read_cgroup_long(path_buffer);
    printf("%-25s: %ld\n", "Active Processes/Threads", pids_current);
    printf("\n----------------------------------\n");
    return 0;
}

int do_freeze(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s freeze <container_pid>\n", argv[0]); return 1; }
    char *pid_str = argv[1]; char freeze_path[PATH_MAX];
    snprintf(freeze_path, sizeof(freeze_path), "%s/container_%s/cgroup.freeze", MY_RUNTIME_CGROUP, pid_str);
    write_file(freeze_path, "1"); printf("Froze container %s.\n", pid_str); return 0;
}

int do_thaw(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s thaw <container_pid>\n", argv[0]); return 1; }
    char *pid_str = argv[1]; char freeze_path[PATH_MAX];
    snprintf(freeze_path, sizeof(freeze_path), "%s/container_%s/cgroup.freeze", MY_RUNTIME_CGROUP, pid_str);
    write_file(freeze_path, "0"); printf("Thawed container %s.\n", pid_str); return 0;
}

int do_stop(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s stop <container_pid>\n", argv[0]); return 1; }
    char *pid_str = argv[1]; pid_t pid = atoi(pid_str);
    if (kill(pid, SIGKILL) != 0) { perror("kill"); return 1; }
    printf("Container %s stopped.\n", pid_str);
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
    char cmd_path[PATH_MAX]; snprintf(cmd_path, sizeof(cmd_path), "%s/command", state_dir);
    // Note: We cannot easily clean up the overlayfs layers, so we don't try.
    // This is a known limitation.
    remove(cmd_path);
    rmdir(state_dir);
    rmdir(cgroup_dir);
    printf("Container %s removed.\n", pid_str);
    return 0;
}

// --- The 'main' function dispatcher with all commands ---
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
        return do_status(argc - 1, &argv[1]);
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