/*
 * Dailution - Minimal POSIX-like command shell (no scripting)
 * Executes external commands directly, without an intermediate shell.
 * Built‑ins: cd, exit.
 * Usage: ./dailution [command ...]
 * Build: gcc -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -o dailution dailution.c
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>

/* ---------------------------------------------------------------------------
 * Data structures
 * --------------------------------------------------------------------------- */
typedef struct {
    char **argv;
    char *infile;
    char *outfile;
    int append;
    char *errfile;
    int err_append;
} SimpleCmd;

typedef struct pipeline {
    SimpleCmd *cmds;
    int ncmds;
    int background;
} Pipeline;

typedef enum { OP_NONE, OP_AND, OP_OR, OP_SEMI, OP_BG } CmdOp;

typedef struct cmdlist {
    Pipeline *pipe;
    CmdOp op;
    struct cmdlist *next;
} CmdList;

/* ---------------------------------------------------------------------------
 * Tokenizer and parser
 * --------------------------------------------------------------------------- */
#define TOK_WORD  0
#define TOK_PIPE  1
#define TOK_AND   2
#define TOK_OR    3
#define TOK_SEMI  4
#define TOK_BG    5
#define TOK_REDIR_IN  6
#define TOK_REDIR_OUT 7
#define TOK_APPEND  8
#define TOK_REDIR_ERR 9
#define TOK_ERR_APPEND 10

typedef struct { int type; char *word; } Token;

static Token *tokens = NULL;
static int tok_count = 0, tok_cap = 0;

static void add_token(int type, const char *word) {
    if (tok_count >= tok_cap) {
        tok_cap = tok_cap ? tok_cap * 2 : 64;
        tokens = realloc(tokens, tok_cap * sizeof(Token));
        if (!tokens) { perror("realloc"); exit(1); }
    }
    tokens[tok_count].type = type;
    tokens[tok_count].word = word ? strdup(word) : NULL;
    tok_count++;
}

static int tokenize(const char *line) {
    const char *p = line;
    char buf[4096];
    int bpos = 0;
    tok_count = 0;
    while (*p) {
        if (isspace(*p)) { p++; continue; }
        if (strncmp(p, "&&", 2) == 0) { add_token(TOK_AND, NULL); p += 2; continue; }
        if (strncmp(p, "||", 2) == 0) { add_token(TOK_OR, NULL); p += 2; continue; }
        if (strncmp(p, ">>", 2) == 0) { add_token(TOK_APPEND, NULL); p += 2; continue; }
        if (strncmp(p, "2>>", 3) == 0) { add_token(TOK_ERR_APPEND, NULL); p += 3; continue; }
        if (strncmp(p, "2>", 2) == 0) { add_token(TOK_REDIR_ERR, NULL); p += 2; continue; }
        if (*p == '|') { add_token(TOK_PIPE, NULL); p++; continue; }
        if (*p == ';') { add_token(TOK_SEMI, NULL); p++; continue; }
        if (*p == '&') { add_token(TOK_BG, NULL); p++; continue; }
        if (*p == '<') { add_token(TOK_REDIR_IN, NULL); p++; continue; }
        if (*p == '>') { add_token(TOK_REDIR_OUT, NULL); p++; continue; }
        bpos = 0;
        int squote = 0, dquote = 0;
        while (*p) {
            if (!squote && !dquote) {
                if (isspace(*p) || strchr("|&;<>", *p)) break;
                if (*p == '\'') { squote = 1; p++; continue; }
                if (*p == '"') { dquote = 1; p++; continue; }
                if (*p == '\\') { p++; if (*p) buf[bpos++] = *p++; continue; }
                buf[bpos++] = *p++;
            } else if (squote) {
                if (*p == '\'') { squote = 0; p++; continue; }
                buf[bpos++] = *p++;
            } else if (dquote) {
                if (*p == '"') { dquote = 0; p++; continue; }
                if (*p == '\\') {
                    p++;
                    if (*p == '\0') return -1;
                    if (strchr("\"\\$`", *p)) buf[bpos++] = *p;
                    else { buf[bpos++] = '\\'; buf[bpos++] = *p; }
                    p++;
                    continue;
                }
                buf[bpos++] = *p++;
            }
        }
        if (squote || dquote) return -1;
        buf[bpos] = '\0';
        add_token(TOK_WORD, buf);
    }
    return 0;
}

static void free_tokens(void) {
    for (int i = 0; i < tok_count; i++) free(tokens[i].word);
    free(tokens);
    tokens = NULL;
    tok_cap = 0;
}

static int cur_tok = 0;
static Token *consume(int type) {
    if (cur_tok < tok_count && tokens[cur_tok].type == type)
        return &tokens[cur_tok++];
    return NULL;
}

static SimpleCmd parse_simple_cmd(void) {
    SimpleCmd cmd = {0};
    int arg_cap = 8, arg_cnt = 0;
    cmd.argv = malloc(arg_cap * sizeof(char*));
    cmd.infile = cmd.outfile = cmd.errfile = NULL;
    cmd.append = cmd.err_append = 0;
    while (cur_tok < tok_count) {
        int t = tokens[cur_tok].type;
        if (t == TOK_PIPE || t == TOK_AND || t == TOK_OR || t == TOK_SEMI || t == TOK_BG) break;
        if (t == TOK_REDIR_IN || t == TOK_REDIR_OUT || t == TOK_APPEND || t == TOK_REDIR_ERR || t == TOK_ERR_APPEND) {
            int rt = t;
            cur_tok++;
            Token *filetok = consume(TOK_WORD);
            if (!filetok) { fprintf(stderr, "Dailution: missing filename for redirection\n"); cmd.argv[arg_cnt] = NULL; return cmd; }
            char *fname = strdup(filetok->word);
            switch (rt) {
                case TOK_REDIR_IN: free(cmd.infile); cmd.infile = fname; break;
                case TOK_REDIR_OUT: free(cmd.outfile); cmd.outfile = fname; cmd.append = 0; break;
                case TOK_APPEND: free(cmd.outfile); cmd.outfile = fname; cmd.append = 1; break;
                case TOK_REDIR_ERR: free(cmd.errfile); cmd.errfile = fname; cmd.err_append = 0; break;
                case TOK_ERR_APPEND: free(cmd.errfile); cmd.errfile = fname; cmd.err_append = 1; break;
            }
            continue;
        }
        Token *w = consume(TOK_WORD);
        if (arg_cnt + 1 >= arg_cap) { arg_cap *= 2; cmd.argv = realloc(cmd.argv, arg_cap * sizeof(char*)); }
        cmd.argv[arg_cnt++] = strdup(w->word);
    }
    cmd.argv[arg_cnt] = NULL;
    return cmd;
}

static Pipeline *parse_pipeline(void) {
    Pipeline *p = malloc(sizeof(Pipeline));
    p->cmds = NULL; p->ncmds = 0; p->background = 0;
    int cap = 4;
    p->cmds = malloc(cap * sizeof(SimpleCmd));
    while (1) {
        SimpleCmd sc = parse_simple_cmd();
        if (sc.argv[0] == NULL && !sc.infile && !sc.outfile && !sc.errfile) {
            free(sc.argv); free(sc.infile); free(sc.outfile); free(sc.errfile);
            break;
        }
        if (p->ncmds >= cap) { cap *= 2; p->cmds = realloc(p->cmds, cap * sizeof(SimpleCmd)); }
        p->cmds[p->ncmds++] = sc;
        if (!consume(TOK_PIPE)) break;
    }
    return p;
}

static CmdList *parse_cmd_list(void) {
    CmdList head = {0}, *tail = &head;
    cur_tok = 0;
    while (cur_tok < tok_count) {
        Pipeline *pl = parse_pipeline();
        if (pl->ncmds == 0) { free(pl->cmds); free(pl); if (cur_tok < tok_count) cur_tok++; continue; }
        CmdList *node = malloc(sizeof(CmdList));
        node->pipe = pl; node->next = NULL;
        if (consume(TOK_AND)) node->op = OP_AND;
        else if (consume(TOK_OR)) node->op = OP_OR;
        else if (consume(TOK_SEMI)) node->op = OP_SEMI;
        else if (consume(TOK_BG)) { pl->background = 1; node->op = OP_BG; }
        else node->op = OP_NONE;
        tail->next = node; tail = node;
    }
    return head.next;
}

static void free_simple_cmd(SimpleCmd *cmd) {
    if (cmd->argv) { for (int i = 0; cmd->argv[i]; i++) free(cmd->argv[i]); free(cmd->argv); }
    free(cmd->infile); free(cmd->outfile); free(cmd->errfile);
}
static void free_pipeline(Pipeline *p) {
    for (int i = 0; i < p->ncmds; i++) free_simple_cmd(&p->cmds[i]);
    free(p->cmds); free(p);
}
static void free_cmdlist(CmdList *list) {
    while (list) { CmdList *next = list->next; free_pipeline(list->pipe); free(list); list = next; }
}

/* ---------------------------------------------------------------------------
 * Execution
 * --------------------------------------------------------------------------- */
static int last_status = 0;

static void exec_cmd(SimpleCmd *cmd) {
    if (cmd->infile) {
        int fd = open(cmd->infile, O_RDONLY);
        if (fd < 0) { perror(cmd->infile); exit(1); }
        dup2(fd, STDIN_FILENO); close(fd);
    }
    if (cmd->outfile) {
        int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->outfile, flags, 0666);
        if (fd < 0) { perror(cmd->outfile); exit(1); }
        dup2(fd, STDOUT_FILENO); close(fd);
    }
    if (cmd->errfile) {
        int flags = O_WRONLY | O_CREAT | (cmd->err_append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->errfile, flags, 0666);
        if (fd < 0) { perror(cmd->errfile); exit(1); }
        dup2(fd, STDERR_FILENO); close(fd);
    }

    /* Built‑ins that reach here are in a child process (pipes/redirections) */
    if (strcmp(cmd->argv[0], "cd") == 0) exit(0);
    if (strcmp(cmd->argv[0], "exit") == 0) exit(0);

    execvp(cmd->argv[0], cmd->argv);
    perror(cmd->argv[0]);
    exit(127);
}

static int execute_pipeline(Pipeline *p) {
    int n = p->ncmds;
    if (n == 0) return 0;

    /* Handle built‑ins that affect the shell process itself (cd, exit) */
    if (n == 1 && !p->cmds[0].infile && !p->cmds[0].outfile && !p->cmds[0].errfile) {
        if (strcmp(p->cmds[0].argv[0], "cd") == 0) {
            const char *path = p->cmds[0].argv[1] ? p->cmds[0].argv[1] : getenv("HOME");
            if (!path) path = "/";
            if (chdir(path) != 0) { perror("cd"); return 1; }
            return 0;
        }
        if (strcmp(p->cmds[0].argv[0], "exit") == 0) {
            int code = p->cmds[0].argv[1] ? atoi(p->cmds[0].argv[1]) : 0;
            exit(code);
        }
    }

    pid_t pids[64];
    int pipes[2][2] = {{-1,-1}, {-1,-1}};
    int in_fd = STDIN_FILENO;
    int out_fd = STDOUT_FILENO;

    for (int i = 0; i < n; i++) {
        if (i < n - 1) {
            if (pipe(pipes[1]) < 0) { perror("pipe"); return 1; }
            out_fd = pipes[1][1];
        } else {
            out_fd = STDOUT_FILENO;
        }
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL); signal(SIGQUIT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
            if (in_fd != STDIN_FILENO) dup2(in_fd, STDIN_FILENO);
            if (out_fd != STDOUT_FILENO) dup2(out_fd, STDOUT_FILENO);
            if (pipes[0][0] != -1) { close(pipes[0][0]); close(pipes[0][1]); }
            if (pipes[1][0] != -1) { close(pipes[1][0]); close(pipes[1][1]); }
            exec_cmd(&p->cmds[i]);
        }
        pids[i] = pid;
        if (i > 0) { close(pipes[0][0]); close(pipes[0][1]); }
        pipes[0][0] = pipes[1][0]; pipes[0][1] = pipes[1][1];
        in_fd = pipes[0][0];
    }
    if (pipes[0][0] != -1) { close(pipes[0][0]); close(pipes[0][1]); }

    int status = 0;
    for (int i = 0; i < n; i++) {
        int wstatus;
        waitpid(pids[i], &wstatus, 0);
        if (i == n - 1) {
            if (WIFEXITED(wstatus)) status = WEXITSTATUS(wstatus);
            else if (WIFSIGNALED(wstatus)) status = 128 + WTERMSIG(wstatus);
        }
    }
    return status;
}

static int execute_cmdlist(CmdList *list) {
    int status = 0;
    while (list) {
        Pipeline *pl = list->pipe;
        if (pl->background) {
            pid_t bg = fork();
            if (bg == 0) { int st = execute_pipeline(pl); exit(st); }
            else if (bg > 0) printf("[%d]\n", bg);
            else perror("fork");
        } else {
            status = execute_pipeline(pl);
            last_status = status;
        }
        if (list->op == OP_AND && status != 0) break;
        if (list->op == OP_OR && status == 0) break;
        list = list->next;
    }
    return status;
}

/* ---------------------------------------------------------------------------
 * Signal handling, input
 * --------------------------------------------------------------------------- */
static volatile sig_atomic_t interrupted = 0;
static void sigint_handler(int sig) {
    (void)sig;
    interrupted = 1;
    /* No escribimos nada aquí, lo maneja el bucle principal */
}

static void reap_background(void) {
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* Lee una línea. Si es interrumpida por Ctrl+C, devuelve NULL */
static char *read_line(void) {
    char *line = NULL;
    size_t len = 0;
    interrupted = 0;
    errno = 0;
    ssize_t n = getline(&line, &len, stdin);
    if (n == -1) {
        free(line);
        if (errno == EINTR || interrupted) {
            /* Interrupción por señal: limpiamos el stream */
            clearerr(stdin);
            return NULL;
        }
        if (feof(stdin)) {
            printf("\n");
            exit(0);
        }
        return NULL;
    }
    /* Eliminar salto de línea final */
    if (n > 0 && line[n-1] == '\n') line[n-1] = '\0';
    return line;
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    signal(SIGINT, sigint_handler);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    /* Modo comando único: ./dailution "echo hola" o ./dailution echo hola */
    if (argc > 1) {
        /* Construir línea uniendo todos los argumentos */
        size_t total_len = 0;
        for (int i = 1; i < argc; i++) total_len += strlen(argv[i]) + 1;
        char *cmdline = malloc(total_len + 1);
        if (!cmdline) { perror("malloc"); exit(1); }
        cmdline[0] = '\0';
        for (int i = 1; i < argc; i++) {
            if (i > 1) strcat(cmdline, " ");
            strcat(cmdline, argv[i]);
        }

        if (tokenize(cmdline) != 0) {
            fprintf(stderr, "Dailution: syntax error\n");
            free(cmdline);
            exit(1);
        }
        free(cmdline);
        CmdList *cmdlist = parse_cmd_list();
        int ret = 0;
        if (cmdlist) {
            ret = execute_cmdlist(cmdlist);
            free_cmdlist(cmdlist);
        }
        free_tokens();
        return ret;
    }

    /* Bucle interactivo */
    printf("Dailution - minimal command shell (type 'exit' to quit)\n");
    while (1) {
        reap_background();

        /* Mostrar prompt solo si no se acaba de interrumpir */
        if (!interrupted) {
            printf("dailution$ ");
            fflush(stdout);
        }

        char *line = read_line();

        if (interrupted) {
            /* Ctrl+C presionado: descartar línea y mostrar nuevo prompt */
            free(line);
            putchar('\n');   /* nueva línea limpia */
            interrupted = 0;
            continue;
        }

        if (!line) {
            /* EOF o error */
            if (feof(stdin)) {
                putchar('\n');
                break;
            }
            continue;
        }

        if (line[0] == '\0') {
            free(line);
            continue;
        }

        if (tokenize(line) != 0) {
            fprintf(stderr, "Dailution: syntax error (unmatched quotes)\n");
            free(line);
            continue;
        }
        free(line);

        CmdList *cmdlist = parse_cmd_list();
        if (cmdlist) {
            last_status = execute_cmdlist(cmdlist);
            free_cmdlist(cmdlist);
        }
        free_tokens();
    }
    return last_status;
}
