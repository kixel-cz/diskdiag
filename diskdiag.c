/*
 * diskdiag - Disk read latency diagnostic tool with ASCII heatmap
 *
 * Usage: sudo diskdiag [OPTIONS] <device>
 *
 * Exit codes:
 *   0  HEALTHY  (< 1 % slow blocks, no errors)
 *   1  GOOD     (1-5 % slow blocks)
 *   2  FAIR     (> 5 % slow blocks)
 *   3  POOR     (read errors or latency >= error threshold)
 *   4  runtime error (bad arguments, cannot open device, ...)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <limits.h>

/* ── Exit codes ─────────────────────────────────────────────────── */
#define EXIT_HEALTHY  0
#define EXIT_GOOD     1
#define EXIT_FAIR     2
#define EXIT_POOR     3
#define EXIT_ERROR    4

/* ── Defaults ───────────────────────────────────────────────────── */
#define DEFAULT_BLOCK_MB    1
#define DEFAULT_WARN_MS     20.0
#define DEFAULT_ERROR_MS    150.0
#define HEATMAP_COLS        72
#define HEATMAP_ROWS        18

/* ── ANSI colours ───────────────────────────────────────────────── */
#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_RED     "\033[31m"
#define COL_CYAN    "\033[36m"
#define COL_MAGENTA "\033[35m"
#define COL_GREY    "\033[90m"

/* ── Options ────────────────────────────────────────────────────── */
typedef struct {
    const char *device;
    size_t      block_mb;
    uint64_t    max_blocks;      /* 0 = entire disk */
    uint64_t    offset_blocks;   /* start offset in blocks */
    double      warn_ms;
    double      error_ms;
    int         yes;             /* -y: skip mount warning prompt */
    int         quiet;           /* -q: no progress bar or heatmap */
    int         json;            /* -j: JSON output */
    int         no_color;        /* --no-color */
} opts_t;

/* ── Globals (signal handler) ───────────────────────────────────── */
static volatile int g_stop   = 0;
static uint64_t     g_n_read = 0;

/* ── Colour helper ──────────────────────────────────────────────── */
static int g_color = 1;
static const char *C(const char *code) { return g_color ? code : ""; }

/* ── Helpers ────────────────────────────────────────────────────── */
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

static double percentile(double *sorted, uint64_t n, double pct)
{
    if (n == 0) return 0.0;
    double   idx = (pct / 100.0) * (double)(n - 1);
    uint64_t lo  = (uint64_t)idx;
    uint64_t hi  = lo + 1;
    if (hi >= n) return sorted[n - 1];
    return sorted[lo] + (idx - (double)lo) * (sorted[hi] - sorted[lo]);
}

static void human_size(uint64_t bytes, char *buf, size_t buflen)
{
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    snprintf(buf, buflen, "%.2f %s", v, units[u]);
}

/* ── Progress bar ───────────────────────────────────────────────── */
static void progress_bar(uint64_t done, uint64_t total, double cur_ms)
{
    int    width  = 40;
    double pct    = (total > 0) ? (double)done / (double)total : 0.0;
    int    filled = (int)(pct * width);

    printf("\r%s[", C(COL_BOLD));
    for (int i = 0; i < width; i++)
        printf("%s", i < filled ? "█" : "░");
    printf("%s] %5.1f%%  cur:%6.1f ms%s",
           C(COL_RESET), pct * 100.0, cur_ms, C(COL_RESET));
    fflush(stdout);
}

/* ── ASCII Heatmap ──────────────────────────────────────────────── */
static const char *cell_colour(double ms, const opts_t *o)
{
    if (ms < 0)              return C(COL_MAGENTA);
    if (ms == 0)             return C(COL_GREY);
    if (ms < o->warn_ms)     return C(COL_GREEN);
    if (ms < o->warn_ms * 3) return C(COL_YELLOW);
    if (ms < o->error_ms)    return C(COL_RED);
    return C(COL_MAGENTA);
}

static char cell_char(double ms, const opts_t *o)
{
    if (ms < 0)                return 'E';
    if (ms == 0)               return ' ';
    if (ms < o->warn_ms / 4)   return '.';
    if (ms < o->warn_ms / 2)   return 'o';
    if (ms < o->warn_ms)       return '*';
    if (ms < o->error_ms / 3)  return '#';
    if (ms < o->error_ms)      return 'X';
    return '!';
}

static void draw_heatmap(double *times, uint64_t n_total,
                         uint64_t n_read, uint64_t block_bytes,
                         const opts_t *o)
{
    int cells = HEATMAP_COLS * HEATMAP_ROWS;

    double *bucket = calloc((size_t)cells, sizeof(double));
    int    *bcount = calloc((size_t)cells, sizeof(int));
    if (!bucket || !bcount) { free(bucket); free(bcount); return; }

    for (uint64_t i = 0; i < n_read; i++) {
        int cell = (int)((double)i / (double)n_total * cells);
        if (cell >= cells) cell = cells - 1;
        bucket[cell] += times[i];
        bcount[cell]++;
    }
    for (int c = 0; c < cells; c++)
        if (bcount[c]) bucket[c] /= bcount[c];

    char sz[32];
    uint64_t cell_bytes = (uint64_t)((double)n_total / cells) * block_bytes;
    human_size(cell_bytes, sz, sizeof(sz));
    printf("\n%s%s  Heatmap  (each cell ≈ %s)%s\n",
           C(COL_BOLD), C(COL_CYAN), sz, C(COL_RESET));

    printf("%s  ┌", C(COL_GREY));
    for (int c = 0; c < HEATMAP_COLS; c++) printf("─");
    printf("┐\n%s", C(COL_RESET));

    for (int r = 0; r < HEATMAP_ROWS; r++) {
        printf("%s  │%s", C(COL_GREY), C(COL_RESET));
        for (int c = 0; c < HEATMAP_COLS; c++) {
            int    idx = r * HEATMAP_COLS + c;
            double ms  = (idx < cells) ? bucket[idx] : 0.0;
            printf("%s%c%s", cell_colour(ms, o), cell_char(ms, o), C(COL_RESET));
        }
        printf("%s│\n%s", C(COL_GREY), C(COL_RESET));
    }

    printf("%s  └", C(COL_GREY));
    for (int c = 0; c < HEATMAP_COLS; c++) printf("─");
    printf("┘\n%s", C(COL_RESET));

    printf("  Legend: %s' '%s unread  "
           "%s'.' fast  '*' <%.0fms%s  "
           "%s'#' slow%s  "
           "%s'X' <%.0fms%s  "
           "%s'!' ≥%.0fms / 'E' error%s\n",
           C(COL_GREY),    C(COL_RESET),
           C(COL_GREEN),   o->warn_ms,  C(COL_RESET),
           C(COL_YELLOW),               C(COL_RESET),
           C(COL_RED),     o->error_ms, C(COL_RESET),
           C(COL_MAGENTA), o->error_ms, C(COL_RESET));

    free(bucket);
    free(bcount);
}

/* ── Health rating ──────────────────────────────────────────────── */
typedef enum { HEALTH_HEALTHY=0, HEALTH_GOOD, HEALTH_FAIR, HEALTH_POOR } health_t;

static health_t health_rating(uint64_t n, uint64_t n_slow, uint64_t n_error)
{
    if (n_error > 0) return HEALTH_POOR;
    double slow_pct = (n > 0) ? (double)n_slow / (double)n * 100.0 : 0.0;
    if (slow_pct > 5.0) return HEALTH_FAIR;
    if (slow_pct > 1.0) return HEALTH_GOOD;
    return HEALTH_HEALTHY;
}

static const char *health_str(health_t h)
{
    switch (h) {
        case HEALTH_HEALTHY: return "HEALTHY";
        case HEALTH_GOOD:    return "GOOD";
        case HEALTH_FAIR:    return "FAIR";
        case HEALTH_POOR:    return "POOR";
    }
    return "UNKNOWN";
}

static const char *health_col(health_t h)
{
    switch (h) {
        case HEALTH_HEALTHY: return COL_GREEN;
        case HEALTH_GOOD:    return COL_YELLOW;
        case HEALTH_FAIR:    return COL_YELLOW;
        case HEALTH_POOR:    return COL_RED;
    }
    return COL_RESET;
}

/* ── Human-readable statistics ──────────────────────────────────── */
static void print_stats(double *times, uint64_t n, double elapsed_s,
                        uint64_t block_bytes, uint64_t n_slow,
                        uint64_t n_error, const opts_t *o)
{
    if (n == 0) { printf("No data.\n"); return; }

    double *sorted = malloc(n * sizeof(double));
    if (!sorted) return;
    memcpy(sorted, times, n * sizeof(double));
    qsort(sorted, n, sizeof(double), cmp_double);

    double sum = 0;
    for (uint64_t i = 0; i < n; i++) sum += times[i];
    double avg = sum / (double)n;

    double total_bytes = (double)n * (double)block_bytes;
    double speed_mbs   = (elapsed_s > 0)
                       ? (total_bytes / 1024.0 / 1024.0) / elapsed_s : 0.0;
    char sz_read[32];
    human_size((uint64_t)total_bytes, sz_read, sizeof(sz_read));

    double   slow_pct  = (double)n_slow  / (double)n * 100.0;
    double   error_pct = (double)n_error / (double)n * 100.0;
    health_t h = health_rating(n, n_slow, n_error);

    printf("\n%s%s══════════════════ Results ══════════════════%s\n",
           C(COL_BOLD), C(COL_CYAN), C(COL_RESET));
    printf("  Blocks read  : %lu  (%s)\n", (unsigned long)n, sz_read);
    printf("  Elapsed      : %.1f s\n", elapsed_s);
    printf("  Throughput   : %.1f MiB/s\n", speed_mbs);
    printf("%s──────────────── Latency (ms) ───────────────%s\n",
           C(COL_BOLD), C(COL_RESET));
    printf("  Min          : %.2f ms\n", sorted[0]);
    printf("  Avg          : %.2f ms\n", avg);
    printf("  Median (P50) : %.2f ms\n", percentile(sorted, n, 50));
    printf("  P90          : %.2f ms\n", percentile(sorted, n, 90));
    printf("  P99          : %.2f ms\n", percentile(sorted, n, 99));
    printf("  Max          : %.2f ms\n", sorted[n - 1]);
    printf("%s──────────────── Health ──────────────────────%s\n",
           C(COL_BOLD), C(COL_RESET));
    printf("  Warn  threshold : %.0f ms\n", o->warn_ms);
    printf("  Error threshold : %.0f ms\n", o->error_ms);
    printf("  Slow blocks  : %lu  (%.2f%%)\n",
           (unsigned long)n_slow, slow_pct);
    printf("  Error blocks : %lu  (%.2f%%)\n",
           (unsigned long)n_error, error_pct);
    printf("%s  Disk health  : %s%s%s%s\n",
           C(COL_BOLD), C(health_col(h)), health_str(h),
           C(COL_RESET), C(COL_RESET));
    printf("%s%s═════════════════════════════════════════════%s\n",
           C(COL_CYAN), C(COL_BOLD), C(COL_RESET));

    free(sorted);
}

/* ── JSON helpers ───────────────────────────────────────────────── */
/*
 * Print a string as a JSON value, escaping characters that would
 * break JSON structure: backslash, double-quote, and control chars.
 */
static void json_print_string(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if      (ch == '"')  printf("\\\"");
        else if (ch == '\\') printf("\\\\");
        else if (ch == '\n') printf("\\n");
        else if (ch == '\r') printf("\\r");
        else if (ch == '\t') printf("\\t");
        else if (ch < 0x20)  printf("\\u%04x", ch); /* other controls */
        else                 putchar(ch);
    }
    putchar('"');
}

/* ── JSON output ────────────────────────────────────────────────── */
static void print_json(double *times, uint64_t n, double elapsed_s,
                       uint64_t block_bytes, uint64_t n_slow,
                       uint64_t n_error, const opts_t *o)
{
    if (n == 0) { printf("{\"error\":\"no data\"}\n"); return; }

    double *sorted = malloc(n * sizeof(double));
    if (!sorted) return;
    memcpy(sorted, times, n * sizeof(double));
    qsort(sorted, n, sizeof(double), cmp_double);

    double sum = 0;
    for (uint64_t i = 0; i < n; i++) sum += times[i];
    double avg = sum / (double)n;

    double   total_bytes = (double)n * (double)block_bytes;
    double   speed_mbs   = (elapsed_s > 0)
                         ? (total_bytes / 1024.0 / 1024.0) / elapsed_s : 0.0;
    double   slow_pct    = (double)n_slow  / (double)n * 100.0;
    double   error_pct   = (double)n_error / (double)n * 100.0;
    health_t h = health_rating(n, n_slow, n_error);

    printf("{\n");
    printf("  \"device\": ");
    json_print_string(o->device);
    printf(",\n");
    printf("  \"block_mib\": %zu,\n", o->block_mb);
    printf("  \"offset_blocks\": %lu,\n", (unsigned long)o->offset_blocks);
    printf("  \"blocks_read\": %lu,\n", (unsigned long)n);
    printf("  \"bytes_read\": %.0f,\n", total_bytes);
    printf("  \"elapsed_s\": %.3f,\n", elapsed_s);
    printf("  \"throughput_mib_s\": %.2f,\n", speed_mbs);
    printf("  \"latency_ms\": {\n");
    printf("    \"min\": %.3f,\n", sorted[0]);
    printf("    \"avg\": %.3f,\n", avg);
    printf("    \"p50\": %.3f,\n", percentile(sorted, n, 50));
    printf("    \"p90\": %.3f,\n", percentile(sorted, n, 90));
    printf("    \"p99\": %.3f,\n", percentile(sorted, n, 99));
    printf("    \"max\": %.3f\n",  sorted[n - 1]);
    printf("  },\n");
    printf("  \"thresholds_ms\": {\n");
    printf("    \"warn\":  %.0f,\n", o->warn_ms);
    printf("    \"error\": %.0f\n",  o->error_ms);
    printf("  },\n");
    printf("  \"slow_blocks\":  %lu,\n", (unsigned long)n_slow);
    printf("  \"error_blocks\": %lu,\n", (unsigned long)n_error);
    printf("  \"slow_pct\":  %.4f,\n", slow_pct);
    printf("  \"error_pct\": %.4f,\n", error_pct);
    printf("  \"health\": \"%s\",\n", health_str(h));
    printf("  \"exit_code\": %d\n", (int)h);
    printf("}\n");

    free(sorted);
}

/* ── Mount check ────────────────────────────────────────────────── */
static int check_mounted(const char *dev, int force)
{
    char real_dev[PATH_MAX];
    if (!realpath(dev, real_dev)) {
        strncpy(real_dev, dev, PATH_MAX - 1);
        real_dev[PATH_MAX - 1] = '\0';
    }

    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;

    char line[1024];
    int  count = 0;
    char matches[16][PATH_MAX * 2];

    while (fgets(line, sizeof(line), f) && count < 16) {
        char mdev[256], mpoint[256];
        if (sscanf(line, "%255s %255s", mdev, mpoint) != 2) continue;
        if (mdev[0] != '/') continue;

        char real_mdev[PATH_MAX];
        if (!realpath(mdev, real_mdev)) {
            strncpy(real_mdev, mdev, PATH_MAX - 1);
            real_mdev[PATH_MAX - 1] = '\0';
        }

        size_t devlen = strlen(real_dev);
        if (strncmp(real_mdev, real_dev, devlen) == 0)
            snprintf(matches[count++], PATH_MAX * 2,
                     "    %s  ->  %s", real_mdev, mpoint);
    }
    fclose(f);

    if (count == 0) return 0;

    /* Always print warning to stderr so it appears even with -j */
    fprintf(stderr, "%s%s\n  WARNING: Device is currently mounted:%s\n",
            C(COL_YELLOW), C(COL_BOLD), C(COL_RESET));
    for (int i = 0; i < count; i++)
        fprintf(stderr, "%s%s\n%s", C(COL_YELLOW), matches[i], C(COL_RESET));
    fprintf(stderr,
        "\n"
        "  Read-only access cannot corrupt data, but latency results\n"
        "  may be skewed by background OS I/O (journaling, writeback,\n"
        "  prefetch). For accurate diagnostics use a live USB.\n\n");

    if (force) {
        fprintf(stderr, "  Continuing due to -y / --yes.\n\n");
        return 1;
    }

    fprintf(stderr, "  Continue anyway? [y/N] ");
    fflush(stderr);

    int answer = getchar();
    int c;
    while ((c = getchar()) != '\n' && c != EOF) ;
    if (answer != 'y' && answer != 'Y') {
        fprintf(stderr, "Aborted.\n\n");
        return -1;
    }
    fprintf(stderr, "\n");
    return 1;
}

/* ── Signal handler ─────────────────────────────────────────────── */
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

/* ── Usage ──────────────────────────────────────────────────────── */
static void usage(const char *prog)
{
    printf(
        "Usage: %s [OPTIONS] <device>\n"
        "\n"
        "Measure sequential read latency across a block device.\n"
        "\n"
        "Options:\n"
        "  -b, --block-size <MiB>      Read block size, 1-1024 (default: %d)\n"
        "  -n, --blocks <N>            Test only the first N blocks\n"
        "  -o, --offset <N>            Start at block offset N\n"
        "      --threshold-warn <ms>   Warn threshold (default: %.0f ms)\n"
        "      --threshold-error <ms>  Error threshold (default: %.0f ms)\n"
        "  -y, --yes                   Skip mounted-device prompt\n"
        "  -q, --quiet                 Suppress progress bar and heatmap;\n"
        "                              print only the statistics table\n"
        "  -j, --json                  Output results as JSON (implies --quiet)\n"
        "      --no-color              Disable ANSI colour output\n"
        "  -h, --help                  Show this help\n"
        "\n"
        "Exit codes:\n"
        "  0  HEALTHY  (< 1 %% slow blocks, no errors)\n"
        "  1  GOOD     (1-5 %% slow blocks)\n"
        "  2  FAIR     (> 5 %% slow blocks)\n"
        "  3  POOR     (read errors or latency >= error threshold)\n"
        "  4  Runtime error (bad arguments, cannot open device, ...)\n"
        "\n"
        "Examples:\n"
        "  sudo diskdiag /dev/sdb\n"
        "  sudo diskdiag -b 4 -n 1000 /dev/sdb\n"
        "  sudo diskdiag -o 500 -n 500 /dev/sdb\n"
        "  sudo diskdiag -y -q --json /dev/sdb > result.json\n"
        "  sudo diskdiag --threshold-warn 5 --threshold-error 50 /dev/nvme0n1\n"
        "  sudo diskdiag -y -q /dev/sdb; echo \"health: $?\"\n",
        prog, DEFAULT_BLOCK_MB, DEFAULT_WARN_MS, DEFAULT_ERROR_MS);
}

/* ── Main ───────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    opts_t o = {
        .device        = NULL,
        .block_mb      = DEFAULT_BLOCK_MB,
        .max_blocks    = 0,
        .offset_blocks = 0,
        .warn_ms       = DEFAULT_WARN_MS,
        .error_ms      = DEFAULT_ERROR_MS,
        .yes           = 0,
        .quiet         = 0,
        .json          = 0,
        .no_color      = 0,
    };

    /* Long-only option codes (above ASCII range to avoid clashes) */
    enum { OPT_WARN = 128, OPT_ERROR, OPT_NOCOLOR };

    static struct option long_opts[] = {
        { "block-size",      required_argument, 0, 'b'       },
        { "blocks",          required_argument, 0, 'n'       },
        { "offset",          required_argument, 0, 'o'       },
        { "threshold-warn",  required_argument, 0, OPT_WARN  },
        { "threshold-error", required_argument, 0, OPT_ERROR },
        { "yes",             no_argument,       0, 'y'       },
        { "quiet",           no_argument,       0, 'q'       },
        { "json",            no_argument,       0, 'j'       },
        { "no-color",        no_argument,       0, OPT_NOCOLOR },
        { "help",            no_argument,       0, 'h'       },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "b:n:o:yqjh", long_opts, NULL)) != -1) {
        char  *end = NULL;
        long   lv;
        double dv;

        switch (opt) {
        case 'b':
            lv = strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' || lv < 1 || lv > 1024) {
                fprintf(stderr,
                    "Invalid block size '%s'. Must be 1-1024 (MiB).\n", optarg);
                return EXIT_ERROR;
            }
            o.block_mb = (size_t)lv;
            break;
        case 'n':
            lv = strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' || lv < 1) {
                fprintf(stderr,
                    "Invalid block count '%s'. Must be >= 1.\n", optarg);
                return EXIT_ERROR;
            }
            o.max_blocks = (uint64_t)lv;
            break;
        case 'o':
            lv = strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' || lv < 0) {
                fprintf(stderr,
                    "Invalid offset '%s'. Must be >= 0.\n", optarg);
                return EXIT_ERROR;
            }
            o.offset_blocks = (uint64_t)lv;
            break;
        case OPT_WARN:
            dv = strtod(optarg, &end);
            if (end == optarg || *end != '\0' || dv <= 0 || isinf(dv) || isnan(dv)) {
                fprintf(stderr, "Invalid warn threshold '%s'.\n", optarg);
                return EXIT_ERROR;
            }
            o.warn_ms = dv;
            break;
        case OPT_ERROR:
            dv = strtod(optarg, &end);
            if (end == optarg || *end != '\0' || dv <= 0 || isinf(dv) || isnan(dv)) {
                fprintf(stderr, "Invalid error threshold '%s'.\n", optarg);
                return EXIT_ERROR;
            }
            o.error_ms = dv;
            break;
        case 'y': o.yes      = 1;        break;
        case 'q': o.quiet    = 1;        break;
        case 'j': o.json     = 1;        break;
        case OPT_NOCOLOR: o.no_color = 1; break;
        case 'h': usage(argv[0]); return EXIT_HEALTHY;
        default:
            fprintf(stderr, "Try '%s --help' for usage.\n", argv[0]);
            return EXIT_ERROR;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: no device specified.\n"
                        "Try '%s --help' for usage.\n", argv[0]);
        return EXIT_ERROR;
    }
    o.device = argv[optind];

    if (o.warn_ms >= o.error_ms) {
        fprintf(stderr,
            "Error: --threshold-warn (%.0f ms) must be less than "
            "--threshold-error (%.0f ms).\n", o.warn_ms, o.error_ms);
        return EXIT_ERROR;
    }

    /* Disable colour when not writing to a terminal or when requested */
    if (o.no_color || !isatty(STDOUT_FILENO))
        g_color = 0;

    /* ── Mount check ────────────────────────────────────────────── */
    if (check_mounted(o.device, o.yes) == -1)
        return EXIT_ERROR;

    /* ── Open device ────────────────────────────────────────────── */
    int fd = open(o.device, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("open");
        fprintf(stderr, "Hint: run as root.\n");
        return EXIT_ERROR;
    }

    /* ── Device size ────────────────────────────────────────────── */
    uint64_t dev_size = 0;
    if (ioctl(fd, BLKGETSIZE64, &dev_size) < 0) {
        off_t sz = lseek(fd, 0, SEEK_END);
        if (sz < 0) { perror("lseek"); close(fd); return EXIT_ERROR; }
        dev_size = (uint64_t)sz;
        lseek(fd, 0, SEEK_SET);
    }

    size_t   block_bytes  = o.block_mb * (size_t)1024 * (size_t)1024;
    uint64_t total_blocks = dev_size / block_bytes;

    if (total_blocks == 0) {
        fprintf(stderr, "Device too small for block size %zu MiB.\n", o.block_mb);
        close(fd); return EXIT_ERROR;
    }
    if (o.offset_blocks >= total_blocks) {
        fprintf(stderr, "Offset (%lu) >= total blocks on device (%lu).\n",
                (unsigned long)o.offset_blocks, (unsigned long)total_blocks);
        close(fd); return EXIT_ERROR;
    }

    /* Seek to offset */
    if (o.offset_blocks > 0) {
        off_t off = (off_t)(o.offset_blocks * block_bytes);
        if (lseek(fd, off, SEEK_SET) < 0) {
            perror("lseek"); close(fd); return EXIT_ERROR;
        }
    }

    /* Determine how many blocks to read */
    uint64_t n_total = total_blocks - o.offset_blocks;
    if (o.max_blocks > 0 && o.max_blocks < n_total)
        n_total = o.max_blocks;

    /* ── Print header ───────────────────────────────────────────── */
    if (!o.quiet && !o.json) {
        char sz_dev[32];
        human_size(dev_size, sz_dev, sizeof(sz_dev));
        printf("%s%s\n  diskdiag – sequential read latency test\n%s",
               C(COL_BOLD), C(COL_CYAN), C(COL_RESET));
        printf("  Device     : %s  (%s)\n", o.device, sz_dev);
        printf("  Block size : %zu MiB\n", o.block_mb);
        printf("  Blocks     : %lu", (unsigned long)n_total);
        if (o.offset_blocks)
            printf("  (offset: %lu)", (unsigned long)o.offset_blocks);
        printf("\n  Thresholds : warn %.0f ms / error %.0f ms\n",
               o.warn_ms, o.error_ms);
        printf("  Press Ctrl-C to stop early and show partial results.\n\n");
    }

    /* ── Allocate buffers ───────────────────────────────────────── */
    void *buf = NULL;
    if (posix_memalign(&buf, 4096, block_bytes) != 0) {
        perror("posix_memalign"); close(fd); return EXIT_ERROR;
    }

    double *times = calloc(n_total, sizeof(double));
    if (!times) {
        perror("calloc"); free(buf); close(fd); return EXIT_ERROR;
    }

    signal(SIGINT, on_sigint);

    /* ── Main read loop ─────────────────────────────────────────── */
    double   t_start = now_ms();
    uint64_t n_read  = 0;
    uint64_t n_slow  = 0;
    uint64_t n_error = 0;

    for (uint64_t i = 0; i < n_total && !g_stop; i++) {
        double  t0 = now_ms();
        ssize_t r  = read(fd, buf, block_bytes);
        double  ms = now_ms() - t0;

        if (r < 0) {
            times[i] = -1.0;
            n_error++;
            lseek(fd, (off_t)block_bytes, SEEK_CUR);
        } else {
            times[i] = ms;
            if (ms >= o.error_ms)    n_error++;
            else if (ms >= o.warn_ms) n_slow++;
        }
        n_read   = i + 1;
        g_n_read = n_read;

        if (!o.quiet && !o.json && (i % 16 == 0))
            progress_bar(n_read, n_total, times[i] < 0 ? 9999.0 : times[i]);
    }

    double elapsed = (now_ms() - t_start) / 1000.0;

    if (!o.quiet && !o.json) {
        /* Final update – guarantees 100 % is always displayed */
        double last_ms = (n_read > 0 && times[n_read - 1] >= 0)
                       ? times[n_read - 1] : 9999.0;
        progress_bar(n_read, n_total, last_ms);
        printf("\n");
    }

    /* ── Output ─────────────────────────────────────────────────── */
    if (o.json) {
        print_json(times, n_read, elapsed, block_bytes, n_slow, n_error, &o);
    } else {
        if (!o.quiet)
            draw_heatmap(times, n_total, n_read, block_bytes, &o);
        print_stats(times, n_read, elapsed, block_bytes, n_slow, n_error, &o);
    }

    health_t h = health_rating(n_read, n_slow, n_error);

    free(times);
    free(buf);
    close(fd);
    return (int)h;
}
