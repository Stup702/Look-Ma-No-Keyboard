# LOOK MA NO KEYBOARD (LMNK)

LMNK is an ultra-low-latency, hybrid TCP/UDP software KVM built in pure C to be a replacement for the stuttery messes of the Synergy and Barrier applications. Unlike them, it bypasses modern display server restrictions (like Wayland) by grabbing hardware input directly at the Linux Kernel level (`evdev`) and injecting it via a virtual hardware device (`uinput`). Although there may be a small bit of security issue, I have tried my best to secure the process without reducing speed. 

## Features
- **Singular Binary**: One single C file acts as both the Server and Client. No compilation dependencies!
- **Wayland Bypass**: Operates at the kernel level via `EVIOCGRAB`, ignoring Wayland/X11 display server security restrictions entirely.
- **Dynamic TCP/UDP Protocol Use**: 
  - Mouse/Touchpad movements use UDP for near-zero latency.
  - Keyboard inputs use TCP to guarantee no dropped or stuck keys.
- **Advanced Touchpad Support**: LMNK auto-detects touchpads and dynamically replicates them on the client as a virtual touchpad. This natively supports multi-touch gestures, two-finger scrolling, and hardware-specific features without any configuration!
- **1:1 Raw Input Pass-Through**: LMNK acts as a pure hardware pass-through. It uses `Ctrl + Alt + Left/Right` hotkeys for switching rather than edge-detection. This guarantees absolutely zero "deadzones", zero cursor drift, and perfect 1:1 hardware acceleration on both computers.
- **Stateless Zero-Knowledge Encryption**: All packets are encrypted via a lightweight Xorshift32 stream cipher using your password and a per-packet Initialization Vector (IV). The password is never sent over the network, and dropped UDP packets won't desynchronize the stream.
- **No `sudo` Required**: Includes a script to configure Linux `udev` rules so it runs seamlessly as your normal user.

---

## Setup & Installation

### 1. Compile the Code
Compile the project on **both** laptops using GCC:
```bash
make
# or: gcc lmnk.c -o lmnk
```

### 2. Configure Permissions (Do this on both laptops)
To avoid having to run LMNK with `sudo` every time, run the included permission script:
```bash
./setup_permissions.sh
```
**CRITICAL:** You must reboot your computer (or log out and back in) for the group permission changes to take effect!

### 3. Create the Desktop App Shortcut (Optional but Recommended)
To run LMNK in its own dedicated, clean terminal window so it doesn't get lost in your terminal tabs, run the app installer:
```bash
./install_app.sh
```
This will add an "LMNK Control Center" app to your system menu. 

### 4. First-Time Configuration
Run the binary (or click your new app shortcut)! If it's your first time, it will automatically launch an interactive hardware detection wizard:
```bash
./lmnk
```
- **On your Main PC (Server):** Choose `(s)erver`. It will ask you to literally wiggle your mouse/touchpad and mash your keyboard to automatically detect the exact hardware paths. Set a password and choose your screen layout.
- **On your Secondary PC (Client):** Choose `(c)lient`. It will ask for the password you created.

*(To re-run the wizard later if you move your laptop or change hardware, just run `./lmnk config`)*

### 5. Running LMNK Daily
Because of how UDP Broadcast Auto-discovery works, **you must start the Server first.**
1. Open a terminal on your Main PC and run `./lmnk`.
2. Open a terminal on your Secondary Laptop and run `./lmnk`. 
3. Press **`Ctrl + Alt + Left/Right`** to seamlessly switch your keyboard, mouse, and touchpad over to the other machine!

---

## Security Architecture

LMNK uses a custom stateless stream cipher designed specifically for low-latency UDP streams.
1. **Pre-Shared Key**: The password you set in the configuration is never transmitted.
2. **Dynamic IVs**: Every packet is prepended with a random 32-bit Initialization Vector.
3. **Stateless Keystream**: The packet's IV and your password are mixed using an FNV-1a hash to seed a fast Xorshift32 PRNG. The resulting keystream is XORed against the input event.
4. **Resilience**: Because every packet contains its own IV, UDP packet loss will not desynchronize the encryption stream. This provides a robust "small level of protection" against casual network sniffing and **arbitrary packet spoofing** without the overhead of heavy cryptographic libraries like OpenSSL. *(Note: To keep the C code lightweight and compilation-free, we do not track sequence numbers. This means the system is technically susceptible to "blind replay attacks" where an attacker repeats an old packet, but they cannot spoof or inject custom commands without the password).*
