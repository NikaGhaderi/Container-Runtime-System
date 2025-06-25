// File: container_final_overlayfs_fixed.c
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

// --- A struct to pass arguments to the container ---
struct container_args {
    char* merged_path;
    char** argv;
};

// The container's main function
int container_main(void *arg) {
    struct container_args* args = (struct container_args*)arg;

    if (chroot(args->merged_path) != 0) { perror("chroot failed"); return 1; }
    if (chdir("/") != 0) { perror("chdir failed"); return 1; }
    
    // Mount procfs
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount proc failed");
        return 1;
    }

    // Execute the user's command
    execv(args->argv[0], args->argv);

    // This only runs if execv fails
    perror("execv failed");
    return 1;
}

// The 'run' command logic with OverlayFS
int do_run(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s run <image_name> <command> [args...]\n", argv[0]);
        return 1;
    }
    char* image_name = argv[1];
    char** container_cmd_argv = &argv[2];

    // --- OverlayFS Directory Setup ---
    char lowerdir[PATH_MAX], upperdir[PATH_MAX], workdir[PATH_MAX], merged[PATH_MAX];
    srand(time(NULL)); // Seed the random number generator
    int random_id = rand() % 10000;
    
    snprintf(lowerdir, sizeof(lowerdir), "%s", image_name);
    snprintf(upperdir, sizeof(upperdir), "overlay_layers/%d/upper", random_id);
    snprintf(workdir, sizeof(workdir), "overlay_layers/%d/work", random_id);
    snprintf(merged, sizeof(merged), "overlay_layers/%d/merged", random_id);

    char command[PATH_MAX * 2];
    sprintf(command, "mkdir -p %s %s %s", upperdir, workdir, merged);
    if (system(command) != 0) { return 1; }
    printf("[PARENT] Created overlay directories for container %d\n", random_id);
    
    // --- Mount OverlayFS ---
    char mount_opts[PATH_MAX * 3];
    snprintf(mount_opts, sizeof(mount_opts), "lowerdir=%s,upperdir=%s,workdir=%s", lowerdir, upperdir, workdir);
    if (mount("overlay", merged, "overlay", 0, mount_opts) != 0) {
        perror("Overlay mount failed");
        return 1;
    }
    printf("[PARENT] Mounted OverlayFS at %s\n", merged);

    // --- Package arguments and clone ---
    struct container_args args;
    args.merged_path = merged;
    args.argv = container_cmd_argv;

    char *container_stack = malloc(STACK_SIZE);
    char *stack_top = container_stack + STACK_SIZE;
    pid_t container_pid = clone(container_main, stack_top, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, &args);

    if (container_pid == -1) { perror("clone"); return 1; }

    printf("Container started with PID %d. Press Ctrl+C to stop.\n", container_pid);
    waitpid(container_pid, NULL, 0);
    printf("Container %d terminated.\n", container_pid);

    // --- The Corrected Cleanup Logic ---
    printf("[PARENT] Cleaning up mounts and directories...\n");
    
    // 1. Unmount the inner procfs first
    char proc_path[PATH_MAX];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", merged);
    if (umount(proc_path) != 0) {
        perror("Failed to unmount container's /proc");
    }

    // 2. Unmount the main overlayfs
    if (umount(merged) != 0) {
        perror("Failed to unmount merged overlay");
    }

    // 3. Remove the temporary overlay directories
    sprintf(command, "rm -rf overlay_layers/%d", random_id);
    system(command);
    
    return 0;
}

// A simplified main that only knows "run" for this test
int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "run") == 0) {
        return do_run(argc - 1, &argv[1]);
    } else {
        fprintf(stderr, "Usage: %s run <image_name> <command> [args...]\n", argv[0]);
        return 1;
    }
    return 0;
}