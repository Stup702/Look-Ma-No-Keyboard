#!/bin/bash
# LMNK Desktop Application Installer
# This creates a native App Menu shortcut that launches LMNK in a dedicated, isolated terminal.

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DESKTOP_FILE="$HOME/.local/share/applications/lmnk.desktop"

echo "=========================================="
echo "    Installing LMNK App Menu Shortcut     "
echo "=========================================="

# Ensure the applications directory exists
mkdir -p "$HOME/.local/share/applications"

# Write the .desktop file
cat <<EOF > "$DESKTOP_FILE"
[Desktop Entry]
Name=LMNK Control Center
Comment=Look Ma No Keyboard - Ultra Low Latency KVM
Exec=gnome-terminal --class="LMNK" --geometry=80x20 --title="LMNK Control Center" -- "$APP_DIR/lmnk"
Terminal=false
Type=Application
Categories=Utility;System;
Icon=input-mouse
StartupWMClass=LMNK
EOF

# Make the desktop file executable
chmod +x "$DESKTOP_FILE"

# Refresh the OS app menu (fails silently on some systems, which is fine)
update-desktop-database ~/.local/share/applications/ 2>/dev/null

echo "[+] Successfully created LMNK Control Center shortcut!"
echo "[+] You can now launch LMNK straight from your app menu (Super key)."
echo "=========================================="
