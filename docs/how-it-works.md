# How Preheat Works

This document explains the operational principles of preheat—how it monitors, learns, predicts, and preloads applications.

---

## High-Level Overview

Preheat operates in a continuous cycle:

```
┌─────────────────────────────────────────────────────────────────┐
│                     PREHEAT OPERATION CYCLE                      │
│                                                                  │
│    Every 20 seconds (configurable):                              │
│                                                                  │
│    ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐ │
│    │  SCAN    │ -> │  LEARN   │ -> │ PREDICT  │ -> │ PRELOAD  │ │
│    │          │    │          │    │          │    │          │ │
│    │ Check    │    │ Update   │    │ Score    │    │ Read     │ │
│    │ /proc    │    │ Markov   │    │ each     │    │ files    │ │
│    │ for      │    │ chain    │    │ app's    │    │ into     │ │
│    │ running  │    │ model    │    │ launch   │    │ memory   │ │
│    │ apps     │    │          │    │ prob.    │    │ cache    │ │
│    └──────────┘    └──────────┘    └──────────┘    └──────────┘ │
│         │                                               │        │
│         └───────────────────────────────────────────────┘        │
│                          (repeat)                                │
└─────────────────────────────────────────────────────────────────┘
```

---

## The Four Phases

### Phase 1: Scan

Preheat monitors running processes by scanning the `/proc` filesystem:

```
/proc/
├── 1234/                    # Process with PID 1234
│   ├── exe -> /usr/bin/firefox    # Executable path
│   └── maps                 # Memory mappings (libraries)
├── 5678/
│   ├── exe -> /usr/bin/code
│   └── maps
└── ...
```

**What it collects:**
- Which executables are currently running
- What shared libraries each process uses
- File paths and sizes

**Filtering rules:**
- Ignores system daemons (paths starting with `/usr/sbin/`)
- Focuses on user applications (paths starting with `/usr/`)
- Respects configured include/exclude patterns

### Phase 2: Learn

The daemon builds a statistical model of application co-occurrence:

**Markov Chain Model:**

A Markov chain tracks which applications tend to run together or in sequence:

```
Example: User often opens Firefox, then VS Code, then Terminal

State transitions recorded:
  Firefox  -->  VS Code    (probability: 0.6)
  Firefox  -->  Terminal   (probability: 0.3)
  Firefox  -->  Other      (probability: 0.1)
  
  VS Code  -->  Terminal   (probability: 0.7)
  VS Code  -->  Firefox    (probability: 0.2)
  ...
```

**Learning over time:**
- Each scan updates transition probabilities
- Recent observations have more weight
- The model adapts as habits change

### Phase 3: Predict

Using the learned model, preheat calculates which applications are most likely to be launched:

**Scoring factors:**

| Factor | Weight | Description |
|--------|--------|-------------|
| **Markov probability** | High | Based on currently running apps |
| **Correlation coefficient** | Medium | Statistical co-occurrence strength |
| **Recency** | Medium | Recently used apps score higher |
| **Frequency** | Low | Overall launch count |

**Example prediction:**
```
Currently running: Firefox, Terminal

Predictions:
  1. VS Code        (score: 0.82)  - Often follows Firefox
  2. File Manager   (score: 0.65)  - Frequently co-occurs
  3. Spotify        (score: 0.41)  - Sometimes used together
  4. LibreOffice    (score: 0.23)  - Rare combination
```

### Phase 4: Preload

High-scoring applications are preloaded into the disk cache:

```
Application: VS Code
Files to preload:
  /usr/share/code/code           (main binary, 120 MB)
  /usr/lib/x86_64-linux-gnu/libnode.so
  /usr/lib/x86_64-linux-gnu/libv8.so
  ... (shared libraries)
```

**Preloading mechanism:**

1. Check available memory (respect memory limits)
2. Get list of files for predicted applications
3. Sort files for optimal I/O (by disk block for HDDs)
4. Call `readahead(2)` system call on each file
5. Kernel reads file data into disk cache

---

## The readahead(2) System Call

The core of preloading is the Linux `readahead()` system call:

```c
readahead(fd, offset, count);
```

**What it does:**
- Initiates asynchronous reading of file data
- Data goes into the kernel's page cache
- Subsequent reads of that data are serviced from memory
- No data is copied to userspace—minimal overhead

**Why it's efficient:**
- Non-blocking (doesn't wait for I/O to complete)
- Uses the kernel's existing caching infrastructure
- No special privileges beyond file read access
- Works with any filesystem

---

## Memory Management

Preheat carefully manages how much memory it uses for preloading:

### Memory Budget Calculation

```
Available for preloading = max(0, Total × memtotal% + Free × memfree%) 
                          + Cached × memcached%
```

With defaults (`memtotal=-10`, `memfree=50`, `memcached=0`):
- Uses 50% of free memory
- Subtracts 10% of total as safety margin
- Result: Conservative preloading that doesn't starve the system

### Example

```
System: 8 GB total RAM, 3 GB free, 2 GB cached

Calculation:
  Total contribution:  8192 MB × (-10%) = -819 MB
  Free contribution:   3072 MB × (50%)  = 1536 MB
  Cached contribution: 2048 MB × (0%)   =    0 MB
  
  Available = max(0, -819 + 1536) + 0 = 717 MB

Preheat will preload up to 717 MB of application data.
```

### Memory Pressure Response

When system memory becomes scarce:
- Preloading budget decreases automatically
- Already-cached data may be evicted by kernel
- Preheat gracefully reduces its activity

---

## I/O Optimization

### Sorting Strategies

How files are ordered for preloading affects disk efficiency:

| Strategy | Best For | Description |
|----------|----------|-------------|
| **0 - None** | SSD, Flash | No sorting (random access is fast) |
| **1 - Path** | Network FS | Group by directory path |
| **2 - Inode** | Most FS | Sort by inode number |
| **3 - Block** | HDD | Sort by physical disk block |

**Block sorting (default, strategy 3):**
```
Before sorting:        After sorting:
  File A (block 500)     File C (block 100)
  File C (block 100)     File A (block 500)
  File B (block 450)     File B (block 450)
  
Result: Disk head moves in one direction, minimizing seeks
```

### Parallel Readahead

Multiple files can be read simultaneously using worker processes:

```
Main daemon
    │
    ├── Worker 1: Reading /usr/bin/firefox
    ├── Worker 2: Reading /usr/lib/libgtk.so
    ├── Worker 3: Reading /usr/share/code/code
    └── ... (up to 30 by default)
```

This utilizes disk queue depth and parallelism for faster preloading.

---

## Timing and Scheduling

### The Cycle Timer

```
Time:   0s      20s     40s     60s     ...
        │       │       │       │
        ▼       ▼       ▼       ▼
      Scan    Scan    Scan    Scan
      Learn   Learn   Learn   Learn
      Predict Predict Predict Predict
      Preload Preload Preload Preload
```

Each cycle (default: 20 seconds):
1. Completes within a few hundred milliseconds
2. Most time is spent waiting for I/O
3. CPU usage during scan: typically <1%

### Nice Level

Preheat runs with elevated nice level (default: 15):
- Lower priority than interactive applications
- Yields CPU to foreground tasks
- I/O priority is also reduced

---

## State Persistence

### What's Saved

The state file contains:
- All tracked applications and their file mappings
- Markov chain transition probabilities
- Launch counts and timestamps
- Correlation coefficients

### Save Triggers

1. **Autosave timer**: Every hour by default
2. **Graceful shutdown**: On SIGTERM
3. **Manual save**: Via `preheat-ctl save` or SIGUSR2

### State File Location

```
/var/lib/preheat/preheat.state    # Binary format
```

**On startup:**
- If state file exists: Load and continue learning
- If missing: Start fresh (first hour has limited predictions)

---

## Interaction with Linux Subsystems

### /proc Filesystem

```
Preheat reads:
  /proc/[pid]/exe      # Symlink to executable
  /proc/[pid]/maps     # Memory mappings
  /proc/[pid]/stat     # Process statistics
  /proc/meminfo        # System memory status
```

### Page Cache (Disk Cache)

```
Kernel Page Cache
┌────────────────────────────────────────┐
│ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐   │
│ │ Page │ │ Page │ │ Page │ │ Page │...│
│ │(file)│ │(file)│ │(anon)│ │(file)│   │
│ └──────┘ └──────┘ └──────┘ └──────┘   │
│                                        │
│ Preheat adds file pages via readahead  │
│ Kernel manages eviction automatically  │
└────────────────────────────────────────┘
```

### systemd Integration

```
systemd
    │
    ├── Starts preheat at boot
    ├── Restarts on failure
    ├── Manages PID file
    └── Handles signals (reload, stop)
```

---

## What Happens When You Launch an App

### Without Preheat

```
1. User clicks Firefox
2. Shell calls exec("/usr/bin/firefox")
3. Kernel checks page cache → MISS
4. Kernel reads firefox binary from disk → SLOW
5. Kernel loads shared libraries from disk → SLOW
6. Firefox initializes → Application ready
```

### With Preheat (Predicted)

```
[Earlier: Preheat predicted Firefox and preloaded it]

1. User clicks Firefox
2. Shell calls exec("/usr/bin/firefox")
3. Kernel checks page cache → HIT!
4. Firefox binary already in memory → FAST
5. Shared libraries already cached → FAST
6. Firefox initializes → Application ready
```

---

## Summary

| Phase | What Happens | Linux Interface |
|-------|--------------|-----------------|
| **Scan** | Find running processes | `/proc` filesystem |
| **Learn** | Update Markov model | In-memory data structures |
| **Predict** | Score applications | Probability calculations |
| **Preload** | Read files into cache | `readahead(2)` syscall |

Preheat's effectiveness comes from:
1. Accurate prediction based on learned patterns
2. Efficient I/O through sorting and parallelism
3. Careful memory management
4. Non-intrusive background operation

---

## Navigation

| Previous | Up | Next |
|----------|----|----- |
| [← Quick Start](quick-start.md) | [Documentation Index](README.md) | [Architecture →](architecture.md) |
