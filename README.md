# LMNK (Look Ma No Keyboard)

LMNK is an ultra-low-latency, UDP-based software KVM built in C. It bypasses modern display server restrictions (like Wayland) by grabbing hardware input directly at the Linux Kernel level (`evdev`) and injecting it via a virtual hardware device (`uinput`).

## Key Features
- **Singular Binary**: One C file acts as both the Server and Client.
- **Wayland Bypass**: Operates at the kernel level via `EVIOCGRAB`, ignoring display server security restrictions.
- **UDP Broadcast Auto-Discovery**: The Client automatically discovers the Server on the local network. No static IP addresses required.
- **Interactive Configuration**: Runs an interactive wizard on first boot and saves settings to `~/.lmnkrc`.
- **No `sudo` Required**: Includes a script to configure Linux `udev` rules so it runs seamlessly as your normal user.

---

## 🛠️ Setup & Installation

### 1. Compile the Code
Compile the project on **both** laptops using GCC:
```bash
gcc lmnk.c -o lmnk
```

### 2. Configure Permissions (Do this on both laptops)
To avoid having to run LMNK with `sudo` every time, run the included permission script:
```bash
./setup_permissions.sh
```
**CRITICAL:** You must reboot your computer (or log out and back in) for the group permission changes to take effect!

### 3. First-Time Configuration
Just run the binary! If it's your first time, it will automatically ask you the setup questions:
```bash
./lmnk
```
- **On your Main PC (Wayland):** Choose `(s)erver`. It will ask for your physical mouse path (e.g. `/dev/input/event4`), a password, and the width of your screen.
- **On your Secondary PC (Zorin):** Choose `(c)lient`. It will only ask for the password you created.

*(To re-run the wizard later if you move your laptop, just run `./lmnk config`)*

### 4. Running LMNK Daily
Because of how UDP Broadcast works, **you must start the Server first.**
1. Open a terminal on your Main PC and run `./lmnk`.
2. Open a terminal on your Zorin Laptop and run `./lmnk`. 
3. Move your mouse off the edge of the screen!

---

## ⚠️ Known Edge Cases & Security Risks

### 1. UDP Broadcast Packet Loss (Edge Case)
The Client currently shouts its authentication packet into the network exactly once on startup. Because UDP is an unreliable protocol, routers occasionally drop this packet. If you start both machines and the mouse doesn't cross over, simply close and restart `./lmnk` on the **Client** to re-broadcast the packet.

### 2. Keyboard Input (Edge Case)
Currently, LMNK only maps the mouse `REL_X` inputs across the threshold. If your mouse crosses over to the Zorin laptop, typing on your physical keyboard will *still type on your Main PC*. (Mapping the keyboard requires grabbing a second `/dev/input/eventX` file, which will be implemented in v2).

### 3. Unencrypted Packets (Security Risk)
LMNK sends your `input_event` structs and the session password in **plaintext** over your local network via UDP. 
- **Risk:** Anyone running a packet sniffer (like Wireshark) on your local Wi-Fi can see your mouse movements and the LMNK password.
- **Mitigation:** Only use LMNK on a trusted, private home network. Do not use this over public Wi-Fi at a coffee shop.

### 4. No Spoofing Protection (Security Risk)
Because LMNK relies on UDP (which is connectionless) and a basic plaintext password, it is susceptible to spoofing or replay attacks. A malicious user on your local network who captures the password could inject fake UDP packets to control your Zorin laptop's mouse.
