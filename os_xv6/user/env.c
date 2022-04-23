#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"

void env(int num_of_tasks, int task_length, char* env_name) {
    int result = 0;
    int n_forks = num_of_tasks;
    int pid = getpid();
    int pids_stauts[n_forks];
    int n_experiments = 1;

    for (int i = 0; i < n_experiments; i++) {
        printf("experiment %d/%d\n", i + 1, n_experiments);

        for(int i = 0; i < n_forks; i++) {
            if(pid == getpid()) {
                fork();
            }
        }
        
        // sleep(50) => 50 miliseconds = 5 seconds
        sleep(50);
        
        if(pid == getpid()) {
            // wait for all childs to exit
            for (int i = 0; i < n_forks; i++) {
                wait(&pids_stauts[i]);
            }
            print_stats();
        }
        else {
            while(result < task_length)
            {
                printf("I have printed this %d/%d times\n", result, task_length);
                result = result + 1;
            }
            exit(0);
        }
    }
}

void env_large() {
    env(10, 500, "env_large");
}

void env_freq() {
    env(50, 100, "env_freq");
}

int
main(int argc, char *argv[])
{
    env_large();
    env_freq();
    exit(0);
}