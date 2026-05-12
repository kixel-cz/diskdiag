# diskdiag

A Linux command-line tool for disk diagnostics based on sequential read
latency measurement. Unlike SMART attribute readers, diskdiag exercises
the actual read path and can reveal degraded areas, ageing HDD heads, or
failing flash cells.

## Dependencies

Only the standard C library and Linux kernel headers are required.
No external dependencies.

```
gcc    # typically the build-essential package (Debian/Ubuntu) or gcc (Fedora)
make
```

## Build and install

```bash
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
sudo diskdiag <device> [block_size_MiB]
```

Examples:

```bash
sudo diskdiag /dev/sdb        # test with default 1 MiB blocks
sudo diskdiag /dev/sdb 4      # larger blocks – faster pass, coarser heatmap
sudo diskdiag /dev/nvme0n1    # NVMe drive
```

Press **Ctrl-C** at any time to stop early; the heatmap and statistics are
printed immediately from the portion of the disk read so far.

## Output

### Progress bar

Shows completion percentage and the latency of the most recently read block
in milliseconds.

### ASCII heatmap

Each cell represents the average read latency of one region of the disk.
The character and colour encode the speed:

| Char | Colour  | Latency      |
|------|---------|--------------|
| `.`  | green   | < 5 ms       |
| `o`  | green   | < 10 ms      |
| `*`  | green   | < 20 ms      |
| `#`  | yellow  | < 50 ms      |
| `X`  | red     | < 150 ms     |
| `!`  | magenta | ≥ 150 ms     |
| `E`  | magenta | read error   |

### Statistics

Minimum, average, median (P50), P90, P99, and maximum latency;
throughput in MiB/s; count of slow and error blocks.

### Health rating

| Rating  | Condition                                  |
|---------|--------------------------------------------|
| HEALTHY | < 1 % slow blocks, no errors               |
| GOOD    | 1–5 % slow blocks                          |
| FAIR    | > 5 % slow blocks                          |
| POOR    | read errors or latency ≥ 150 ms detected   |

## Notes

**The disk does not need to be unmounted.** Read-only access cannot corrupt
data. If the device is currently mounted, diskdiag will warn you and ask for
confirmation before proceeding. Results may be skewed by concurrent OS I/O
(journaling, writeback, prefetch); for precise diagnostics test an unmounted
disk or boot from a live USB.

The device is opened with `O_DIRECT` to bypass the page cache, so latency
reflects actual media performance rather than RAM speed.

**Typical latency reference values:**

| Drive type      | Expected latency |
|-----------------|------------------|
| NVMe SSD        | < 1 ms           |
| SATA SSD        | 1–5 ms           |
| HDD (healthy)   | 5–20 ms          |
| HDD (degraded)  | > 50 ms          |

Sustained latency above 50 ms on a spinning disk is a strong indicator of
head issues or imminent failure and warrants immediate attention.

## Man page

A man page is included and installed automatically by `make install`:

```bash
man diskdiag
```

## See also

`smartctl(8)`, `hdparm(8)`, `badblocks(8)`, `nvme(1)`

## License

Licensed under the [European Union Public Licence v1.2 (EUPL-1.2)](LICENSE),
a copyleft licence compatible with GPL-2.0, LGPL, MPL, and several others.