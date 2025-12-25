#!/bin/bash
#
# Preheat Setup Script
# Unified installer, reinstaller, and uninstaller
#
# Usage:
#   sudo bash setup.sh                     # Interactive menu
#   sudo bash setup.sh install             # Install preheat (uses local repo if available)
#   sudo bash setup.sh reinstall           # Reinstall (preserve data)
#   sudo bash setup.sh reinstall --clean   # Reinstall (fresh)
#   sudo bash setup.sh uninstall           # Uninstall (preserve data)
#   sudo bash setup.sh uninstall --purge   # Uninstall (remove all data)
set -e

# Colors and formatting
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BLUE='\033[0;34m'
MAGENTA='\033[0;35m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

# Data locations
STATE_DIR="/usr/local/var/lib/preheat"
CONFIG_DIR="/usr/local/etc/preheat.d"
LOG_FILE="/usr/local/var/log/preheat.log"
MAIN_CONFIG="/usr/local/etc/preheat.conf"

# Check root
check_root() {
    if [[ $EUID -ne 0 ]]; then
        echo -e "${RED}${BOLD}[X] Error:${NC} This script must be run as root"
        echo -e "${DIM}  Try: ${CYAN}sudo $0${NC}"
        exit 1
    fi
}

# Spinner animation
spinner() {
    local pid=$1
    local msg=$2
    local spinstr='⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏'
    local i=0
    
    while kill -0 $pid 2>/dev/null; do
        local temp=${spinstr:i++%${#spinstr}:1}
        printf "\r${CYAN}${temp}${NC} ${msg}..."
        sleep 0.1
    done
    printf "\r${GREEN}[OK]${NC} ${msg}... ${GREEN}done${NC}\n"
}

# Progress bar
progress_bar() {
    local current=$1
    local total=$2
    local width=40
    local percent=$((current * 100 / total))
    local filled=$((current * width / total))
    
    printf "\r${CYAN}Progress:${NC} ["
    printf "%${filled}s" | tr ' ' '█'
    printf "%$((width - filled))s" | tr ' ' '░'
    printf "] ${BOLD}%d%%${NC}" $percent
}

# Show header
show_header() {
    local title="$1"
    local width=50
    local border=$(printf '═%.0s' $(seq 1 $width))
    local spaces=$(printf ' %.0s' $(seq 1 $width))
    local title_len=${#title}
    local pad_left=$(( (width - title_len) / 2 ))
    local pad_right=$(( width - title_len - pad_left ))
    
    echo ""
    echo -e "${MAGENTA}${BOLD}╔${border}╗${NC}"
    echo -e "${MAGENTA}${BOLD}║${spaces}║${NC}"
    echo -e "${MAGENTA}${BOLD}║$(printf '%*s' $pad_left '')${title}$(printf '%*s' $pad_right '')║${NC}"
    echo -e "${MAGENTA}${BOLD}║${spaces}║${NC}"
    echo -e "${MAGENTA}${BOLD}╚${border}╝${NC}"
    echo ""
}

# Show success banner
show_success() {
    local message="$1"
    local width=50
    local border=$(printf '═%.0s' $(seq 1 $width))
    local spaces=$(printf ' %.0s' $(seq 1 $width))
    local msg_len=${#message}
    local pad_left=$(( (width - msg_len) / 2 ))
    local pad_right=$(( width - msg_len - pad_left ))
    
    echo ""
    echo -e "${GREEN}${BOLD}╔${border}╗${NC}"
    echo -e "${GREEN}${BOLD}║${spaces}║${NC}"
    echo -e "${GREEN}${BOLD}║$(printf '%*s' $pad_left '')${message}$(printf '%*s' $pad_right '')║${NC}"
    echo -e "${GREEN}${BOLD}║${spaces}║${NC}"
    echo -e "${GREEN}${BOLD}╚${border}╝${NC}"
    echo ""
}

# Stop preheat service
stop_service() {
    echo -e "${CYAN}Stopping service...${NC}"
    if systemctl is-active --quiet preheat.service 2>/dev/null; then
        systemctl stop preheat.service
        echo -e "${GREEN}  [OK] Service stopped${NC}"
    else
        echo -e "${DIM}  Service not running${NC}"
    fi

    if systemctl is-enabled --quiet preheat.service 2>/dev/null; then
        systemctl disable preheat.service > /dev/null 2>&1
        echo -e "${GREEN}  [OK] Service disabled${NC}"
    fi
}

# Remove binaries
remove_binaries() {
    echo -e "${CYAN}Removing binaries...${NC}"
    
    rm -f /usr/local/bin/preheat 2>/dev/null
    rm -f /usr/local/bin/preheat-ctl 2>/dev/null
    rm -f /usr/local/sbin/preheat 2>/dev/null
    rm -f /usr/local/sbin/preheat-ctl 2>/dev/null
    rm -f /etc/systemd/system/preheat.service 2>/dev/null
    rm -f /usr/lib/systemd/system/preheat.service 2>/dev/null
    rm -f /usr/local/lib/systemd/system/preheat.service 2>/dev/null
    rm -f /run/preheat.pid 2>/dev/null
    rm -f /run/preheat.pause 2>/dev/null
    rm -f /run/preheat.stats 2>/dev/null
    rm -f /usr/local/share/man/man1/preheat-ctl.1 2>/dev/null
    rm -f /usr/local/share/man/man5/preheat.conf.5 2>/dev/null
    rm -f /usr/local/share/man/man8/preheat.8 2>/dev/null
    
    systemctl daemon-reload 2>/dev/null
    echo -e "${GREEN}  [OK] Binaries removed${NC}"
}

# Remove data
remove_data() {
    echo -e "${YELLOW}Removing all data...${NC}"
    
    [ -d "$STATE_DIR" ] && rm -rf "$STATE_DIR" && echo -e "${DIM}  Removed: $STATE_DIR${NC}"
    [ -d "$CONFIG_DIR" ] && rm -rf "$CONFIG_DIR" && echo -e "${DIM}  Removed: $CONFIG_DIR${NC}"
    [ -f "$LOG_FILE" ] && rm -f "$LOG_FILE" && echo -e "${DIM}  Removed: $LOG_FILE${NC}"
    [ -f "$MAIN_CONFIG" ] && rm -f "$MAIN_CONFIG" && echo -e "${DIM}  Removed: $MAIN_CONFIG${NC}"
    
    echo -e "${GREEN}  [OK] All data removed${NC}"
}

# Show preserved data
show_preserved_data() {
    echo -e "${GREEN}Data preserved:${NC}"
    [ -d "$STATE_DIR" ] && echo -e "${DIM}  [OK] $STATE_DIR${NC}"
    [ -d "$CONFIG_DIR" ] && echo -e "${DIM}  [OK] $CONFIG_DIR${NC}"
    [ -f "$MAIN_CONFIG" ] && echo -e "${DIM}  [OK] $MAIN_CONFIG${NC}"
    [ -f "$LOG_FILE" ] && echo -e "${DIM}  [OK] $LOG_FILE${NC}"
}

# Run a command with status message (hybrid approach)
# Usage: run_step "message" command args...
run_step() {
local msg="$1"
shift

printf "${CYAN}  >${NC} %s..." "$msg"

if "$@" >> /tmp/preheat_install.log 2>&1; then
    printf "\r${GREEN}  [OK]${NC} %s\n" "$msg"
    return 0
else
    printf "\r${RED}  [X]${NC} %s ${DIM}(check /tmp/preheat_install.log)${NC}\n" "$msg"
    return 1
fi
}

# Full install from source
do_install() {
show_header "PREHEAT INSTALLER"
echo -e "${DIM}Adaptive Readahead • Faster App Launches${NC}"
echo ""

# Initialize log
: > /tmp/preheat_install.log

# Check and install dependencies
echo -e "${CYAN}Checking prerequisites...${NC}"
NEED_INSTALL=false
INSTALL_PKGS=""

for cmd in autoconf automake pkg-config gcc make; do
    if ! command -v $cmd &>/dev/null; then
        NEED_INSTALL=true
        case $cmd in
            autoconf) INSTALL_PKGS="$INSTALL_PKGS autoconf" ;;
            automake) INSTALL_PKGS="$INSTALL_PKGS automake" ;;
            pkg-config) INSTALL_PKGS="$INSTALL_PKGS pkg-config" ;;
            gcc) INSTALL_PKGS="$INSTALL_PKGS build-essential" ;;
            make) INSTALL_PKGS="$INSTALL_PKGS build-essential" ;;
        esac
    fi
done

if ! pkg-config --exists glib-2.0 2>/dev/null; then
    NEED_INSTALL=true
    INSTALL_PKGS="$INSTALL_PKGS libglib2.0-dev"
fi

if [ "$NEED_INSTALL" = true ]; then
    UNIQUE_PKGS=$(echo "$INSTALL_PKGS" | tr ' ' '\n' | sort -u | tr '\n' ' ')
    echo -e "${YELLOW}  [!] Missing dependencies:${NC}${UNIQUE_PKGS}"
    echo ""
    echo -e "${DIM}Install with:${NC}"
    echo -e "${CYAN}  apt-get install${UNIQUE_PKGS}${NC}"
    echo ""
    read -p "$(echo -e ${BOLD}Install now? [Y/n]:${NC} )" install_choice
    install_choice=${install_choice:-Y}
    
    if [[ "$install_choice" =~ ^[Yy]$ ]]; then
        run_step "Updating package lists" apt-get update -qq
        run_step "Installing dependencies" apt-get install -y -qq $UNIQUE_PKGS
        echo -e "${GREEN}  [OK] All dependencies installed${NC}"
    else
        echo -e "${RED}Cannot continue without dependencies.${NC}"
        exit 1
    fi
else
    echo -e "${GREEN}  [OK] All prerequisites present${NC}"
fi
echo ""

# System info
echo -e "${DIM}┌─ System Information${NC}"
echo -e "${DIM}│${NC} OS: $(lsb_release -ds 2>/dev/null || echo "Unknown")"
echo -e "${DIM}│${NC} Kernel: $(uname -r)"
echo -e "${DIM}│${NC} RAM: $(free -h | awk '/^Mem:/{print $2}')"
echo -e "${DIM}└───────────────────${NC}"
echo ""

# Step 1: Locate source
echo -e "${CYAN}${BOLD}[1/4]${NC} Locating source code..."

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
USE_LOCAL=false
SOURCE_DIR=""
CLEANUP_DIR=""

if [ -f "$SCRIPT_DIR/configure.ac" ] && grep -q "preheat" "$SCRIPT_DIR/configure.ac" 2>/dev/null; then
    USE_LOCAL=true
    SOURCE_DIR="$SCRIPT_DIR"
    echo -e "${GREEN}  [OK] Using local repository${NC} ${DIM}($SOURCE_DIR)${NC}"
    if [ -d "$SOURCE_DIR/.git" ]; then
        COMMIT=$(cd "$SOURCE_DIR" && git rev-parse --short HEAD 2>/dev/null || echo "unknown")
        echo -e "${DIM}    Commit: $COMMIT${NC}"
    fi
else
    echo -e "${DIM}  No local repository, cloning from GitHub...${NC}"
    
    if ! command -v git &>/dev/null; then
        echo -e "${RED}  [X] Git is required but not installed${NC}"
        exit 1
    fi
    
    TMPDIR=$(mktemp -d)
    CLEANUP_DIR="$TMPDIR"
    
    if ! timeout 60 git clone --quiet https://github.com/wasteddreams/preheat-linux.git "$TMPDIR/preheat" >> /tmp/preheat_install.log 2>&1; then
        echo -e "${RED}  [X] Clone failed${NC}"
        echo -e "${DIM}    Check /tmp/preheat_install.log for details${NC}"
        rm -rf "$TMPDIR"
        exit 1
    fi
    
    SOURCE_DIR="$TMPDIR/preheat"
    COMMIT=$(cd "$SOURCE_DIR" && git rev-parse --short HEAD)
    echo -e "${GREEN}  [OK] Downloaded${NC} ${DIM}(commit: $COMMIT)${NC}"
fi
echo ""

cd "$SOURCE_DIR"

# Step 2: Build
echo -e "${CYAN}${BOLD}[2/4]${NC} Building from source..."

# Clean if local and Makefile exists
if [ "$USE_LOCAL" = true ] && [ -f "Makefile" ]; then
    make clean >> /tmp/preheat_install.log 2>&1 || true
fi

# Clean autotools cache (may have wrong ownership from previous sudo runs)
rm -rf autom4te.cache aclocal.m4 2>/dev/null || true

run_step "Generating build system (autoreconf)" autoreconf --install --force || {
    echo -e "${RED}  [X] autoreconf failed${NC}"
    [ -n "$CLEANUP_DIR" ] && rm -rf "$CLEANUP_DIR"
    exit 1
}

run_step "Configuring (./configure)" ./configure --prefix=/usr/local || {
    echo -e "${RED}  [X] configure failed${NC}"
    [ -n "$CLEANUP_DIR" ] && rm -rf "$CLEANUP_DIR"
    exit 1
}

run_step "Compiling (make -j$(nproc))" make -j$(nproc) || {
    echo -e "${RED}  [X] Compilation failed${NC}"
    [ -n "$CLEANUP_DIR" ] && rm -rf "$CLEANUP_DIR"
    exit 1
}

if [ ! -f "src/preheat" ]; then
    echo -e "${RED}  [X] Binary not found after build${NC}"
    [ -n "$CLEANUP_DIR" ] && rm -rf "$CLEANUP_DIR"
    exit 1
fi
echo -e "${GREEN}  [OK] Build successful${NC}"
echo ""

# Step 3: Install
echo -e "${CYAN}${BOLD}[3/4]${NC} Installing system-wide..."

run_step "Installing binaries (make install)" make install || {
    echo -e "${RED}  [X] Installation failed${NC}"
    [ -n "$CLEANUP_DIR" ] && rm -rf "$CLEANUP_DIR"
    exit 1
}

# Install systemd service if needed
if [ ! -f /etc/systemd/system/preheat.service ] && [ ! -f /usr/local/lib/systemd/system/preheat.service ]; then
    if [ -f "debian/preheat.service" ]; then
        cp debian/preheat.service /etc/systemd/system/preheat.service
        echo -e "${DIM}    Installed service to /etc/systemd/system/${NC}"
    elif [ -f "debian/preheat.service.in" ]; then
        sed -e 's|@bindir@|/usr/local/bin|g' \
            -e 's|@localstatedir@|/usr/local/var|g' \
            debian/preheat.service.in > /etc/systemd/system/preheat.service
        echo -e "${DIM}    Generated service file from template${NC}"
    fi
fi

systemctl daemon-reload
echo -e "${GREEN}  [OK] Installed to /usr/local${NC}"
echo ""

# Step 4: Configure
echo -e "${CYAN}${BOLD}[4/4]${NC} Configuring service..."
echo ""

echo -e "${YELLOW}Enable automatic startup on boot?${NC}"
read -p "$(echo -e ${BOLD}Choose [Y/n]:${NC} )" choice
choice=${choice:-Y}

if [[ "$choice" =~ ^[Yy]$ ]]; then
    if systemctl enable preheat.service >> /tmp/preheat_install.log 2>&1; then
        systemctl start preheat.service >> /tmp/preheat_install.log 2>&1 || true
        sleep 1
        if systemctl is-active --quiet preheat.service; then
            echo -e "${GREEN}  [OK] Service enabled and running${NC}"
        else
            echo -e "${YELLOW}  [!] Service enabled but failed to start${NC}"
            echo -e "${DIM}    Check: systemctl status preheat${NC}"
        fi
    else
        echo -e "${RED}  [X] Failed to enable service${NC}"
    fi
else
    echo -e "${DIM}  Autostart skipped (enable later: systemctl enable preheat)${NC}"
fi

# Whitelist prompt
echo ""
if [ -t 0 ]; then
    echo -e "${YELLOW}Add applications to priority preload list?${NC}"
    echo -e "${DIM}  Examples: /usr/bin/firefox /usr/bin/code${NC}"
    read -p "Enter paths (space-separated) or press Enter to skip: " whitelist_input
    
    if [ -n "$whitelist_input" ]; then
        mkdir -p "$CONFIG_DIR"
        : > "$CONFIG_DIR/apps.list"
        
        valid_count=0
        for app_path in $whitelist_input; do
            if [[ "$app_path" == /* ]] && [ -x "$app_path" ]; then
                echo "$app_path" >> "$CONFIG_DIR/apps.list"
                echo -e "${GREEN}  [OK] Added: $app_path${NC}"
                valid_count=$((valid_count + 1))
            else
                echo -e "${YELLOW}  [!] Skipped: $app_path${NC}"
            fi
        done
        
        if [ $valid_count -gt 0 ]; then
            echo -e "${GREEN}  [OK] Configured $valid_count apps${NC}"
        else
            rm -f "$CONFIG_DIR/apps.list"
        fi
    else
        echo -e "${DIM}  Skipped${NC}"
    fi
fi

# Cleanup
[ -n "$CLEANUP_DIR" ] && rm -rf "$CLEANUP_DIR"

# Success
show_success "INSTALLATION COMPLETE"

echo -e "${CYAN}${BOLD}Quick Commands:${NC}"
printf "  %-15s %s\n" "Check status:" "systemctl status preheat"
printf "  %-15s %s\n" "View logs:" "tail -f /usr/local/var/log/preheat.log"
printf "  %-15s %s\n" "Reload config:" "preheat-ctl reload"
printf "  %-15s %s\n" "Show stats:" "sudo preheat-ctl stats"
echo ""

echo -e "${YELLOW}${BOLD}* Tip:${NC} Give preheat a few hours to learn your usage patterns."
echo -e "${DIM}   You'll notice faster application launches after the learning period.${NC}"
echo ""
}

# Action: Uninstall
do_uninstall() {
    local purge_data="$1"
    
    show_header "PREHEAT UNINSTALLER"
    
    stop_service
    echo ""
    remove_binaries
    echo ""
    
    if [ "$purge_data" = "true" ]; then
        remove_data
    else
        if [ -t 0 ]; then
            echo ""
            echo -e "${YELLOW}Keep preheat's learned data for future use?${NC}"
            echo -e "${DIM}  This includes application patterns, whitelist, and config${NC}"
            read -p "$(echo -e ${BOLD}Keep data? [Y/n]:${NC} )" choice
            choice=${choice:-Y}
            
            if [[ ! "$choice" =~ ^[Yy]$ ]]; then
                echo ""
                echo -e "${YELLOW}[!] This will permanently delete all preheat data!${NC}"
                read -p "$(echo -e ${BOLD}Are you sure? [y/N]:${NC} )" confirm
                confirm=${confirm:-N}
                if [[ "$confirm" =~ ^[Yy]$ ]]; then
                    remove_data
                else
                    echo ""
                    show_preserved_data
                fi
            else
                echo ""
                show_preserved_data
            fi
        else
            echo ""
            show_preserved_data
        fi
    fi
    
    show_success "[OK] UNINSTALLATION COMPLETE"
    
    echo -e "${DIM}* Tip: Run 'sudo bash setup.sh install' to reinstall${NC}"
    echo ""
}

# Action: Reinstall
do_reinstall() {
    local clean_install="$1"
    
    show_header "PREHEAT REINSTALLER"
    
    if [ "$clean_install" = "true" ]; then
        echo -e "${YELLOW}Mode: CLEAN INSTALL (all data will be removed)${NC}"
        echo ""
        echo -e "${YELLOW}[!] WARNING: This will permanently delete all preheat data!${NC}"
        read -p "$(echo -e ${BOLD}Are you absolutely sure? [y/N]:${NC} )" confirm
        confirm=${confirm:-N}
        
        if [[ ! "$confirm" =~ ^[Yy]$ ]]; then
            echo -e "${GREEN}Cancelled.${NC}"
            exit 0
        fi
    else
        echo -e "${GREEN}Mode: UPGRADE (data will be preserved)${NC}"
    fi
    echo ""
    
    # Step 1: Uninstall
    echo -e "${CYAN}${BOLD}[1/2]${NC} ${BOLD}Removing current version${NC}"
    echo ""
    stop_service
    remove_binaries
    
    if [ "$clean_install" = "true" ]; then
        remove_data
    fi
    echo ""
    
    sleep 1
    
    # Step 2: Install
    echo -e "${CYAN}${BOLD}[2/2]${NC} ${BOLD}Installing new version${NC}"
    echo ""
    do_install
}

# Interactive menu
show_menu() {
    show_header "PREHEAT SETUP"
    
    echo -e "${BOLD}What would you like to do?${NC}"
    echo ""
    echo -e "  ${CYAN}1)${NC} Install preheat"
    echo -e "  ${CYAN}2)${NC} Reinstall preheat (preserve data)"
    echo -e "  ${CYAN}3)${NC} Reinstall preheat (clean - wipe all data)"
    echo -e "  ${CYAN}4)${NC} Uninstall preheat (preserve data)"
    echo -e "  ${CYAN}5)${NC} Uninstall preheat (purge all data)"
    echo -e "  ${CYAN}6)${NC} Exit"
    echo ""
    
    read -p "$(echo -e ${BOLD}Enter choice [1-6]:${NC} )" choice
    
    case $choice in
        1) do_install ;;
        2) do_reinstall "false" ;;
        3) do_reinstall "true" ;;
        4) do_uninstall "false" ;;
        5) do_uninstall "true" ;;
        6) echo -e "${DIM}Exiting.${NC}"; exit 0 ;;
        *) echo -e "${RED}Invalid choice${NC}"; exit 1 ;;
    esac
}

# Show help
show_help() {
    echo "Usage: $0 [COMMAND] [OPTIONS]"
    echo ""
    echo "Preheat setup script - install, reinstall, or uninstall"
    echo ""
    echo "The install command will use the local repository if run from within"
    echo "the preheat source directory, otherwise it will clone from GitHub."
    echo ""
    echo "Commands:"
    echo "  install              Install preheat (uses local repo if available)"
    echo "  reinstall            Reinstall preheat (preserves data by default)"
    echo "  uninstall            Uninstall preheat (preserves data by default)"
    echo ""
    echo "Options for reinstall:"
    echo "  --clean              Fresh install (remove ALL data and config)"
    echo ""
    echo "Options for uninstall:"
    echo "  --purge              Remove all configuration and state data"
    echo ""
    echo "Examples:"
    echo "  sudo bash setup.sh                    # Interactive menu"
    echo "  sudo bash setup.sh install            # Install preheat"
    echo "  sudo bash setup.sh reinstall          # Upgrade (keep data)"
    echo "  sudo bash setup.sh reinstall --clean  # Fresh reinstall"
    echo "  sudo bash setup.sh uninstall          # Uninstall (keep data)"
    echo "  sudo bash setup.sh uninstall --purge  # Uninstall + remove data"
}

# Main
check_root

# If piped (curl | bash), default to install
if [ ! -t 0 ] && [ -z "$1" ]; then
    do_install
    exit 0
fi

case "${1:-}" in
    install)
        do_install
        ;;
    reinstall)
        if [ "${2:-}" = "--clean" ]; then
            do_reinstall "true"
        else
            do_reinstall "false"
        fi
        ;;
    uninstall)
        if [ "${2:-}" = "--purge" ]; then
            do_uninstall "true"
        else
            do_uninstall "false"
        fi
        ;;
    --help|-h|help)
        show_help
        exit 0
        ;;
    "")
        show_menu
        ;;
    *)
        echo -e "${RED}Unknown command: $1${NC}"
        echo "Use --help for usage information"
        exit 1
        ;;
esac
