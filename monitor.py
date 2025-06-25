#!/usr/bin/python3
from bcc import BPF
import ctypes as ct
import time

# This is our eBPF program written in C.
bpf_text = """
#include <linux/sched.h>
#include <uapi/linux/bpf.h>
#include <asm/unistd_64.h>

// The data structure that our BPF program will send to user-space
struct data_t {
    u32 pid;
    char comm[TASK_COMM_LEN];
    char syscall_name[32];
};

BPF_PERF_OUTPUT(events);

TRACEPOINT_PROBE(raw_syscalls, sys_enter) {
    u64 syscall_id = args->id;
    struct data_t data = {};

    // --- THE FIX IS HERE: A new, verifier-safe way to copy strings ---
    if (syscall_id == __NR_clone) {
        // Create a character array on the stack (which is safe)
        char name[] = "clone";
        // Copy it byte-by-byte into our data struct
        for (int i = 0; i < sizeof(name); i++) {
            data.syscall_name[i] = name[i];
        }
    } else if (syscall_id == __NR_unshare) {
        char name[] = "unshare";
        for (int i = 0; i < sizeof(name); i++) {
            data.syscall_name[i] = name[i];
        }
    } else if (syscall_id == __NR_mkdir) {
        char name[] = "mkdir";
        for (int i = 0; i < sizeof(name); i++) {
            data.syscall_name[i] = name[i];
        }
    } else {
        return 0;
    }
    // --- END OF FIX ---

    data.pid = bpf_get_current_pid_tgid() >> 32;
    bpf_get_current_comm(&data.comm, sizeof(data.comm));
    events.perf_submit(args, &data, sizeof(data));
    return 0;
}
"""

# --- The User-Space Python Program ---
# (This part is unchanged)
class Data(ct.Structure):
    _fields_ = [
        ("pid", ct.c_uint),
        ("comm", ct.c_char * 16),
        ("syscall_name", ct.c_char * 32),
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

# --- Main program execution ---
print("Starting eBPF monitoring... Press Ctrl+C to exit.")
try:
    b = BPF(text=bpf_text)
    b["events"].open_perf_buffer(print_event)
    while True:
        try:
            b.perf_buffer_poll()
        except KeyboardInterrupt:
            exit()
except Exception as e:
    print(f"Error: {e}")