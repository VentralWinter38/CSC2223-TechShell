// Name(s): Cole Pellegrin
// Description: TechShell assignment

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <ctype.h>

typedef struct {
    char **argv;        // NULL-terminated argv for execvp
    int argc;
    char *in_file;      // filename after <
    char *out_file;     // filename after >
} ShellCommand;

/* ---------- Required output format for our shell errors ---------- */
static void tech_error(void) {
    fprintf(stderr, "Error %d (%s)\n", errno, strerror(errno));
}

// "~$ " at HOME, else smth like Desktop$
static void display_prompt(void) {
    char cwd[4096];

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        printf("$ ");
        fflush(stdout);
        return;
    }

    const char *home = getenv("HOME");
    if (home && strcmp(cwd, home) == 0) {
        printf("~$ ");
        fflush(stdout);
        return;
    }

    const char *base = strrchr(cwd, '/');
    base = (base && base[1] != '\0') ? base + 1 : cwd;

    printf("%s$ ", base);
    fflush(stdout);
}

/* ---------- Tokenizer ----------
Rules:
- Whitespace separates tokens
- Backslash escapes the next character (so \  gives a literal space, etc.)
- Single quotes '...' and double quotes "..." keep everything inside as one token
- '<' and '>' are always separate tokens (even if attached like wc<file or ls>out)
*/
static char *next_token(const char *s, size_t *i) {
    // skip whitespace
    while (s[*i] && isspace((unsigned char)s[*i])) (*i)++;
    if (!s[*i]) return NULL;

    // redirection tokens are standalone
    if (s[*i] == '<' || s[*i] == '>') {
        char *tok = (char *)malloc(2);
        if (!tok) return NULL;
        tok[0] = s[*i];
        tok[1] = '\0';
        (*i)++;
        return tok;
    }

    // dynamic buffer
    size_t cap = 64, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;

    char quote = 0;
    if (s[*i] == '"' || s[*i] == '\'') {
        quote = s[*i];
        (*i)++;
    }

    while (s[*i]) {
        char c = s[*i];

        if (quote) {
            if (c == quote) { // end quote
                (*i)++;
                break;
            }
        } else {
            // stop token on whitespace or redirection start
            if (isspace((unsigned char)c) || c == '<' || c == '>') break;
        }

        if (c == '\\') {
            (*i)++;
            if (!s[*i]) break;
            c = s[*i];
        }

        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = (char *)realloc(buf, cap);
            if (!tmp) {
                free(buf);
                return NULL;
            }
            buf = tmp;
        }

        buf[len++] = c;
        (*i)++;
    }

    buf[len] = '\0';
    return buf;
}

/* ---------- Parse input into argv + redirection files ---------- */
static ShellCommand parse_input(const char *line) {
    ShellCommand cmd;
    cmd.argv = NULL;
    cmd.argc = 0;
    cmd.in_file = NULL;
    cmd.out_file = NULL;

    int argv_cap = 8;
    cmd.argv = (char **)calloc((size_t)argv_cap, sizeof(char *));
    if (!cmd.argv) return cmd;

    size_t i = 0;
    for (;;) {
        char *tok = next_token(line, &i);
        if (!tok) break;

        if (strcmp(tok, "<") == 0) {
            free(tok);
            char *f = next_token(line, &i);
            if (!f) {
                errno = EINVAL;
                tech_error();
                break;
            }
            free(cmd.in_file);
            cmd.in_file = f;
            continue;
        }

        if (strcmp(tok, ">") == 0) {
            free(tok);
            char *f = next_token(line, &i);
            if (!f) {
                errno = EINVAL;
                tech_error();
                break;
            }
            free(cmd.out_file);
            cmd.out_file = f;
            continue;
        }

        // add normal token to argv
        if (cmd.argc + 1 >= argv_cap) {
            argv_cap *= 2;
            char **tmp = (char **)realloc(cmd.argv, (size_t)argv_cap * sizeof(char *));
            if (!tmp) {
                free(tok);
                errno = ENOMEM;
                tech_error();
                break;
            }
            // zero newly allocated region (optional but nice)
            for (int k = cmd.argc; k < argv_cap; k++) tmp[k] = NULL;
            cmd.argv = tmp;
        }

        cmd.argv[cmd.argc++] = tok;
        cmd.argv[cmd.argc] = NULL;
    }

    return cmd;
}

static void free_command(ShellCommand *cmd) {
    if (!cmd) return;

    if (cmd->argv) {
        for (int j = 0; cmd->argv[j]; j++) free(cmd->argv[j]);
        free(cmd->argv);
    }
    free(cmd->in_file);
    free(cmd->out_file);

    cmd->argv = NULL;
    cmd->in_file = NULL;
    cmd->out_file = NULL;
    cmd->argc = 0;
}

/* ---------- Builtins ---------- */
static int is_builtin(const char *name) {
    return (name &&
            (strcmp(name, "cd") == 0 ||
             strcmp(name, "exit") == 0 ||
             strcmp(name, "quit") == 0));
}

static void run_builtin(char **argv) {
    if (!argv || !argv[0]) return;

    if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "quit") == 0) {
        exit(0);
    }

    if (strcmp(argv[0], "cd") == 0) {
        const char *target = argv[1];

        if (!target) {
            target = getenv("HOME");
            if (!target) {
                errno = ENOENT;
                tech_error();
                return;
            }
        }

        if (chdir(target) != 0) {
            // chdir sets errno (e.g., EACCES for /root)
            tech_error();
        }
    }
}

/* ---------- Execute ---------- */
static void execute_command(ShellCommand *cmd) {
    if (!cmd || !cmd->argv || !cmd->argv[0]) return;

    if (is_builtin(cmd->argv[0])) {
        run_builtin(cmd->argv);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        tech_error();
        return;
    }

    if (pid == 0) {
        // child: handle redirection then exec

        if (cmd->in_file) {
            int fd = open(cmd->in_file, O_RDONLY);
            if (fd < 0) {
                tech_error();
                _exit(1);
            }
            if (dup2(fd, STDIN_FILENO) < 0) {
                tech_error();
                close(fd);
                _exit(1);
            }
            close(fd);
        }

        if (cmd->out_file) {
            int fd = open(cmd->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                tech_error();
                _exit(1);
            }
            if (dup2(fd, STDOUT_FILENO) < 0) {
                tech_error();
                close(fd);
                _exit(1);
            }
            close(fd);
        }

        execvp(cmd->argv[0], cmd->argv);

        // execvp only returns on failure; errno set (ENOENT, etc.)
        tech_error();
        _exit(127);
    }

    // parent: wait
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        tech_error();
    }
}

/* ---------- Main loop ---------- */
int main(void) {
    char *line = NULL;
    size_t cap = 0;

    for (;;) {
        display_prompt();

        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) {
            // Ctrl-D or input error
            printf("\n");
            break;
        }

        // strip trailing newline
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';

        // ignore blank lines
        size_t p = 0;
        while (line[p] && isspace((unsigned char)line[p])) p++;
        if (!line[p]) continue;

        ShellCommand cmd = parse_input(line);
        execute_command(&cmd);
        free_command(&cmd);
    }

    free(line);
    return 0;
}
