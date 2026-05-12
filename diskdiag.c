/*
 * diskdiag - Disk read diagnostic tool with ASCII heatmap
 * Usage: sudo ./diskdiag /dev/sdX [block_size_mb]
 *
 * Measures sequential read latency across the entire disk,
 * detects slow sectors, and renders an ASCII heatmap.
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
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <limits.h>

/* ── Configuration ──────────────────────────────────────────────── */
#define DEFAULT_BLOCK_MB     1
#define HEATMAP_COLS         72
#define HEATMAP_ROWS         18
#define SLOW_THRESHOLD_MS    20.0   /* warn if block read > this */
#define ERROR_THRESHOLD_MS   150.0  /* treat as error/bad sector  */

/* ── ANSI colours ───────────────────────────────────────────────── */
#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_RED     "\033[31m"
#define COL_CYAN    "\033[36m"
#define COL_MAGENTA "\033[35m"
#define COL_WHITE   "\033[37m"
#define COL_GREY    "\033[90m"

/* ── Globals (for signal handler) ───────────────────────────────── */
static volatile int g_stop = 0;
static double      *g_times   = NULL;
static uint64_t     g_n_read  = 0;
static uint64_t     g_n_total = 0;

/* ── Helpers ────────────────────────────────────────────────────── */
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double percentile(double *sorted, uint64_t n, double pct)
{
    if (n == 0) return 0.0;
    double idx = (pct / 100.0) * (n - 1);
    uint64_t lo = (uint64_t)idx;
    uint64_t hi = lo + 1;
    if (hi >= n) return sorted[n - 1];
    double frac = idx - lo;
    return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
}

static void human_size(uint64_t bytes, char *buf, size_t buflen)
{
    const char *units[] = {"B","KiB","MiB","GiB","TiB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    snprintf(buf, buflen, "%.2f %s", v, units[u]);
}

static void progress_bar(uint64_t done, uint64_t total,
                         double elapsed_s, double cur_ms)
{
    int width = 40;
    double pct = (total > 0) ? (double)done / total : 0.0;
    int filled = (int)(pct * width);

    (void)elapsed_s;

    printf("\r" COL_BOLD "[");
    for (int i = 0; i < width; i++)
        printf(i < filled ? "█" : "░");
    printf("] %5.1f%%  cur:%5.1f ms" COL_RESET, pct * 100.0, cur_ms);
    fflush(stdout);
}

/* ── ASCII Heatmap ──────────────────────────────────────────────── */
/*
 * Cells use block-element characters for 2× vertical resolution.
 * Colour gradient: grey → green → yellow → red → magenta (very slow)
 */
static const char *cell_colour(double ms)
{
    if (ms < 0)             return COL_MAGENTA; /* error / unread   */
    if (ms == 0)            return COL_GREY;    /* not yet read     */
    if (ms < SLOW_THRESHOLD_MS)   return COL_GREEN;
    if (ms < SLOW_THRESHOLD_MS*3) return COL_YELLOW;
    if (ms < ERROR_THRESHOLD_MS)  return COL_RED;
    return COL_MAGENTA;
}

static char cell_char(double ms)
{
    if (ms < 0)  return 'E'; /* error */
    if (ms == 0) return ' '; /* unread */
    /* intensity glyph based on latency bucket */
    if (ms < 5)   return '.';
    if (ms < 10)  return 'o';
    if (ms < 20)  return '*';
    if (ms < 50)  return '#';
    if (ms < 150) return 'X';
    return '!';
}

static void draw_heatmap(double *times, uint64_t n_total,
                         uint64_t n_read, uint64_t block_bytes)
{
    int cols = HEATMAP_COLS;
    int rows = HEATMAP_ROWS;
    int cells = cols * rows;

    /* bucket average */
    double *bucket = calloc(cells, sizeof(double));
    int    *bcount = calloc(cells, sizeof(int));
    if (!bucket || !bcount) { free(bucket); free(bcount); return; }

    for (uint64_t i = 0; i < n_read; i++) {
        int cell = (int)((double)i / n_total * cells);
        if (cell >= cells) cell = cells - 1;
        bucket[cell] += times[i];
        bcount[cell]++;
    }
    for (int c = 0; c < cells; c++)
        if (bcount[c]) bucket[c] /= bcount[c];

    /* draw */
    printf("\n" COL_BOLD "  Heatmap  (each cell ≈ ");
    char sz[32];
    uint64_t cell_bytes = (uint64_t)((double)n_total / cells) * block_bytes;
    human_size(cell_bytes, sz, sizeof(sz));
    printf("%s)\n" COL_RESET, sz);

    printf(COL_GREY "  ┌");
    for (int c = 0; c < cols; c++) printf("─");
    printf("┐\n" COL_RESET);

    for (int r = 0; r < rows; r++) {
        printf(COL_GREY "  │" COL_RESET);
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            double ms = (idx < cells) ? bucket[idx] : 0.0;
            printf("%s%c" COL_RESET, cell_colour(ms), cell_char(ms));
        }
        printf(COL_GREY "│\n" COL_RESET);
    }

    printf(COL_GREY "  └");
    for (int c = 0; c < cols; c++) printf("─");
    printf("┘\n" COL_RESET);

    /* legend */
    printf("  Legend: "
           COL_GREY "' '" COL_RESET " unread  "
           COL_GREEN "'.' <5ms  'o' <10ms  '*' <20ms" COL_RESET "  "
           COL_YELLOW "'#' <50ms" COL_RESET "  "
           COL_RED "'X' <150ms" COL_RESET "  "
           COL_MAGENTA "'!' ≥150ms / 'E' error\n" COL_RESET);

    free(bucket);
    free(bcount);
}

/* ── Statistics ─────────────────────────────────────────────────── */
static void print_stats(double *times, uint64_t n,
                        double elapsed_s, uint64_t block_bytes,
                        uint64_t n_slow, uint64_t n_error)
{
    if (n == 0) { printf("No data.\n"); return; }

    double *sorted = malloc(n * sizeof(double));
    if (!sorted) return;
    memcpy(sorted, times, n * sizeof(double));
    qsort(sorted, n, sizeof(double), cmp_double);

    double sum = 0;
    for (uint64_t i = 0; i < n; i++) sum += times[i];
    double avg = sum / n;

    double total_bytes = (double)n * block_bytes;
    double speed_mbs   = (elapsed_s > 0)
                       ? (total_bytes / 1024.0 / 1024.0) / elapsed_s
                       : 0.0;

    char sz_read[32], sz_total[32];
    human_size((uint64_t)total_bytes, sz_read, sizeof(sz_read));
    human_size((uint64_t)((double)n * block_bytes), sz_total, sizeof(sz_total));

    printf("\n" COL_BOLD COL_CYAN
           "══════════════════ Results ══════════════════\n" COL_RESET);
    printf("  Blocks read  : %lu  (%s)\n", (unsigned long)n, sz_read);
    printf("  Elapsed      : %.1f s\n", elapsed_s);
    printf("  Throughput   : %.1f MiB/s\n", speed_mbs);
    printf(COL_BOLD
           "──────────────── Latency (ms) ───────────────\n" COL_RESET);
    printf("  Min          : %.2f ms\n", sorted[0]);
    printf("  Avg          : %.2f ms\n", avg);
    printf("  Median (P50) : %.2f ms\n", percentile(sorted, n, 50));
    printf("  P90          : %.2f ms\n", percentile(sorted, n, 90));
    printf("  P99          : %.2f ms\n", percentile(sorted, n, 99));
    printf("  Max          : %.2f ms\n", sorted[n-1]);
    printf(COL_BOLD
           "──────────────── Health ──────────────────────\n" COL_RESET);

    double slow_pct  = (double)n_slow  / n * 100.0;
    double error_pct = (double)n_error / n * 100.0;

    printf("  Slow blocks  : %lu  (%.2f%%)\n",
           (unsigned long)n_slow, slow_pct);
    printf("  Error blocks : %lu  (%.2f%%)\n",
           (unsigned long)n_error, error_pct);

    /* overall health score */
    printf(COL_BOLD "  Disk health  : ");
    if (n_error > 0 || error_pct > 0.1)
        printf(COL_RED "⚠  POOR – bad sectors detected!\n" COL_RESET);
    else if (slow_pct > 5.0)
        printf(COL_YELLOW "△  FAIR – many slow blocks\n" COL_RESET);
    else if (slow_pct > 1.0)
        printf(COL_YELLOW "◇  GOOD – some slow blocks\n" COL_RESET);
    else
        printf(COL_GREEN "✓  HEALTHY\n" COL_RESET);

    printf(COL_CYAN
           "═════════════════════════════════════════════\n" COL_RESET);

    free(sorted);
}

/* ── Mount check ────────────────────────────────────────────────── */
/*
 * Resolves symlinks on both the requested device and each entry in
 * /proc/mounts so that e.g. /dev/disk/by-id/... matches /dev/sdb.
 * Returns  0 = not mounted (proceed normally)
 *          1 = mounted, user chose to continue
 *         -1 = mounted, user aborted
 */
static int check_mounted(const char *dev)
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

        /* match exact device or any of its partitions */
        size_t devlen = strlen(real_dev);
        if (strncmp(real_mdev, real_dev, devlen) == 0)
            snprintf(matches[count++], PATH_MAX * 2, "    %s  →  %s", real_mdev, mpoint);
    }
    fclose(f);

    if (count == 0) return 0;

    printf(COL_YELLOW COL_BOLD
           "\n  ⚠  Device is currently mounted:\n" COL_RESET);
    for (int i = 0; i < count; i++)
        printf(COL_YELLOW "%s\n" COL_RESET, matches[i]);
    printf(
        "\n"
        "  Read-only access cannot corrupt data, but latency results\n"
        "  may be skewed by background OS I/O (journaling, writeback,\n"
        "  prefetch). For accurate diagnostics use a live USB.\n"
        "\n"
        "  Continue anyway? [y/N] ");
    fflush(stdout);

    int answer = getchar();
    int c;
    while ((c = getchar()) != '\n' && c != EOF) ; /* flush rest of line */
    if (answer != 'y' && answer != 'Y') {
        printf("Aborted.\n\n");
        return -1;
    }
    printf("\n");
    return 1;
}

/* ── Signal handler ─────────────────────────────────────────────── */
static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ── Main ───────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <device> [block_size_MiB]\n"
            "Example: sudo %s /dev/sdb 1\n", argv[0], argv[0]);
        return 1;
    }

    const char *device = argv[1];

    /* Parse block size safely – atoi() has undefined behaviour on overflow */
    size_t block_mb = DEFAULT_BLOCK_MB;
    if (argc >= 3) {
        char *end = NULL;
        long val  = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || val < 1 || val > 1024) {
            fprintf(stderr,
                "Invalid block size '%s'. Must be 1-1024 (MiB).\n", argv[2]);
            return 1;
        }
        block_mb = (size_t)val;
    }

    /* Explicit size_t arithmetic – avoids int overflow before assignment */
    size_t block_bytes = block_mb * (size_t)1024 * (size_t)1024;

    /* Check whether device (or its partitions) is mounted */
    if (check_mounted(device) == -1)
        return 0;

    /* Open with O_DIRECT to bypass page cache */
    int fd = open(device, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("open");
        fprintf(stderr, "Hint: run as root and make sure the device is unmounted.\n");
        return 1;
    }

    /* Get device size */
    uint64_t dev_size = 0;
    if (ioctl(fd, BLKGETSIZE64, &dev_size) < 0) {
        /* fallback: try seeking to end */
        off_t sz = lseek(fd, 0, SEEK_END);
        if (sz < 0) { perror("ioctl/lseek"); close(fd); return 1; }
        dev_size = (uint64_t)sz;
        lseek(fd, 0, SEEK_SET);
    }

    uint64_t n_total = dev_size / block_bytes;
    if (n_total == 0) {
        fprintf(stderr, "Device too small for block size %zu MiB\n", block_mb);
        close(fd); return 1;
    }

    char sz_dev[32];
    human_size(dev_size, sz_dev, sizeof(sz_dev));

    printf(COL_BOLD COL_CYAN
           "\n  diskdiag – sequential read latency test\n" COL_RESET);
    printf("  Device  : %s  (%s)\n", device, sz_dev);
    printf("  Block   : %zu MiB   Blocks: %lu\n",
           block_mb, (unsigned long)n_total);
    printf("  Press Ctrl-C to stop early and show partial results.\n\n");

    /* Allocate aligned buffer for O_DIRECT (must be 4096-aligned) */
    void *buf = NULL;
    if (posix_memalign(&buf, 4096, block_bytes) != 0) {
        perror("posix_memalign");
        close(fd); return 1;
    }

    /* Allocate timing array */
    double *times = malloc(n_total * sizeof(double));
    if (!times) { perror("malloc"); free(buf); close(fd); return 1; }
    memset(times, 0, n_total * sizeof(double));

    g_times   = times;
    g_n_total = n_total;

    signal(SIGINT, on_sigint);

    double t_start     = now_ms();
    uint64_t n_read    = 0;
    uint64_t n_slow    = 0;
    uint64_t n_error   = 0;

    for (uint64_t i = 0; i < n_total && !g_stop; i++) {
        double t0 = now_ms();
        ssize_t r = read(fd, buf, block_bytes);
        double t1 = now_ms();

        if (r < 0) {
            times[i] = -1.0; /* mark as error */
            n_error++;
            /* try to skip past bad sector */
            lseek(fd, (off_t)block_bytes, SEEK_CUR);
        } else {
            double ms = t1 - t0;
            times[i] = ms;
            if (ms >= ERROR_THRESHOLD_MS) n_error++;
            else if (ms >= SLOW_THRESHOLD_MS) n_slow++;
        }
        n_read++;
        g_n_read = n_read;

        /* update progress every 16 blocks */
        if (i % 16 == 0) {
            double elapsed = (now_ms() - t_start) / 1000.0;
            progress_bar(i, n_total, elapsed, times[i] < 0 ? 9999 : times[i]);
        }
    }

    double elapsed = (now_ms() - t_start) / 1000.0;
    printf("\n"); /* end progress line */

    draw_heatmap(times, n_total, n_read, block_bytes);
    print_stats(times, n_read, elapsed, block_bytes, n_slow, n_error);

    free(times);
    free(buf);
    close(fd);
    return 0;
}
