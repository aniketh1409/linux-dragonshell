#include <limits.h>
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#define LINE_LENGTH 100
#define MAX_ARGS 5
#define MAX_LENGTH 20
#define MAX_TOKENS 20
#define PROMPT "dragonshell > "

typedef enum { JOB_RUNNING, JOB_STOPPED } job_state_t;

typedef struct job {
    pid_t pid;
    job_state_t state;
    char *cmdline;
    struct job *next;
} job_t;

static job_t *job_head = NULL;
static volatile pid_t fg_pid = 0;

static void add_job(pid_t pid, job_state_t state, const char *cmdline) {
    job_t *j = malloc(sizeof(job_t));
    if (!j) return;
    j->pid = pid;
    j->state = state;
    
    int len = 0;
    if (cmdline) {
        while (cmdline[len]) len++; 
        j->cmdline = malloc(len + 1);
        if (j->cmdline) {
            for (int i = 0; i <= len; i++) j->cmdline[i] = cmdline[i];
        }
    } else {
        j->cmdline = malloc(1);
        if (j->cmdline) j->cmdline[0] = '\0';
    }
    j->next = job_head;
    job_head = j;
}

static void remove_job(pid_t pid) {
    job_t **pp = &job_head;
    while (*pp) {
        if ((*pp)->pid == pid) {
            job_t *tmp = *pp;
            *pp = tmp->next;
            free(tmp->cmdline);
            free(tmp);
            return;
        }
        pp = &((*pp)->next);
    }
}

static job_t *find_job(pid_t pid) {
    job_t *p = job_head;
    while (p) {
        if (p->pid == pid) return p;
        p = p->next;
    }
    return NULL;
}

static void print_jobs() {
    job_t *p = job_head;
    while (p) {
        char state_c = (p->state == JOB_RUNNING) ? 'R' : 'T';
        printf("%d %c %s\n", (int)p->pid, state_c, p->cmdline);
        p = p->next;
    }
}

static void sigint_handler(int signo) {
    if (fg_pid > 0) {
        kill(-fg_pid, SIGINT);
    } else {
        write(STDOUT_FILENO, "\n", 1);
    }
}

static void sigtstp_handler(int signo) {
    if (fg_pid > 0) {
        kill(-fg_pid, SIGTSTP);
    } else {
        write(STDOUT_FILENO, "\n", 1);
    }
}

static void sigchld_handler(int signo) {
    int saved_errno = errno;
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        job_t *j = find_job(pid);
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (j) remove_job(pid);
            if (pid == fg_pid) fg_pid = 0;
        } else if (WIFSTOPPED(status)) {
            if (j) j->state = JOB_STOPPED;
            if (pid == fg_pid) fg_pid = 0;
        } else if (WIFCONTINUED(status)) {
            if (j) j->state = JOB_RUNNING;
        }
    }
    errno = saved_errno;
}

int tokenize(char *line, char ***tokens) {
    static char *argv[MAX_ARGS + 1];
    int argc = 0;
    char *p = line;
    
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break; 
        argv[argc++] = p;
    
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) {
            *p = '\0';
            p++;
        }
    }
    argv[argc] = NULL;
    *tokens = argv;
    return argc;
}

static int execute_single_command(char **tokens, int tcount,int cmd_start, int cmd_end,const char *in_file, 
    const char *out_file, int background, const char *full_cmdline) {
    if (cmd_start >= cmd_end) return -1;

    if (strcmp(tokens[cmd_start], "pwd") == 0) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            perror("getcwd");
        } else {
            printf("%s\n", cwd);
        }
        return 0;
    } else if (strcmp(tokens[cmd_start], "cd") == 0) {
        if (cmd_end - cmd_start < 2) {
            printf("dragonshell: Expected argument to \"cd\"\n");
        } else {
            if (chdir(tokens[cmd_start + 1]) != 0) {
                printf("dragonshell: No such file or directory\n");
            }
        }
        return 0;
    } else if (strcmp(tokens[cmd_start], "jobs") == 0) {
        print_jobs();
        return 0;
    } else if (strcmp(tokens[cmd_start], "exit") == 0) {
        job_t *p = job_head;
        while (p) {
            kill(p->pid, SIGTERM);
            p = p->next;
        }
        sleep(1);
        while (job_head) remove_job(job_head->pid);
        exit(0);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    } else if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        if (in_file) {
            int fd = open(in_file, O_RDONLY);
            if (fd < 0) { perror("dragonshell"); exit(1); }
            dup2(fd, STDIN_FILENO); close(fd);
        }
        if (out_file) {
            int fd = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { perror("dragonshell"); exit(1); }
            dup2(fd, STDOUT_FILENO); close(fd);
        }
        execve(tokens[cmd_start], &tokens[cmd_start], NULL);
        perror("dragonshell: Command not found");
        exit(1);
    } else {
        if (background) {
            add_job(pid, JOB_RUNNING, full_cmdline);
            printf("PID %d is sent to background\n", (int)pid);
        } else {
            fg_pid = pid;
            int status;
            waitpid(pid, &status, WUNTRACED);
            if (WIFSTOPPED(status)) {
                add_job(pid, JOB_STOPPED, full_cmdline);
            }
            fg_pid = 0;
        }
    }
    return 0;
}

static int execute_pipe(char **tokens, int cmd1_s, int cmd1_e, int cmd2_s, int cmd2_e,
                        const char *full_cmdline, int background) {
    int pipefd[2];
    if (pipe(pipefd) == -1) { perror("dragonshell"); return -1; }

    pid_t pid1 = fork();
    if (pid1 == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execve(tokens[cmd1_s], &tokens[cmd1_s], NULL);
        perror("dragonshell: Command not found"); exit(1);
    }

    pid_t pid2 = fork();
    if (pid2 == 0) {
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execve(tokens[cmd2_s], &tokens[cmd2_s], NULL);
        perror("dragonshell: Command not found"); exit(1);
    }

    close(pipefd[0]); close(pipefd[1]);
    waitpid(pid1, NULL, 0);
    waitpid(pid2, NULL, 0);
    return 0;
}

static void handle_command_line(char *line) {
    char **tokens;
    int tcount = tokenize(line, &tokens);
    if (tcount == 0) return;
    char full_cmdline[LINE_LENGTH+1];
    int i;
    for (i = 0; i < LINE_LENGTH && line[i]; i++) {
        full_cmdline[i] = line[i];
    }
    full_cmdline[i] = '\0';
    int background = 0;
    if (tcount > 0 && strcmp(tokens[tcount-1], "&") == 0) {
        background = 1;
        tokens[tcount-1] = NULL;
        tcount--;
    }
    int pipe_idx = -1;
    for (int i = 0; i < tcount; ++i) {
        if (strcmp(tokens[i], "|") == 0) { pipe_idx = i; break; }
    }
    const char *in_file = NULL;
    const char *out_file = NULL;
    if (pipe_idx == -1) {
        for (int i = 0; i < tcount; ++i) {
            if (strcmp(tokens[i], "<") == 0) {
                if (i + 1 < tcount) {
                    in_file = tokens[i+1];
                    for (int j = i; j + 2 < tcount; ++j) tokens[j] = tokens[j+2];
                    tcount -= 2;
                    i--;
                } else {
                    printf("dragonshell: Missing input filename\n");
                    return;
                }
            } else if (strcmp(tokens[i], ">") == 0) {
                if (i + 1 < tcount) {
                    out_file = tokens[i+1];
                    for (int j = i; j + 2 < tcount; ++j) tokens[j] = tokens[j+2];
                    tcount -= 2;
                    i--;
                } else {
                    printf("dragonshell: Missing output filename\n");
                    return;
                }
            }
        }
        execute_single_command(tokens, tcount, 0, tcount, in_file, out_file, background, full_cmdline);
    } else {
        if (pipe_idx == 0 || pipe_idx == tcount-1) {
            printf("dragonshell: Invalid pipe usage\n");
            return;
        }
        execute_pipe(tokens, 0, pipe_idx, pipe_idx+1, tcount, full_cmdline, background);
    }
}

int main(int argc, char **argv) {
    struct sigaction sa_int, sa_tstp, sa_chld;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa_int, NULL);

    memset(&sa_tstp, 0, sizeof(sa_tstp));
    sa_tstp.sa_handler = sigtstp_handler;
    sigemptyset(&sa_tstp.sa_mask);
    sa_tstp.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &sa_tstp, NULL);

    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, NULL);

    printf("Welcome to Dragon Shell!\n\n");

    char line[LINE_LENGTH + 2];
    while (1) {
        printf("%s", PROMPT);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            job_t *p = job_head;
            while (p) {
                kill(p->pid, SIGTERM);
                p = p->next;
            }
            break;
        }
        char *tmp = line;
        while (*tmp == ' ' || *tmp == '\t') tmp++;
        if (*tmp == '\0' || *tmp == '\n') continue;
        handle_command_line(line);
    }
    return 0;
}
