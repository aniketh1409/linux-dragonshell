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
    j->cmdline = strdup(cmdline ? cmdline : "");
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

static void trim_newline(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    if (n == 0) return;
    if (s[n-1] == '\n') s[n-1] = '\0';
}

static int tokenize_input(char *line, char *tokens[], int max_tokens) {
    int count = 0;
    char *p = line;
    while (*p && count < max_tokens) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '>' || *p == '<' || *p == '|' || *p == '&') {
            char buf[2] = {*p, 0};
            tokens[count++] = strdup(buf);
            p++;
            continue;
        }
        char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '>' && *p != '<' && *p != '|' && *p != '&') p++;
        size_t len = p - start;
        char *tok = malloc(len + 1);
        strncpy(tok, start, len);
        tok[len] = '\0';
        tokens[count++] = tok;
    }
    tokens[count] = NULL;
    return count;
}

static void free_tokens(char *tokens[], int count) {
    for (int i = 0; i < count; ++i) {
        free(tokens[i]);
        tokens[i] = NULL;
    }
}

static int is_executable_file(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR)) return 1;
    }
    return 0;
}

static char **build_exec_argv(char *tokens[], int start, int end) {
    int argc = end - start;
    char **argv = malloc((argc + 1) * sizeof(char*));
    for (int i = 0; i < argc; ++i) argv[i] = tokens[start + i];
    argv[argc] = NULL;
    return argv;
}

static int execute_single_command(char *tokens[], int tcount,
                                  int cmd_start, int cmd_end,
                                  const char *in_file, const char *out_file,
                                  int background, const char *full_cmdline) {
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

    char *cmd = tokens[cmd_start];
    char exec_path[PATH_MAX];
    int found = 0;
    if (strchr(cmd, '/')) {
        strncpy(exec_path, cmd, sizeof(exec_path)-1);
        exec_path[sizeof(exec_path)-1] = '\0';
        if (is_executable_file(exec_path)) found = 1;
    } else {
        snprintf(exec_path, sizeof(exec_path), "./%s", cmd);
        if (is_executable_file(exec_path)) found = 1;
    }
    if (!found) {
        printf("dragonshell: Command not found\n");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    } else if (pid == 0) {
        setpgid(0, 0);
        if (in_file) {
            int fd = open(in_file, O_RDONLY);
            if (fd < 0) {
                perror("open input");
                _exit(1);
            }
            if (dup2(fd, STDIN_FILENO) < 0) {
                perror("dup2 input");
                _exit(1);
            }
            close(fd);
        }
        if (out_file) {
            int fd = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                perror("open output");
                _exit(1);
            }
            if (dup2(fd, STDOUT_FILENO) < 0) {
                perror("dup2 output");
                _exit(1);
            }
            close(fd);
        }
        char **child_argv = build_exec_argv(tokens, cmd_start, cmd_end);
        extern char **environ;
        execve(exec_path, child_argv, environ);
        perror("execve");
        _exit(1);
    } else {
        setpgid(pid, pid);
        if (background) {
            add_job(pid, JOB_RUNNING, full_cmdline);
            printf("PID %d is sent to background\n", (int)pid);
            return 0;
        } else {
            fg_pid = pid;
            int wstatus;
            while (1) {
                pid_t w = waitpid(pid, &wstatus, WUNTRACED);
                if (w == -1) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (WIFEXITED(wstatus) || WIFSIGNALED(wstatus)) {
                    fg_pid = 0;
                    remove_job(pid);
                    break;
                }
                if (WIFSTOPPED(wstatus)) {
                    add_job(pid, JOB_STOPPED, full_cmdline);
                    fg_pid = 0;
                    break;
                }
            }
        }
    }
    return 0;
}

static int execute_pipe(char *tokens[], int cmd1_s, int cmd1_e, int cmd2_s, int cmd2_e,
                        const char *full_cmdline, int background) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }

    pid_t p1 = fork();
    if (p1 < 0) {
        perror("fork");
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    } else if (p1 == 0) {
        setpgid(0, 0);
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) { perror("dup2"); _exit(1); }
        close(pipefd[1]);
        char **argv1 = build_exec_argv(tokens, cmd1_s, cmd1_e);
        extern char **environ;
        char *cmd = argv1[0];
        char exec_path[PATH_MAX];
        int found = 0;
        if (strchr(cmd, '/')) {
            strncpy(exec_path, cmd, sizeof(exec_path)-1);
            exec_path[sizeof(exec_path)-1] = '\0';
            if (is_executable_file(exec_path)) found = 1;
        } else {
            snprintf(exec_path, sizeof(exec_path), "./%s", cmd);
            if (is_executable_file(exec_path)) found = 1;
        }
        if (!found) {
            printf("dragonshell: Command not found\n");
            _exit(1);
        }
        execve(exec_path, argv1, environ);
        perror("execve");
        _exit(1);
    }

    pid_t p2 = fork();
    if (p2 < 0) {
        perror("fork");
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    } else if (p2 == 0) {
        setpgid(0, p1 == 0 ? 0 : p1);
        close(pipefd[1]);
        if (dup2(pipefd[0], STDIN_FILENO) < 0) { perror("dup2"); _exit(1); }
        close(pipefd[0]);
        char **argv2 = build_exec_argv(tokens, cmd2_s, cmd2_e);
        extern char **environ;
        char *cmd = argv2[0];
        char exec_path[PATH_MAX];
        int found = 0;
        if (strchr(cmd, '/')) {
            strncpy(exec_path, cmd, sizeof(exec_path)-1);
            exec_path[sizeof(exec_path)-1] = '\0';
            if (is_executable_file(exec_path)) found = 1;
        } else {
            snprintf(exec_path, sizeof(exec_path), "./%s", cmd);
            if (is_executable_file(exec_path)) found = 1;
        }
        if (!found) {
            printf("dragonshell: Command not found\n");
            _exit(1);
        }
        execve(exec_path, argv2, environ);
        perror("execve");
        _exit(1);
    }

    close(pipefd[0]); close(pipefd[1]);
    setpgid(p1, p1);
    setpgid(p2, p1);

    if (background) {
        add_job(p1, JOB_RUNNING, full_cmdline);
        add_job(p2, JOB_RUNNING, full_cmdline);
        printf("PID %d is sent to background\n", (int)p1);
        return 0;
    } else {
        fg_pid = p1;
        int status;
        waitpid(p1, &status, WUNTRACED);
        if (WIFSTOPPED(status)) {
            add_job(p1, JOB_STOPPED, full_cmdline);
            add_job(p2, JOB_STOPPED, full_cmdline);
            fg_pid = 0;
            return 0;
        }
        waitpid(p2, &status, WUNTRACED);
        fg_pid = 0;
    }
    return 0;
}

static void handle_command_line(char *line) {
    char *tokens[MAX_TOKENS+1];
    int tcount = tokenize_input(line, tokens, MAX_TOKENS);
    if (tcount == 0) return;
    char full_cmdline[LINE_LENGTH+1];
    strncpy(full_cmdline, line, LINE_LENGTH);
    full_cmdline[LINE_LENGTH] = '\0';
    trim_newline(full_cmdline);
    int background = 0;
    if (tcount > 0 && strcmp(tokens[tcount-1], "&") == 0) {
        background = 1;
        free(tokens[tcount-1]);
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
                    free_tokens(tokens, tcount);
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
                    free_tokens(tokens, tcount);
                    return;
                }
            }
        }
        execute_single_command(tokens, tcount, 0, tcount, in_file, out_file, background, full_cmdline);
    } else {
        if (pipe_idx == 0 || pipe_idx == tcount-1) {
            printf("dragonshell: Invalid pipe usage\n");
            free_tokens(tokens, tcount);
            return;
        }
        execute_pipe(tokens, 0, pipe_idx, pipe_idx+1, tcount, full_cmdline, background);
    }
    free_tokens(tokens, tcount);
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
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
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
        trim_newline(line);
        char *tmp = line;
        while (*tmp == ' ' || *tmp == '\t') tmp++;
        if (*tmp == '\0') continue;
        handle_command_line(line);
    }
    return 0;
}

