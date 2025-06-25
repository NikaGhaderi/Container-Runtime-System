// File: container_with_readonly.c
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
#include <sys/syscall.h> // Needed for the raw syscall()

#define STACK_SIZE (1024 * 1024)
#define MY_RUNTIME_CGROUP "/sys/fs/cgroup/my_runtime"
#define MY_RUNTIME_STATE "/run/my_runtime"

// --- Helper functions from our stable version ---
void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f == NULL) { perror("fopen"); return; }
    fprintf(f, "%s", content);
    fclose(f);
}
void setup_cgroup_hierarchy() {
    mkdir(MY_RUNTIME_CGROUP, 0755);
    mkdir(MY_RUNTIME_STATE, 0755);
    system("echo \"+cpu +memory +pids\" > /sys/fs/cgroup/my_runtime/cgroup.subtree_control 2>/dev/null || true");
}

// --- NEW: A struct to pass multiple arguments to container_main ---
struct container_args {
    char **argv;      // The command to run, e.g. {"/bin/bash", NULL}
    char *rootfs;     // The path to the container's root filesystem
    int readonly;     // A flag (1 if readonly, 0 otherwise)
};

// The corrected container_main function
int container_main(void *arg) {
    struct container_args *args = (struct container_args*)arg;

    sethostname("container", 9);
    if (chroot(args->rootfs) != 0) { perror("chroot failed"); return 1; }
    if (chdir("/") != 0) { perror("chdir failed"); return 1; }
    
    if (args->readonly) {
        // Make all mounts private to this namespace first.
        // This prevents our read-only change from affecting the host.
        if (syscall(SYS_mount, "none", "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
            perror("mount private failed");
        }
        
        // --- THE FIX IS HERE: We removed MS_BIND ---
        // Also changed "none" to NULL, which is more standard for remounts.
        if (syscall(SYS_mount, NULL, "/", NULL, MS_RDONLY | MS_REMOUNT, NULL) != 0) {
            perror("remount readonly failed");
        }

        // Mount a new, writable tmpfs on /tmp
        if (mount("tmpfs", "/tmp", "tmpfs", 0, NULL) != 0) {
            perror("mount tmpfs failed");
        }
    }

    // Mount the new /proc for the container's PID namespace
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) { perror("mount proc failed"); }
    
    execv(args->argv[0], args->argv);
    perror("execv failed");
    return 1;
}

// --- MODIFIED: do_run now handles the --readonly flag ---
int do_run(int argc, char *argv[]) {
    setup_cgroup_hierarchy();
    
    char *mem_limit = NULL; char *cpu_quota = NULL; int pin_cpu_flag = 0;
    int readonly_flag = 0; // The flag for our new option

    static struct option long_options[] = {
        {"pin-cpu",  no_argument, NULL, 'p'},
        {"readonly", no_argument, NULL, 'r'}, // New --readonly option returns 'r'
        {0, 0, 0, 0}
    };
    int opt;
    // Add 'r' to the getopt string
    while ((opt = getopt_long(argc, argv, "+m:C:pr", long_options, NULL)) != -1) {
        switch (opt) {
            case 'm': mem_limit = optarg; break;
            case 'C': cpu_quota = optarg; break;
            case 'p': pin_cpu_flag = 1; break;
            case 'r': readonly_flag = 1; break; // Set our flag when --readonly is seen
            default: fprintf(stderr, "Usage: %s run ...\n", argv[0]); return 1;
        }
    }
    if (optind + 1 >= argc) { fprintf(stderr, "Usage: %s run <rootfs> <cmd>...\n", argv[0]); return 1; }
    
    // Package arguments into our new struct
    struct container_args args;
    args.rootfs = argv[optind];
    args.argv = &argv[optind + 1]; // The command is everything after the rootfs path
    args.readonly = readonly_flag;

    char *container_stack = malloc(STACK_SIZE);
    char *stack_top = container_stack + STACK_SIZE;
    
    // Pass the address of our args struct to clone
    pid_t container_pid = clone(container_main, stack_top, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS, &args);

    if (container_pid == -1) { perror("clone"); return 1; }

    // This version only supports foreground containers. The parent always waits.
    printf("Container started with PID %d. Press Ctrl+C to stop.\n", container_pid);
    waitpid(container_pid, NULL, 0);
    printf("Container %d terminated.\n", container_pid);
    
    return 0;
}

// --- All other functions (list, status, main, etc.) are NOT needed for this test ---
// We are simplifying to test one feature at a time.
int main(int argc, char *argv[]) {
    // A simplified main that only knows "run"
    if (argc > 1 && strcmp(argv[1], "run") == 0) {
        // Pass argc-1 and argv+1 to hide the "run" command itself
        return do_run(argc - 1, &argv[1]);
    } else {
        fprintf(stderr, "Usage: %s run [options] <rootfs> <cmd>...\n", argv[0]);
        return 1;
    }
    return 0;
}