// This is the correct, stable code you provided.
// File: container_stable_cli.c
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

// All functions (do_freeze, do_thaw, setup_cgroup_hierarchy, write_file, 
// container_main, do_run, find_cgroup_value, do_list, do_status, main)
// are exactly as you posted in the last message. That code is our correct baseline.
// I will include the full code here just for absolute clarity and to avoid any confusion.

void write_file(const char *path, const char *content) {
   FILE *f = fopen(path, "w");
   if (f == NULL) { perror("fopen"); return; }
   fprintf(f, "%s", content);
   fclose(f);
}

int do_freeze(int argc, char *argv[]) {
   if (argc < 2) { fprintf(stderr, "Usage: %s freeze <container_pid>\n", argv[0]); return 1; }
   char *pid_str = argv[1];
   char freeze_path[PATH_MAX];
   snprintf(freeze_path, sizeof(freeze_path), "%s/container_%s/cgroup.freeze", MY_RUNTIME_CGROUP, pid_str);
   write_file(freeze_path, "1");
   printf("Froze container %s.\n", pid_str);
   return 0;
}

int do_thaw(int argc, char *argv[]) {
   if (argc < 2) { fprintf(stderr, "Usage: %s thaw <container_pid>\n", argv[0]); return 1; }
   char *pid_str = argv[1];
   char freeze_path[PATH_MAX];
   snprintf(freeze_path, sizeof(freeze_path), "%s/container_%s/cgroup.freeze", MY_RUNTIME_CGROUP, pid_str);
   write_file(freeze_path, "0");
   printf("Thawed container %s.\n", pid_str);
   return 0;
}

void setup_cgroup_hierarchy() {
   mkdir(MY_RUNTIME_CGROUP, 0755);
   mkdir(MY_RUNTIME_STATE, 0755);
   system("echo \"+cpu +memory +pids\" > /sys/fs/cgroup/my_runtime/cgroup.subtree_control 2>/dev/null || true");
}

int container_main(void *arg) {
   sethostname("container", 9);
   char *rootfs = ((char **)arg)[0];
   if (chroot(rootfs) != 0) { perror("chroot failed"); return 1; }
   if (chdir("/") != 0) { perror("chdir failed"); return 1; }
   if (mount("proc", "/proc", "proc", 0, NULL) != 0) { perror("mount proc failed"); return 1; }
   char **argv = &(((char **)arg)[1]);
   execv(argv[0], argv);
   perror("execv failed");
   return 1;
}

int do_run(int argc, char *argv[]) {
   setup_cgroup_hierarchy();
   char *mem_limit = NULL; char *cpu_quota = NULL; int pin_cpu_flag = 0; int detach_flag = 0;
   static struct option long_options[] = {
           {"pin-cpu", no_argument, NULL, 'p'}, {"detach",  no_argument, NULL, 'd'}, {0, 0, 0, 0}
   };
   int opt;
   while ((opt = getopt_long(argc, argv, "+m:C:pd", long_options, NULL)) != -1) {
       switch (opt) {
           case 'm': mem_limit = optarg; break; case 'C': cpu_quota = optarg; break;
           case 'p': pin_cpu_flag = 1; break; case 'd': detach_flag = 1; break;
           default: fprintf(stderr, "Usage: %s run [--detach] ...\n", argv[0]); return 1;
       }
   }
   if (optind + 1 >= argc) { fprintf(stderr, "Usage: %s run [--detach] ...\n", argv[0]); return 1; }

   if (detach_flag) {
       if (fork() != 0) { exit(0); }
       setsid();
       freopen("/dev/null", "r", stdin); freopen("/dev/null", "w", stdout); freopen("/dev/null", "w", stderr);
   }

   char **container_argv = &argv[optind];
   char *container_stack = malloc(STACK_SIZE);
   char *stack_top = container_stack + STACK_SIZE;
   int clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS;
   pid_t container_pid = clone(container_main, stack_top, clone_flags, container_argv);

   if (container_pid == -1) { perror("clone"); return 1; }

   char state_dir[PATH_MAX]; snprintf(state_dir, sizeof(state_dir), "%s/%d", MY_RUNTIME_STATE, container_pid); mkdir(state_dir, 0755);
   char cgroup_path[PATH_MAX]; snprintf(cgroup_path, sizeof(cgroup_path), "%s/container_%d", MY_RUNTIME_CGROUP, container_pid); mkdir(cgroup_path, 0755);
   char cmd_path[PATH_MAX]; snprintf(cmd_path, sizeof(cmd_path), "%s/command", state_dir);
   FILE *cmd_file = fopen(cmd_path, "w");
   if (cmd_file) {
       for (int i = 0; container_argv[i] != NULL; i++) { fprintf(cmd_file, "%s ", container_argv[i]); }
       fclose(cmd_file);
   }
   char procs_path[PATH_MAX]; char pid_str[16];
   snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path);
   snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
   write_file(procs_path, pid_str);

   if (detach_flag) { return 0; }

   printf("Container started with PID %d. Press Ctrl+C to stop.\n", container_pid);
   waitpid(container_pid, NULL, 0);

   char cmd_to_remove[PATH_MAX]; snprintf(cmd_to_remove, sizeof(cmd_to_remove), "%s/command", state_dir); remove(cmd_to_remove);
   rmdir(state_dir); rmdir(cgroup_path);
   return 0;
}

long find_cgroup_value(const char* path, const char* key) {
   FILE* f = fopen(path, "r");
   if (!f) return -1;
   char line_buf[256];
   long value = -1;
   while (fgets(line_buf, sizeof(line_buf), f) != NULL) {
       char key_buf[128]; long val_buf;
       if (sscanf(line_buf, "%s %ld", key_buf, &val_buf) == 2) {
           if (strcmp(key_buf, key) == 0) { value = val_buf; break; }
       }
   }
   fclose(f);
   return value;
}

int do_list(int argc, char *argv[]) {
   DIR *d = opendir(MY_RUNTIME_STATE);
   if (d == NULL) {
       if (errno == ENOENT) { printf("No running containers.\n"); return 0; }
       perror("opendir"); return 1;
   }
   printf("%-15s\t%s\n", "CONTAINER PID", "COMMAND");
   struct dirent *dir_entry;
   while ((dir_entry = readdir(d)) != NULL) {
       if (dir_entry->d_type != DT_DIR || strcmp(dir_entry->d_name, ".") == 0 || strcmp(dir_entry->d_name, "..") == 0) continue;
       char proc_path[PATH_MAX]; snprintf(proc_path, sizeof(proc_path), "/proc/%s", dir_entry->d_name);
       if (access(proc_path, F_OK) == 0) {
           char cmd_path[PATH_MAX], cmd_buf[1024] = {0};
           snprintf(cmd_path, sizeof(cmd_path), "%s/%s/command", MY_RUNTIME_STATE, dir_entry->d_name);
           FILE *cmd_file = fopen(cmd_path, "r");
           if (cmd_file) {
               fgets(cmd_buf, sizeof(cmd_buf) - 1, cmd_file);
               cmd_buf[strcspn(cmd_buf, "\n")] = 0;
               fclose(cmd_file);
           }
           printf("%-15s\t%s\n", dir_entry->d_name, cmd_buf);
       } else {
           printf("Reaping stale container %s...\n", dir_entry->d_name);
           char state_dir[PATH_MAX]; snprintf(state_dir, sizeof(state_dir), "%s/%s", MY_RUNTIME_STATE, dir_entry->d_name);
           char cgroup_dir[PATH_MAX]; snprintf(cgroup_dir, sizeof(cgroup_dir), "%s/container_%s", MY_RUNTIME_CGROUP, dir_entry->d_name);
           char cmd_to_remove[PATH_MAX]; snprintf(cmd_to_remove, sizeof(cmd_to_remove), "%s/command", state_dir); remove(cmd_to_remove);
           rmdir(state_dir); rmdir(cgroup_dir);
       }
   }
   closedir(d);
   return 0;
}

int do_status(int argc, char *argv[]) {
   if (argc < 2) {
       fprintf(stderr, "Usage: %s status <container_pid>\n", argv[0]);
       return 1;
   }
   char *pid_str = argv[1];
   char path_buffer[PATH_MAX];

   printf("--- Status for Container PID %s ---\n", pid_str);
   char cgroup_path[PATH_MAX];
   snprintf(cgroup_path, sizeof(cgroup_path), "%s/container_%s", MY_RUNTIME_CGROUP, pid_str);
   snprintf(path_buffer, sizeof(path_buffer), "%s/cpu.stat", cgroup_path);
   long cpu_micros = find_cgroup_value(path_buffer, "usage_usec");

   if (cpu_micros >= 0) {
       printf("%-20s: %.2f seconds\n", "Total CPU Time", (double)cpu_micros / 1000000.0);
   } else {
       printf("Could not read CPU stats for container.\n");
   }
   return 0;
}

int main(int argc, char *argv[]) {
   if (argc < 2) {
       fprintf(stderr, "Usage: %s <command> [args...]\nCommands: run, list, status, freeze, thaw\n", argv[0]);
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
   } else {
       fprintf(stderr, "Unknown command: %s\n", argv[1]);
       return 1;
   }
   return 0;
}