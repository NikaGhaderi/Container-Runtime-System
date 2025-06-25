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

// This is a helper macro to make the string copying cleaner
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
        // --- NEW FILTERING LOGIC ---
        // For mkdir, we need to read the path argument to see if it's for a cgroup.
        char path[128];
        // Read the first argument of the syscall (the path) into our buffer
        bpf_probe_read_user_str(&path, sizeof(path), (const char *)PT_REGS_PARM1(args));

        // Check if the path starts with "/sys/fs/cgroup"
        char cgroup_path[] = "/sys/fs/cgroup";
        for (int i = 0; i < sizeof(cgroup_path) - 1; ++i) {
            if (path[i] != cgroup_path[i]) {
                return 0; // If it doesn't match, ignore this mkdir and exit
            }
        }

        // If it does match, we log it.
        char name[] = "mkdir (cgroup)";
        COPY_SYSCALL_NAME(name);

    } else {
        return 0; // Not a syscall we care about
    }

    // This part only runs if we didn't exit above
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