/*
 * Dailution - Shell de comandos mínima (sin scripting)
 * Versión: 1.1
 * Autor:  David Baña Szymaniak
 * Desc:   Ejecuta comandos externos directamente, sin shell
 *
 * Soporta tuberías, redirecciones, operadores lógicos, trabajos en segundo plano,
 * edición de línea con flechas e historial en RAM (sin bibliotecas externas).
 * Expande variables ($VAR, ${VAR}, $?, $$) y la tilde (~).
 * Carga configuración desde ~/.dailutionrc (lo crea si no existe con datos reales).
 * Ctrl+Z mata el proceso en primer plano (SIGKILL).
 * Ctrl+C durante la ejecución va al proceso hijo, no al shell.
 *
 * Built‑ins: cd, exit.
 * Uso: ./dailution [opción] [comando ...] o sin argumentos entras al shell.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <termios.h>
#include <pwd.h>
#include <grp.h>

/* ---------------------------------------------------------------------------
 * Variables del proyecto (visibles con --version / --edition)
 * --------------------------------------------------------------------------- */
#define VERSION         "1.1"
#define AUTHOR          "David Baña Szymaniak"
#define DESCRIPTION     "Dailution: shell mínima para ejecutar comandos."

/* ---------------------------------------------------------------------------
 * Estructuras de datos del shell
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
 * Editor de línea propio (sin readline/linenoise)
 * --------------------------------------------------------------------------- */
#define HIST_MAX 100
static char **historial = NULL;
static int hist_count = 0;

static void hist_add(const char *line) {
    if (!line || line[0] == '\0') return;
    int solo_espacios = 1;
    for (const char *p = line; *p; p++) if (!isspace(*p)) { solo_espacios = 0; break; }
    if (solo_espacios) return;
    if (hist_count > 0 && strcmp(historial[hist_count-1], line) == 0) return;

    if (hist_count >= HIST_MAX) {
        free(historial[0]);
        memmove(historial, historial + 1, (HIST_MAX - 1) * sizeof(char*));
        hist_count--;
    }
    historial = realloc(historial, (hist_count + 1) * sizeof(char*));
    if (!historial) { perror("realloc"); exit(1); }
    historial[hist_count++] = strdup(line);
}

static void hist_free(void) {
    for (int i = 0; i < hist_count; i++) free(historial[i]);
    free(historial);
    historial = NULL;
    hist_count = 0;
}

static char *leer_linea(const char *prompt) {
    struct termios viejo, nuevo;
    tcgetattr(STDIN_FILENO, &viejo);
    nuevo = viejo;
    nuevo.c_lflag &= ~(ICANON | ECHO | ISIG);
    nuevo.c_cc[VMIN] = 1;
    nuevo.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &nuevo);

    write(STDOUT_FILENO, prompt, strlen(prompt));

    char *buf = NULL;
    size_t len = 0, cap = 0;
    int pos = 0;
    int idx_hist = -1;

    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) {
            free(buf);
            tcsetattr(STDIN_FILENO, TCSANOW, &viejo);
            return NULL;
        }

        if (c == 127 || c == 8) { /* backspace */
            if (pos > 0) {
                memmove(&buf[pos-1], &buf[pos], len - pos);
                len--; pos--;
                buf[len] = '\0';
                write(STDOUT_FILENO, "\b \b", 3);
                for (size_t i = pos; i < len; i++) write(STDOUT_FILENO, &buf[i], 1);
                write(STDOUT_FILENO, " ", 1);
                for (int i = len - pos + 1; i > 0; i--) write(STDOUT_FILENO, "\b", 1);
            }
            continue;
        }

        if (c == 27) { /* secuencias de escape */
            char seq[2];
            if (read(STDIN_FILENO, seq, 2) != 2) continue;
            if (seq[0] == '[') {
                switch (seq[1]) {
                    case 'D': if (pos > 0) { pos--; write(STDOUT_FILENO, "\b", 1); } break;
                    case 'C': if (pos < (int)len) { write(STDOUT_FILENO, &buf[pos], 1); pos++; } break;
                    case 'A':
                        if (hist_count > 0) {
                            if (idx_hist == -1) idx_hist = hist_count - 1;
                            else if (idx_hist > 0) idx_hist--;
                            write(STDOUT_FILENO, "\x1b[2K\r", 5);
                            write(STDOUT_FILENO, prompt, strlen(prompt));
                            free(buf);
                            buf = strdup(historial[idx_hist]);
                            len = strlen(buf); cap = len + 1; pos = len;
                            write(STDOUT_FILENO, buf, len);
                        }
                        break;
                    case 'B':
                        if (idx_hist != -1) {
                            idx_hist++;
                            write(STDOUT_FILENO, "\x1b[2K\r", 5);
                            write(STDOUT_FILENO, prompt, strlen(prompt));
                            free(buf);
                            if (idx_hist < hist_count) {
                                buf = strdup(historial[idx_hist]);
                                len = strlen(buf); cap = len + 1; pos = len;
                                write(STDOUT_FILENO, buf, len);
                            } else {
                                buf = NULL; len = 0; cap = 0; pos = 0; idx_hist = -1;
                            }
                        }
                        break;
                    case 'H': while (pos > 0) { write(STDOUT_FILENO, "\b", 1); pos--; } break;
                    case 'F': while (pos < (int)len) { write(STDOUT_FILENO, &buf[pos], 1); pos++; } break;
                    case '3':
                        if (read(STDIN_FILENO, &c, 1) == 1 && c == '~' && pos < (int)len) {
                            memmove(&buf[pos], &buf[pos+1], len - pos);
                            len--;
                            for (size_t i = pos; i < len; i++) write(STDOUT_FILENO, &buf[i], 1);
                            write(STDOUT_FILENO, " ", 1);
                            for (int i = len - pos + 1; i > 0; i--) write(STDOUT_FILENO, "\b", 1);
                        }
                        break;
                }
            }
            continue;
        }

        if (c == 4) { /* Ctrl+D */
            if (len == 0) { free(buf); tcsetattr(STDIN_FILENO, TCSANOW, &viejo); return NULL; }
            continue;
        }

        if (c == 3) { /* Ctrl+C */
            write(STDOUT_FILENO, "\x1b[2K\r", 5);
            write(STDOUT_FILENO, "^C\n", 3);
            free(buf);
            buf = NULL; len = 0; cap = 0; pos = 0; idx_hist = -1;
            write(STDOUT_FILENO, prompt, strlen(prompt));
            continue;
        }

        if (c == '\n' || c == '\r') {
            write(STDOUT_FILENO, "\n", 1);
            break;
        }

        if (len + 1 >= cap) {
            cap = cap ? cap * 2 : 64;
            buf = realloc(buf, cap);
            if (!buf) { perror("realloc"); exit(1); }
        }
        memmove(&buf[pos+1], &buf[pos], len - pos + 1);
        buf[pos] = c;
        len++; pos++;
        buf[len] = '\0';
        write(STDOUT_FILENO, &buf[pos-1], len - pos + 1);
        for (int i = len - pos; i > 0; i--) write(STDOUT_FILENO, "\b", 1);
    }

    /* CORRECCIÓN: si se pulsó Enter sin escribir nada, devolvemos cadena vacía */
    if (!buf) {
        buf = strdup("");
        if (!buf) { perror("strdup"); exit(1); }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &viejo);
    return buf;
}

/* ---------------------------------------------------------------------------
 * Expansión de variables
 * --------------------------------------------------------------------------- */
static int last_status = 0;

static char *expand_var(const char *name) {
    if (strcmp(name, "?") == 0) {
        char val[32]; snprintf(val, sizeof(val), "%d", last_status);
        return strdup(val);
    }
    if (strcmp(name, "$") == 0) {
        char val[32]; snprintf(val, sizeof(val), "%d", getpid());
        return strdup(val);
    }
    const char *env = getenv(name);
    if (env) return strdup(env);
    if (strcmp(name, "UID") == 0) {
        char val[32]; snprintf(val, sizeof(val), "%d", getuid());
        return strdup(val);
    }
    if (strcmp(name, "HOME") == 0) {
        struct passwd *pw = getpwuid(getuid());
        return strdup(pw ? pw->pw_dir : "/");
    }
    if (strcmp(name, "USER") == 0) {
        struct passwd *pw = getpwuid(getuid());
        return strdup(pw ? pw->pw_name : "nobody");
    }
    if (strcmp(name, "TERM") == 0) {
        return strdup(getenv("TERM") ? getenv("TERM") : "xterm-256color");
    }
    return strdup("");
}

/* ---------------------------------------------------------------------------
 * Tokenizador y parser (con expansión de ~, $VAR, ${VAR}, $?, $$)
 * --------------------------------------------------------------------------- */
#define TOK_WORD       0
#define TOK_PIPE       1
#define TOK_AND        2
#define TOK_OR         3
#define TOK_SEMI       4
#define TOK_BG         5
#define TOK_REDIR_IN   6
#define TOK_REDIR_OUT  7
#define TOK_APPEND     8
#define TOK_REDIR_ERR  9
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

static void free_tokens(void) {
    for (int i = 0; i < tok_count; i++) free(tokens[i].word);
    free(tokens);
    tokens = NULL; tok_count = 0; tok_cap = 0;
}

static int tokenize(const char *line) {
    const char *p = line;
    char *buf = NULL;
    size_t buf_cap = 0;
    tok_count = 0;

    while (*p) {
        if (isspace(*p)) { p++; continue; }
        if (*p == '#') break;

        if (strncmp(p, "2>>", 3) == 0) { add_token(TOK_ERR_APPEND, NULL); p += 3; continue; }
        if (strncmp(p, "2>", 2) == 0)  { add_token(TOK_REDIR_ERR, NULL); p += 2; continue; }
        if (strncmp(p, ">>", 2) == 0)  { add_token(TOK_APPEND, NULL);     p += 2; continue; }
        if (strncmp(p, "&&", 2) == 0)  { add_token(TOK_AND, NULL);        p += 2; continue; }
        if (strncmp(p, "||", 2) == 0)  { add_token(TOK_OR, NULL);         p += 2; continue; }
        if (*p == '|') { add_token(TOK_PIPE, NULL); p++; continue; }
        if (*p == ';') { add_token(TOK_SEMI, NULL); p++; continue; }
        if (*p == '&') { add_token(TOK_BG, NULL);   p++; continue; }
        if (*p == '<') { add_token(TOK_REDIR_IN, NULL); p++; continue; }
        if (*p == '>') { add_token(TOK_REDIR_OUT, NULL); p++; continue; }

        size_t bpos = 0;
        if (buf_cap < 64) { buf_cap = 64; buf = realloc(buf, buf_cap); if (!buf) { perror("realloc"); exit(1); } }
        int squote = 0, dquote = 0;
        while (*p) {
            if (bpos + 2 >= buf_cap) { buf_cap *= 2; buf = realloc(buf, buf_cap); if (!buf) { perror("realloc"); exit(1); } }
            if (!squote && !dquote) {
                if (isspace(*p) || strchr("|&;<>", *p)) break;
                if (*p == '#') { if (bpos == 0) { free(buf); return 0; } buf[bpos++] = *p++; continue; }
                if (*p == '\'') { squote = 1; p++; continue; }
                if (*p == '"')  { dquote = 1; p++; continue; }
                if (*p == '\\') { p++; if (*p) buf[bpos++] = *p++; continue; }
                if (*p == '~' && bpos == 0) {
                    char next = *(p+1);
                    if (next == '/' || next == '\0' || isspace(next) || strchr("|&;<>", next)) {
                        const char *home = getenv("HOME"); if (!home) home = "/";
                        size_t hlen = strlen(home);
                        while (bpos + hlen + 2 >= buf_cap) { buf_cap *= 2; buf = realloc(buf, buf_cap); }
                        memcpy(buf + bpos, home, hlen); bpos += hlen; p++; continue;
                    }
                }
                if (*p == '$') {
                    p++; char varname[256]; int vpos = 0;
                    if (*p == '{') {
                        p++; while (*p && *p != '}') { if (vpos < 255) varname[vpos++] = *p; p++; }
                        if (*p == '}') p++;
                    } else {
                        while (*p && (isalnum(*p) || *p == '_')) { if (vpos < 255) varname[vpos++] = *p; p++; }
                        if (vpos == 0 && (*p == '?' || *p == '$')) { varname[vpos++] = *p; p++; }
                    }
                    if (vpos == 0) { buf[bpos++] = '$'; }
                    else {
                        varname[vpos] = '\0'; char *valor = expand_var(varname);
                        size_t vlen = strlen(valor);
                        while (bpos + vlen + 2 >= buf_cap) { buf_cap *= 2; buf = realloc(buf, buf_cap); }
                        memcpy(buf + bpos, valor, vlen); bpos += vlen; free(valor);
                    }
                    continue;
                }
                buf[bpos++] = *p++;
            } else if (squote) {
                if (*p == '\'') { squote = 0; p++; continue; }
                buf[bpos++] = *p++;
            } else if (dquote) {
                if (*p == '"') { dquote = 0; p++; continue; }
                if (*p == '\\') {
                    p++; if (*p == '\0') { free_tokens(); free(buf); return -1; }
                    if (strchr("\"\\$`", *p)) buf[bpos++] = *p;
                    else { buf[bpos++] = '\\'; buf[bpos++] = *p; }
                    p++; continue;
                }
                if (*p == '$') {
                    p++; char varname[256]; int vpos = 0;
                    if (*p == '{') {
                        p++; while (*p && *p != '}') { if (vpos < 255) varname[vpos++] = *p; p++; }
                        if (*p == '}') p++;
                    } else {
                        while (*p && (isalnum(*p) || *p == '_')) { if (vpos < 255) varname[vpos++] = *p; p++; }
                        if (vpos == 0 && (*p == '?' || *p == '$')) { varname[vpos++] = *p; p++; }
                    }
                    if (vpos == 0) { buf[bpos++] = '$'; }
                    else {
                        varname[vpos] = '\0'; char *valor = expand_var(varname);
                        size_t vlen = strlen(valor);
                        while (bpos + vlen + 2 >= buf_cap) { buf_cap *= 2; buf = realloc(buf, buf_cap); }
                        memcpy(buf + bpos, valor, vlen); bpos += vlen; free(valor);
                    }
                    continue;
                }
                buf[bpos++] = *p++;
            }
        }
        if (squote || dquote) { free_tokens(); free(buf); return -1; }
        buf[bpos] = '\0'; add_token(TOK_WORD, buf);
    }
    free(buf); return 0;
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
        if (t >= TOK_REDIR_IN && t <= TOK_ERR_APPEND) {
            int rt = t; cur_tok++;
            Token *filetok = consume(TOK_WORD);
            if (!filetok) { fprintf(stderr, "Dailution: falta nombre de archivo para la redirección\n"); cmd.argv[arg_cnt] = NULL; return cmd; }
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
    Pipeline *p = malloc(sizeof(Pipeline)); p->cmds = NULL; p->ncmds = 0; p->background = 0;
    int cap = 4; p->cmds = malloc(cap * sizeof(SimpleCmd));
    while (1) {
        SimpleCmd sc = parse_simple_cmd();
        if (sc.argv[0] == NULL && !sc.infile && !sc.outfile && !sc.errfile) {
            free(sc.argv); free(sc.infile); free(sc.outfile); free(sc.errfile); break;
        }
        if (p->ncmds >= cap) { cap *= 2; p->cmds = realloc(p->cmds, cap * sizeof(SimpleCmd)); }
        p->cmds[p->ncmds++] = sc;
        if (!consume(TOK_PIPE)) break;
    }
    return p;
}

static CmdList *parse_cmd_list(void) {
    CmdList head = {0}, *tail = &head; cur_tok = 0;
    while (cur_tok < tok_count) {
        Pipeline *pl = parse_pipeline();
        if (pl->ncmds == 0) { free(pl->cmds); free(pl); if (cur_tok < tok_count) cur_tok++; continue; }
        CmdList *node = malloc(sizeof(CmdList)); node->pipe = pl; node->next = NULL;
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
 * Ejecución (con manejo de terminal y Ctrl+Z -> SIGKILL)
 * --------------------------------------------------------------------------- */
static struct termios term_orig;

static void exec_cmd(SimpleCmd *cmd) {
    if (cmd->infile) {
        int fd = open(cmd->infile, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "%s: %s\n", cmd->infile, strerror(errno)); exit(1); }
        dup2(fd, STDIN_FILENO); close(fd);
    }
    if (cmd->outfile) {
        int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->outfile, flags, 0666);
        if (fd < 0) { fprintf(stderr, "%s: %s\n", cmd->outfile, strerror(errno)); exit(1); }
        dup2(fd, STDOUT_FILENO); close(fd);
    }
    if (cmd->errfile) {
        int flags = O_WRONLY | O_CREAT | (cmd->err_append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->errfile, flags, 0666);
        if (fd < 0) { fprintf(stderr, "%s: %s\n", cmd->errfile, strerror(errno)); exit(1); }
        dup2(fd, STDERR_FILENO); close(fd);
    }
    if (strcmp(cmd->argv[0], "cd") == 0) exit(0);
    if (strcmp(cmd->argv[0], "exit") == 0) exit(0);
    execvp(cmd->argv[0], cmd->argv);
    fprintf(stderr, "%s: comando no encontrado\n", cmd->argv[0]);
    exit(127);
}

static int execute_pipeline(Pipeline *p) {
    int n = p->ncmds;
    if (n == 0) return 0;
    if (n == 1 && !p->cmds[0].infile && !p->cmds[0].outfile && !p->cmds[0].errfile) {
        if (strcmp(p->cmds[0].argv[0], "cd") == 0) {
            if (p->cmds[0].argv[1] && p->cmds[0].argv[2]) { fprintf(stderr, "cd: demasiados argumentos\n"); return 1; }
            const char *path = p->cmds[0].argv[1] ? p->cmds[0].argv[1] : getenv("HOME");
            if (!path) path = "/";
            if (chdir(path) != 0) { fprintf(stderr, "cd: %s: %s\n", path, strerror(errno)); return 1; }
            return 0;
        }
        if (strcmp(p->cmds[0].argv[0], "exit") == 0) {
            int code = p->cmds[0].argv[1] ? atoi(p->cmds[0].argv[1]) : 0;
            exit(code);
        }
    }

    pid_t pids[64];
    pid_t lider = 0;
    int pipes[2][2] = {{-1,-1}, {-1,-1}};
    int in_fd = STDIN_FILENO;

    for (int i = 0; i < n; i++) {
        int out_fd = STDOUT_FILENO;
        if (i < n - 1) { pipe(pipes[1]); out_fd = pipes[1][1]; }

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }

        if (pid == 0) {
            if (i == 0) setpgid(0, 0);
            else setpgid(0, lider);
            signal(SIGINT, SIG_DFL); signal(SIGQUIT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            if (in_fd != STDIN_FILENO) { dup2(in_fd, STDIN_FILENO); close(in_fd); }
            if (out_fd != STDOUT_FILENO) { dup2(out_fd, STDOUT_FILENO); close(out_fd); }
            if (pipes[0][0] != -1) { close(pipes[0][0]); close(pipes[0][1]); }
            if (pipes[1][0] != -1) { close(pipes[1][0]); close(pipes[1][1]); }
            exec_cmd(&p->cmds[i]);
        }

        pids[i] = pid;
        if (i == 0) { lider = pid; setpgid(pid, pid); }
        else setpgid(pid, lider);

        if (i < n - 1) close(pipes[1][1]);
        if (i > 0) { close(pipes[0][0]); close(pipes[0][1]); }
        if (i < n - 1) { pipes[0][0] = pipes[1][0]; pipes[0][1] = -1; in_fd = pipes[0][0]; }
        else in_fd = STDIN_FILENO;
    }

    if (!p->background) {
        tcsetpgrp(STDIN_FILENO, lider);
        tcsetattr(STDIN_FILENO, TCSANOW, &term_orig);
    }

    int status = 0;
    for (int i = 0; i < n; i++) {
        int wstatus;
        while (waitpid(pids[i], &wstatus, WUNTRACED) == -1 && errno == EINTR);

        if (WIFSTOPPED(wstatus)) {
            kill(pids[i], SIGKILL);
            waitpid(pids[i], &wstatus, 0);
        }

        if (i == n - 1) {
            if (WIFEXITED(wstatus)) status = WEXITSTATUS(wstatus);
            else if (WIFSIGNALED(wstatus)) status = 128 + WTERMSIG(wstatus);
        }
    }

    if (!p->background) {
        tcsetpgrp(STDIN_FILENO, getpgrp());
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
        if (list->op == OP_OR  && status == 0) break;
        list = list->next;
    }
    return status;
}

static void reap_background(void) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

/* ---------------------------------------------------------------------------
 * Configuración ~/.dailutionrc (creación y carga)
 * --------------------------------------------------------------------------- */
static void crear_rc_si_no_existe(const char *rcpath) {
    struct stat st;
    if (stat(rcpath, &st) == 0) return;
    uid_t uid = getuid();
    struct passwd pwd, *result = NULL;
    char buf[1024];
    if (getpwuid_r(uid, &pwd, buf, sizeof(buf), &result) != 0 || !result) return;

    gid_t grupos[32];
    int ngrupos = getgroups(32, grupos);
    char lista_grupos[512] = "";
    for (int i = 0; i < ngrupos; i++) {
        struct group grp, *grp_result;
        char grp_buf[256];
        if (getgrgid_r(grupos[i], &grp, grp_buf, sizeof(grp_buf), &grp_result) == 0 && grp_result) {
            if (i > 0) strcat(lista_grupos, ",");
            strcat(lista_grupos, grp.gr_name);
        }
    }
    const char *term = getenv("TERM") ? getenv("TERM") : "xterm-256color";

    FILE *f = fopen(rcpath, "w");
    if (!f) return;
    fprintf(f, "# Configuración de Dailution\n");
    fprintf(f, "export PATH=/usr/bin\n");
    fprintf(f, "export PATH=~/.local/bin\n");
    fprintf(f, "export HOME=%s\n", pwd.pw_dir);
    fprintf(f, "export USER=%s\n", pwd.pw_name);
    fprintf(f, "export UID=%d\n", uid);
    fprintf(f, "export GROUPS=%s\n", lista_grupos);
    fprintf(f, "export TERM=%s\n", term);
    fclose(f);
}

static void cargar_rc(const char *rcpath) {
    FILE *f = fopen(rcpath, "r");
    if (!f) return;
    char linea[1024];
    while (fgets(linea, sizeof(linea), f)) {
        size_t len = strlen(linea);
        if (len > 0 && linea[len-1] == '\n') linea[--len] = '\0';
        if (len == 0 || linea[0] == '#') continue;
        char *p = linea;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "export", 6)) continue;
        p += 6; while (*p == ' ' || *p == '\t') p++;
        char *igual = strchr(p, '=');
        if (!igual) continue;
        char *var = p, *val = igual + 1;
        while (igual > var && (*(igual-1) == ' ' || *(igual-1) == '\t')) igual--;
        *igual = '\0';
        while (*val == ' ' || *val == '\t') val++;
        char *fin = val + strlen(val) - 1;
        while (fin >= val && (*fin == ' ' || *fin == '\t')) *fin-- = '\0';
        if (strcmp(var, "PATH") == 0) {
            char valor_exp[1024];
            if (val[0] == '~' && (val[1] == '/' || val[1] == '\0')) {
                const char *home = getenv("HOME"); if (!home) home = "/";
                snprintf(valor_exp, sizeof(valor_exp), "%s%s", home, val + 1);
            } else snprintf(valor_exp, sizeof(valor_exp), "%s", val);
            const char *path_actual = getenv("PATH");
            int found = 0;
            if (path_actual && path_actual[0]) {
                const char *token = path_actual;
                while (*token) {
                    const char *end = strchr(token, ':'); if (!end) end = token + strlen(token);
                    if (end - token == (int)strlen(valor_exp) && strncmp(token, valor_exp, end - token) == 0) { found = 1; break; }
                    token = (*end == ':') ? end + 1 : end;
                }
            }
            if (!found) {
                char nuevo_path[4096];
                if (path_actual && path_actual[0]) snprintf(nuevo_path, sizeof(nuevo_path), "%s:%s", path_actual, valor_exp);
                else snprintf(nuevo_path, sizeof(nuevo_path), "%s", valor_exp);
                setenv("PATH", nuevo_path, 1);
            }
        } else setenv(var, val, 1);
    }
    fclose(f);
}

/* ---------------------------------------------------------------------------
 * Ayuda del programa
 * --------------------------------------------------------------------------- */
static void mostrar_ayuda(void) {
    printf("Uso: dailution [opción] [comando ...]\n\n");
    printf("Opciones:\n");
    printf("  --version   Muestra solo el número de versión\n");
    printf("  --edition   Muestra información completa del proyecto\n");
    printf("  --help      Muestra esta ayuda\n");
    printf("  -c          Ejecuta el comando proporcionado (compatible POSIX)\n\n");
    printf("Si se invoca sin argumentos, inicia la shell interactiva.\n");
    printf("Si se proporciona un comando (o varios), lo ejecuta y termina.\n");
    printf("Ejemplos:\n");
    printf("  dailution -c \"ls -la\"\n");
    printf("  dailution echo Hola mundo\n\n");
    printf("Características:\n");
    printf("  Tuberías (|), redirecciones (>, <, >>, 2>, 2>>)\n");
    printf("  Operadores lógicos (&&, ||), punto y coma (;)\n");
    printf("  Trabajos en segundo plano (&)\n");
    printf("  Expansión de variables ($VAR, ${VAR}, $?, $$) y tilde (~)\n");
    printf("  Historial en RAM (flechas arriba/abajo)\n");
    printf("  Built‑ins: cd, exit\n");
    printf("  Ctrl+C limpia la línea actual, Ctrl+Z mata el proceso en primer plano\n");
    printf("  Carga configuración desde ~/.dailutionrc\n");
}

/* ---------------------------------------------------------------------------
 * Señales
 * --------------------------------------------------------------------------- */
static volatile sig_atomic_t interrupted = 0;
static void sigint_handler(int sig) { (void)sig; interrupted = 1; }

/* ---------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    tcgetattr(STDIN_FILENO, &term_orig);

    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    /* ── Opciones especiales antes de cualquier otra cosa ── */
    if (argc > 1) {
        if (strcmp(argv[1], "--version") == 0) {
            puts(VERSION);
            return 0;
        }
        if (strcmp(argv[1], "--edition") == 0) {
            printf("Dailution %s\n", VERSION);
            printf("Autor: %s\n", AUTHOR);
            printf("Descripción: %s\n", DESCRIPTION);
            printf("Compilado: %s %s\n", __DATE__, __TIME__);
            return 0;
        }
        if (strcmp(argv[1], "--help") == 0) {
            mostrar_ayuda();
            return 0;
        }
    }

    /* Exportar variable DAILUTION_VERSION */
    setenv("DAILUTION_VERSION", VERSION, 1);

    /* Tomar control del terminal */
    if (isatty(STDIN_FILENO)) {
        setpgid(0, 0);
        tcsetpgrp(STDIN_FILENO, getpgrp());
    }

    if (!getenv("HOME")) {
        struct passwd *pw = getpwuid(getuid());
        setenv("HOME", pw ? pw->pw_dir : "/", 0);
    }
    if (!getenv("USER")) {
        struct passwd *pw = getpwuid(getuid());
        setenv("USER", pw ? pw->pw_name : "nobody", 0);
    }
    if (!getenv("UID")) {
        char uid_str[32]; snprintf(uid_str, sizeof(uid_str), "%d", getuid()); setenv("UID", uid_str, 0);
    }
    if (!getenv("TERM")) setenv("TERM", "xterm-256color", 0);

    if (argc > 1) {
        int cmd_start = 1;             // índice del primer argumento del comando
        if (strcmp(argv[1], "-c") == 0) {
            cmd_start = 2;             // saltar el -c
        }
        if (cmd_start < argc) {
            // Construir línea de comando a partir de los argumentos restantes
            size_t total_len = 0;
            for (int i = cmd_start; i < argc; i++) total_len += strlen(argv[i]) + 1;
            char *cmdline = malloc(total_len + 1);
            cmdline[0] = '\0';
            for (int i = cmd_start; i < argc; i++) {
                if (i > cmd_start) strcat(cmdline, " ");
                strcat(cmdline, argv[i]);
            }
            if (tokenize(cmdline) != 0) {
                fprintf(stderr, "Dailution: error de sintaxis\n");
                free(cmdline);
                exit(1);
            }
            free(cmdline);
            CmdList *cmdlist = parse_cmd_list();
            int ret = 0;
            if (cmdlist) { ret = execute_cmdlist(cmdlist); free_cmdlist(cmdlist); }
            free_tokens();
            return ret;
        } else {
            // -c sin comando
            fprintf(stderr, "Dailution: -c requiere un comando\n");
            return 1;
        }
    }

    const char *home_dir = getenv("HOME");
    if (!home_dir) { struct passwd *pw = getpwuid(getuid()); home_dir = pw ? pw->pw_dir : "/"; }
    char rcpath[1024]; snprintf(rcpath, sizeof(rcpath), "%s/.dailutionrc", home_dir);
    crear_rc_si_no_existe(rcpath);
    cargar_rc(rcpath);

    printf("Dailution - shell mínima (escriba 'exit' para salir)\n");
    while (1) {
        reap_background();
        if (interrupted) { putchar('\n'); interrupted = 0; }

        char *line = leer_linea("dailution$ ");
        if (interrupted) { interrupted = 0; free(line); continue; }
        if (!line) { putchar('\n'); break; }
        if (line[0] == '\0') { free(line); continue; }

        hist_add(line);
        if (tokenize(line) != 0) { fprintf(stderr, "Dailution: error de sintaxis (comillas sin cerrar)\n"); free(line); continue; }
        free(line);
        CmdList *cmdlist = parse_cmd_list();
        if (cmdlist) { last_status = execute_cmdlist(cmdlist); free_cmdlist(cmdlist); }
        free_tokens();
    }

    hist_free();
    return last_status;
}
