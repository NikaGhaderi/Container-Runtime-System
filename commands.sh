gcc -o my_runner_debug container_debug.c

# In Terminal 1
sudo ./my_runner_debug /bin/sleep 30

# In Terminal 2
pstree -p | grep my_runner

# In Terminal 2, use the HOST PID of the container
# In our example, the PID of the sleep process is 34567
sudo cat /proc/34567/mountinfo

