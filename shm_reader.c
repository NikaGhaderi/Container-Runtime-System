#include <sys/shm.h>
#include <stdio.h>
#include <unistd.h>
int main() {
    int shmid = shmget(1234, 1024, 0666);
    if (shmid == -1) { perror("shmget"); return 1; }
    char *data = shmat(shmid, NULL, 0);
    if (data == (char *)-1) { perror("shmat"); return 1; }
    printf("Read from shared memory: %s\n", data);
    shmdt(data);
    shmctl(shmid, IPC_RMID, NULL); // Clean up
    return 0;
}