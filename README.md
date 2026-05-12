# diskdiag

A command-line tool for disk diagnostics based on sequential read latency
measurement. Unlike SMART attribute readers, diskdiag exercises the actual
read path and can reveal degraded areas, ageing HDD heads, or failing flash
cells.

Supports **Linux** and **macOS**.

![diskdiag screenshot](assets/screenshot.png)

## Dependencies

Only the standard C library and system headers are required.
No external dependencies.

| Platform | Compiler | Notes |
|----------|----------|-------|
| Linux | `gcc` | `build-essential` package on Debian/Ubuntu, `gcc` on Fedora |
| macOS | `clang` or `gcc` | Xcode Command Line Tools: `xcode-select --install` |

## Build and install

Clone the repository and build:

```bash
git clone https://github.com/kixel-cz/diskdiag.git
cd diskdiag
make
sudo make install          # installs to /usr/local/bin
```

With a custom prefix:

```bash
sudo make install PREFIX=/usr
```

Uninstall:

```bash
sudo make uninstall
```

## Usage

```
sudo diskdiag [OPTIONS] <device>
```

### Device names by platform

| Platform | Example devices |
|----------|----------------|
| Linux | `/dev/sda`, `/dev/sdb`, `/dev/nvme0n1` |
| macOS | `/dev/disk0`, `/dev/disk2` |

On macOS, use `diskutil list` to identify the correct device before testing.

### Options

| Option | Description |
|--------|-------------|
| `-b, --block-size <MiB>` | Read block size, 1–1024 (default: 1) |
| `-n, --blocks <N>` | Test only the first N blocks |
| `-o, --offset <N>` | Start at block offset N |
| `--threshold-warn <ms>` | Slow block threshold (default: 20 ms) |
| `--threshold-error <ms>` | Error block threshold (default: 150 ms) |
| `-y, --yes` | Skip the mounted-device prompt |
| `-q, --quiet` | Suppress progress bar and heatmap; print only the statistics table |
| `-j, --json` | Output results as JSON |
| `--no-color` | Disable ANSI colour output |
| `-h, --help` | Show help |

### Exit codes

| Code | Meaning |
|------|---------|
| `0` | **HEALTHY** – < 1 % slow blocks, no errors |
| `1` | **GOOD** – 1–5 % slow blocks |
| `2` | **FAIR** – > 5 % slow blocks |
| `3` | **POOR** – read errors or latency >= error threshold |
| `4` | Runtime error (bad arguments, cannot open device, ...) |

## Examples

```bash
# Full test with default settings
sudo diskdiag /dev/sdb            # Linux
sudo diskdiag /dev/disk2          # macOS

# Larger blocks – faster pass, coarser heatmap
sudo diskdiag -b 4 /dev/sdb

# Quick smoke test of the first 1000 blocks
sudo diskdiag -n 1000 /dev/sdb

# Test a specific region (blocks 500–999)
sudo diskdiag -o 500 -n 500 /dev/sdb

# Tighter thresholds for NVMe drives
sudo diskdiag --threshold-warn 2 --threshold-error 20 /dev/nvme0n1

# Non-interactive JSON output for scripts or monitoring pipelines
sudo diskdiag -y -q --json /dev/sdb > result.json

# Use the exit code in a shell script
sudo diskdiag -y -q /dev/sdb
case $? in
  0) echo "Healthy" ;;
  1) echo "Good" ;;
  2) echo "Fair – investigation recommended" ;;
  3) echo "POOR – immediate attention required" ;;
esac
```

Press **Ctrl-C** at any time to stop early; the heatmap and statistics are
printed immediately from the portion of the disk read so far.

## Output

### Progress bar

Shows completion percentage and the latency of the most recently read block
in milliseconds.

### ASCII heatmap

Each cell represents the average read latency of one region of the disk.
The character and colour encode the speed relative to the configured thresholds:

| Char | Colour  | Latency             |
|------|---------|---------------------|
| `.`  | green   | < warn / 4          |
| `o`  | green   | < warn / 2          |
| `*`  | green   | < warn threshold    |
| `#`  | yellow  | < error / 3         |
| `X`  | red     | < error threshold   |
| `!`  | magenta | >= error threshold  |
| `E`  | magenta | read error          |

### Statistics

Minimum, average, median (P50), P90, P99, and maximum latency;
throughput in MiB/s; count of slow and error blocks.

### Health rating

| Rating  | Condition                                  |
|---------|--------------------------------------------|
| HEALTHY | < 1 % slow blocks, no errors               |
| GOOD    | 1–5 % slow blocks                          |
| FAIR    | > 5 % slow blocks                          |
| POOR    | read errors or latency >= error threshold  |

### JSON output

When `-j / --json` is used, results are written to stdout as a single JSON
object. Mount warnings are still printed to stderr so they do not interfere
with downstream parsing. The device path is properly escaped in the JSON output.

```json
{
  "device": "/dev/sdb",
  "block_mib": 1,
  "offset_blocks": 0,
  "blocks_read": 7452,
  "bytes_read": 7812808704,
  "elapsed_s": 142.551,
  "throughput_mib_s": 52.14,
  "latency_ms": {
    "min": 4.123,
    "avg": 9.847,
    "p50": 8.901,
    "p90": 14.233,
    "p99": 38.441,
    "max": 187.002
  },
  "thresholds_ms": { "warn": 20, "error": 150 },
  "slow_blocks": 312,
  "error_blocks": 1,
  "slow_pct": 4.1867,
  "error_pct": 0.0134,
  "health": "POOR",
  "exit_code": 3
}
```

## Notes

**The disk does not need to be unmounted.** Read-only access cannot corrupt
data. If the device is currently mounted, diskdiag prints a warning to stderr
and asks for confirmation. Pass `-y / --yes` to skip the prompt in automated
use. Results may be skewed by concurrent OS I/O (journaling, writeback,
prefetch); for precise diagnostics unmount the disk first.

The device is opened with `O_DIRECT` (Linux) or `F_NOCACHE` (macOS) to
bypass the page cache, so latency reflects actual media performance rather
than RAM speed.

Colour output is automatically disabled when stdout is not a terminal
(pipe, redirect). Use `--no-color` to force it off in any context.

### macOS system disk

On macOS 10.15 and later, the system volume is protected by System Integrity
Protection (SIP) and mounted as a read-only snapshot. Direct access via
`/dev/diskX` is blocked even for root. This affects only the system disk —
external and secondary drives work without any restrictions.

To identify available disks on macOS:

```bash
diskutil list
```

**Typical latency reference values:**

The numbers below assume a quality controller and a fast bus. Real-world
results can differ significantly depending on the controller and connection:

| Drive type      | Expected latency |
|-----------------|------------------|
| NVMe SSD        | < 1 ms           |
| SATA SSD        | 1–5 ms           |
| HDD (healthy)   | 5–20 ms          |
| HDD (degraded)  | > 50 ms          |

**Controller and bus impact:**

A slow or low-quality controller can add several milliseconds of overhead
regardless of the drive itself. Budget USB enclosures and chipsets are a
common culprit — the drive may be perfectly healthy while the enclosure
limits throughput and inflates latency.

Connection speed matters equally:

| Interface      | Typical throughput |
|----------------|--------------------|
| USB 1.1        | ~1 MB/s            |
| USB 2.0        | ~40 MB/s           |
| USB 3.2 Gen 1  | ~400 MB/s          |
| USB 3.2 Gen 2  | ~900 MB/s          |
| Thunderbolt 3/4| ~2500 MB/s         |

An HDD connected over USB 2.0 may show latency of 50–100 ms not because
the drive is failing, but because the bus itself is the bottleneck.
If results look unexpectedly poor, try a different cable, port, or enclosure
before drawing conclusions about the drive's health.

## Man page

A man page is included and installed automatically by `make install`:

```bash
man diskdiag
```

## License

Licensed under the [European Union Public Licence v1.2 (EUPL-1.2)](LICENSE),
a copyleft licence compatible with GPL-2.0, LGPL, MPL, and several others.

## See also

`smartctl(8)`, `hdparm(8)`, `badblocks(8)`, `nvme(1)`, `diskutil(8)`
