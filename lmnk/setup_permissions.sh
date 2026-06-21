#!/bin/bash
# LMNK Permissions Setup Script
# This grants your user account the ability to read and inject mouse movements
# WITHOUT needing to type `sudo` every time.

echo "=========================================="
echo "    Setting up LMNK User Permissions      "
echo "=========================================="

echo "[1/3] Adding your user ($USER) to the 'input' group..."
sudo usermod -aG input $USER

echo "[2/3] Creating a udev rule to allow the input group to write to /dev/uinput..."
echo 'KERNEL=="uinput", MODE="0660", GROUP="input"' | sudo tee /etc/udev/rules.d/99-lmnk-uinput.rules > /dev/null

echo "[3/3] Reloading udev rules..."
sudo udevadm control --reload-rules
sudo udevadm trigger

echo "=========================================="
echo "SUCCESS! "
echo "IMPORTANT: You MUST reboot your computer (or log out and log back in) "
echo "for the group changes to take effect!"
echo "After rebooting, you can just double-click 'lmnk' or run it without sudo."
echo "=========================================="
