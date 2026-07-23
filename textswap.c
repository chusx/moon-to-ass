// textswap.so — LD interposition library that rewrites text in kimi's
// terminal output. Interposes write(2)/writev(2) and applies substring,
// case-insensitive replacements to bytes written to a TTY fd. Piped
// output is passed through untouched.
//
// Replacements are read from $KIMI_TEXTSWAP_CONF or
// ~/.kimi-code/textswap/replacements.txt, one per line:  from => to
// Matching is a plain substring match: "moonshot" also matches inside
// "moonshotai" or "unmoonshotable".
//
// Build:  gcc -shared -fPIC -O2 -o textswap.so textswap.c -ldl
#define _GNU_SOURCE
#include <dlfcn.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>

typedef ssize_t (*write_fn)(int, const void *, size_t);
typedef ssize_t (*writev_fn)(int, const struct iovec *, int);

static write_fn real_write;
static writev_fn real_writev;

struct repl {
    char *from;
    char *to;
    size_t from_len;
    size_t to_len;
};

static struct repl *repls;
static size_t nrepls;
static size_t max_from_len;

// Bytes held back because they may complete a match once the next chunk
// arrives. Prepended to the next filtered write.
static unsigned char hold[4096];
static size_t hold_len;

static int ci_equal(const unsigned char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (tolower(a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

static void load_replacements(void) {
    const char *path = getenv("KIMI_TEXTSWAP_CONF");
    char def[4096];
    if (!path) {
        const char *home = getenv("HOME");
        if (!home) return;
        snprintf(def, sizeof def, "%s/.kimi-code/textswap/replacements.txt", home);
        path = def;
    }
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[4096];
    size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        char *sep = strstr(line, " => ");
        if (!sep) continue;
        *sep = '\0';
        char *from = line, *to = sep + 4;
        to[strcspn(to, "\r\n")] = '\0';
        if (!*from || !*to || *from == '#') continue;
        if (nrepls == cap) {
            cap = cap ? cap * 2 : 16;
            repls = realloc(repls, cap * sizeof *repls);
        }
        repls[nrepls].from = strdup(from);
        repls[nrepls].to = strdup(to);
        repls[nrepls].from_len = strlen(from);
        repls[nrepls].to_len = strlen(to);
        if (repls[nrepls].from_len > max_from_len)
            max_from_len = repls[nrepls].from_len;
        nrepls++;
    }
    fclose(f);
    // Longest keys first so overlapping phrases match greedily.
    for (size_t i = 0; i < nrepls; i++)
        for (size_t j = i + 1; j < nrepls; j++)
            if (repls[j].from_len > repls[i].from_len) {
                struct repl t = repls[i];
                repls[i] = repls[j];
                repls[j] = t;
            }
}

static void flush_hold(void) {
    if (hold_len && real_write) {
        ssize_t w = real_write(STDOUT_FILENO, hold, hold_len);
        (void)w;
    }
    hold_len = 0;
}

__attribute__((constructor)) static void init(void) {
    real_write = (write_fn)dlsym(RTLD_NEXT, "write");
    real_writev = (writev_fn)dlsym(RTLD_NEXT, "writev");
    load_replacements();
    atexit(flush_hold);
}

// Returns total bytes written to fd (fully drained), or -1 on error.
static ssize_t write_all(int fd, const unsigned char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = real_write(fd, buf + off, len - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return (ssize_t)len;
}

// Apply replacements to buf[0..len). May retain a tail in `hold`.
// Writes the transformed result to fd. Returns nothing; caller reports
// the original input length to its own caller.
static void filter_and_write(int fd, const unsigned char *buf, size_t len) {
    // Prepend any held bytes.
    unsigned char stack_in[8192];
    const unsigned char *in = buf;
    size_t in_len = len;
    if (hold_len) {
        if (hold_len + len <= sizeof stack_in) {
            memcpy(stack_in, hold, hold_len);
            memcpy(stack_in + hold_len, buf, len);
            in = stack_in;
        } else {
            unsigned char *heap = malloc(hold_len + len);
            if (!heap) { write_all(fd, buf, len); hold_len = 0; return; }
            memcpy(heap, hold, hold_len);
            memcpy(heap + hold_len, buf, len);
            in = heap;
        }
        in_len = hold_len + len;
        hold_len = 0;
    }

    size_t out_cap = in_len * 2 + 256;
    unsigned char *out = malloc(out_cap);
    if (!out) { write_all(fd, in, in_len); goto done; }
    size_t out_len = 0, i = 0;

    while (i < in_len) {
        int matched = 0, stop = 0;
        for (size_t r = 0; r < nrepls; r++) {
            size_t fl = repls[r].from_len;
            size_t avail = in_len - i;
            size_t cmp = avail < fl ? avail : fl;
            if (cmp == 0 || !ci_equal(in + i, repls[r].from, cmp))
                continue;
            if (avail < fl) {
                // Proper prefix of a key at the chunk end: the rest may
                // arrive in the next write, so hold it back.
                stop = 1;
                break;
            }
            if (out_len + repls[r].to_len > out_cap) {
                out_cap = (out_len + repls[r].to_len) * 2;
                out = realloc(out, out_cap);
            }
            memcpy(out + out_len, repls[r].to, repls[r].to_len);
            out_len += repls[r].to_len;
            i += fl;
            matched = 1;
            break;
        }
        if (stop) break;
        if (matched) continue;
        if (out_len + 1 > out_cap) {
            out_cap *= 2;
            out = realloc(out, out_cap);
        }
        out[out_len++] = in[i++];
    }

    if (i < in_len && in_len - i <= sizeof hold) {
        memcpy(hold, in + i, in_len - i);
        hold_len = in_len - i;
    } else if (i < in_len) {
        // Tail too large to hold (shouldn't happen); emit unfiltered.
        memcpy(out + out_len, in + i, in_len - i);
        out_len += in_len - i;
    }

    write_all(fd, out, out_len);
    free(out);

done:
    if (in != buf && in != stack_in) free((void *)in);
}

static int filter_fd(int fd) {
    // kimi's runtime dups the terminal to a high fd, so match any TTY,
    // not just stdout/stderr.
    return nrepls > 0 && isatty(fd);
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (!real_write)
        real_write = (write_fn)dlsym(RTLD_NEXT, "write");
    if (count == 0 || !filter_fd(fd))
        return real_write(fd, buf, count);
    filter_and_write(fd, buf, count);
    return (ssize_t)count; // report the caller's bytes as consumed
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    if (!real_writev)
        real_writev = (writev_fn)dlsym(RTLD_NEXT, "writev");
    if (iovcnt <= 0 || !filter_fd(fd))
        return real_writev(fd, iov, iovcnt);
    size_t total = 0;
    for (int k = 0; k < iovcnt; k++) total += iov[k].iov_len;
    if (total == 0) return real_writev(fd, iov, iovcnt);
    unsigned char stack_flat[8192];
    unsigned char *flat = stack_flat;
    if (total > sizeof stack_flat) flat = malloc(total);
    if (!flat) return real_writev(fd, iov, iovcnt);
    size_t off = 0;
    for (int k = 0; k < iovcnt; k++) {
        memcpy(flat + off, iov[k].iov_base, iov[k].iov_len);
        off += iov[k].iov_len;
    }
    filter_and_write(fd, flat, total);
    if (flat != stack_flat) free(flat);
    return (ssize_t)total;
}