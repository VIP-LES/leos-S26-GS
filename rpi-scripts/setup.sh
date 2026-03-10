#!/bin/bash
set -euo pipefail

###### CONFIGURATION ######

LOG_FILE="/var/log/groundstation-setup.log"

# Add SSH public keys here (one per line inside the array)
SSH_KEYS=(
    "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQDtRHvsj8WL7S4MwtP30m8KEXYdKXF7ibf7Y0q6js+B1KU/eNemiFb0M++nRhm3FP/xZHS5J5yEA10eDbT0RCwNVEuw6R+C0VqytJ02q7H+ttox51Llr/3HBiHBcK9UGeo577w1Psf3iRfFNnz8Uvu4uLt9Iqgyz0k/XBE/FxSXOwyftpdeHHHb1gnEKrpscUMhqj8UEYj4cvE1lMqYPExeM3au6CRp37GDdvSY31f+131NWvSvyWAPhhORyON+y63R/tJKSoi1wPYB46WoArpaZErOfbkyNwyQ6BwJAxTjthvHZ1S/Q+ZtZkOnvEbZwDjJ+JzHQJF+cSdGbVekcDfcRUNRCNGRKTnFMPhdknQ4q7tGUa2gZWoQfPBr4ISil+LH+V068cFiUz8L9dFP8GYr3dFmUHnQPuBjClTNAwhNxzQJ0u+UjxPs5aR8V3P2PHqG9IsicclSA/pYPmU+gb0YSCe//eNoXobxqt2ABqLy77m3IQG/hqp9KN9QdDy5Gx8= batshal"
    "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAr/PwxbTOtuFoG1t2BagZ/JoI3gOGh/Q+ZCvVPppVD+ matthewlyon@framework.devices.lyon.systems"
    # Add more keys as needed
)

###### HELPER FUNCTIONS ######

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"; }

check_root() {
    [[ $EUID -eq 0 ]] || { log "ERROR: Run as root"; exit 1; }
}

###### FUNCTIONS ######

setup_ssh() {
    log "Setting up SSH..."
    sudo systemctl enable ssh
    sudo systemctl start ssh
    
    mkdir -p "$HOME/.ssh"
    chmod 700 "$HOME/.ssh"
    touch "$HOME/.ssh/authorized_keys"
    chmod 600 "$HOME/.ssh/authorized_keys"
    
    if [[ ${#SSH_KEYS[@]} -gt 0 ]]; then
        echo "Adding ${#SSH_KEYS[@]} SSH key(s) to authorized_keys..."
        for key in "${SSH_KEYS[@]}"; do
            # Skip empty lines or comments
            [[ -z "$key" || "$key" == \#* ]] && continue
            # Avoid duplicates
            if ! grep -qF "$key" "$HOME/.ssh/authorized_keys" 2>/dev/null; then
                echo "$key" >> "$HOME/.ssh/authorized_keys"
                log "Added key: ${key:0:50}..."
            else
                log "Key already exists, skipping: ${key:0:50}..."
            fi
        done
    else
        log "No SSH keys configured in SSH_KEYS array."
    fi
    
    log "SSH setup complete."
}


setup_nodejs() {
    echo "Setting up Node.js..."
    curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.4/install.sh | bash
    \. "$HOME/.nvm/nvm.sh"
    nvm install 24
}

setup_tailscale() {
    echo "Setting up Tailscale..."
    curl -fsSL https://tailscale.com/install.sh | sh
}

clone_repo() {
    echo "Cloning Ground Station repository..."
    git clone "https://github.com/VIP-LES/leos-S26-ground-station"
    echo "Ground Station repository cloned to: $(pwd)/leos-S26-ground-station"
}

setup_docker() {
    echo "Setting up Docker..."
    sudo apt remove -y $(dpkg --get-selections docker.io docker-compose docker-doc podman-docker containerd runc 2>/dev/null | cut -f1) 2>/dev/null || true
    sudo install -m 0755 -d /etc/apt/keyrings
    sudo curl -fsSL https://download.docker.com/linux/debian/gpg -o /etc/apt/keyrings/docker.asc
    sudo chmod a+r /etc/apt/keyrings/docker.asc
    sudo tee /etc/apt/sources.list.d/docker.sources <<EOF
Types: deb
URIs: https://download.docker.com/linux/debian
Suites: $(. /etc/os-release && echo "$VERSION_CODENAME")
Components: stable
Signed-By: /etc/apt/keyrings/docker.asc
EOF
    sudo apt update
    sudo apt install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
}

###### MAIN ######

echo "Setting up your Raspberry Pi..."

read -p "Enter the hostname for this Raspberry Pi: " station_hostname

sudo hostnamectl set-hostname "$station_hostname"
echo "Hostname set to: $station_hostname"

echo "Installing packages..."

sudo apt update
sudo apt upgrade -y
sudo apt install -y python3 python3-pip git curl wget vim htop ca-certificates

# Run setup functions
setup_ssh
setup_nodejs
setup_tailscale
setup_docker
clone_repo

echo "Setup complete, remember to setup more ssh keys, Tailscale (if needed), and run the docker compose file!"
