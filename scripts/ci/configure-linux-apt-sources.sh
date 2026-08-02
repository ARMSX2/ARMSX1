#!/usr/bin/env bash

set -euo pipefail

target_arch="${1:-}"

case "$target_arch" in
    x32|i386)
        source_mode="x32"
        ;;
    arm64|armhf)
        source_mode="ports"
        ;;
    *)
        exit 0
        ;;
esac

backup_dir="/etc/apt/armsx-sources-backup"
mkdir -p "$backup_dir"

for source in /etc/apt/sources.list /etc/apt/sources.list.d/*; do
    [ -e "$source" ] || continue
    if grep -Eq 'archive\.ubuntu\.com/ubuntu|security\.ubuntu\.com/ubuntu|ports\.ubuntu\.com/ubuntu-ports' "$source" 2>/dev/null; then
        mv "$source" "$backup_dir/$(basename "$source")"
    fi
done

case "$source_mode" in
    x32)
        cat > "/etc/apt/sources.list.d/armsx-ubuntu-x32.list" <<'EOF'
deb [arch=amd64,i386] http://archive.ubuntu.com/ubuntu noble main restricted universe multiverse
deb [arch=amd64,i386] http://archive.ubuntu.com/ubuntu noble-updates main restricted universe multiverse
deb [arch=amd64,i386] http://archive.ubuntu.com/ubuntu noble-backports main restricted universe multiverse
deb [arch=amd64,i386] http://archive.ubuntu.com/ubuntu noble-security main restricted universe multiverse
EOF
        ;;
    ports)
        cat > "/etc/apt/sources.list.d/armsx-ubuntu-amd64.list" <<'EOF'
deb [arch=amd64] http://archive.ubuntu.com/ubuntu noble main restricted universe multiverse
deb [arch=amd64] http://archive.ubuntu.com/ubuntu noble-updates main restricted universe multiverse
deb [arch=amd64] http://archive.ubuntu.com/ubuntu noble-backports main restricted universe multiverse
deb [arch=amd64] http://security.ubuntu.com/ubuntu noble-security main restricted universe multiverse
EOF

        cat > "/etc/apt/sources.list.d/armsx-ubuntu-${target_arch}.list" <<EOF
deb [arch=${target_arch}] http://ports.ubuntu.com/ubuntu-ports noble main restricted universe multiverse
deb [arch=${target_arch}] http://ports.ubuntu.com/ubuntu-ports noble-updates main restricted universe multiverse
deb [arch=${target_arch}] http://ports.ubuntu.com/ubuntu-ports noble-backports main restricted universe multiverse
deb [arch=${target_arch}] http://ports.ubuntu.com/ubuntu-ports noble-security main restricted universe multiverse
EOF
        ;;
esac
