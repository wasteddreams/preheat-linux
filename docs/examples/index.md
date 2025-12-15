---
layout: default
title: Examples
---

# Configuration Examples

Real-world usage scenarios and configuration examples.

---

## Available Examples

| Example | Difficulty | Description |
|---------|------------|-------------|
| [Basic Example](basic-example.md) | Beginner | First-time setup and verification |
| [Advanced Example](advanced-example.md) | Intermediate | Custom configurations for specific scenarios |

---

## Quick Configuration Templates

### Minimal (Safe Defaults)
```ini
[model]
cycle = 20

[system]
doscan = true
dopredict = true
```

### SSD Optimized
```ini
[system]
sortstrategy = 0
processes = 50
```

### Low Memory (2-4 GB RAM)
```ini
[model]
cycle = 30
memfree = 25
memtotal = -20

[system]
processes = 10
```

---

[← Back to Documentation](../index.md)
