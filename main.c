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
#include <fcntl.h>


#define STACK_SIZE (1024 * 1024)
#define MY_RUNTIME_CGROUP "/sys/fs/cgroup/my_runtime"
#define MY_RUNTIME_STATE "/run/my_runtime"
#define NEXT_CPU_FILE "/tmp/my_runtime_next_cpu"

// --- Helper Functions ---
void read_file_string(const char *path, char *buf, size_t size) {
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(buf, size, f) != NULL) {
            buf[strcspn(buf, "\n")] = 0; // Remove trailing newline
        } else {
            buf[0] = '\0'; // Clear buffer on read error
        }
        fclose(f);
    } else {
        buf[0] = '\0'; // Clear buffer if file doesn't exist
    }
}

void cleanup_mounts(int pid) {
    char state_dir[PATH_MAX];
    snprintf(state_dir, sizeof(state_dir), "%s/%d", MY_RUNTIME_STATE, pid);
    char overlay_id_path[PATH_MAX];
    snprintf(overlay_id_path, sizeof(overlay_id_path), "%s/overlay_id", state_dir);
    int random_id = -1;
    FILE* id_file = fopen(overlay_id_path, "r");
    if (id_file) {
        fscanf(id_file, "%d", &random_id);
        fclose(id_file);
    }
    if (random_id != -1) {
        char merged[PATH_MAX];
        snprintf(merged, sizeof(merged), "overlay_layers/%d/merged", random_id);
        char proc_to_unmount[PATH_MAX];
        snprintf(proc_to_unmount, sizeof(proc_to_unmount), "%s/proc", merged);

        // Unmount /proc with lazy detach
        if (umount2(proc_to_unmount, MNT_DETACH) != 0) {
            if (errno != ENOENT && errno != EINVAL) {
                perror("umount2 proc failed");
            }
        }

        // ADDED: Unmount the propagated mount if it exists
        char propagate_mount_path[PATH_MAX];
        snprintf(propagate_mount_path, sizeof(propagate_mount_path), "%s/propagate_mount_dir", state_dir);
        FILE* p_file = fopen(propagate_mount_path, "r");
        if (p_file) {
            char p_dir[PATH_MAX];
            if (fgets(p_dir, sizeof(p_dir), p_file)) {
                p_dir[strcspn(p_dir, "\n")] = 0; // Remove newline
                char container_mount_point[PATH_MAX];
                snprintf(container_mount_point, sizeof(container_mount_point), "%s%s", merged, p_dir);
                if (umount2(container_mount_point, MNT_DETACH) != 0) {
                    if (errno != ENOENT && errno != EINVAL) {
                        perror("umount2 propagated mount failed");
                    }
                }
            }
            fclose(p_file);
        }

        // Unmount overlay with lazy detach
        if (umount2(merged, MNT_DETACH) != 0) {
            if (errno != ENOENT && errno != EINVAL) {
                perror("umount2 overlay failed");
            }
        }
    }
}


void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "ERROR: Failed to open %s for writing: ", path);
        perror("");
        return;
    }
    fprintf(f, "%s", content);
    fclose(f);
}

void setup_cgroup_hierarchy() {
    mkdir(MY_RUNTIME_CGROUP, 0755);
    mkdir(MY_RUNTIME_STATE, 0755);
    system("echo \"+cpu +memory +pids +io\" > /sys/fs/cgroup/my_runtime/cgroup.subtree_control 2>/dev/null || true");
}

struct container_args {
    char* merged_path;
    char** argv;
    char* propagate_mount_dir;
    int sync_pipe_read_fd;
};

int container_main(void *arg) {
    struct container_args* args = (struct container_args*)arg;

    // Wait for parent to set up mappings
    char buf;
    if (read(args->sync_pipe_read_fd, &buf, 1) != 1) {
        perror("Failed to read from sync pipe");
        // Proceed anyway to avoid hanging, but log the error
    }
    close(args->sync_pipe_read_fd);


    // Bring up the loopback interface in the new network namespace.
    // This is necessary to have a functional, albeit isolated, network environment.
    if (system("ip link set lo up") != 0) {
        perror("Failed to set lo up");
    }

    if (args->propagate_mount_dir) {
        char container_mount_path[PATH_MAX];
        char command[PATH_MAX * 2];

        snprintf(container_mount_path, sizeof(container_mount_path), "%s%s", args->merged_path, args->propagate_mount_dir);

        snprintf(command, sizeof(command), "mkdir -p %s", container_mount_path);
        if (system(command) != 0) {
            perror("mkdir -p for propagated mount failed");
        }

        if (mount(args->propagate_mount_dir, container_mount_path, NULL, MS_BIND, NULL) != 0) {
            perror("bind mount for propagation failed");
        }
    }

    sethostname("container", 9);
    if (chroot(args->merged_path) != 0) { perror("chroot failed"); return 1; }
    if (chdir("/") != 0) { perror("chdir failed"); return 1; }
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) { perror("mount proc failed"); }

    execv(args->argv[0], args->argv);
    perror("execv failed");
    return 1;
}

long read_cgroup_long(const char *path) {
    long value = -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (fscanf(f, "%ld", &value) != 1) { value = -1; }
    fclose(f);
    return value;
}

void format_bytes(long bytes, char *buf, size_t size) {
    const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    double d_bytes = bytes;
    if (bytes < 0) {
        snprintf(buf, size, "N/A");
        return;
    }
    while (d_bytes >= 1024 && i < 4) {
        d_bytes /= 1024;
        i++;
    }
    snprintf(buf, size, "%.2f %s", d_bytes, suffixes[i]);
}

long find_cgroup_value(const char* path, const char* key) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line_buf[256];
    long value = -1;
    while (fgets(line_buf, sizeof(line_buf), f) != NULL) {
        char key_buf[128];
        long val_buf;
        if (sscanf(line_buf, "%s %ld", key_buf, &val_buf) == 2) {
            if (strcmp(key_buf, key) == 0) {
                value = val_buf;
                break;
            }
        }
    }
    fclose(f);
    return value;
}


// --- CLI Command Functions ---

int do_run(int argc, char *argv[]) {
    setup_cgroup_hierarchy();
    char *mem_limit = NULL;
    char *cpu_quota = NULL;
    char *io_read_bps = NULL;
    char *io_write_bps = NULL;
    char *propagate_mount_dir = NULL;
    int pin_cpu_flag = 0;
    int detach_flag = 0;
    int share_ipc_flag = 0;
    char pid_str[16];

    static struct option long_options[] = {
            {"mem", required_argument, 0, 'm'},
            {"cpu", required_argument, 0, 'C'},
            {"io-read-bps", required_argument, 0, 'r'},
            {"io-write-bps", required_argument, 0, 'w'},
            {"pin-cpu", no_argument, NULL, 'p'},
            {"detach", no_argument, NULL, 'd'},
            {"share-ipc", no_argument, NULL, 'i'},
            {"propagate-mount", required_argument, 0, 'M'},
            {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "+m:C:r:w:pdiM:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'm': mem_limit = optarg; break;
            case 'C': cpu_quota = optarg; break;
            case 'r': io_read_bps = optarg; break;
            case 'w': io_write_bps = optarg; break;
            case 'p': pin_cpu_flag = 1; break;
            case 'd': detach_flag = 1; break;
            case 'i': share_ipc_flag = 1; break;
            case 'M': propagate_mount_dir = optarg; break;
            default: return 1;
        }
    }
    if (optind + 1 >= argc) { fprintf(stderr, "Usage: %s run [opts] <image> <cmd>...\n", argv[0]); return 1; }
    char* image_name = argv[optind];
    char** container_cmd_argv = &argv[optind + 1];

    if (propagate_mount_dir) {
        if (mount(NULL, propagate_mount_dir, NULL, MS_REC | MS_SHARED, NULL) != 0) {
            perror("Failed to set mount propagation to SHARED");
            fprintf(stderr, "Hint: Make sure the directory '%s' exists and is a mount point.\n", propagate_mount_dir);
            return 1;
        }
    }

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


    // Create synchronization pipe
    int sync_pipe[2];
    if (pipe(sync_pipe) == -1) {
        perror("pipe");
        return 1;
    }

    struct container_args args;
    args.merged_path = merged;
    args.argv = container_cmd_argv;
    args.propagate_mount_dir = propagate_mount_dir;
    args.sync_pipe_read_fd = sync_pipe[0]; // Pass read end to child

    char *container_stack = malloc(STACK_SIZE);
    char *stack_top = container_stack + STACK_SIZE;

    int clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWUSER | CLONE_NEWNET | SIGCHLD;
    if (!share_ipc_flag) {
        clone_flags |= CLONE_NEWIPC;
    }
    pid_t container_pid = clone(container_main, stack_top, clone_flags, &args);
    if (container_pid == -1) {
        perror("clone");
        free(container_stack);
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        return 1;
    }

    // Close read end in parent
    close(sync_pipe[0]);

    // Set up user namespace mappings
    char path_buffer[PATH_MAX];
    uid_t host_uid = getuid();
    gid_t host_gid = getgid();

    snprintf(path_buffer, sizeof(path_buffer), "/proc/%d/setgroups", container_pid);
    write_file(path_buffer, "deny");

    snprintf(path_buffer, sizeof(path_buffer), "/proc/%d/gid_map", container_pid);
    char map_buffer[100];
    snprintf(map_buffer, sizeof(map_buffer), "0 %d 1", host_gid);
    write_file(path_buffer, map_buffer);

    snprintf(path_buffer, sizeof(path_buffer), "/proc/%d/uid_map", container_pid);
    snprintf(map_buffer, sizeof(map_buffer), "0 %d 1", host_uid);
    write_file(path_buffer, map_buffer);

    // Signal child that mappings are set
    if (write(sync_pipe[1], "1", 1) != 1) {
        perror("write to sync pipe");
    }
    close(sync_pipe[1]); // Close write end


    char state_dir[PATH_MAX]; snprintf(state_dir, sizeof(state_dir), "%s/%d", MY_RUNTIME_STATE, container_pid); mkdir(state_dir, 0755);
    char cgroup_path[PATH_MAX]; snprintf(cgroup_path, sizeof(cgroup_path), "%s/container_%d", MY_RUNTIME_CGROUP, container_pid);
    if(mkdir(cgroup_path, 0755) != 0 && errno != EEXIST) { perror("Failed to create container cgroup"); }

    snprintf(path_buffer, sizeof(path_buffer), "%s/command", state_dir);
    FILE *cmd_file = fopen(path_buffer, "w");
    if (cmd_file) {
        for (int i = 0; container_cmd_argv[i] != NULL; i++) { fprintf(cmd_file, "%s ", container_cmd_argv[i]); }
        fclose(cmd_file);
    }

    snprintf(path_buffer, sizeof(path_buffer), "%s/image_name", state_dir);
    write_file(path_buffer, image_name);

    char random_id_str[16];
    snprintf(random_id_str, sizeof(random_id_str), "%d", random_id);
    snprintf(path_buffer, sizeof(path_buffer), "%s/overlay_id", state_dir);
    write_file(path_buffer, random_id_str);

    if (detach_flag) {
        snprintf(path_buffer, sizeof(path_buffer), "%s/detach", state_dir);
        write_file(path_buffer, "1");
    }

    if (share_ipc_flag) {
        snprintf(path_buffer, sizeof(path_buffer), "%s/share_ipc", state_dir);
        write_file(path_buffer, "1");
    }

    if (propagate_mount_dir) {
        snprintf(path_buffer, sizeof(path_buffer), "%s/propagate_mount_dir", state_dir);
        write_file(path_buffer, propagate_mount_dir);
    }

    if (pin_cpu_flag) {
        snprintf(path_buffer, sizeof(path_buffer), "%s/pin_cpu", state_dir);
        write_file(path_buffer, "1");
        FILE *f = fopen(NEXT_CPU_FILE, "r+");
        int next_cpu = 0;
        if (f) { fscanf(f, "%d", &next_cpu); }
        else { f = fopen(NEXT_CPU_FILE, "w"); }
        long num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
        int target_cpu = next_cpu % num_cpus;
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(target_cpu, &cpuset);
        if (sched_setaffinity(container_pid, sizeof(cpu_set_t), &cpuset) != 0) {
            perror("sched_setaffinity failed");
        }
        struct sched_param param = { .sched_priority = 50 };
        if (sched_setscheduler(container_pid, SCHED_RR, &param) != 0) {
            perror("sched_setscheduler failed");
        }
        fseek(f, 0, SEEK_SET);
        fprintf(f, "%d", (next_cpu + 1) % num_cpus);
        fclose(f);
    }
    if (mem_limit) {
        snprintf(path_buffer, sizeof(path_buffer), "%s/mem_limit", state_dir);
        write_file(path_buffer, mem_limit);
        snprintf(path_buffer, sizeof(path_buffer), "%s/memory.max", cgroup_path);
        write_file(path_buffer, mem_limit);
        snprintf(path_buffer, sizeof(path_buffer), "%s/memory.swap.max", cgroup_path);
        write_file(path_buffer, "0");
    }
    if (cpu_quota) {
        snprintf(path_buffer, sizeof(path_buffer), "%s/cpu_quota", state_dir);
        write_file(path_buffer, cpu_quota);
        char cpu_content[64];
        snprintf(path_buffer, sizeof(path_buffer), "%s/cpu.max", cgroup_path);
        snprintf(cpu_content, sizeof(cpu_content), "%s 100000", cpu_quota);
        write_file(path_buffer, cpu_content);
    }
    if (io_read_bps || io_write_bps) {
        snprintf(path_buffer, sizeof(path_buffer), "%s/io_read_bps", state_dir);
        write_file(path_buffer, io_read_bps ? io_read_bps : "max");
        snprintf(path_buffer, sizeof(path_buffer), "%s/io_write_bps", state_dir);
        write_file(path_buffer, io_write_bps ? io_write_bps : "max");
        char io_content[128];
        snprintf(io_content, sizeof(io_content), "8:0 rbps=%s wbps=%s",
                 io_read_bps ? io_read_bps : "max", io_write_bps ? io_write_bps : "max");
        snprintf(path_buffer, sizeof(path_buffer), "%s/io.max", cgroup_path);
        write_file(path_buffer, io_content);
    }

    char procs_path[PATH_MAX];
    snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path);
    snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
    write_file(procs_path, pid_str);

    if (detach_flag) {
        printf("Container started with PID %d\n", container_pid);
        return 0;
    }

    printf("Container started with PID %d. Press Ctrl+C to stop.\n", container_pid);
    waitpid(container_pid, NULL, 0);
    printf("Container %d has exited. Use 'rm' to clean up.\n", container_pid);
    return 0;
}


int do_list(int argc, char *argv[]) {
    DIR *d = opendir(MY_RUNTIME_STATE);
    if (d == NULL) {
        if (errno == ENOENT) {
            printf("No containers exist.\n");
            return 0;
        }
        perror("opendir");
        return 1;
    }

    struct dirent *dir_entry;
    int found = 0;

    while ((dir_entry = readdir(d)) != NULL) {
        if (dir_entry->d_type != DT_DIR || strcmp(dir_entry->d_name, ".") == 0 || strcmp(dir_entry->d_name, "..") == 0)
            continue;

        if (!found) {
            // Print header only once, and only if we find at least one container
            printf("%-15s\t%-10s\t%s\n", "CONTAINER PID", "STATUS", "COMMAND");
            found = 1;
        }

        char proc_path[PATH_MAX];
        snprintf(proc_path, sizeof(proc_path), "/proc/%s", dir_entry->d_name);
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

    if (!found) {
        printf("No containers exist.\n");
    }

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

    // ADDED: Display propagated mount in status
    snprintf(path_buffer, sizeof(path_buffer), "%s/propagate_mount_dir", state_dir);
    f = fopen(path_buffer, "r");
    if (f) {
        char p_dir[PATH_MAX] = {0};
        fgets(p_dir, sizeof(p_dir)-1, f);
        p_dir[strcspn(p_dir, "\n")] = 0;
        printf("%-25s: %s\n", "Propagated Mount", p_dir);
        fclose(f);
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

// // REPLACE your old do_status function with this new, more detailed version.
//int do_status(int argc, char *argv[]) {
//    if (argc < 2) {
//        fprintf(stderr, "Usage: %s status <container_pid>\n", argv[0]);
//        return 1;
//    }
//    char *pid_str = argv[1];
//    char path_buffer[PATH_MAX], format_buffer[64], content_buffer[1024];
//
//    char state_dir[PATH_MAX];
//    snprintf(state_dir, sizeof(state_dir), "%s/%s", MY_RUNTIME_STATE, pid_str);
//    if (access(state_dir, F_OK) != 0) {
//        fprintf(stderr, "Error: No container with PID %s found.\n", pid_str);
//        return 1;
//    }
//
//    printf("--- Status for Container PID %s ---\n", pid_str);
//
//    // --- State ---
//    char proc_path[PATH_MAX];
//    snprintf(proc_path, sizeof(proc_path), "/proc/%s", pid_str);
//    const char* status_str = "Stopped";
//    if (access(proc_path, F_OK) == 0) {
//        status_str = "Running";
//        char freeze_path[PATH_MAX];
//        snprintf(freeze_path, sizeof(freeze_path), "%s/container_%s/cgroup.freeze", MY_RUNTIME_CGROUP, pid_str);
//        if (read_cgroup_long(freeze_path) == 1) {
//            status_str = "Frozen";
//        }
//    }
//    printf("%-20s: %s\n", "State", status_str);
//
//    // --- Details ---
//    snprintf(path_buffer, sizeof(path_buffer), "%s/image_name", state_dir);
//    read_file_string(path_buffer, content_buffer, sizeof(content_buffer));
//    printf("%-20s: %s\n", "Image", content_buffer);
//
//    snprintf(path_buffer, sizeof(path_buffer), "%s/command", state_dir);
//    read_file_string(path_buffer, content_buffer, sizeof(content_buffer));
//    printf("%-20s: %s\n", "Command", content_buffer);
//
//    printf("\n--- Resources ---\n");
//    char cgroup_path[PATH_MAX];
//    snprintf(cgroup_path, sizeof(cgroup_path), "%s/container_%s", MY_RUNTIME_CGROUP, pid_str);
//
//    // Memory
//    snprintf(path_buffer, sizeof(path_buffer), "%s/memory.current", cgroup_path);
//    long mem_current = read_cgroup_long(path_buffer);
//    format_bytes(mem_current, format_buffer, sizeof(format_buffer));
//    snprintf(path_buffer, sizeof(path_buffer), "%s/mem_limit", state_dir);
//    read_file_string(path_buffer, content_buffer, sizeof(content_buffer));
//    printf("%-20s: %s / %s\n", "Memory Usage", format_buffer, strlen(content_buffer) > 0 ? content_buffer : "No Limit");
//
//    // CPU
//    snprintf(path_buffer, sizeof(path_buffer), "%s/cpu.stat", cgroup_path);
//    long cpu_micros = find_cgroup_value(path_buffer, "usage_usec");
//    snprintf(path_buffer, sizeof(path_buffer), "%s/cpu_quota", state_dir);
//    read_file_string(path_buffer, content_buffer, sizeof(content_buffer));
//    printf("%-20s: %.2f s (Quota: %s)\n", "Total CPU Time", (double)cpu_micros / 1000000.0, strlen(content_buffer) > 0 ? content_buffer : "No Limit");
//
//    // PIDs
//    snprintf(path_buffer, sizeof(path_buffer), "%s/pids.current", cgroup_path);
//    long pids_current = read_cgroup_long(path_buffer);
//    printf("%-20s: %ld\n", "Active Processes", pids_current);
//
//    // I/O
//    long rbytes = -1, wbytes = -1;
//    snprintf(path_buffer, sizeof(path_buffer), "%s/io.stat", cgroup_path);
//    FILE* io_stat_file = fopen(path_buffer, "r");
//    if (io_stat_file) {
//        char line[256];
//        while (fgets(line, sizeof(line), io_stat_file)) {
//            char *ptr;
//            if ((ptr = strstr(line, "rbytes="))) sscanf(ptr, "rbytes=%ld", &rbytes);
//            if ((ptr = strstr(line, "wbytes="))) sscanf(ptr, "wbytes=%ld", &wbytes);
//        }
//        fclose(io_stat_file);
//    }
//    format_bytes(rbytes, format_buffer, sizeof(format_buffer));
//    printf("%-20s: %s\n", "Bytes Read", format_buffer);
//    format_bytes(wbytes, format_buffer, sizeof(format_buffer));
//    printf("%-20s: %s\n", "Bytes Written", format_buffer);
//
//    printf("\n--- Configuration ---\n");
//    snprintf(path_buffer, sizeof(path_buffer), "%s/pin_cpu", state_dir);
//    printf("%-20s: %s\n", "CPU Pinning", access(path_buffer, F_OK) == 0 ? "Enabled" : "Disabled");
//
//    snprintf(path_buffer, sizeof(path_buffer), "%s/share_ipc", state_dir);
//    printf("%-20s: %s\n", "Shared IPC", access(path_buffer, F_OK) == 0 ? "Enabled" : "Disabled");
//
//    snprintf(path_buffer, sizeof(path_buffer), "%s/propagate_mount_dir", state_dir);
//    read_file_string(path_buffer, content_buffer, sizeof(content_buffer));
//    printf("%-20s: %s\n", "Propagated Mount", strlen(content_buffer) > 0 ? content_buffer : "Disabled");
//
//    printf("\n----------------------------------\n");
//    return 0;
//}

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
    if (argc < 2) {
        fprintf(stderr, "Usage: %s stop <container_pid>\n", argv[0]);
        return 1;
    }
    pid_t pid = atoi(argv[1]);
    printf("Stopping container %d...\n", pid);
    if (kill(pid, SIGKILL) != 0) {
        perror("kill failed");
    } else {
        // Wait for process to exit
        waitpid(pid, NULL, 0);
        // Clean up mounts after stopping
        cleanup_mounts(pid);
    }
    printf("Container %d stopped.\n", pid);
    return 0;
}


// REPLACE your old do_start function with this new, corrected version.
int do_start(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s start <stopped_container_pid>\n", argv[0]);
        return 1;
    }
    char *pid_str = argv[1];
    char path_buffer[PATH_MAX];

    char proc_path[PATH_MAX];
    snprintf(proc_path, sizeof(proc_path), "/proc/%s", pid_str);
    if (access(proc_path, F_OK) == 0) {
        fprintf(stderr, "Error: Container %s is already running.\n", pid_str);
        return 1;
    }

    char old_state_dir[PATH_MAX];
    snprintf(old_state_dir, sizeof(old_state_dir), "%s/%s", MY_RUNTIME_STATE, pid_str);
    if (access(old_state_dir, F_OK) != 0) {
        fprintf(stderr, "Error: No stopped container with ID %s found.\n", pid_str);
        return 1;
    }

    printf("Starting container %s...\n", pid_str);

    // --- Read all configuration from the old state directory ---
    char image_name[PATH_MAX] = {0};
    char overlay_id[16] = {0};
    char command_str[1024] = {0};
    char mem_limit[32] = {0};
    char cpu_quota[32] = {0};
    int pin_cpu_flag = 0;
    int share_ipc_flag = 0;
    int original_detach_flag = 0;
    char propagate_mount_dir[PATH_MAX] = {0}

    snprintf(path_buffer, sizeof(path_buffer), "%s/image_name", old_state_dir);
    read_file_string(path_buffer, image_name, sizeof(image_name));

    snprintf(path_buffer, sizeof(path_buffer), "%s/overlay_id", old_state_dir);
    read_file_string(path_buffer, overlay_id, sizeof(overlay_id));

    // Read the full command string, but don't modify it with strtok yet
    snprintf(path_buffer, sizeof(path_buffer), "%s/command", old_state_dir);
    read_file_string(path_buffer, command_str, sizeof(command_str));

    snprintf(path_buffer, sizeof(path_buffer), "%s/detach", old_state_dir);
    if (access(path_buffer, F_OK) == 0) {
        original_detach_flag = 1;
    }

    snprintf(path_buffer, sizeof(path_buffer), "%s/mem_limit", old_state_dir);
    read_file_string(path_buffer, mem_limit, sizeof(mem_limit));

    snprintf(path_buffer, sizeof(path_buffer), "%s/cpu_quota", old_state_dir);
    read_file_string(path_buffer, cpu_quota, sizeof(cpu_quota));

    snprintf(path_buffer, sizeof(path_buffer), "%s/pin_cpu", old_state_dir);
    if (access(path_buffer, F_OK) == 0) { pin_cpu_flag = 1; }

    snprintf(path_buffer, sizeof(path_buffer), "%s/share_ipc", old_state_dir);
    if (access(path_buffer, F_OK) == 0) { share_ipc_flag = 1; }

    if (strlen(image_name) == 0 || strlen(overlay_id) == 0 || strlen(command_str) == 0) {
        fprintf(stderr, "Error: Container configuration is corrupt or missing.\n");
        return 1;
    }

    // --- Re-mount the overlay filesystem ---
    char lowerdir[PATH_MAX], upperdir[PATH_MAX], workdir[PATH_MAX], merged[PATH_MAX];
    snprintf(lowerdir, sizeof(lowerdir), "%s", image_name);
    snprintf(upperdir, sizeof(upperdir), "overlay_layers/%s/upper", overlay_id);
    snprintf(workdir, sizeof(workdir), "overlay_layers/%s/work", overlay_id);
    snprintf(merged, sizeof(merged), "overlay_layers/%s/merged", overlay_id);

    char mount_opts[PATH_MAX * 3];
    snprintf(mount_opts, sizeof(mount_opts), "lowerdir=%s,upperdir=%s,workdir=%s", lowerdir, upperdir, workdir);
    if (mount("overlay", merged, "overlay", 0, mount_opts) != 0) {
        perror("Overlay mount failed on start");
        return 1;
    }

    // --- Create a new container process ---
    char *argv_for_container[64];

    // MODIFIED: Smart argument parsing
    char temp_command_str[1024];
    strcpy(temp_command_str, command_str); // Use a temporary copy for parsing

    if (strncmp(temp_command_str, "/bin/sh -c ", 11) == 0) {
        argv_for_container[0] = "/bin/sh";
        argv_for_container[1] = "-c";
        // The third argument is the entire rest of the string
        argv_for_container[2] = temp_command_str + 11;
        argv_for_container[3] = NULL;
    } else {
        // Fallback to the old logic for simple commands
        int i = 0;
        char *token = strtok(temp_command_str, " \n");
        while(token != NULL) {
            argv_for_container[i++] = token;
            token = strtok(NULL, " \n");
        }
        argv_for_container[i] = NULL;
    }
    // END MODIFICATION

    int sync_pipe[2];
    if (pipe(sync_pipe) == -1) {
        perror("pipe");
        return 1;
    }

    struct container_args args;
    args.merged_path = merged;
    args.argv = argv_for_container;
    args.propagate_mount_dir = NULL;
    args.sync_pipe_read_fd = sync_pipe[0];

    char *container_stack = malloc(STACK_SIZE);
    char *stack_top = container_stack + STACK_SIZE;
    int clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET | SIGCHLD;
    if (!share_ipc_flag) { // Fix the logic to match do_run
        clone_flags |= CLONE_NEWIPC;
    }
    pid_t new_pid = clone(container_main, stack_top, clone_flags, &args);
    if (new_pid == -1) {
        perror("clone failed on start");
        free(container_stack);
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        return 1;
    }

    close(sync_pipe[0]);

    // Set up user namespace and cgroups for the new PID
    uid_t host_uid;
    gid_t host_gid;
    char *sudo_uid_str = getenv("SUDO_UID");
    char *sudo_gid_str = getenv("SUDO_GID");

    if (sudo_uid_str && sudo_gid_str) {
        host_uid = atoi(sudo_uid_str);
        host_gid = atoi(sudo_gid_str);
    } else {
        host_uid = getuid();
        host_gid = getgid();
    }

    snprintf(path_buffer, sizeof(path_buffer), "/proc/%ld/setgroups", (long)new_pid);
    write_file(path_buffer, "deny");
    char map_buffer[100];
    snprintf(path_buffer, sizeof(path_buffer), "/proc/%ld/gid_map", (long)new_pid);
    snprintf(map_buffer, sizeof(map_buffer), "0 %d 1", host_gid);
    write_file(path_buffer, map_buffer);
    snprintf(path_buffer, sizeof(path_buffer), "/proc/%ld/uid_map", (long)new_pid);
    snprintf(map_buffer, sizeof(map_buffer), "0 %d 1", host_uid);
    write_file(path_buffer, map_buffer);

    // Signal child that mappings are set
    if (write(sync_pipe[1], "1", 1) != 1) {
        perror("write to sync pipe");
    }
    close(sync_pipe[1]); // Close write end

    // --- Rename state directory and re-apply cgroup settings ---
    char new_state_dir[PATH_MAX];
    snprintf(new_state_dir, sizeof(new_state_dir), "%s/%ld", MY_RUNTIME_STATE, (long)new_pid);
    rename(old_state_dir, new_state_dir);

    char cgroup_path[PATH_MAX];
    snprintf(cgroup_path, sizeof(cgroup_path), "%s/container_%ld", MY_RUNTIME_CGROUP, (long)new_pid);
    mkdir(cgroup_path, 0755);

    if (pin_cpu_flag) {
        FILE *f_cpu = fopen(NEXT_CPU_FILE, "r+");
        int next_cpu = 0;
        if (f_cpu) { fscanf(f_cpu, "%d", &next_cpu); }
        else { f_cpu = fopen(NEXT_CPU_FILE, "w"); }
        long num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
        int target_cpu = next_cpu % num_cpus;
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(target_cpu, &cpuset);
        if (sched_setaffinity(new_pid, sizeof(cpu_set_t), &cpuset) != 0) {
            perror("sched_setaffinity failed");
            return 1;
        }
        struct sched_param param = { .sched_priority = 50 };
        if (sched_setscheduler(new_pid, SCHED_RR, &param) != 0) {
            perror("sched_setscheduler failed");
            return 1;
        }
        fseek(f_cpu, 0, SEEK_SET);
        fprintf(f_cpu, "%d", (next_cpu + 1) % num_cpus);
        fclose(f_cpu);
    }
    if (strlen(mem_limit) > 0) {
        snprintf(path_buffer, sizeof(path_buffer), "%s/memory.max", cgroup_path);
        write_file(path_buffer, mem_limit);
    }
    if (strlen(cpu_quota) > 0) {
        char cpu_content[64];
        snprintf(path_buffer, sizeof(path_buffer), "%s/cpu.max", cgroup_path);
        snprintf(cpu_content, sizeof(cpu_content), "%s 100000", cpu_quota);
        write_file(path_buffer, cpu_content);
    }

    char procs_path[PATH_MAX];
    snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path);
    char new_pid_str[16];
    snprintf(new_pid_str, sizeof(new_pid_str), "%ld", (long)new_pid);
    write_file(procs_path, new_pid_str);

    if (original_detach_flag) {
        printf("Container %s started with new PID %ld\n", pid_str, (long)new_pid);
        return 0;
    } else {
        printf("Container %s started with new PID %ld. Press Ctrl+C to stop.\n", pid_str, (long)new_pid);
        waitpid(new_pid, NULL, 0);
        printf("Container %ld has exited. Use 'rm' to clean up.\n", (long)new_pid);
    }

    return 0;
}



int do_rm(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s rm <container_pid>\n", argv[0]);
        return 1;
    }
    char *pid_str = argv[1];
    char proc_path[PATH_MAX];
    snprintf(proc_path, sizeof(proc_path), "/proc/%s", pid_str);
    if (access(proc_path, F_OK) == 0) {
        fprintf(stderr, "Error: Cannot remove a running container. Use 'stop' first.\n");
        return 1;
    }
    printf("Removing container %s...\n", pid_str);
    cleanup_mounts(atoi(pid_str));
    char command[PATH_MAX];
    char state_dir[PATH_MAX];
    snprintf(state_dir, sizeof(state_dir), "%s/%s", MY_RUNTIME_STATE, pid_str);
    char overlay_id_path[PATH_MAX];
    snprintf(overlay_id_path, sizeof(overlay_id_path), "%s/overlay_id", state_dir);
    int random_id = -1;
    FILE* id_file = fopen(overlay_id_path, "r");
    if (id_file) {
        fscanf(id_file, "%d", &random_id);
        fclose(id_file);
    }
    if (random_id != -1) {
        snprintf(command, sizeof(command), "rm -rf overlay_layers/%d", random_id);
        system(command);
    }
    snprintf(command, sizeof(command), "rm -rf %s", state_dir);
    system(command);
    // Remove the cgroup directory
    char cgroup_dir[PATH_MAX];
    snprintf(cgroup_dir, sizeof(cgroup_dir), "%s/container_%s", MY_RUNTIME_CGROUP, pid_str);
    if (rmdir(cgroup_dir) != 0) {
        if (errno != ENOENT) {
            perror("Failed to remove cgroup directory");
        }
    }
    printf("Container %s removed.\n", pid_str);
    return 0;
}



int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args...]\nCommands: run, list, status, freeze, thaw, stop, start, rm\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "run") == 0) { return do_run(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "list") == 0) { return do_list(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "status") == 0) { return do_status(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "freeze") == 0) { return do_freeze(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "thaw") == 0) { return do_thaw(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "stop") == 0) { return do_stop(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "start") == 0) { return do_start(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "rm") == 0) { return do_rm(argc - 1, &argv[1]);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
    return 0;
}