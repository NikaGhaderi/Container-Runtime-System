// File: container_final_overlayfs.c
// All includes and helper functions are the same.
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

// All helper functions (write_file, setup_cgroup_hierarchy, etc.) and
// other `do_` functions (list, status, etc.) are the same as our stable version.
void setup_cgroup_hierarchy() { /* ... same ... */ }
void write_file(const char *path, const char *content) { /* ... same ... */ }
int do_list(int argc, char *argv[]) { /* ... same ... */ }
// ... and so on

// The container_main is simplified, as the parent now prepares the rootfs.
int container_main(void *arg) {
    char *rootfs = (char *)arg; // The 'merged' directory path

    sethostname("container", 9);
    if (chroot(rootfs) != 0) { perror("chroot failed"); return 1; }
    if (chdir("/") != 0) { perror("chdir failed"); return 1; }
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) { perror("mount proc failed"); }
    
    // The command is now hardcoded as bash for this example.
    // A more advanced version would pass this in.
    char *const argv[] = {"/bin/bash", NULL};
    execv(argv[0], argv);

    perror("execv failed");
    return 1;
}

// --- The 'run' command logic with OverlayFS ---
int do_run(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s run <image_name>\n", argv[0]);
        return 1;
    }
    char* image_name = argv[1];

    setup_cgroup_hierarchy();

    // --- OverlayFS Directory Setup ---
    char lowerdir[PATH_MAX], upperdir[PATH_MAX], workdir[PATH_MAX], merged[PATH_MAX];
    
    // Create a unique name for this container's layers
    // For simplicity, we'll use a random number. A real runtime would use a UUID.
    int random_id = rand() % 10000;
    
    snprintf(lowerdir, sizeof(lowerdir), "%s", image_name); // The base image
    snprintf(upperdir, sizeof(upperdir), "overlay_layers/%d/upper", random_id);
    snprintf(workdir, sizeof(workdir), "overlay_layers/%d/work", random_id);
    snprintf(merged, sizeof(merged), "overlay_layers/%d/merged", random_id);

    // Create the required directories
    char command[PATH_MAX * 2];
    sprintf(command, "mkdir -p %s %s %s", upperdir, workdir, merged);
    if (system(command) != 0) {
        fprintf(stderr, "Failed to create overlay directories\n");
        return 1;
    }
    printf("[PARENT] Created overlay directories for container %d\n", random_id);
    
    // --- Mount OverlayFS ---
    char mount_opts[PATH_MAX * 3];
    snprintf(mount_opts, sizeof(mount_opts), "lowerdir=%s,upperdir=%s,workdir=%s", lowerdir, upperdir, workdir);
    
    if (mount("overlay", merged, "overlay", 0, mount_opts) != 0) {
        perror("Overlay mount failed");
        return 1;
    }
    printf("[PARENT] Mounted OverlayFS at %s\n", merged);

    // --- Clone the container process ---
    // The rest of the logic is simplified as we are not using other flags for this example.
    char *container_stack = malloc(STACK_SIZE);
    char *stack_top = container_stack + STACK_SIZE;
    
    // We pass the path to the 'merged' directory to the container
    pid_t container_pid = clone(container_main, stack_top, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS, merged);

    if (container_pid == -1) { perror("clone"); return 1; }

    printf("Container started with PID %d. Press Ctrl+C to stop.\n", container_pid);
    waitpid(container_pid, NULL, 0);
    printf("Container %d terminated.\n", container_pid);

    // --- Cleanup ---
    printf("[PARENT] Cleaning up mounts and directories...\n");
    umount(merged);
    sprintf(command, "rm -rf overlay_layers/%d", random_id);
    system(command);
    
    return 0;
}


// A simplified main that only knows "run" for this test
int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "run") == 0) {
        return do_run(argc - 1, &argv[1]);
    } else {
        fprintf(stderr, "Usage: %s run <image_name>\n", argv[0]);
        return 1;
    }
    return 0;
}