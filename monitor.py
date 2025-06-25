#!/usr/bin/python3
from bcc import BPF
import ctypes as ct
import time

bpf_text = """
#include <linux/sched.h>
#include <uapi/linux/bpf.h>
#include <asm/unistd_64.h>

struct data_t {
    u32 pid;
    char comm[TASK_COMM_LEN];
    char syscall_name[32];
};

BPF_PERF_OUTPUT(events);

#define COPY_SYSCALL_NAME(name) __builtin_memcpy(&data.syscall_name, name, sizeof(name))

TRACEPOINT_PROBE(raw_syscalls, sys_enter) {
    u64 syscall_id = args->id;
    struct data_t data = {};

    if (syscall_id == __NR_clone) {
        char name[] = "clone";
        COPY_SYSCALL_NAME(name);
    } else if (syscall_id == __NR_unshare) {
        char name[] = "unshare";
        COPY_SYSCALL_NAME(name);
    } else if (syscall_id == __NR_mkdir) {
        char path[128];
        
        // --- THE FINAL FIX IS HERE ---
        // We use args->args[0] to get the first syscall argument, which is the path.
        bpf_probe_read_user_str(&path, sizeof(path), (const char *)args->args[0]);

        char cgroup_path[] = "/sys/fs/cgroup";
        for (int i = 0; i < sizeof(cgroup_path) - 1; ++i) {
            if (path[i] != cgroup_path[i]) {
                return 0;
            }
        }
        char name[] = "mkdir (cgroup)";
        COPY_SYSCALL_NAME(name);

    } else {
        return 0;
    }

    data.pid = bpf_get_current_pid_tgid() >> 32;
    bpf_get_current_comm(&data.comm, sizeof(data.comm));
    events.perf_submit(args, &data, sizeof(data));
    return 0;
}
"""

# --- The User-Space Python Program is unchanged ---
class Data(ct.Structure):
    _fields_ = [
        ("pid", ct.c_uint), ("comm", ct.c_char * 16), ("syscall_name", ct.c_char * 32),
    ]
def print_event(cpu, data, size):
    event = ct.cast(data, ct.POINTER(Data)).contents
    current_time = time.strftime("%Y-%m-%d %H:%M:%S")
    comm = event.comm.decode('utf-8', 'replace')
    syscall = event.syscall_name.decode('utf-8', 'replace')
    log_line = f"{current_time} | PID: {event.pid:<7} | COMM: {comm:<15} | SYSCALL: {syscall}\n"
    print(log_line, end="")
    with open("ebpf_log.txt", "a") as f:
        f.write(log_line)

# --- Main program execution is unchanged ---
print("Starting eBPF monitoring... Press Ctrl+C to exit.")
try:
    b = BPF(text=bpf_text)
    b["events"].open_perf_buffer(print_event)
    while True:
        try: b.perf_buffer_poll()
        except KeyboardInterrupt: exit()
except Exception as e:
    print(f"Error: {e}")