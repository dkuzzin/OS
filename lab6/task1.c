#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>

#define ERR -1
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define CHILD_PROCESS 0
#define NOT_FILE -1
#define OFFSET_NOT_FILE 0
#define DEFAULT_OPT 0

volatile sig_atomic_t stop = 0;

void handle_sigint(int sig) {
    (void)sig;
    stop = 1;
}

void reader(unsigned int *buf, size_t count) {
    unsigned int prev = 0;
    int first = 1;

    while (!stop) {
        for (size_t i = 0; i < count && !stop; i++) {
            unsigned int cur = buf[i];

            if (!first && cur != prev + 1) {
                printf("Mismatch: after %u expected %u, got %u\n", prev, prev + 1, cur);
            }

            prev = cur;
            first = 0;
        }
    }
}

void writer(unsigned int *buf, size_t count) {
    unsigned int val = 0;

    while (!stop) {
        for (size_t i = 0; i < count && !stop; i++) {
            buf[i] = val++;
        }
    }
}

unsigned int* create_shared_memory(size_t *count){
    long page = sysconf(_SC_PAGESIZE);
    if (page == ERR) {
        perror("sysconf");
        return NULL;
    }

    size_t page_size = (size_t)page;
    *count = page_size / sizeof(unsigned int); 
    
    unsigned int *buf = mmap(NULL, page_size, PROT_READ | PROT_WRITE, 
        MAP_SHARED | MAP_ANONYMOUS, NOT_FILE, OFFSET_NOT_FILE);

    if (buf == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    return buf;
}

int main() {
    if (signal(SIGINT, handle_sigint) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }
    
    size_t count;
    unsigned int *buf = create_shared_memory(&count);
    size_t page_size = count * sizeof(unsigned int);
    if (buf == NULL) return EXIT_FAILURE;

    pid_t pid = fork();
    if (pid == ERR) {
        perror("fork");
        if (munmap(buf, page_size) == ERR) perror("munmap");
        return EXIT_FAILURE;
    }

    if (pid == CHILD_PROCESS) {
        reader(buf, count);
    } else {
        writer(buf, count);
        if (kill(pid, SIGINT) == ERR && errno != ESRCH) perror("kill");
        
        while (waitpid(pid, NULL, DEFAULT_OPT) == ERR) {
            if (errno != EINTR) {
                perror("waitpid");
                break;
            }
        }
    }

    if (munmap(buf, page_size) == ERR) {
        perror("munmap");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
