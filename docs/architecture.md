# Architecture

This document describes preheat's internal architecture, including component design, data flow, and system interactions.

---

## System Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           PREHEAT DAEMON                                │
│                                                                         │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐   │
│  │   DAEMON    │  │   CONFIG    │  │   STATE     │  │   LOGGING   │   │
│  │   CORE      │  │   PARSER    │  │   MANAGER   │  │   SYSTEM    │   │
│  │             │  │             │  │             │  │             │   │
│  │ Main loop   │  │ INI parser  │  │ Serialize   │  │ File logger │   │
│  │ Init/term   │  │ Validation  │  │ Persist     │  │ Levels      │   │
│  │ Signals     │  │ Defaults    │  │ Load/save   │  │ Rotation    │   │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘   │
│         │                │                │                │          │
│  ┌──────┴────────────────┴────────────────┴────────────────┴──────┐   │
│  │                     SHARED DATA STRUCTURES                      │   │
│  │  ┌─────────────────────────────────────────────────────────┐   │   │
│  │  │  kp_state: Global state containing all application      │   │   │
│  │  │            data, Markov chains, and maps                │   │   │
│  │  └─────────────────────────────────────────────────────────┘   │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│         │                                                              │
│  ┌──────┴──────────────────────────────────────────────┐              │
│  │                 MONITORING LAYER                     │              │
│  │                                                      │              │
│  │  ┌───────────────┐  ┌───────────────────────────┐   │              │
│  │  │  PROC SCANNER │  │      SPY MODULE           │   │              │
│  │  │               │  │                           │   │              │
│  │  │  /proc reader │  │  Track app launches       │   │              │
│  │  │  Maps parser  │  │  Update statistics        │   │              │
│  │  │  Process enum │  │  Manage exe entries       │   │              │
│  │  └───────────────┘  └───────────────────────────┘   │              │
│  └──────┬──────────────────────────────────────────────┘              │
│         │                                                              │
│  ┌──────┴──────────────────────────────────────────────┐              │
│  │                 PREDICTION LAYER                     │              │
│  │                                                      │              │
│  │  ┌─────────────────────────────────────────────┐    │              │
│  │  │            PROPHET MODULE                    │    │              │
│  │  │                                              │    │              │
│  │  │  Markov chain analysis                       │    │              │
│  │  │  Correlation computation                     │    │              │
│  │  │  Score calculation                           │    │              │
│  │  │  Prediction generation                       │    │              │
│  │  └─────────────────────────────────────────────┘    │              │
│  └──────┬──────────────────────────────────────────────┘              │
│         │                                                              │
│  ┌──────┴──────────────────────────────────────────────┐              │
│  │                 READAHEAD LAYER                      │              │
│  │                                                      │              │
│  │  ┌─────────────────────────────────────────────┐    │              │
│  │  │          READAHEAD MODULE                    │    │              │
│  │  │                                              │    │              │
│  │  │  Memory budget calculation                   │    │              │
│  │  │  File sorting (block/inode/path)            │    │              │
│  │  │  Parallel readahead workers                  │    │              │
│  │  │  readahead(2) syscall wrapper               │    │              │
│  │  └─────────────────────────────────────────────┘    │              │
│  └─────────────────────────────────────────────────────┘              │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
         │                    │                    │
         ▼                    ▼                    ▼
    ┌─────────┐         ┌─────────┐         ┌──────────────┐
    │ /proc   │         │ Config  │         │ State File   │
    │ (read)  │         │ File    │         │ (read/write) │
    └─────────┘         └─────────┘         └──────────────┘
```

---

## Core Components

### Daemon Core (`daemon/`)

**Files**: `main.c`, `daemon.c`, `signals.c`

**Responsibilities**:
- Command-line argument parsing
- Daemonization (fork, setsid, chdir)
- Main event loop
- Signal handling (SIGHUP, SIGUSR1, SIGUSR2, SIGTERM)
- Graceful shutdown

**Main Loop Pseudocode**:
```
initialize()
load_config()
load_state()

while (running):
    kp_spy_scan()        # Monitor processes
    kp_spy_update_model() # Update Markov chains
    kp_prophet_predict()  # Calculate predictions
    kp_prophet_readahead() # Preload files
    sleep(cycle_time)

save_state()
cleanup()
```

### Configuration (`config/`)

**Files**: `config.c`, `config.h`, `confkeys.h`

**Data Structure**:
```c
struct _kp_conf {
    struct {
        int cycle;           // Scan interval
        gboolean usecorrelation;
        int minsize;
        int memtotal, memfree, memcached;
    } model;
    
    struct {
        gboolean doscan, dopredict;
        int autosave;
        char *mapprefix, *exeprefix;
        int processes;
        int sortstrategy;
    } system;
    
    struct {
        gboolean enable_preheat_scoring;
        double preheat_tool_boost;
    } preheat;
};
```

### State Management (`state/`)

**Files**: `state.c`, `state.h`

**What's Stored**:
- Application registry (paths, sizes, launch counts)
- Memory maps for each application
- Markov chain nodes and transitions
- Bad executable list (failed opens)

**Serialization Format**:
```
[Header]
PREHEAT_STATE_V1
timestamp

[Executables]
exe_count
for each exe:
    path, size, last_seen, launch_count
    map_count
    for each map:
        path, offset, length

[Markov Chains]
markov_count
for each chain:
    from_app, to_app, transition_count, probability

[Bad Exes]
bad_count
paths of unreadable executables
```

---

## Monitoring Layer

### Process Scanner (`monitor/proc.c`)

**Function**: `kp_proc_scan()`

Enumerates `/proc` to find running processes:

```c
// Pseudocode
for each /proc/[pid]:
    exe_path = readlink("/proc/[pid]/exe")
    
    if exe_path matches exeprefix:
        maps = parse("/proc/[pid]/maps")
        
        for each map in maps:
            if map.path matches mapprefix:
                add_to_process_list(exe_path, map)
```

**Maps File Format** (`/proc/[pid]/maps`):
```
address           perms offset   dev   inode      pathname
7f1234560000-... r-xp  00000000 08:01 1234567    /usr/lib/libc.so
```

### Spy Module (`monitor/spy.c`)

**Functions**: `kp_spy_scan()`, `kp_spy_update_model()`

Tracks application lifecycles:
- Detects new process launches
- Records application exits
- Updates Markov chain on transitions

---

## Prediction Layer

### Prophet Module (`predict/prophet.c`)

**Functions**: `kp_prophet_predict()`, `kp_prophet_readahead()`

**Prediction Algorithm**:

1. **Get running applications**
2. **For each known application**:
   - Look up Markov transitions FROM running apps TO this app
   - Calculate transition probability
   - Apply correlation coefficient
   - Compute final score
3. **Sort by score descending**
4. **Return top N predictions**

**Score Calculation**:
```c
score = 0.0;

for each running_app:
    p = markov_probability(running_app -> candidate_app)
    if (usecorrelation):
        p *= correlation(running_app, candidate_app)
    score += p

// Normalize
score = log(score + EPSILON)
```

### Markov Chain

**Data Structure**:
```c
struct _kp_markov {
    char *from_app;      // Source application
    GHashTable *transitions;  // to_app -> count
    int total_count;     // Sum of all transitions
};

// Probability of transition A -> B
P(A->B) = count(A->B) / total_transitions_from(A)
```

---

## Readahead Layer

### Readahead Module (`readahead/readahead.c`)

**Function**: `kp_readahead_file()`

**Process**:
```
1. Check memory budget (meminfo)
2. Get predicted apps from prophet
3. Collect all files for predicted apps
4. Sort files by strategy (block/inode/path)
5. For each file:
     fd = open(path, O_RDONLY)
     readahead(fd, 0, file_size)
     close(fd)
6. Track bytes preloaded
```

**Sorting Implementation**:
```c
switch (sortstrategy):
    case SORT_NONE:
        // No sorting
    case SORT_PATH:
        qsort(files, by_path)
    case SORT_INODE:
        qsort(files, by_inode)
    case SORT_BLOCK:
        for each file:
            block = get_block_number(fd)
        qsort(files, by_block)
```

**Getting Block Number** (for HDD optimization):
```c
// Uses FIBMAP ioctl
ioctl(fd, FIBMAP, &block_number)
```

---

## Data Flow

### Cycle Data Flow

```
┌──────────────────────────────────────────────────────────────────┐
│                        ONE CYCLE                                  │
└──────────────────────────────────────────────────────────────────┘

Step 1: SCAN
/proc ─────────────────────────────> Process List
         read /proc/*/exe                 │
         read /proc/*/maps                │
                                          ▼
                              ┌───────────────────┐
                              │ Running Processes │
                              │ + Their Maps      │
                              └─────────┬─────────┘
                                        │
Step 2: LEARN                           │
                                        ▼
                              ┌───────────────────┐
                              │ Compare with      │
                              │ Previous State    │
                              └─────────┬─────────┘
                                        │
                     ╭──────────────────┴──────────────────╮
                     │                                      │
              New processes                          Exited processes
                     │                                      │
                     ▼                                      ▼
            ┌─────────────────┐                    ┌─────────────────┐
            │ Add to Markov   │                    │ Record exit     │
            │ chain           │                    │ transition      │
            └─────────────────┘                    └─────────────────┘
                     │                                      │
                     └──────────────────┬──────────────────┘
                                        │
                                        ▼
                              ┌───────────────────┐
                              │ Updated State     │
                              │ (apps + Markov)   │
                              └─────────┬─────────┘
                                        │
Step 3: PREDICT                         │
                                        ▼
                              ┌───────────────────┐
                              │ Prophet: Score    │
                              │ all known apps    │
                              └─────────┬─────────┘
                                        │
                                        ▼
                              ┌───────────────────┐
                              │ Ranked Prediction │
                              │ List              │
                              └─────────┬─────────┘
                                        │
Step 4: PRELOAD                         │
                                        ▼
                              ┌───────────────────┐
                              │ Get files for     │
                              │ top predictions   │
                              └─────────┬─────────┘
                                        │
                                        ▼
                              ┌───────────────────┐
                              │ readahead(2)      │──────> Disk Cache
                              │ each file         │
                              └───────────────────┘
```

---

## File Structure

```
src/
├── daemon/
│   ├── main.c          # Entry point, argument parsing
│   ├── daemon.c        # Daemonization, main loop
│   └── signals.c       # Signal handlers
├── config/
│   ├── config.c        # Configuration loading
│   ├── config.h        # Config structures
│   └── confkeys.h      # Key name definitions
├── monitor/
│   ├── proc.c          # /proc filesystem scanner
│   ├── proc.h
│   ├── spy.c           # Application tracker
│   └── spy.h
├── predict/
│   ├── prophet.c       # Prediction engine
│   └── prophet.h
├── readahead/
│   ├── readahead.c     # Preloading implementation
│   └── readahead.h
├── state/
│   ├── state.c         # State persistence
│   └── state.h
└── utils/
    ├── logging.c       # Logging system
    └── logging.h
```

---

## External Dependencies

### GLib

Preheat uses GLib for:
- Hash tables (`GHashTable`)
- Dynamic arrays (`GArray`, `GPtrArray`)
- String handling (`GString`)
- Main loop (optional)
- Memory management

### System Interfaces

| Interface | Purpose |
|-----------|---------|
| `/proc` filesystem | Process enumeration, maps |
| `readahead(2)` | Non-blocking file read |
| `ioctl(FIBMAP)` | Get file block numbers |
| `open/close` | File access |
| `signal(2)` | Signal handling |
| `fork/setsid` | Daemonization |

---

## Memory Layout

### Runtime Memory Usage

```
Daemon Process Memory
┌─────────────────────────────────────────┐
│ Code Segment (~150 KB)                  │
├─────────────────────────────────────────┤
│ Heap:                                   │
│   ├── Application entries (variable)   │
│   ├── Markov chain nodes (variable)    │
│   ├── Map entries (variable)           │
│   └── Working buffers (~100 KB)        │
├─────────────────────────────────────────┤
│ Stack (~1 MB limit)                     │
└─────────────────────────────────────────┘

Typical total: 5-15 MB depending on tracked apps
```

### Key Data Structures Size

| Structure | Approximate Size |
|-----------|------------------|
| Application entry | ~500 bytes + path strings |
| Map entry | ~200 bytes + path string |
| Markov node | ~100 bytes + transitions |
| Transition | ~20 bytes |

---

## Thread Safety

Preheat is **single-threaded** with one exception:

- Main daemon: Single-threaded event loop
- Readahead workers: Forked processes (not threads)

This design:
- Avoids locking complexity
- Simplifies state management
- Forked workers have copy-on-write for read-only data

---

## Error Handling

### Error Recovery Strategy

| Error Type | Handling |
|------------|----------|
| Config parse error | Use defaults, log warning |
| State file missing | Start fresh, normal operation |
| State file corrupt | Discard, start fresh |
| `/proc` read error | Skip process, continue |
| `readahead` fail | Log, continue with next file |
| Out of memory | Reduce preload budget |

### Defensive Coding

- All allocations checked for NULL
- File descriptors always closed (even on error paths)
- Signals handled with async-safe functions
- Graceful degradation over crashes

---

## Navigation

| Previous | Up | Next |
|----------|----|----- |
| [← How It Works](how-it-works.md) | [Documentation Index](index.md) | [Configuration →](configuration.md) |
