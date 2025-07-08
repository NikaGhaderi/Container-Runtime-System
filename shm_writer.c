#include <sys/shm.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define SHM_KEY 1234
#define SHM_SIZE 1024
int main() {
    int shmid = shmget(SHM_KEY, SHM_SIZE, IPC_CREAT | 0666);
    if (shmid == -1) { perror("shmget"); return 1; }
    char *data = shmat(shmid, NULL, 0);
    if (data == (char *)-1) { perror("shmat"); return 1; }
    strcpy(data, "Hello from writer!");
    printf("Wrote to shared memory: %s\n", data);
    sleep(10); // Wait for reader
    shmdt(data);
    return 0;
}
