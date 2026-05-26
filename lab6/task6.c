#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <asm-generic/signal.h>

#define ERR -1
#define SUCCESS 0
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define CHILD_PROCESS 0
#define NOT_FILE -1
#define OFFSET_NOT_FILE 0
#define DEFAULT_OPT 0

#define SIGNAL_SENT 0
#define PROCESS_NOT_FOUND 1

int create_signal_mask(sigset_t *signal_mask) {
    if (sigemptyset(signal_mask) == ERR) {
        perror("sigemptyset");
        return ERR;
    }

    if (sigaddset(signal_mask, SIGUSR1) == ERR) {
        perror("sigaddset SIGUSR1");
        return ERR;
    }

    if (sigaddset(signal_mask, SIGUSR2) == ERR) {
        perror("sigaddset SIGUSR2");
        return ERR;
    }

    if (sigaddset(signal_mask, SIGINT) == ERR) {
        perror("sigaddset SIGINT");
        return ERR;
    }

    if (sigaddset(signal_mask, SIGTERM) == ERR) {
        perror("sigaddset SIGTERM");
        return ERR;
    }

    if (sigprocmask(SIG_BLOCK, signal_mask, NULL) == ERR) {
        perror("sigprocmask");
        return ERR;
    }

    return SUCCESS;
}

int wait_signal(sigset_t *signal_mask) {
    int signal_number;

    while ((signal_number = sigwaitinfo(signal_mask, NULL)) == ERR) {
        if (errno != EINTR) {
            perror("sigwaitinfo");
            return ERR;
        }
    }

    return signal_number;
}

int send_signal(pid_t pid, int signal_number) {
    if (kill(pid, signal_number) == ERR) {
        if (errno == ESRCH) {
            return PROCESS_NOT_FOUND;
        }

        perror("kill");
        return ERR;
    }

    return SIGNAL_SENT;
}

unsigned int *create_shared_memory(size_t *count, size_t *page_size) {
    long page = sysconf(_SC_PAGESIZE);
    if (page == ERR) {
        perror("sysconf");
        return NULL;
    }

    *page_size = (size_t)page;
    *count = *page_size / sizeof(unsigned int);

    unsigned int *buf = mmap(NULL, *page_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
                NOT_FILE, OFFSET_NOT_FILE);

    if (buf == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    return buf;
}

int destroy_shared_memory(unsigned int *buf, size_t page_size) {
    if (munmap(buf, page_size) == ERR) {
        perror("munmap");
        return ERR;
    }

    return EXIT_SUCCESS;
}

int wait_child(pid_t pid) {
    while (waitpid(pid, NULL, DEFAULT_OPT) == ERR) {
        if (errno != EINTR) {
            perror("waitpid");
            return ERR;
        }
    }

    return EXIT_SUCCESS;
}

void check_buffer(unsigned int *buf, size_t count, unsigned int *prev, int *first) {
    for (size_t i = 0; i < count; i++) {
        unsigned int cur = buf[i];

        if (!(*first) && cur != *prev + 1) {
            printf("Mismatch: after %u expected %u, got %u\n", *prev, *prev + 1, cur);
        }

        *prev = cur;
        *first = 0;
    }
}

int reader(unsigned int *buf, size_t count, pid_t writer_pid, sigset_t *signal_mask) {
    unsigned int prev = 0;
    int first = 1;

    while (1) {
        int signal_number = wait_signal(signal_mask);
        if (signal_number == ERR) {
            return EXIT_FAILURE;
        }

        if (signal_number == SIGINT) {
            printf("Reader received SIGINT\n");
            send_signal(writer_pid, SIGTERM);
            return EXIT_SUCCESS;
        }

        if (signal_number == SIGTERM) {
            printf("Reader stopped because writer stopped\n");
            return EXIT_SUCCESS;
        }

        if (signal_number == SIGUSR1) {
            check_buffer(buf, count, &prev, &first);

            int send_result = send_signal(writer_pid, SIGUSR2);
            if (send_result == ERR) {
                return EXIT_FAILURE;
            }
            if (send_result == PROCESS_NOT_FOUND) {
                return EXIT_SUCCESS;
            }
        }
    }
}

int writer(unsigned int *buf, size_t count, pid_t reader_pid, sigset_t *signal_mask) {
    unsigned int val = 0;

    while (1) {
        for (size_t i = 0; i < count; i++) buf[i] = val++;
        
        int send_result = send_signal(reader_pid, SIGUSR1);
        if (send_result == ERR) {
            return EXIT_FAILURE;
        }
        if (send_result == PROCESS_NOT_FOUND) {
            return EXIT_SUCCESS;
        }

        int signal_number = wait_signal(signal_mask);
        if (signal_number == ERR) {
            send_signal(reader_pid, SIGTERM);
            return EXIT_FAILURE;
        }

        if (signal_number == SIGUSR2) {
            continue;
        }

        if (signal_number == SIGINT) {
            printf("Writer received SIGINT\n");
            send_signal(reader_pid, SIGTERM);
            return EXIT_SUCCESS;
        }

        if (signal_number == SIGTERM) {
            printf("Writer stopped because reader stopped\n");
            return EXIT_SUCCESS;
        }
    }
}

int run_processes(unsigned int *buf, size_t count, sigset_t *signal_mask) {
    pid_t pid = fork();

    if (pid == ERR) {
        perror("fork");
        return EXIT_FAILURE;
    }

    if (pid == CHILD_PROCESS) {
        return reader(buf, count, getppid(), signal_mask);
    }

    int exit_code = writer(buf, count, pid, signal_mask);

    if (wait_child(pid) == ERR) {
        exit_code = EXIT_FAILURE;
    }

    return exit_code;
}

int main() {
    sigset_t signal_mask;

    if (create_signal_mask(&signal_mask) == ERR) {
        return EXIT_FAILURE;
    }

    size_t count;
    size_t page_size;

    unsigned int *buf = create_shared_memory(&count, &page_size);

    if (buf == NULL) {
        return EXIT_FAILURE;
    }

    int exit_code = run_processes(buf, count, &signal_mask);

    if (destroy_shared_memory(buf, page_size) == ERR) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
