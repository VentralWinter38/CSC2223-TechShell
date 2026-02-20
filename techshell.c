// Name: Cole Pellegrin
// Description: TechShell assignment

// ensure modern POSIX
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>      // has printf, fflush, stdin, etc
#include <stdlib.h>     // has malloc, calloc, exit, etc
#include <string.h>     // has strlen, strerror, etc
#include <unistd.h>     // has fork, dup2, execvp, etc
#include <errno.h>
#include <fcntl.h>      // has O_RDONLY, O_WRONLY, etc
#include <sys/wait.h>   // has various wait commands
#include <ctype.h>      // has character checks


typedef struct {
    char **argv;
    int argc;
    char *in_file;      // after <
    char *out_file;     // after >
} ShellCommand;
/* Usage:
    stores arguments like this
    ex. wc -l < ps.out > wc.out
    argv -> ["wc", "-l", NULL]
    argc -> 2
    in_file -> "ps.out"
    out_file -> "wc.out"
*/

// error formatting
static void tech_error(void) {
    fprintf(stderr, "Error %d (%s)\n", errno, strerror(errno));
}
// ex output
// Error 2 (No such file for directory)


// prompt formatting
static void display_prompt(void) {
    char cwd[4096];

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        printf("$ ");
        fflush(stdout);
        return;
    }

    // read HOME environment variable
    const char *home = getenv("HOME");

    // shows '~$ ' if in home directory
    if (home && strcmp(cwd, home) == 0) {
        printf("~$ ");
        fflush(stdout);
        return;
    }

    // find last '/' and isolate highest level directory
    // ex. 'home/cole/Desktop' isolates to 'Desktop'
    // places '$ ' after destination like 'Desktop$ '
    const char *base = strrchr(cwd, '/');
    base = (base && base[1] != '\0') ? base + 1 : cwd;
    printf("%s$ ", base);
    fflush(stdout);
}

// tokenizer
static char *next_token (const char *s, size_t *i) {
    // skip whitespace
    // advance cursor if not end of string and char is whitespace
    while (s[*i] && isspace((unsigned char)s[*i])) (*i)++;
        // no more tokens if we hit '\0', so end
        if (!s[*i]) return NULL;

    // redirection tokens
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

    // checks if token starts with ' or " and store
    char quote = 0;
    if (s[*i] == '"' || s[*i] == '\'') {
        quote = s[*i];
        (*i)++;
    }

    while (s[*i]) {
        char c = s[*i];

        if (quote) {
            if (c == quote) { // end quote found
                (*i)++;
                break;
            }
        }
        else {
            // stop token on whitespace or redirection
            if (isspace((unsigned char)c) || c == '<' || c == '>') break;
        }

        // makes '\ ' treat space as a literal character
        if (c == '\\') {
            (*i)++;
            if (!s[*i]) break;
            c = s[*i];
        }

        // double capacity if running out of room
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

    // NULL terminate and return
    buf[len] = '\0';
    return buf;
}

// parse input to build struct
// repeatedly use next_token to fill ShellCommand
static ShellCommand parse_input (const char *line) {
    // initialize variables
    ShellCommand cmd;
    cmd.argv = NULL;
    cmd.argc = 0;
    cmd.in_file = NULL;
    cmd.out_file = NULL;

    // start with space for 8 pointers
    // zero with calloc so argv starts as all NULL
    int argv_cap = 8;
    cmd.argv = (char **)calloc((size_t)argv_cap, sizeof(char *));
    if (!cmd.argv) return cmd;

    // stop when no more tokens
    size_t i = 0;
    for (;;) {
        char *tok = next_token(line, &i);
        if (!tok) break;

        // if token is <, then filename must follow
        // store filename in cmd.in_file
        if(strcmp(tok, "<") == 0) {
            free(tok);
            char *f = next_token(line, &i);
            if (!f) {
                errno = EINVAL;
                tech_error();
                break;
            }
            // free before storing incase multiple < were typed
            free(cmd.in_file);
            cmd.in_file = f;
            continue;
        }

        // if token is >, then filename must follow
        // store filename in cmd.out_file
        // literally the same as input, except it's output
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
            argv_cap *= 2; // ensure capacity
            char **tmp = (char **)realloc(cmd.argv, (size_t)argv_cap * sizeof(char *));
            if (!tmp) {
                free(tok);
                errno = ENOMEM;
                tech_error();
                break;
            }
            // zero newly allocated region
            for (int k = cmd.argc; k < argv_cap; k++) tmp[k] = NULL;
            cmd.argv = tmp;
        }

        // make sure argv is properly terminated
        cmd.argv[cmd.argc++] = tok;
        cmd.argv[cmd.argc] = NULL;
    }

    return cmd;

}

// cleans memory allocated by the parser
static void free_command (ShellCommand *cmd) {
    if (!cmd) return;

    if (cmd->argv) {
        // free each argv string
        for (int j = 0; cmd->argv[j]; j++) free(cmd->argv[j]);

        // free argv array
        free(cmd->argv);
    }

    // free redirection filenames
    free(cmd->in_file);
    free(cmd->out_file);

    // set pointers to NULL
    cmd->argv       = NULL;
    cmd->in_file    = NULL;
    cmd->out_file   = NULL;
    cmd->argc       = 0;
}


// check command name against known built in commands
static int is_builtin(const char *name) {
    return (name && 
        (strcmp(name, "cd") == 0 ||
         strcmp(name, "exit") == 0 ||
         strcmp(name, "quit") == 0));
}


// run the builtin commands
static void run_builtin(char **argv) {
    if (!argv || !argv[0]) return;

    // exit the 'terminal' if first argument is 'exit' or 'quit'
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
            // chdir sets errno
            tech_error();
        }
    }
}


// execute commands
static void execute_command (ShellCommand *cmd) {
    // ignore if empty
    if (!cmd || !cmd->argv || !cmd->argv[0]) return;

    // check for builtin commands and run if so
    if (is_builtin(cmd->argv[0])) {
        run_builtin(cmd->argv);
        return;
    }

    // fork into parent/child
    pid_t pid = fork();

    // error check
    if (pid < 0) {
        tech_error();
        return;
    }

    // child process
    if (pid == 0) {
        // handle redirection
        // then execute

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

        // execvp only returns on fail
        execvp(cmd->argv[0], cmd->argv);
        tech_error();
        _exit(127);
    }

    // parent waits for child to finish
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        tech_error();
    }
}


// MAIN LOOP (finally here)
int main (void) {
    char *line = NULL;
    size_t cap = 0;

    for (;;) {
        display_prompt();

        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) {
            // ctrl-D or input error
            printf("\n");
            break;
        }

        // get rid of trailing newline
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