# Examples

Real-world usage scenarios and configuration examples for preheat.

---

## Available Examples

| Example | Description | Difficulty |
|---------|-------------|------------|
| [Basic Example](basic-example.md) | First-time setup and verification | Beginner |
| [Advanced Example](advanced-example.md) | Custom configurations for specific scenarios | Intermediate-Advanced |

---

## Quick Links

### For Beginners
Start with the [Basic Example](basic-example.md) to understand:
- Installation verification
- How learning works
- Daily usage patterns

### For Power Users
See the [Advanced Example](advanced-example.md) for:
- HDD optimization
- SSD configuration
- Low-memory systems
- Developer workstations
- Kali Linux security testing
- Custom whitelists and blacklists
- Monitoring scripts

---

## Configuration Templates

### Minimal (Just Works)

```ini
[model]
cycle = 20

[system]
doscan = true
dopredict = true
```

### Balanced (Recommended)

```ini
[model]
cycle = 20
usecorrelation = true
memfree = 50
memtotal = -10

[system]
sortstrategy = 3
processes = 30
autosave = 3600
```

### Conservative (Low Resources)

```ini
[model]
cycle = 30
memfree = 25
memtotal = -20
minsize = 5000000

[system]
processes = 10
autosave = 7200
```

### Aggressive (Fast Hardware)

```ini
[model]
cycle = 15
memfree = 70
memtotal = 0
minsize = 1000000

[system]
sortstrategy = 0
processes = 50
autosave = 1800
```

---

## Navigation

| Previous | Up |
|----------|------|
| [← Troubleshooting](../troubleshooting.md) | [Documentation Index](../README.md) |
