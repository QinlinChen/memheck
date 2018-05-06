#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <regex.h>
#include <stdint.h>

#define MAXLINE 1024

/* utilities */
void unix_error(const char *msg);
void app_error(const char *msg);
char *readline(const char *prompt, char *buf, int size, FILE *stream);

/* list */
typedef struct _node_t {
    char *addr;
    struct _node_t *prev;
    struct _node_t *next;
} node_t;

typedef struct _list_t {
    node_t NIL;
    int size;
} list_t;

void init_list(list_t *list);
void add_list(list_t *list, char *addr);
void remove_list(list_t *list, node_t *node);
void filter_list(list_t *list, char byte, int (*pred)(char *, char));
void print_list(list_t *list, pid_t pid);

/* ptrace wrapper */
void ptrace_attach(pid_t pid);
void ptrace_detach(pid_t pid);
long ptrace_peekdata(pid_t pid, void *addr);
void ptrace_pokedata(pid_t pid, void *addr, long data);
void ptrace_read(pid_t pid, void *addr, void *buf, size_t size);
void ptrace_write(pid_t pid, void *addr, void *buf, size_t size);

/* cmd */
int cmd_pause();
int cmd_resume();
int cmd_lookup();
int cmd_setup();
int cmd_exit();

struct {
    const char *name;
    int (*handler)();
} cmd_table [] = {
    { "pause", cmd_pause },
    { "resume", cmd_resume },
    { "lookup", cmd_lookup },
    { "setup", cmd_setup },
    { "exit", cmd_exit },
};

#define NR_CMD (sizeof(cmd_table) / sizeof(cmd_table[0]))

/* global */
#define MAX_AREA 10

struct {
    pid_t pid;
    list_t list;
    struct {
        char *start;
        char *end;
    } area[MAX_AREA];
    int nr_area;
} G;

void init_area();

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s PID\n", argv[0]);
        return 1;
    }
    
    // initialize
    G.pid = atoi(argv[1]);
    init_list(&G.list);
    init_area();

    // begin
    char line[MAXLINE];
    while (readline("(memheck) ", line, MAXLINE, stdin) != NULL) {
        char *cmd = strtok(line, " ");
        if (cmd == NULL)
            continue;
        
        for (int i = 0; i < NR_CMD; ++i) {
            if (strcmp(cmd, cmd_table[i].name) == 0) {
                if (cmd_table[i].handler() < 0)
                    return 0;
                break;
            }
        }
    }
    
    return 0;
}

void unix_error(const char *msg) {
    perror(msg);
    exit(1);
}

void app_error(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

char *readline(const char *prompt, char *buf, int size, FILE *stream) {
    char *ret_val, *find;

    if (prompt)
        printf("%s", prompt);

    if (((ret_val = fgets(buf, size, stream)) == NULL) && ferror(stream))
        app_error("readline error");

    if (ret_val) {
        find = strchr(buf, '\n');
        if (find)
            *find = '\0';
        else 
            while (getchar() != '\n')
                continue;
    }

    return ret_val;
}

void init_list(list_t *list) {
    list->NIL.addr = NULL;
    list->size = 0;
    list->NIL.next = list->NIL.prev = &list->NIL;
}

void add_list(list_t *list, char *addr) {
    node_t *node = (node_t *)malloc(sizeof(node_t));
    node->addr = addr;
    node->next = list->NIL.next;
    list->NIL.next->prev = node;
    list->NIL.next = node;
    node->prev = &list->NIL;
    list->size += 1;
}

void remove_list(list_t *list, node_t *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    free(node);
    list->size -= 1;
}

void filter_list(list_t *list, char byte, int (*pred)(char *, char)) {
    node_t *scan = list->NIL.next;
    while (scan != &list->NIL) {
        if (pred(scan->addr, byte)) {
            scan = scan->prev;
            remove_list(list, scan->next);
        }
        scan = scan->next;
    }
}

void print_list(list_t *list, pid_t pid) {
    printf("Find %d result(s).\n", list->size);
    if (list->size == 0)
        return;
    printf("%-16s %-5s %-5s %-10s %-16s\n", 
        "ADDRESS", "BYTE", "WORD", "DWORD", "QWORD");
    node_t *scan = list->NIL.next;
    while (scan != &list->NIL) {
        assert(scan->next->prev == scan);
        unsigned long data = 0;
        unsigned char byte = 0;
        // Attention: memory alignment
        char *addr_end = (char *)
            (((long)scan->addr & ~(sizeof(long) - 1)) + sizeof(long));
        for (char *addr = addr_end; addr != scan->addr - 1; addr--) {
            ptrace_read(pid, addr, &byte, sizeof(byte));
            data = (data << (sizeof(byte) * 8)) + byte;
        }
        printf("%-16p %-5d %-5d %-10d %-16d\n", scan->addr, 
            (char)data, (short)data, (int)data, (long)data);
        scan = scan->next;
    }
}

void ptrace_attach(pid_t pid) {
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) == -1)
        unix_error("Ptrace attach error");
}

void ptrace_detach(pid_t pid) {
    if (ptrace(PTRACE_DETACH, pid, NULL, NULL) == -1)
        unix_error("Ptrace detach error");
}

long ptrace_peekdata(pid_t pid, void *addr) {
    errno = 0;
    long data = ptrace(PTRACE_PEEKDATA, pid, addr, NULL);
    if (data == -1 && errno != 0)
        unix_error("Ptrace peekdata error");
    return data;
}

void ptrace_pokedata(pid_t pid, void *addr, long data) {
    if (ptrace(PTRACE_POKEDATA, pid, addr, (void *)data) == -1)
        unix_error("Ptrace pokedata error");
}

void ptrace_read(pid_t pid, void *addr, void *buf, size_t size) {
    assert(size > 0);
    char *src = (char *)addr;
    char *dst = (char *)buf;

    char *aligned = src - (long)src % sizeof(long);
    if (aligned != src) {
        long data = ptrace_peekdata(pid, aligned);
        size_t lsize = src - aligned;
        size_t rsize = sizeof(long) - lsize;
        size_t minsize = (rsize < size ? rsize : size);
        memcpy(dst, (char *)&data + lsize, minsize);
        if (minsize == size)
            return;
        size -= minsize;
        src += minsize;
        dst += minsize;
    }
    
    while (size >= sizeof(long)) {
        *(long *)dst = ptrace_peekdata(pid, src);
        size -= sizeof(long);
        dst += sizeof(long);
        src += sizeof(long);
    }

    if (size != 0) {
        long data = ptrace_peekdata(pid, src);
        memcpy(dst, &data, size);
    }
}

void ptrace_write(pid_t pid, void *addr, void *buf, size_t size) {
    assert(size > 0);
    char *src = (char *)buf;
    char *dst = (char *)addr;

    char *aligned = dst - (long)dst % sizeof(long);
    if (aligned != dst) {
        long data = ptrace_peekdata(pid, aligned);
        size_t lsize = dst - aligned;
        size_t rsize = sizeof(long) - lsize;
        size_t minsize = (rsize < size ? rsize : size);
        memcpy((char *)&data + lsize, src, minsize);
        ptrace_pokedata(pid, aligned, data);
        if (minsize == size)
            return;
        size -= minsize;
        dst += minsize;
        src += minsize;
    }

    while (size >= sizeof(long)) {
        ptrace_pokedata(pid, dst, *(long *)src);
        size -= sizeof(long);
        dst += sizeof(long);
        src += sizeof(long);
    }

    if (size != 0) {
        long data = ptrace_peekdata(pid, dst);
        memcpy(&data, src, size);
        ptrace_pokedata(pid, dst, data);
    }
}

static long regmatch_htol(char *str, regmatch_t *match) {
    char buf[17];
    int end = 0;
    for (int i = match->rm_so; i < match->rm_eo; ++i)
        buf[end++] = str[i];
    buf[end++] = '\0';
    assert(end <= 17);

    long ret;
    sscanf(buf, "%lx", &ret);
    return ret;
}

void init_area() {
    char errbuf[MAXLINE], filepath[MAXLINE], line[MAXLINE];
    int rc;
    regex_t reg;
    regmatch_t match[3];
    FILE *fp;

    /* initialize regex */
    rc = regcomp(&reg, "([0-9a-fA-F]+)\\-([0-9a-fA-f]+)\\s+[rwxps-]+\\s+"
        "[0-9a-fA-F]+\\s+[0-9a-fA-F]+:[0-9a-fA-F]+\\s+[0-9]+\\s+.*",
        REG_EXTENDED);
    if (rc != 0) {
        regerror(rc, &reg, errbuf, MAXLINE);
        printf("regex compilation failed: %s\n", errbuf);
        exit(1);
    }

    sprintf(filepath, "/proc/%d/maps", G.pid);
    if ((fp = fopen(filepath, "r")) == NULL)
        app_error("Fail to open file");

    /* match and add to area*/
    // while (readline(NULL, line, MAXLINE, fp) != NULL) {
    //     if (regexec(&reg, line, 3, match, 0) == 0) {
    //         printf("%lx-%lx\n", regmatch_htol(line, match + 1),
    //             regmatch_htol(line, match + 2));
    //     }
    // }
    readline(NULL, line, MAXLINE, fp);
    readline(NULL, line, MAXLINE, fp);
    readline(NULL, line, MAXLINE, fp);
    if (regexec(&reg, line, 3, match, 0) == 0) {
        G.area[G.nr_area].start = (char *)regmatch_htol(line, match + 1);
        G.area[G.nr_area].end = (char *)regmatch_htol(line, match + 2);
        printf("Area added: %p-%p\n", G.area[G.nr_area].start,
            G.area[G.nr_area].end);
        G.nr_area++;
    }

    if (fclose(fp) != 0)
        app_error("Fail to close file");
}

int cmd_pause() {
    ptrace_attach(G.pid);

    if ((wait(NULL) != G.pid))
        app_error("Wait error");

    return 0;
}

int cmd_resume() {
    ptrace_detach(G.pid);

    return 0;
}

int cmd_exit() {
    return -1;
}

static int filter_pred(char *addr, char lower_byte) {
    char byte;
    ptrace_read(G.pid, addr, &byte, sizeof(byte));
    return byte != lower_byte;
}

int cmd_lookup() {
    char *arg = strtok(NULL, " ");
    if (arg == NULL) {
        printf("Usage: lookup <number>\n");
        return 0;
    }
    long number = atol(arg);
    char lower_byte = *(char *)&number;

    /* First search: add address to list */
    if (G.list.size == 0) {
        for (int i = 0; i < G.nr_area; ++i) {
            for (char *addr = G.area[i].start; addr != G.area[i].end; ++addr) {
                char byte;
                ptrace_read(G.pid, addr, &byte, sizeof(byte));
                if (byte == lower_byte) 
                    add_list(&G.list, addr);
            }
        }
    } 
    /* Otherwise: filter list according to the lowerbyte of number */
    else {
        filter_list(&G.list, lower_byte, filter_pred);
    }
    
    /* show result */
    print_list(&G.list, G.pid);

    return 0;
}

int cmd_setup() {
    char *arg = strtok(NULL, " ");
    if (arg == NULL) {
        printf("Usage: setup <number>\n");
        return 0;
    }

    long number = atol(arg);

    if (G.list.size != 1) {
        printf("You are suppose to use lookup command to "
            "filter results until one qualified address left.");
        return 0;
    }

    long size = atol(strtok(NULL, " "));

    ptrace_write(G.pid, G.list.NIL.next->addr, &number, size);

    printf("Modify success!\n");
    return 0;
}
