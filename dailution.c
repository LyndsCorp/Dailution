/*
 * Dailution - Shell de comandos mínima (estilo POSIX, sin scripting)
 * Ejecuta comandos externos directamente, sin shell intermedio.
 * Soporta tuberías, redirecciones, operadores lógicos, trabajos en segundo plano,
 * edición de línea con flechas e historial en RAM (sin bibliotecas externas).
 * Expande variables ($VAR, ${VAR}, $?, $$).
 * Carga configuración desde ~/.dailutionrc (lo crea si no existe con datos reales).
 * Built‑ins: cd, exit.
 * Uso: ./dailution [comando ...] o ./dailution y te abre el shell interactivo
 * Compilación: gcc -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -o dailution dailution.c
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
    if (hist_count >= HIST_MAX) {
        free(historial[0]);
        memmove(historial, historial + 1, (HIST_MAX - 1) * sizeof(char*));
        hist_count--;
    }
    historial = realloc(historial, (hist_count + 1) * sizeof(char*));
    historial[hist_count++] = strdup(line);
}

static void hist_free(void) {
    for (int i = 0; i < hist_count; i++) free(historial[i]);
    free(historial);
    historial = NULL;
    hist_count = 0;
}

/* Lee una línea con edición básica usando termios.
 *  Retorna la línea leída (debe liberarse con free) o NULL en EOF/Ctrl+D. */
static char *leer_linea(const char *prompt) {
    struct termios viejo, nuevo;
    tcgetattr(STDIN_FILENO, &viejo);
    nuevo = viejo;
    nuevo.c_lflag &= ~(ICANON | ECHO);
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
            if (buf) free(buf);
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
                            while (pos > 0) { write(STDOUT_FILENO, "\b \b", 3); pos--; }
                            write(STDOUT_FILENO, "\r\x1b[K", 4);
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
                            while (pos > 0) { write(STDOUT_FILENO, "\b \b", 3); pos--; }
                            write(STDOUT_FILENO, "\r\x1b[K", 4);
                            write(STDOUT_FILENO, prompt, strlen(prompt));
                            free(buf);
                            if (idx_hist < hist_count) {
                                buf = strdup(historial[idx_hist]);
                                len = strlen(buf); cap = len + 1; pos = len;
                                write(STDOUT_FILENO, buf, len);
                            } else {
                                buf = NULL; len = 0; cap = 0; pos = 0;
                                idx_hist = -1;
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
            while (pos > 0) { write(STDOUT_FILENO, "\b \b", 3); pos--; }
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
        }
        memmove(&buf[pos+1], &buf[pos], len - pos + 1);
        buf[pos] = c;
        len++; pos++;
        buf[len] = '\0';
        write(STDOUT_FILENO, &buf[pos-1], len - pos + 1);
        for (int i = len - pos; i > 0; i--) write(STDOUT_FILENO, "\b", 1);
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &viejo);
    return buf;
}

/* ---------------------------------------------------------------------------
 * Expansión de variables
 * --------------------------------------------------------------------------- */
static int last_status = 0;

/* Retorna el valor de una variable de entorno o especial.
 *  El resultado debe liberarse con free(). */
static char *expand_var(const char *name) {
    if (strcmp(name, "?") == 0) {
        char val[32];
        snprintf(val, sizeof(val), "%d", last_status);
        return strdup(val);
    }
    if (strcmp(name, "$") == 0) {
        char val[32];
        snprintf(val, sizeof(val), "%d", getpid());
        return strdup(val);
    }
    const char *env = getenv(name);
    return env ? strdup(env) : strdup("");
}

/* ---------------------------------------------------------------------------
 * Tokenizador y parser (con expansión de $VAR)
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
    tokens = NULL;
    tok_count = 0;
    tok_cap = 0;
}

static int tokenize(const char *line) {
    const char *p = line;
    char *buf = NULL;
    size_t buf_cap = 0;
    tok_count = 0;

    while (*p) {
        if (isspace(*p)) { p++; continue; }
        if (*p == '#') break;   /* comentario hasta fin de línea */

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

            /* Palabra (posiblemente con comillas y variables) */
            size_t bpos = 0;
            if (buf_cap < 64) { buf_cap = 64; buf = realloc(buf, buf_cap); if (!buf) { perror("realloc"); exit(1); } }
            int squote = 0, dquote = 0;
            while (*p) {
                if (bpos + 2 >= buf_cap) { buf_cap *= 2; buf = realloc(buf, buf_cap); if (!buf) { perror("realloc"); exit(1); } }

                if (!squote && !dquote) {
                    if (isspace(*p) || strchr("|&;<>", *p)) break;
                    if (*p == '#') {
                        if (bpos == 0) { free(buf); return 0; }
                        buf[bpos++] = *p++;
                        continue;
                    }
                    if (*p == '\'') { squote = 1; p++; continue; }
                    if (*p == '"')  { dquote = 1; p++; continue; }
                    if (*p == '\\') { p++; if (*p) buf[bpos++] = *p++; continue; }
                    /* Expansión de $ */
                    if (*p == '$') {
                        p++; /* saltamos el $ */
                        char varname[256];
                        int vpos = 0;
                        if (*p == '{') {
                            p++; /* saltamos { */
                            while (*p && *p != '}') {
                                if (vpos < 255) varname[vpos++] = *p;
                                p++;
                            }
                            if (*p == '}') p++;
                        } else {
                            while (*p && (isalnum(*p) || *p == '_')) {
                                if (vpos < 255) varname[vpos++] = *p;
                                p++;
                            }
                            /* Casos especiales $? y $$ */
                            if (vpos == 0 && (*p == '?' || *p == '$')) {
                                varname[vpos++] = *p;
                                p++;
                            }
                        }
                        if (vpos == 0) {
                            /* $ sin nombre: dejamos literalmente $ */
                            buf[bpos++] = '$';
                        } else {
                            varname[vpos] = '\0';
                            char *valor = expand_var(varname);
                            size_t vlen = strlen(valor);
                            /* Asegurar capacidad suficiente */
                            while (bpos + vlen + 2 >= buf_cap) {
                                buf_cap *= 2;
                                buf = realloc(buf, buf_cap);
                                if (!buf) { perror("realloc"); exit(1); }
                            }
                            memcpy(buf + bpos, valor, vlen);
                            bpos += vlen;
                            free(valor);
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
                        p++;
                        if (*p == '\0') { free_tokens(); free(buf); return -1; }
                        if (strchr("\"\\$`", *p)) buf[bpos++] = *p;
                        else { buf[bpos++] = '\\'; buf[bpos++] = *p; }
                        p++;
                        continue;
                    }
                    /* Expansión de $ dentro de comillas dobles */
                    if (*p == '$') {
                        p++;
                        char varname[256];
                        int vpos = 0;
                        if (*p == '{') {
                            p++;
                            while (*p && *p != '}') {
                                if (vpos < 255) varname[vpos++] = *p;
                                p++;
                            }
                            if (*p == '}') p++;
                        } else {
                            while (*p && (isalnum(*p) || *p == '_')) {
                                if (vpos < 255) varname[vpos++] = *p;
                                p++;
                            }
                            if (vpos == 0 && (*p == '?' || *p == '$')) {
                                varname[vpos++] = *p;
                                p++;
                            }
                        }
                        if (vpos == 0) {
                            buf[bpos++] = '$';
                        } else {
                            varname[vpos] = '\0';
                            char *valor = expand_var(varname);
                            size_t vlen = strlen(valor);
                            while (bpos + vlen + 2 >= buf_cap) {
                                buf_cap *= 2;
                                buf = realloc(buf, buf_cap);
                                if (!buf) { perror("realloc"); exit(1); }
                            }
                            memcpy(buf + bpos, valor, vlen);
                            bpos += vlen;
                            free(valor);
                        }
                        continue;
                    }
                    buf[bpos++] = *p++;
                }
            }
            if (squote || dquote) { free_tokens(); free(buf); return -1; }
            buf[bpos] = '\0';
            add_token(TOK_WORD, buf);
    }
    free(buf);
    return 0;
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
    if (!cmd.argv) { perror("malloc"); exit(1); }
    cmd.infile = cmd.outfile = cmd.errfile = NULL;
    cmd.append = cmd.err_append = 0;

    while (cur_tok < tok_count) {
        int t = tokens[cur_tok].type;
        if (t == TOK_PIPE || t == TOK_AND || t == TOK_OR || t == TOK_SEMI || t == TOK_BG) break;
        if (t == TOK_REDIR_IN || t == TOK_REDIR_OUT || t == TOK_APPEND || t == TOK_REDIR_ERR || t == TOK_ERR_APPEND) {
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
 * Ejecución
 * --------------------------------------------------------------------------- */
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
    fprintf(stderr, "%s: %s\n", cmd->argv[0], strerror(errno));
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
    int pipes[2][2] = {{-1,-1}, {-1,-1}};
    int in_fd = STDIN_FILENO;
    for (int i = 0; i < n; i++) {
        int out_fd = STDOUT_FILENO;
        if (i < n - 1) { pipe(pipes[1]); out_fd = pipes[1][1]; }
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL); signal(SIGQUIT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
            if (in_fd != STDIN_FILENO) { dup2(in_fd, STDIN_FILENO); close(in_fd); }
            if (out_fd != STDOUT_FILENO) { dup2(out_fd, STDOUT_FILENO); close(out_fd); }
            if (pipes[0][0] != -1) { close(pipes[0][0]); close(pipes[0][1]); }
            if (pipes[1][0] != -1) { close(pipes[1][0]); close(pipes[1][1]); }
            exec_cmd(&p->cmds[i]);
        }
        pids[i] = pid;
        if (i < n - 1) close(pipes[1][1]);
        if (i > 0) { close(pipes[0][0]); close(pipes[0][1]); }
        if (i < n - 1) { pipes[0][0] = pipes[1][0]; pipes[0][1] = -1; in_fd = pipes[0][0]; }
        else in_fd = STDIN_FILENO;
    }
    int status = 0;
    for (int i = 0; i < n; i++) {
        int wstatus;
        while (waitpid(pids[i], &wstatus, 0) == -1 && errno == EINTR);
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
        if (list->op == OP_OR  && status == 0) break;
        list = list->next;
    }
    return status;
}

static void reap_background(void) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

/* ---------------------------------------------------------------------------
 * Configuración ~/.dailutionrc (creación inteligente)
 * --------------------------------------------------------------------------- */
static void crear_rc_si_no_existe(const char *rcpath) {
    struct stat st;
    if (stat(rcpath, &st) == 0) return;

    uid_t uid = getuid();
    struct passwd pwd;
    struct passwd *result = NULL;
    char buf[1024];
    int ret = getpwuid_r(uid, &pwd, buf, sizeof(buf), &result);
    if (ret != 0 || result == NULL) {
        fprintf(stderr, "Dailution: no se pudo obtener información del usuario\n");
        return;
    }

    gid_t grupos[32];
    int ngrupos = getgroups(32, grupos);
    char lista_grupos[512] = "";
    for (int i = 0; i < ngrupos; i++) {
        struct group grp;
        struct group *grp_result;
        char grp_buf[256];
        if (getgrgid_r(grupos[i], &grp, grp_buf, sizeof(grp_buf), &grp_result) == 0 && grp_result != NULL) {
            if (i > 0) strcat(lista_grupos, ",");
            strcat(lista_grupos, grp.gr_name);
        }
    }

    FILE *f = fopen(rcpath, "w");
    if (!f) {
        fprintf(stderr, "Dailution: no se pudo crear %s\n", rcpath);
        return;
    }

    fprintf(f, "# Configuración de Dailution\n");
    fprintf(f, "export PATH=/usr/bin\n");
    fprintf(f, "export PATH=~/.local/bin\n");
    fprintf(f, "export HOME=%s\n", pwd.pw_dir);
    fprintf(f, "export USER=%s\n", pwd.pw_name);
    fprintf(f, "export UID=%d\n", uid);
    fprintf(f, "export GROUPS=%s\n", lista_grupos);

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
        if (strncmp(p, "export", 6) != 0) continue;
        p += 6;
        while (*p == ' ' || *p == '\t') p++;

        char *igual = strchr(p, '=');
        if (!igual) continue;

        char *var = p;
        char *val = igual + 1;
        while (igual > var && (*(igual-1) == ' ' || *(igual-1) == '\t')) igual--;
        *igual = '\0';
        while (*val == ' ' || *val == '\t') val++;
        char *fin = val + strlen(val) - 1;
        while (fin >= val && (*fin == ' ' || *fin == '\t')) *fin-- = '\0';

        if (strcmp(var, "PATH") == 0) {
            char valor_exp[1024];
            if (val[0] == '~' && (val[1] == '/' || val[1] == '\0')) {
                const char *home = getenv("HOME");
                if (!home) home = "/";
                snprintf(valor_exp, sizeof(valor_exp), "%s%s", home, val + 1);
            } else {
                strncpy(valor_exp, val, sizeof(valor_exp));
            }
            const char *path_actual = getenv("PATH");
            char nuevo_path[4096];
            if (path_actual && path_actual[0]) {
                snprintf(nuevo_path, sizeof(nuevo_path), "%s:%s", path_actual, valor_exp);
            } else {
                snprintf(nuevo_path, sizeof(nuevo_path), "%s", valor_exp);
            }
            setenv("PATH", nuevo_path, 1);
        } else {
            setenv(var, val, 1);
        }
    }
    fclose(f);
}

/* ---------------------------------------------------------------------------
 * Señal
 * --------------------------------------------------------------------------- */
static volatile sig_atomic_t interrupted = 0;
static void sigint_handler(int sig) {
    (void)sig;
    interrupted = 1;
}

/* ---------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    signal(SIGINT, sigint_handler);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    if (argc > 1) {
        size_t total_len = 0;
        for (int i = 1; i < argc; i++) total_len += strlen(argv[i]) + 1;
        char *cmdline = malloc(total_len + 1);
        cmdline[0] = '\0';
        for (int i = 1; i < argc; i++) {
            if (i > 1) strcat(cmdline, " ");
            strcat(cmdline, argv[i]);
        }
        if (tokenize(cmdline) != 0) { fprintf(stderr, "Dailution: error de sintaxis\n"); free(cmdline); exit(1); }
        free(cmdline);
        CmdList *cmdlist = parse_cmd_list();
        int ret = 0;
        if (cmdlist) { ret = execute_cmdlist(cmdlist); free_cmdlist(cmdlist); }
        free_tokens();
        return ret;
    }

    /* ── Configuración inicial ── */
    const char *home_dir = getenv("HOME");
    if (!home_dir) {
        struct passwd *pw = getpwuid(getuid());
        home_dir = pw ? pw->pw_dir : "/";
    }
    char rcpath[1024];
    snprintf(rcpath, sizeof(rcpath), "%s/.dailutionrc", home_dir);
    crear_rc_si_no_existe(rcpath);
    cargar_rc(rcpath);

    printf("Dailution - shell mínima (escriba 'exit' para salir)\n");
    while (1) {
        reap_background();
        if (interrupted) { putchar('\n'); interrupted = 0; }

        char *line = leer_linea("dailution$ ");
        if (interrupted) {
            interrupted = 0;
            if (line) free(line);
            continue;
        }
        if (!line) {
            putchar('\n');
            break;
        }
        if (line[0] == '\0') { free(line); continue; }

        {
            int solo_esp = 1;
            for (char *p = line; *p; p++) if (!isspace(*p)) { solo_esp = 0; break; }
            if (!solo_esp) hist_add(line);
        }

        if (tokenize(line) != 0) {
            fprintf(stderr, "Dailution: error de sintaxis (comillas sin cerrar)\n");
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

    hist_free();
    return last_status;
}
