// lginputd: remote-key remapper for LG webOS TVs, without injecting into
// LG processes.
//
// It finds the remote's input devices by name, creates a twin uinput device
// for each (same name, IDs and capabilities), asks webOS's input manager
// (libim in surface-manager) to rescan input devices, takes the originals
// over with EVIOCGRAB and forwards their events. Keys bound in keybinds.json
// are remapped, dropped, or run a command instead. Killing the daemon
// releases the grabs and the originals keep working.
//
// keybinds.json uses lginputhook's format, e.g.:
//   {"428":  {"action": "exec",    "command": "/path/script.sh"},
//    "1037": {"action": "launch",  "id": "org.jellyfin.webos", "params": {...}},
//    "1083": {"action": "replace", "keycode": 403},
//    "1199": {"action": "ignore"}}
// exec runs "<command> <state>" on press (1) and release (0).
//
// SIGHUP reloads keybinds.json (it is also reloaded when it changes);
// remotes that disappear and reappear (e.g. lginput2 restarting) are
// picked up again through inotify on /dev/input.
//
// Usage: lginputd [-n name-substring]... [-c keybinds.json] [-v]
//   default names: "LGE M-RCU - Builtin", "LGE RCU"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <mqueue.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// input_event as the TV's 32-bit kernel ABI sees it (struct timeval with
// 32-bit fields). Defined here so it doesn't depend on the libc's time_t.
struct ev {
    uint32_t sec;
    uint32_t usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

_Static_assert(sizeof(struct ev) == 16, "input_event must match the 32-bit kernel ABI");
_Static_assert(sizeof(struct uinput_user_dev) == 0x45c, "uinput_user_dev must match libim's 0x45c");

// LG kernels use key codes above the standard KEY_MAX (e.g. 1037, 1199);
// libim enables bits up to 0x4ff, so size the bitmaps generously.
#define MAX_BITS 0x600
#define BITS_BYTES (MAX_BITS / 8)
#define MAX_DEVICES 8
#define MAX_PATTERNS 8

#define DEFAULT_CONFIG "/home/root/.config/lginputhook/keybinds.json"

static volatile sig_atomic_t g_stop = 0;
static int g_verbose = 0;

static void logf_(const char* fmt, ...) {
    va_list ap;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[%ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int test_bit(const unsigned char* bits, int n) {
    return (bits[n / 8] >> (n % 8)) & 1;
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ---------- keybinds ---------- */

enum action { ACT_PASS, ACT_REPLACE, ACT_IGNORE, ACT_EXEC, ACT_LAUNCH };

struct bind {
    int keycode;
    enum action action;
    int new_keycode;   // replace
    char* command;     // exec
    char* launch_json; // launch: full payload for applicationManager/launch
};

static struct bind* g_binds = NULL;
static int g_nbinds = 0;

static void free_binds(struct bind* b, int n) {
    for (int i = 0; i < n; i++) {
        free(b[i].command);
        free(b[i].launch_json);
    }
    free(b);
}

static const struct bind* find_bind(int keycode) {
    for (int i = 0; i < g_nbinds; i++) {
        if (g_binds[i].keycode == keycode) return &g_binds[i];
    }
    return NULL;
}

// Minimal JSON reader, enough for keybinds.json: objects, strings, numbers,
// true/false/null and arrays. Values we don't interpret are kept as their
// raw text (used to pass "params" through to luna-send unchanged).
struct jp {
    const char* s;
    const char* end;
    int err;
};

static void jp_ws(struct jp* p) {
    while (p->s < p->end && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r')) p->s++;
}

static int jp_skip_value(struct jp* p);

// Parses a JSON string at p->s; returns a malloc'd, unescaped copy.
static char* jp_string(struct jp* p) {
    jp_ws(p);
    if (p->s >= p->end || *p->s != '"') {
        p->err = 1;
        return NULL;
    }
    p->s++;
    size_t cap = 64, len = 0;
    char* out = malloc(cap);
    while (p->s < p->end && *p->s != '"') {
        char c = *p->s++;
        if (c == '\\' && p->s < p->end) {
            char e = *p->s++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u':
                // Keep \uXXXX escapes as-is; keybinds don't need them decoded.
                if (len + 6 >= cap) out = realloc(out, cap *= 2);
                out[len++] = '\\';
                c = 'u';
                break;
            default: c = e; break;
            }
        }
        if (len + 2 >= cap) out = realloc(out, cap *= 2);
        out[len++] = c;
    }
    if (p->s >= p->end) {
        p->err = 1;
        free(out);
        return NULL;
    }
    p->s++; // closing quote
    out[len] = '\0';
    return out;
}

static int jp_skip_value(struct jp* p) {
    jp_ws(p);
    if (p->s >= p->end) return -1;
    char c = *p->s;
    if (c == '"') {
        free(jp_string(p));
        return p->err ? -1 : 0;
    }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        p->s++;
        jp_ws(p);
        if (p->s < p->end && *p->s == close) {
            p->s++;
            return 0;
        }
        while (p->s < p->end) {
            if (c == '{') {
                free(jp_string(p));
                jp_ws(p);
                if (p->err || p->s >= p->end || *p->s != ':') return -1;
                p->s++;
            }
            if (jp_skip_value(p) < 0) return -1;
            jp_ws(p);
            if (p->s < p->end && *p->s == ',') {
                p->s++;
                continue;
            }
            if (p->s < p->end && *p->s == close) {
                p->s++;
                return 0;
            }
            return -1;
        }
        return -1;
    }
    // number / true / false / null
    while (p->s < p->end && *p->s != ',' && *p->s != '}' && *p->s != ']' && *p->s != ' ' &&
           *p->s != '\n' && *p->s != '\r' && *p->s != '\t')
        p->s++;
    return 0;
}

// Writes s as a JSON string literal into a malloc'd buffer.
static char* json_quote(const char* s) {
    size_t n = strlen(s);
    char* out = malloc(n * 2 + 3);
    char* o = out;
    *o++ = '"';
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') *o++ = '\\';
        *o++ = *s;
    }
    *o++ = '"';
    *o = '\0';
    return out;
}

// Parses one binding object ({"action": ..., ...}) for keycode.
static int parse_bind(struct jp* p, int keycode, struct bind* b) {
    memset(b, 0, sizeof *b);
    b->keycode = keycode;
    b->action = ACT_PASS;
    b->new_keycode = -1;
    char* action = NULL;
    char* command = NULL;
    char* id = NULL;
    char* params_raw = NULL;

    jp_ws(p);
    if (p->s >= p->end || *p->s != '{') return -1;
    p->s++;
    jp_ws(p);
    if (p->s < p->end && *p->s == '}') {
        p->s++;
        return 0;
    }
    while (p->s < p->end) {
        char* key = jp_string(p);
        jp_ws(p);
        if (p->err || !key || p->s >= p->end || *p->s != ':') {
            free(key);
            goto fail;
        }
        p->s++;
        jp_ws(p);
        if (!strcmp(key, "action") && *p->s == '"') {
            action = jp_string(p);
        } else if (!strcmp(key, "command") && *p->s == '"') {
            command = jp_string(p);
        } else if (!strcmp(key, "id") && *p->s == '"') {
            id = jp_string(p);
        } else if (!strcmp(key, "keycode")) {
            b->new_keycode = atoi(p->s);
            jp_skip_value(p);
        } else if (!strcmp(key, "params")) {
            const char* start = p->s;
            if (jp_skip_value(p) < 0) {
                free(key);
                goto fail;
            }
            params_raw = strndup(start, p->s - start);
        } else if (jp_skip_value(p) < 0) {
            free(key);
            goto fail;
        }
        free(key);
        jp_ws(p);
        if (p->s < p->end && *p->s == ',') {
            p->s++;
            continue;
        }
        if (p->s < p->end && *p->s == '}') {
            p->s++;
            break;
        }
        goto fail;
    }

    if (action && !strcmp(action, "replace") && b->new_keycode >= 0) {
        b->action = ACT_REPLACE;
    } else if (action && !strcmp(action, "ignore")) {
        b->action = ACT_IGNORE;
    } else if (action && !strcmp(action, "exec") && command) {
        b->action = ACT_EXEC;
        b->command = command;
        command = NULL;
    } else if (action && !strcmp(action, "launch") && id) {
        b->action = ACT_LAUNCH;
        char* qid = json_quote(id);
        size_t n = strlen(qid) + (params_raw ? strlen(params_raw) : 0) + 32;
        b->launch_json = malloc(n);
        if (params_raw)
            snprintf(b->launch_json, n, "{\"id\":%s,\"params\":%s}", qid, params_raw);
        else
            snprintf(b->launch_json, n, "{\"id\":%s}", qid);
        free(qid);
    }
    free(action);
    free(command);
    free(id);
    free(params_raw);
    return 0;

fail:
    free(action);
    free(command);
    free(id);
    free(params_raw);
    return -1;
}

static int load_binds(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        logf_("can't open %s: %s", path, strerror(errno));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 1024 * 1024) {
        fclose(f);
        logf_("%s: unexpected size %ld", path, size);
        return -1;
    }
    char* text = malloc(size);
    size_t got = fread(text, 1, size, f);
    fclose(f);

    struct jp p = {text, text + got, 0};
    struct bind* binds = NULL;
    int n = 0, cap = 0;

    jp_ws(&p);
    if (p.s >= p.end || *p.s != '{') goto fail;
    p.s++;
    jp_ws(&p);
    if (p.s < p.end && *p.s == '}') goto done;
    while (p.s < p.end) {
        char* key = jp_string(&p);
        jp_ws(&p);
        if (p.err || !key || p.s >= p.end || *p.s != ':') {
            free(key);
            goto fail;
        }
        p.s++;
        int keycode = atoi(key);
        free(key);
        if (n == cap) binds = realloc(binds, sizeof *binds * (cap = cap ? cap * 2 : 16));
        if (parse_bind(&p, keycode, &binds[n]) < 0) goto fail;
        n++;
        jp_ws(&p);
        if (p.s < p.end && *p.s == ',') {
            p.s++;
            continue;
        }
        if (p.s < p.end && *p.s == '}') break;
        goto fail;
    }

done:
    free(text);
    free_binds(g_binds, g_nbinds);
    g_binds = binds;
    g_nbinds = n;
    logf_("loaded %d keybinds from %s", n, path);
    for (int i = 0; i < n; i++) {
        static const char* names[] = {"pass", "replace", "ignore", "exec", "launch"};
        logf_("  %d: %s %s", binds[i].keycode, names[binds[i].action],
              binds[i].command ? binds[i].command : binds[i].launch_json ? binds[i].launch_json : "");
    }
    return 0;

fail:
    free(text);
    free_binds(binds, n);
    logf_("%s: parse error, keeping previous keybinds", path);
    return -1;
}

/* ---------- commands ---------- */

// Runs cmd via /bin/sh in a child process. SIGCHLD is ignored, so children
// are reaped automatically.
static void run_command(const char* cmd) {
    if (g_verbose) logf_("run: %s", cmd);
    pid_t pid = fork();
    if (pid < 0) {
        logf_("fork failed: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        // Child: don't inherit the device fds or our signal handlers.
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_DFL;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGHUP, &sa, NULL);
        sigaction(SIGCHLD, &sa, NULL);
        // Discard the command's normal output (e.g. luna-send replies);
        // keep stderr so failures still show up in our log.
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
}

static void run_exec(const struct bind* b, int state) {
    size_t n = strlen(b->command) + 16;
    char* cmd = malloc(n);
    snprintf(cmd, n, "%s %d", b->command, state);
    run_command(cmd);
    free(cmd);
}

static void run_launch(const struct bind* b) {
    // Single-quote the payload for sh; escape any single quotes inside.
    const char* prefix = "luna-send -n 1 luna://com.webos.applicationManager/launch '";
    size_t n = strlen(prefix) + strlen(b->launch_json) * 4 + 4;
    char* cmd = malloc(n);
    char* o = cmd + sprintf(cmd, "%s", prefix);
    for (const char* s = b->launch_json; *s; s++) {
        if (*s == '\'') o += sprintf(o, "'\\''");
        else *o++ = *s;
    }
    *o++ = '\'';
    *o = '\0';
    run_command(cmd);
    free(cmd);
}

/* ---------- devices ---------- */

struct device {
    int src;          // grabbed original, -1 when the slot is free
    int ui;           // our twin
    char name[256];
    char path[32];    // original's event node
    char twin[32];    // twin's event node, so rescans never grab it
};

static struct device g_devs[MAX_DEVICES];
static const char* g_patterns[MAX_PATTERNS];
static int g_npatterns = 0;

// Same request as libim's IM_RequestToCheckInput(): one 84-byte message
// {type = 1, pid} on the /IM_MQ_WEBOS queue makes libim rescan /dev/input.
static int request_input_rescan(void) {
    mqd_t mq = mq_open("/IM_MQ_WEBOS", O_WRONLY);
    if (mq == (mqd_t)-1) {
        logf_("mq_open(/IM_MQ_WEBOS) failed: %s", strerror(errno));
        return -1;
    }
    unsigned char msg[0x54];
    memset(msg, 0, sizeof msg);
    int32_t type = 1;
    int32_t pid = getpid();
    memcpy(msg, &type, 4);
    memcpy(msg + 4, &pid, 4);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    int rc = mq_timedsend(mq, (const char*)msg, sizeof msg, 0, &deadline);
    if (rc != 0) logf_("mq_timedsend failed: %s", strerror(errno));
    mq_close(mq);
    return rc;
}

static int name_matches(const char* name) {
    for (int p = 0; p < g_npatterns; p++) {
        if (strstr(name, g_patterns[p])) return 1;
    }
    return 0;
}

// True if path is one of our devices: an original we already hold, or one
// of our twins (which carry the same names as the originals).
static int is_ours(const char* path) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_devs[i].src < 0) continue;
        if (!strcmp(g_devs[i].path, path) || !strcmp(g_devs[i].twin, path)) return 1;
    }
    return 0;
}

// Finds the /dev/input/eventN node of the uinput device behind ui.
static void twin_event_node(int ui, char* out, size_t len) {
    out[0] = '\0';
    char sysname[64] = {0};
    if (ioctl(ui, UI_GET_SYSNAME(sizeof sysname), sysname) < 0) {
        logf_("UI_GET_SYSNAME failed: %s", strerror(errno));
        return;
    }
    // /sys/devices/virtual/input/inputN/ contains an eventM entry.
    for (int i = 0; i < 64; i++) {
        char p[128];
        snprintf(p, sizeof p, "/sys/devices/virtual/input/%s/event%d", sysname, i);
        if (access(p, F_OK) == 0) {
            snprintf(out, len, "/dev/input/event%d", i);
            return;
        }
    }
    logf_("no event node found for twin %s", sysname);
}

// Create a uinput device mirroring src: name, IDs, event types and every
// capability bit, plus absolute-axis ranges. All key bits the kernel
// supports are enabled, so "replace" can emit any key code.
static int create_twin(int src, const char* name) {
    struct input_id id;
    memset(&id, 0, sizeof id);
    ioctl(src, EVIOCGID, &id);

    int ui = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (ui < 0) {
        logf_("open /dev/uinput failed: %s", strerror(errno));
        return -1;
    }

    unsigned char types[BITS_BYTES];
    memset(types, 0, sizeof types);
    if (ioctl(src, EVIOCGBIT(0, sizeof types), types) < 0) {
        logf_("EVIOCGBIT(0) failed: %s", strerror(errno));
        close(ui);
        return -1;
    }

    struct uinput_user_dev dev;
    memset(&dev, 0, sizeof dev);
    snprintf(dev.name, sizeof dev.name, "%s", name);
    dev.id = id;

    static const struct {
        int type;
        unsigned long set_ioctl;
    } kinds[] = {
        {EV_KEY, UI_SET_KEYBIT}, {EV_REL, UI_SET_RELBIT}, {EV_ABS, UI_SET_ABSBIT},
        {EV_MSC, UI_SET_MSCBIT}, {EV_LED, UI_SET_LEDBIT}, {EV_SND, UI_SET_SNDBIT},
        {EV_SW, UI_SET_SWBIT},
    };

    for (int t = 0; t < EV_MAX; t++) {
        if (test_bit(types, t)) ioctl(ui, UI_SET_EVBIT, t);
    }

    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        if (!test_bit(types, kinds[k].type)) continue;
        unsigned char bits[BITS_BYTES];
        memset(bits, 0, sizeof bits);
        int len = ioctl(src, EVIOCGBIT(kinds[k].type, sizeof bits), bits);
        if (len < 0) continue;
        for (int b = 0; b < len * 8; b++) {
            int want = test_bit(bits, b) || kinds[k].type == EV_KEY;
            if (!want || ioctl(ui, kinds[k].set_ioctl, b) < 0) continue;
            if (kinds[k].type == EV_ABS && b < ABS_CNT) {
                struct input_absinfo ai;
                if (ioctl(src, EVIOCGABS(b), &ai) == 0) {
                    dev.absmin[b] = ai.minimum;
                    dev.absmax[b] = ai.maximum;
                    dev.absfuzz[b] = ai.fuzz;
                    dev.absflat[b] = ai.flat;
                }
            }
        }
    }

    unsigned char props[BITS_BYTES];
    memset(props, 0, sizeof props);
    int plen = ioctl(src, EVIOCGPROP(sizeof props), props);
    for (int b = 0; plen > 0 && b < plen * 8; b++) {
        if (test_bit(props, b)) ioctl(ui, UI_SET_PROPBIT, b);
    }

    if (write(ui, &dev, sizeof dev) != (ssize_t)sizeof dev || ioctl(ui, UI_DEV_CREATE) < 0) {
        logf_("creating twin \"%s\" failed: %s", name, strerror(errno));
        close(ui);
        return -1;
    }
    return ui;
}

static void release_device(struct device* d) {
    if (d->src >= 0) {
        ioctl(d->src, EVIOCGRAB, 0);
        close(d->src);
    }
    if (d->ui >= 0) {
        ioctl(d->ui, UI_DEV_DESTROY);
        close(d->ui);
    }
    d->src = d->ui = -1;
    d->path[0] = d->twin[0] = '\0';
}

// Scans /dev/input for matching devices we don't hold yet, creates twins
// for them, asks libim to pick the twins up and then grabs the originals.
// Returns the number of devices taken over.
static int scan_devices(void) {
    int pending[MAX_DEVICES];
    int npending = 0;

    for (int i = 0; i < 64; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        if (is_ours(path)) continue;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char name[256] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0 || !name_matches(name)) {
            close(fd);
            continue;
        }
        int slot = -1;
        for (int s = 0; s < MAX_DEVICES; s++) {
            if (g_devs[s].src < 0) {
                slot = s;
                break;
            }
        }
        if (slot < 0) {
            logf_("too many devices, ignoring %s \"%s\"", path, name);
            close(fd);
            break;
        }
        struct device* d = &g_devs[slot];
        d->src = fd;
        snprintf(d->name, sizeof d->name, "%s", name);
        snprintf(d->path, sizeof d->path, "%s", path);
        d->ui = create_twin(fd, name);
        if (d->ui < 0) {
            release_device(d);
            continue;
        }
        twin_event_node(d->ui, d->twin, sizeof d->twin);
        logf_("found %s \"%s\", twin %s", path, name, d->twin);
        pending[npending++] = slot;
    }
    if (npending == 0) return 0;

    // Let the kernel publish the twins, then have libim pick them up before
    // we silence the originals.
    usleep(200 * 1000);
    request_input_rescan();
    usleep(500 * 1000);

    for (int k = 0; k < npending; k++) {
        struct device* d = &g_devs[pending[k]];
        if (ioctl(d->src, EVIOCGRAB, 1) < 0) {
            logf_("EVIOCGRAB %s failed: %s", d->path, strerror(errno));
            release_device(d);
            continue;
        }
        logf_("grabbed %s \"%s\"", d->path, d->name);
    }
    return npending;
}

/* ---------- event handling ---------- */

static const char* state_name(int v) {
    return v == 1 ? "press" : v == 0 ? "release" : "repeat";
}

// Applies keybinds to one batch read from a device. Returns the number of
// events kept in buf (modified in place).
static int handle_events(struct ev* buf, int count) {
    int out = 0;
    for (int i = 0; i < count; i++) {
        struct ev e = buf[i];
        if (e.type == EV_KEY) {
            const struct bind* b = find_bind(e.code);
            if (g_verbose) logf_("key %u %s%s", e.code, state_name(e.value), b ? " (bound)" : "");
            if (b) {
                switch (b->action) {
                case ACT_REPLACE:
                    e.code = b->new_keycode;
                    break;
                case ACT_IGNORE:
                    continue;
                case ACT_EXEC:
                    if (e.value == 0 || e.value == 1) run_exec(b, e.value);
                    continue;
                case ACT_LAUNCH:
                    if (e.value == 1) run_launch(b);
                    continue;
                case ACT_PASS:
                    break;
                }
            }
        }
        buf[out++] = e;
    }
    return out;
}

static volatile sig_atomic_t g_reload = 0;

static void on_sighup(int sig) {
    (void)sig;
    g_reload = 1;
}

int main(int argc, char** argv) {
    const char* config = DEFAULT_CONFIG;
    int opt;
    while ((opt = getopt(argc, argv, "n:c:v")) != -1) {
        if (opt == 'n' && g_npatterns < MAX_PATTERNS) g_patterns[g_npatterns++] = optarg;
        else if (opt == 'c') config = optarg;
        else if (opt == 'v') g_verbose = 1;
        else {
            fprintf(stderr, "usage: %s [-n name-substring]... [-c keybinds.json] [-v]\n", argv[0]);
            return 2;
        }
    }
    if (g_npatterns == 0) {
        g_patterns[g_npatterns++] = "LGE M-RCU - Builtin";
        g_patterns[g_npatterns++] = "LGE RCU";
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_sighup;
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);

    for (int i = 0; i < MAX_DEVICES; i++) {
        g_devs[i].src = g_devs[i].ui = -1;
        g_devs[i].path[0] = g_devs[i].twin[0] = '\0';
    }

    load_binds(config);
    struct stat st;
    time_t config_mtime = stat(config, &st) == 0 ? st.st_mtime : 0;

    // inotify on /dev/input tells us when remotes (re)appear; a periodic
    // rescan covers anything inotify misses.
    int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino < 0 || inotify_add_watch(ino, "/dev/input", IN_CREATE | IN_DELETE) < 0) {
        logf_("inotify on /dev/input unavailable (%s), using periodic rescans only", strerror(errno));
        if (ino >= 0) close(ino);
        ino = -1;
    }

    if (scan_devices() == 0) logf_("no matching input devices yet, waiting");

    unsigned long forwarded = 0;
    time_t last_scan = time(NULL);
    while (!g_stop) {
        struct pollfd pfds[MAX_DEVICES + 1];
        int map[MAX_DEVICES + 1];
        int n = 0;
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (g_devs[i].src < 0) continue;
            pfds[n].fd = g_devs[i].src;
            pfds[n].events = POLLIN;
            map[n++] = i;
        }
        if (ino >= 0) {
            pfds[n].fd = ino;
            pfds[n].events = POLLIN;
            map[n++] = -1;
        }

        int rc = poll(pfds, n, 1000);
        if (rc < 0 && errno != EINTR) {
            logf_("poll failed: %s", strerror(errno));
            break;
        }

        if (g_reload || (stat(config, &st) == 0 && st.st_mtime != config_mtime)) {
            g_reload = 0;
            config_mtime = stat(config, &st) == 0 ? st.st_mtime : 0;
            load_binds(config);
        }

        int rescan = 0;
        for (int k = 0; rc > 0 && k < n; k++) {
            if (!(pfds[k].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            if (map[k] < 0) {
                // Drain inotify; any create/delete under /dev/input triggers a rescan.
                char ibuf[4096];
                while (read(ino, ibuf, sizeof ibuf) > 0) {
                }
                rescan = 1;
                continue;
            }
            struct device* d = &g_devs[map[k]];
            struct ev buf[64];
            ssize_t got = read(d->src, buf, sizeof buf);
            if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (got <= 0 || got % (ssize_t)sizeof(struct ev) != 0) {
                logf_("%s \"%s\" gone (read %zd: %s), releasing", d->path, d->name, got,
                      got < 0 ? strerror(errno) : "");
                release_device(d);
                request_input_rescan();
                rescan = 1;
                continue;
            }
            int kept = handle_events(buf, got / sizeof(struct ev));
            if (kept > 0 && write(d->ui, buf, kept * sizeof(struct ev)) < 0)
                logf_("forward to twin of %s failed: %s", d->path, strerror(errno));
            forwarded += kept;
        }

        if (rescan || time(NULL) - last_scan >= 10) {
            // New nodes may need a moment before their name is readable.
            if (rescan) usleep(100 * 1000);
            scan_devices();
            last_scan = time(NULL);
        }
    }

    logf_("stopping: forwarded %lu events", forwarded);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_devs[i].src >= 0) release_device(&g_devs[i]);
    }
    request_input_rescan();
    free_binds(g_binds, g_nbinds);
    return 0;
}
