# Basic Example: Getting Started with Preheat

A step-by-step walkthrough for new users.

---

## Scenario

You have a Kali Linux laptop with a mechanical hard drive. Applications like Firefox, VS Code, and Burp Suite take several seconds to start. You want to speed up your daily workflow.

---

## Step 1: Install Preheat

```bash
# Install dependencies
sudo apt update
sudo apt install -y build-essential libglib2.0-dev autoconf automake

# Clone and install
git clone https://github.com/wasteddreams/preheat-linux.git
cd preheat-linux
sudo ./scripts/install.sh
```

---

## Step 2: Verify Installation

```bash
# Check service status
sudo systemctl status preheat
```

Expected output:
```
● preheat.service - Adaptive readahead daemon
     Loaded: loaded (...; enabled; preset: enabled)
     Active: active (running) since ...
```

---

## Step 3: Use Your System Normally

Just use your computer as you normally would:
- Open Firefox to browse
- Launch VS Code to code
- Start Burp Suite for security testing
- Use the terminal

**Preheat is learning in the background.**

---

## Step 4: Check Learning Progress (After a Few Hours)

```bash
# Dump statistics to log
sudo preheat-ctl dump

# View tracked applications
sudo tail -100 /var/log/preheat.log
```

You should see output like:
```
[timestamp] 47 running processes, 23 tracked applications
```

---

## Step 5: Notice the Improvement

After 1-2 days of normal use:

**Before preheat:** Cold-starting Firefox takes 4-5 seconds.

**After preheat learns:** Firefox starts in 1-2 seconds because it's already in the disk cache.

---

## Understanding What Happened

```
Day 1 (Learning):
  Morning:  You open Firefox, VS Code, Terminal
  Preheat:  "User often uses these together, I'll remember that"
  
Day 2 (Predicting):
  Morning:  You log in
  Preheat:  "Based on yesterday, Firefox is likely to be opened"
  Preheat:  Preloads Firefox into memory
  Result:   When you click Firefox, it starts instantly
```

---

## Quick Commands Reference

| Task | Command |
|------|---------|
| Check if running | `sudo systemctl status preheat` |
| View logs | `sudo tail -f /var/log/preheat.log` |
| See statistics | `sudo preheat-ctl dump` then check log |
| Restart | `sudo systemctl restart preheat` |

---

## Configuration (Optional)

The default configuration works well. If you want to view it:

```bash
cat /usr/local/etc/preheat.conf
```

See [Configuration Reference](../configuration.md) for customization options.

---

## What's Next?

- [Advanced Example](advanced-example.md) - Power-user configuration
- [How It Works](../how-it-works.md) - Understand the technology
- [Troubleshooting](../troubleshooting.md) - If something goes wrong

---

## Navigation

| Previous | Up | Next |
|----------|----|----- |
| [← Troubleshooting](../troubleshooting.md) | [Examples Index](./) | [Advanced Example →](advanced-example.md) |
