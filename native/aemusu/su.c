// aemusu — the "su" of the guest (arm64, no libc, static).
//
// Inside the emulator the guest's user ids are kept by aemu64 (setuid and friends always succeed, see run64.c),
// so becoming root takes nothing but asking for it. What su adds is the question: it asks the AEmulator app over
// /dev/socket/aemu_su whether the calling app may have root, and the app answers after a prompt or by a
// remembered choice. Granted, it switches to uid 0 and runs the shell; refused, it says so and exits with 1.
//
//   su [-] [-l|--login] [-c COMMAND] [-s SHELL] [-p|-m|--preserve-environment] [--mount-master] [USER [ARGS...]]
//   su -v | -V | --version | -h | --help
//
// Root apps reach it the usual ways: "su -c cmd", or "su" with commands written to its standard input (libsu).

typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

enum {
    NR_dup3 = 24, NR_openat = 56, NR_close = 57, NR_read = 63, NR_write = 64, NR_exit_group = 94,
    NR_setregid = 143, NR_setresuid = 147, NR_setresgid = 149, NR_setgroups = 159, NR_getpid = 172,
    NR_getppid = 173, NR_getuid = 174, NR_socket = 198, NR_connect = 203, NR_execve = 221,
    NR_faccessat = 48,
};

static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
    return x0;
}
#define S0(n) sc6(n, 0, 0, 0, 0, 0, 0)
#define S1(n, a) sc6(n, (long)(a), 0, 0, 0, 0, 0)
#define S2(n, a, b) sc6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define S3(n, a, b, c) sc6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define S4(n, a, b, c, d) sc6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)

#define VERSION "aemusu 1"
#define AT_FDCWD (-100)
#define SOCK_PATH "/dev/socket/aemu_su"

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static int pre(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
void *memcpy(void *d, const void *s, u64 n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, u64 n) { u8 *a = d; while (n--) *a++ = (u8)c; return d; }
static void out(int fd, const char *s) { S3(NR_write, fd, s, slen(s)); }
static __attribute__((noreturn)) void quit(int code) { S1(NR_exit_group, code); for (;;); }
static char *utoa(u64 v, char *end) { *--end = 0; do *--end = (char)('0' + v % 10); while (v /= 10); return end; }
static int isnum(const char *s) { if (!*s) return 0; for (; *s; s++) if (*s < '0' || *s > '9') return 0; return 1; }
static long atol_(const char *s) { long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }

// a growing buffer in static memory: the request is small and bounded
static char g_req[8192];
static u64 g_reqn;
static void put(const char *s) { u64 n = slen(s); if (g_reqn + n < sizeof g_req - 1) { memcpy(g_req + g_reqn, s, n); g_reqn += n; } }
static void putn(u64 v) { char t[24]; put(utoa(v, t + sizeof t)); }
// a value on one line: newlines inside the command are shown as spaces
static void putline(const char *s) {
    for (; *s && g_reqn < sizeof g_req - 2; s++) g_req[g_reqn++] = (*s == '\n' || *s == '\r') ? ' ' : *s;
}

// Asks the app. 1 granted, 0 refused or no answer.
static int ask(long uid, const char *cmd) {
    long fd = S3(NR_socket, 1 /*AF_UNIX*/, 1 /*SOCK_STREAM*/ | 0x80000 /*SOCK_CLOEXEC*/, 0);
    if (fd < 0) return 0;
    struct { u16 family; char path[108]; } sa;
    memset(&sa, 0, sizeof sa);
    sa.family = 1;
    memcpy(sa.path, SOCK_PATH, slen(SOCK_PATH) + 1);
    if (S3(NR_connect, fd, &sa, 2 + slen(SOCK_PATH) + 1) < 0) { S1(NR_close, fd); return -1; }
    g_reqn = 0;
    put("AEMUSU 1\nuid="); putn((u64)uid);
    put("\npid="); putn((u64)S0(NR_getpid));
    put("\nppid="); putn((u64)S0(NR_getppid));
    put("\ncmd="); putline(cmd ? cmd : "");
    put("\n\n");
    for (u64 o = 0; o < g_reqn;) {
        long w = S3(NR_write, fd, g_req + o, g_reqn - o);
        if (w <= 0) { S1(NR_close, fd); return 0; }
        o += (u64)w;
    }
    char r[8]; long n = S3(NR_read, fd, r, sizeof r);   // the app holds the line open while the prompt is shown
    S1(NR_close, fd);
    return n > 0 && r[0] == '1';
}

static void usage(void) {
    out(1, "usage: su [-] [-l] [-c COMMAND] [-s SHELL] [-p] [--mount-master] [USER [ARGS...]]\n"
           "AEmulator su: asks the AEmulator app for root (uid 0) and runs a shell.\n");
}

// The guest's own copy of the environment, with the variables of a root login changed or added.
static char *g_env[512];
static void env_build(char **envp, int login, int keep) {
    static char e_home[] = "HOME=/data", e_user[] = "USER=root", e_logname[] = "LOGNAME=root", e_shell[] = "SHELL=/system/bin/sh";
    u64 k = 0;
    for (; envp && *envp && k < 500; envp++) {
        if (!keep && (pre(*envp, "USER=") || pre(*envp, "LOGNAME=") || pre(*envp, "HOME=") || pre(*envp, "SHELL="))) continue;
        g_env[k++] = *envp;
    }
    if (!keep || login) { g_env[k++] = e_home; g_env[k++] = e_user; g_env[k++] = e_logname; g_env[k++] = e_shell; }
    g_env[k] = 0;
}

void su_main(u64 *sp) {
    long argc = (long)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    const char *cmd = 0, *shell = "/system/bin/sh";
    int login = 0, keep = 0;
    long target = 0;
    long i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (seq(a, "-") || seq(a, "-l") || seq(a, "--login")) login = 1;
        else if (seq(a, "-c") || seq(a, "--command")) { if (i + 1 < argc) cmd = argv[++i]; else { usage(); quit(2); } }
        else if (pre(a, "--command=")) cmd = a + 10;
        else if (seq(a, "-s") || seq(a, "--shell")) { if (i + 1 < argc) shell = argv[++i]; else { usage(); quit(2); } }
        else if (pre(a, "--shell=")) shell = a + 8;
        else if (seq(a, "-p") || seq(a, "-m") || seq(a, "--preserve-environment")) keep = 1;
        else if (seq(a, "-mm") || seq(a, "-M") || seq(a, "--mount-master")) { /* one mount view for everyone here */ }
        else if (seq(a, "-v") || seq(a, "--version")) { out(1, VERSION ":AEMUSU\n"); quit(0); }
        else if (seq(a, "-V")) { out(1, "1\n"); quit(0); }
        else if (seq(a, "-h") || seq(a, "--help")) { usage(); quit(0); }
        else if (seq(a, "--")) { i++; break; }
        else if (a[0] == '-' && a[1] == 'c' && a[2]) cmd = a + 2;
        else break;
    }
    if (i < argc) {                         // a user: root or a number, the rest goes to the shell
        const char *u = argv[i];
        if (seq(u, "root")) target = 0;
        else if (isnum(u)) target = atol_(u);
        else { out(2, "su: unknown user "); out(2, u); out(2, "\n"); quit(1); }
        i++;
    }

    long uid = S0(NR_getuid);
    if (uid != 0 && uid != 2000 /*adb shell*/) {
        int ok = ask(uid, cmd);
        if (ok < 0) { out(2, "su: AEmulator root is off for this system\n"); quit(1); }
        if (!ok) { out(2, "su: permission denied\n"); quit(1); }
    }

    S3(NR_setresgid, target, target, target);
    S2(NR_setgroups, 0, 0);
    if (S3(NR_setresuid, target, target, target) < 0) { out(2, "su: cannot switch user\n"); quit(1); }

    env_build(envp, login, keep);
    static char *nargv[256];
    long k = 0;
    nargv[k++] = (char *)(login ? "-sh" : shell);
    if (cmd) { nargv[k++] = "-c"; nargv[k++] = (char *)cmd; }
    for (; i < argc && k < 254; i++) nargv[k++] = argv[i];
    nargv[k] = 0;
    long r = S3(NR_execve, shell, nargv, g_env);
    out(2, "su: cannot run "); out(2, shell); out(2, "\n");
    quit(r == -2 ? 127 : 126);
}

__asm__(
    ".text\n.globl _start\n_start:\n"
    "  mov x0, sp\n"
    "  bl su_main\n"
    "  mov x8, #94\n  mov x0, #1\n  svc #0\n");
