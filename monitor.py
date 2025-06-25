#!/usr/bin/python3
from bcc import BPF
import ctypes as ct
import time

# This is our eBPF program written in C, using separate, stable tracepoints.
bpf_text = """
#include <linux/sched.h>
#include <uapi/linux/bpf.h>

// The data structure is the same
struct data_t {
    u32 pid;
    char comm[TASK_COMM_LEN];
    char syscall_name[32];
};
BPF_PERF_OUTPUT(events);

// Helper macro for copying the syscall name
#define COPY_SYSCALL_NAME(name) __builtin_memcpy(&data.syscall_name, name, sizeof(name))

// --- PROBE 1: For the 'clone' syscall ---
TRACEPOINT_PROBE(syscalls, sys_enter_clone) {
    // For this specific tracepoint, the flags are in a clean field `args->clone_flags`
    unsigned long clone_flags = args->clone_flags;
    
    // The namespace flags are defined in linux/sched.h
    if (clone_flags & (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET)) {
        struct data_t data = {};
        data.pid = bpf_get_current_pid_tgid() >> 32;
        bpf_get_current_comm(&data.comm, sizeof(data.comm));
        
        char name[] = "clone (new ns)";
        COPY_SYSCALL_NAME(name);

        events.perf_submit(args, &data, sizeof(data));
    }
    return 0;
}

// --- PROBE 2: For the 'mkdir' syscall ---
TRACEPOINT_PROBE(syscalls, sys_enter_mkdir) {
    // For this tracepoint, the path is in a clean field `args->pathname`
    const char* path = (const char*)args->pathname;
    
    char cgroup_path[] = "/sys/fs/cgroup";
    char path_buf[15]; // Only need to read the beginning of the path
    bpf_probe_read_user_str(&path_buf, sizeof(path_buf), path);

    // Check if the path starts with our target
    for (int i = 0; i < sizeof(cgroup_path) - 1; ++i) {
        if (path_buf[i] != cgroup_path[i]) {
            return 0; // Not a cgroup path, ignore it
        }
    }

    // If we get here, it's a cgroup-related mkdir
    struct data_t data = {};
    data.pid = bpf_get_current_pid_tgid() >> 32;
    bpf_get_current_comm(&data.comm, sizeof(data.comm));
    
    char name[] = "mkdir (cgroup)";
    COPY_SYSCALL_NAME(name);

    events.perf_submit(args, &data, sizeof(data));
    return 0;
}
"""

# The Python part of the script is completely unchanged.
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

print("Starting eBPF monitoring... Press Ctrl+C to exit.")
try:
    b = BPF(text=bpf_text)
    # The Python part does not need to change. BCC handles attaching all probes defined in the C code.
    b["events"].open_perf_buffer(print_event)
    while True:
        try: b.perf_buffer_poll()
        except KeyboardInterrupt: exit()
except Exception as e:
    print(f"Error: {e}")