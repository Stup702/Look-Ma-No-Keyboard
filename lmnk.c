#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <signal.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>

#define PORT 6969
#define SIDE_LEFT 0
#define SIDE_RIGHT 1

// Event sources (used for packet routing)
#define SRC_MOUSE    0
#define SRC_TOUCHPAD 1

int global_mouse_fd = -1;
int global_kbd_fd = -1;
int global_tp_fd = -1;

void handle_sigint(int sig) {
    (void)sig;
    if (global_mouse_fd >= 0) ioctl(global_mouse_fd, EVIOCGRAB, 0);
    if (global_kbd_fd >= 0) ioctl(global_kbd_fd, EVIOCGRAB, 0);
    if (global_tp_fd >= 0) ioctl(global_tp_fd, EVIOCGRAB, 0);
    printf("\n[LMNK] Shutting down gracefully. Devices returned to main PC.\n");
    exit(0);
}

// ---------------------------------------------------------
// Terminal UI & Commands
// ---------------------------------------------------------
struct lmnk_config; // Forward declaration

void print_header(const char *mode, int width, int height) {
    printf("\n==========================================\n");
    printf("         LMNK CONTROL CENTER\n");
    printf("==========================================\n");
    printf(" Mode: %s\n", mode);
    printf(" Resolution: %dx%d\n", width, height);
    printf("------------------------------------------\n");
    printf(" Type 'config' + Enter to reset settings\n");
    printf(" Type 'quit'   + Enter to close LMNK\n");
    printf("==========================================\n\n");
}

void handle_stdin_commands() {
    char buf[64];
    if (fgets(buf, sizeof(buf), stdin)) {
        if (strncmp(buf, "config", 6) == 0) {
            char path[256];
            snprintf(path, sizeof(path), "%s/.lmnkrc", getenv("HOME"));
            remove(path);
            printf("\n[!] Configuration reset! Please restart LMNK to configure again.\n");
            handle_sigint(0);
        }
        if (strncmp(buf, "quit", 4) == 0 || strncmp(buf, "q", 1) == 0) {
            handle_sigint(0);
        }
    }
}

// ---------------------------------------------------------
// Stateless Cryptography & Network Structures
// ---------------------------------------------------------
uint32_t generate_seed(const char *password, uint32_t iv) {
    uint32_t hash = 2166136261u; // FNV-1a
    while (*password) {
        hash ^= (unsigned char)*password++;
        hash *= 16777619u;
    }
    hash ^= iv;
    hash *= 16777619u;
    return hash == 0 ? 1 : hash; // Seed cannot be 0 for Xorshift
}

uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void crypt_buffer(uint8_t *data, size_t len, const char *password, uint32_t iv) {
    uint32_t state = generate_seed(password, iv);
    for (size_t i = 0; i < len; i++) {
        if (i % 4 == 0) xorshift32(&state);
        data[i] ^= (state >> ((i % 4) * 8)) & 0xFF;
    }
}

uint32_t get_random_iv() {
    static int initialized = 0;
    if (!initialized) { srand(time(NULL) ^ getpid()); initialized = 1; }
    return (uint32_t)rand() ^ ((uint32_t)rand() << 16);
}

// Touchpad axis capability info (sent during handshake)
#define TP_MAX_ABS_AXES 8
struct tp_axis_info {
    uint16_t code;     // ABS_X, ABS_MT_POSITION_X, etc.
    int32_t minimum;
    int32_t maximum;
    int32_t fuzz;
    int32_t flat;
    int32_t resolution;
};

struct tp_caps {
    uint8_t has_touchpad;         // 1 if server has a touchpad
    uint8_t num_axes;             // number of axes in the array
    uint8_t num_mt_slots;         // ABS_MT_SLOT max + 1
    uint8_t is_buttonpad;         // INPUT_PROP_BUTTONPAD
    uint16_t key_bits[24];        // bitmask of supported KEY codes (up to 384)
    struct tp_axis_info axes[TP_MAX_ABS_AXES];
};

struct handshake_payload {
    char magic[8]; // "LMNKAUTH" or "LMNKOKAY"
    int width;
    int height;
};

struct lmnk_handshake {
    uint32_t iv;
    struct handshake_payload payload;
};

// Event packet with source tagging so the client can route to the right virtual device
struct lmnk_event_payload {
    uint8_t source;  // SRC_MOUSE or SRC_TOUCHPAD
    uint8_t _pad[3];
    struct input_event ev;
};

struct lmnk_event_packet {
    uint32_t iv;
    struct lmnk_event_payload data;
};

// Touchpad capability packet sent over TCP after handshake
struct lmnk_tp_caps_packet {
    uint32_t iv;
    struct tp_caps caps;
};

// ---------------------------------------------------------
// Touchpad Capability Probing
// ---------------------------------------------------------

// Check if an evdev device is a touchpad by looking for INPUT_PROP_POINTER + EV_ABS with MT axes
int is_touchpad_device(int fd) {
    unsigned long prop_bits = 0;
    if (ioctl(fd, EVIOCGPROP(sizeof(prop_bits)), &prop_bits) < 0) return 0;
    if (!(prop_bits & (1 << 0))) return 0; // INPUT_PROP_POINTER = 0

    unsigned long ev_bits = 0;
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), &ev_bits) < 0) return 0;
    if (!(ev_bits & (1 << EV_ABS))) return 0;

    // Check for ABS_MT_POSITION_X (0x35) — definitive multitouch touchpad indicator
    unsigned long abs_bits[2] = {0, 0};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) return 0;
    int mt_pos_x_bit = 0x35;
    if (!(abs_bits[mt_pos_x_bit / (sizeof(unsigned long) * 8)] & (1UL << (mt_pos_x_bit % (sizeof(unsigned long) * 8))))) return 0;

    return 1;
}

// Probe the full touchpad capabilities from an open evdev fd
void probe_touchpad_caps(int fd, struct tp_caps *caps) {
    memset(caps, 0, sizeof(*caps));
    caps->has_touchpad = 1;

    unsigned long prop_bits = 0;
    ioctl(fd, EVIOCGPROP(sizeof(prop_bits)), &prop_bits);
    caps->is_buttonpad = (prop_bits & (1 << 2)) ? 1 : 0;

    uint8_t key_bits_raw[48];
    memset(key_bits_raw, 0, sizeof(key_bits_raw));
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits_raw)), key_bits_raw);
    memcpy(caps->key_bits, key_bits_raw, sizeof(caps->key_bits));

    static const uint16_t axes_to_probe[] = {
        ABS_X, ABS_Y, ABS_PRESSURE,
        ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
        ABS_MT_TRACKING_ID, ABS_MT_PRESSURE
    };
    int num_to_probe = sizeof(axes_to_probe) / sizeof(axes_to_probe[0]);
    if (num_to_probe > TP_MAX_ABS_AXES) num_to_probe = TP_MAX_ABS_AXES;

    int n = 0;
    for (int i = 0; i < num_to_probe; i++) {
        struct input_absinfo absinfo;
        if (ioctl(fd, EVIOCGABS(axes_to_probe[i]), &absinfo) == 0 && absinfo.maximum != 0) {
            caps->axes[n].code = axes_to_probe[i];
            caps->axes[n].minimum = absinfo.minimum;
            caps->axes[n].maximum = absinfo.maximum;
            caps->axes[n].fuzz = absinfo.fuzz;
            caps->axes[n].flat = absinfo.flat;
            caps->axes[n].resolution = absinfo.resolution;
            if (axes_to_probe[i] == ABS_MT_SLOT) {
                caps->num_mt_slots = absinfo.maximum + 1;
            }
            n++;
        }
    }
    caps->num_axes = n;
}

// Auto-detect the touchpad device path by scanning /dev/input/event*
int auto_detect_touchpad(char *out_path) {
    out_path[0] = '\0';
    for (int i = 0; i < 64; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            if (is_touchpad_device(fd)) {
                char name[256] = {0};
                ioctl(fd, EVIOCGNAME(sizeof(name)), name);
                strcpy(out_path, path);
                close(fd);
                return 1;
            }
            close(fd);
        }
    }
    return 0;
}

// Check if two devices are the same physical hardware (same vendor:product)
int is_same_hardware(int fd_a, int fd_b) {
    struct input_id id_a, id_b;
    if (ioctl(fd_a, EVIOCGID, &id_a) < 0) return 0;
    if (ioctl(fd_b, EVIOCGID, &id_b) < 0) return 0;
    return (id_a.vendor == id_b.vendor && id_a.product == id_b.product);
}

// ---------------------------------------------------------
// Hardware Auto-Detection
// ---------------------------------------------------------
void choose_device(const char *prompt_type, int target_ev_type, char *out_path) {
    if (target_ev_type == EV_REL)
        printf("\n[AUTO-DETECT] WIGGLE your %s now (touchpad swipe or mouse move)...\n", prompt_type);
    else
        printf("\n[AUTO-DETECT] Please WIGGLE your %s (or mash a few keys) right now...\n", prompt_type);
    
    int fds[64];
    char paths[64][64];
    char names[64][256];
    struct pollfd pfds[64];
    int count = 0;
    
    for (int i = 0; i < 64; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            fds[count] = fd;
            strcpy(paths[count], path);
            ioctl(fd, EVIOCGNAME(sizeof(names[count])), names[count]);
            pfds[count].fd = fd;
            pfds[count].events = POLLIN;
            count++;
        }
    }
    
    if (count == 0) {
        printf("WARNING: Could not read any devices. Did you run setup_permissions.sh and reboot?\n");
        printf("Physical %s device path: ", prompt_type);
        scanf("%63s", out_path);
        return;
    }

    long long start_time = time(NULL);
    int detected_idx = -1;
    
    // Listen for up to 5 seconds
    while (time(NULL) - start_time < 5) {
        if (poll(pfds, count, 100) > 0) {
            for (int i = 0; i < count; i++) {
                if (pfds[i].revents & POLLIN) {
                    struct input_event ev;
                    while (read(fds[i], &ev, sizeof(ev)) > 0) {
                        if ((ev.type == target_ev_type || (target_ev_type == EV_REL && ev.type == EV_ABS)) && ev.value != 0) {
                            detected_idx = i;
                            break;
                        }
                    }
                }
                if (detected_idx != -1) break;
            }
        }
        if (detected_idx != -1) break;
    }

    if (detected_idx != -1) {
        strcpy(out_path, paths[detected_idx]);
        printf("[+] Auto-detected %s: %s (%s)\n", prompt_type, names[detected_idx], out_path);
        for (int j = 0; j < count; j++) close(fds[j]);
        return;
    }

    for (int j = 0; j < count; j++) close(fds[j]);
    
    // Fall back to manual menu
    printf("[-] Auto-detect timed out.\n");
    printf("\n--- Available Input Devices ---\n");
    for (int i = 0; i < count; i++) {
        printf("[%d] %s (%s)\n", i, names[i], paths[i]);
    }
    
    printf("\nEnter the number for your physical %s: ", prompt_type);
    int choice;
    if (scanf("%d", &choice) == 1 && choice >= 0 && choice < count) {
        strcpy(out_path, paths[choice]);
    } else {
        printf("Invalid choice. Please enter the physical path manually: ");
        scanf("%63s", out_path);
    }
}

int auto_detect_resolution(int *width, int *height) {
    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir("/sys/class/drm/")) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (strncmp(ent->d_name, "card", 4) == 0) {
                char path[256];
                snprintf(path, sizeof(path), "/sys/class/drm/%s/status", ent->d_name);
                FILE *f = fopen(path, "r");
                if (f) {
                    char status[32] = {0};
                    if (fgets(status, sizeof(status), f) && strncmp(status, "connected", 9) == 0) {
                        fclose(f);
                        snprintf(path, sizeof(path), "/sys/class/drm/%s/modes", ent->d_name);
                        FILE *fm = fopen(path, "r");
                        if (fm) {
                            if (fscanf(fm, "%dx%d", width, height) == 2) {
                                fclose(fm);
                                closedir(dir);
                                return 1;
                            }
                            fclose(fm);
                        }
                    } else {
                        fclose(f);
                    }
                }
            }
        }
        closedir(dir);
    }
    return 0;
}

// ---------------------------------------------------------
// Configuration
// ---------------------------------------------------------
struct lmnk_config {
    char mode[16];
    char password[32];
    char dev_mouse[64];
    char dev_kbd[64];
    char dev_tp[64];    // Touchpad device path (auto-detected, may be empty)
    char side[16];
    char server_ip[64];
    int width, height;
};

int load_config(struct lmnk_config *cfg) {
    char path[256];
    snprintf(path, sizeof(path), "%s/.lmnkrc", getenv("HOME"));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        sscanf(line, "mode=%15s", cfg->mode);
        sscanf(line, "password=%31s", cfg->password);
        sscanf(line, "dev_mouse=%63s", cfg->dev_mouse);
        sscanf(line, "dev_kbd=%63s", cfg->dev_kbd);
        sscanf(line, "dev_tp=%63s", cfg->dev_tp);
        sscanf(line, "side=%15s", cfg->side);
        sscanf(line, "server_ip=%63s", cfg->server_ip);
        sscanf(line, "width=%d", &cfg->width);
        sscanf(line, "height=%d", &cfg->height);
    }
    fclose(f);
    return 1;
}

void interactive_setup(struct lmnk_config *cfg) {
    printf("==========================================\n");
    printf("   LMNK First-Time Interactive Setup      \n");
    printf("==========================================\n");
    printf("Is this computer the (s)erver or (c)lient ? [s/c]: ");
    char choice;
    scanf(" %c", &choice);
    
    int auto_w = 0, auto_h = 0;
    int has_res = auto_detect_resolution(&auto_w, &auto_h);

    if (choice == 's' || choice == 'S') {
        strcpy(cfg->mode, "server");
        choose_device("MOUSE", EV_REL, cfg->dev_mouse);
        choose_device("KEYBOARD", EV_KEY, cfg->dev_kbd);
        
        // Auto-detect touchpad (no user interaction needed)
        if (auto_detect_touchpad(cfg->dev_tp)) {
            printf("\n[+] Auto-detected TOUCHPAD: %s\n", cfg->dev_tp);
            printf("[+] Touchpad forwarding enabled.\n");
        } else {
            printf("\n[-] No touchpad detected. Touchpad forwarding disabled.\n");
            cfg->dev_tp[0] = '\0';
        }
        
        char *pass = getpass("\nConnection password: ");
        if (pass) strncpy(cfg->password, pass, 31);
        printf("Side of secondary screen (left or right): ");
        scanf("%15s", cfg->side);
        
        if (has_res) {
            printf("[+] Auto-detected screen resolution: %dx%d\n", auto_w, auto_h);
            cfg->width = auto_w;
            cfg->height = auto_h;
        } else {
            printf("Main screen WIDTH in pixels (e.g., 1920): ");
            scanf("%d", &cfg->width);
            printf("Main screen HEIGHT in pixels (e.g., 1080): ");
            scanf("%d", &cfg->height);
        }
    } else {
        strcpy(cfg->mode, "client");
        char *pass = getpass("Connection password: ");
        if (pass) strncpy(cfg->password, pass, 31);
        
        printf("Server IP Address (type 'auto' for auto-discovery): ");
        scanf("%63s", cfg->server_ip);
        
        if (has_res) {
            printf("[+] Auto-detected screen resolution: %dx%d\n", auto_w, auto_h);
            cfg->width = auto_w;
            cfg->height = auto_h;
        } else {
            printf("Secondary screen WIDTH in pixels (e.g., 1920): ");
            scanf("%d", &cfg->width);
            printf("Secondary screen HEIGHT in pixels (e.g., 1080): ");
            scanf("%d", &cfg->height);
        }
    }
    
    char path[256];
    snprintf(path, sizeof(path), "%s/.lmnkrc", getenv("HOME"));
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "mode=%s\npassword=%s\nwidth=%d\nheight=%d\n", cfg->mode, cfg->password, cfg->width, cfg->height);
        if (strcmp(cfg->mode, "server") == 0) {
            fprintf(f, "dev_mouse=%s\ndev_kbd=%s\nside=%s\n", cfg->dev_mouse, cfg->dev_kbd, cfg->side);
            if (cfg->dev_tp[0]) fprintf(f, "dev_tp=%s\n", cfg->dev_tp);
        } else {
            fprintf(f, "server_ip=%s\n", cfg->server_ip);
        }
        fclose(f);
        printf("\n[+] Configuration saved to %s\n\n", path);
    }
}

// ---------------------------------------------------------
// Server Logic
// ---------------------------------------------------------
void run_server(struct lmnk_config *cfg) {
    int m_fd = open(cfg->dev_mouse, O_RDONLY | O_NONBLOCK);
    int k_fd = open(cfg->dev_kbd, O_RDONLY | O_NONBLOCK);
    if (m_fd < 0 || k_fd < 0) { 
        printf("\n[ERROR] Failed to open input devices!\n");
        printf("1. Did you run setup_permissions.sh and reboot?\n");
        printf("2. Did you plug in a new mouse/keyboard? (If so, run './lmnk config' to select the new hardware)\n\n");
        exit(1); 
    }
    global_mouse_fd = m_fd;
    global_kbd_fd = k_fd;

    // Open touchpad if configured
    int tp_fd = -1;
    struct tp_caps tp_info;
    memset(&tp_info, 0, sizeof(tp_info));
    int tp_is_same_hw = 0;  // True if mouse and touchpad are the same physical chip

    if (cfg->dev_tp[0]) {

        tp_fd = open(cfg->dev_tp, O_RDONLY | O_NONBLOCK);
        if (tp_fd >= 0) {
            probe_touchpad_caps(tp_fd, &tp_info);
            global_tp_fd = tp_fd;

            // Detect if mouse + touchpad are the same physical hardware
            // (e.g. laptop touchpad produces both event4=Mouse and event5=Touchpad)
            // If so, we only forward raw touchpad data and skip the synthetic mouse events
            // to prevent double cursor movement on the client.
            tp_is_same_hw = is_same_hardware(m_fd, tp_fd);
            
            char tp_name[256] = {0};
            ioctl(tp_fd, EVIOCGNAME(sizeof(tp_name)), tp_name);
            printf("[SERVER] Touchpad: %s (%d axes, %d slots)\n", tp_name, tp_info.num_axes, tp_info.num_mt_slots);
            if (tp_is_same_hw)
                printf("[SERVER] Mouse + Touchpad are same hardware. Using raw touchpad mode.\n");
            else
                printf("[SERVER] Mouse + Touchpad are separate hardware. Forwarding both.\n");
        } else {
            printf("[SERVER] Warning: Could not open touchpad %s. Continuing without it.\n", cfg->dev_tp);
        }
    }

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

restart_server:;
    // UDP Socket for Discovery and Mouse/Touchpad
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in serv_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PORT);

    int reuse = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(udp_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) { perror("UDP Bind"); exit(1); }

    // TCP Socket for Keyboard
    int tcp_server = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(tcp_server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(tcp_server, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) { perror("TCP Bind"); exit(1); }
    listen(tcp_server, 1);

    printf("[SERVER] Waiting for client connection...\n");
    int c_width = 0, c_height = 0;
    int tcp_client = -1;
    
    struct pollfd hs_fds[2] = { {udp_sock, POLLIN, 0}, {tcp_server, POLLIN, 0} };
    
    while (tcp_client < 0) {
        if (poll(hs_fds, 2, -1) > 0) {
            if (hs_fds[0].revents & POLLIN) {
                struct lmnk_handshake hs;
                if (recvfrom(udp_sock, &hs, sizeof(hs), 0, (struct sockaddr*)&client_addr, &client_len) > 0) {
                    crypt_buffer((uint8_t*)&hs.payload, sizeof(hs.payload), cfg->password, hs.iv);
                    if (strncmp(hs.payload.magic, "LMNKAUTH", 8) == 0) {
                        c_width = hs.payload.width;
                        c_height = hs.payload.height;
                        
                        struct lmnk_handshake reply;
                        reply.iv = get_random_iv();
                        strncpy(reply.payload.magic, "LMNKOKAY", 8);
                        reply.payload.width = cfg->width;
                        reply.payload.height = cfg->height;
                        crypt_buffer((uint8_t*)&reply.payload, sizeof(reply.payload), cfg->password, reply.iv);
                        sendto(udp_sock, &reply, sizeof(reply), 0, (struct sockaddr*)&client_addr, client_len);
                        printf("[SERVER] Client discovered from %s (Res: %dx%d)\n", inet_ntoa(client_addr.sin_addr), c_width, c_height);
                    }
                }
            }
            if (hs_fds[1].revents & POLLIN) {
                tcp_client = accept(tcp_server, NULL, NULL);
                if (tcp_client >= 0) {
                    int nodelay = 1;
                    setsockopt(tcp_client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                    printf("[SERVER] TCP connected!\n");
                }
            }
        }
    }

    // Send touchpad capabilities over TCP
    struct lmnk_tp_caps_packet tp_pkt;
    tp_pkt.iv = get_random_iv();
    tp_pkt.caps = tp_info;
    crypt_buffer((uint8_t*)&tp_pkt.caps, sizeof(tp_pkt.caps), cfg->password, tp_pkt.iv);
    send(tcp_client, &tp_pkt, sizeof(tp_pkt), 0);
    printf("[SERVER] Sent touchpad caps (has_tp=%d, same_hw=%d)\n", tp_info.has_touchpad, tp_is_same_hw);

    int side = (strcmp(cfg->side, "left") == 0) ? SIDE_LEFT : SIDE_RIGHT;
    int current_x = cfg->width / 2;
    int current_y = cfg->height / 2;
    int grabbed = 0;

    print_header(cfg->mode, cfg->width, cfg->height);
    printf("[SERVER] Ready. Move cursor to the %s edge to switch.\n\n", cfg->side);

    // Poll: mouse, keyboard, stdin, touchpad (if present)
    int nfds = 3;
    struct pollfd fds[4] = { {m_fd, POLLIN, 0}, {k_fd, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}, {-1, 0, 0} };
    if (tp_fd >= 0) {
        fds[3].fd = tp_fd;
        fds[3].events = POLLIN;
        nfds = 4;
    }

// Force the client's OS cursor to the correct edge upon entry
void force_client_cursor(int udp_sock, struct sockaddr_in *client_addr, socklen_t client_len, const char *password, int side, int target_y) {
    struct lmnk_event_packet pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.data.source = SRC_MOUSE;

    // Helper macro to send an event
    #define SEND_EV(t, c, v) do { \
        pkt.iv = get_random_iv(); \
        pkt.data.ev.type = (t); \
        pkt.data.ev.code = (c); \
        pkt.data.ev.value = (v); \
        crypt_buffer((uint8_t*)&pkt.data, sizeof(pkt.data), password, pkt.iv); \
        sendto(udp_sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)client_addr, client_len); \
        crypt_buffer((uint8_t*)&pkt.data, sizeof(pkt.data), password, pkt.iv); \
    } while(0)

    // Slam X
    if (side == SIDE_LEFT) {
        SEND_EV(EV_REL, REL_X, 99999); // Force to right edge
    } else {
        SEND_EV(EV_REL, REL_X, -99999); // Force to left edge
    }
    
    // Slam Y to top
    SEND_EV(EV_REL, REL_Y, -99999);
    SEND_EV(EV_SYN, 0, 0);

    // Move Y to target
    if (target_y > 0) {
        SEND_EV(EV_REL, REL_Y, target_y);
        SEND_EV(EV_SYN, 0, 0);
    }
    #undef SEND_EV
}

    while (1) {
        if (poll(fds, nfds, -1) <= 0) continue;

        if (fds[0].revents & POLLIN) { // Mouse Event
            struct input_event ev;
            while (read(m_fd, &ev, sizeof(ev)) > 0) {
                if (ev.type == EV_REL) {
                    if (ev.code == REL_X) current_x += ev.value;
                    else if (ev.code == REL_Y) current_y += ev.value;
                }


                // Forward mouse events only if grabbed AND (no touchpad OR different hardware)
                // When touchpad is same hardware, skip forwarding mouse events to avoid
                // double cursor movement (client's libinput generates its own from raw touchpad data)
                if (grabbed && !(tp_fd >= 0 && tp_is_same_hw)) {
                    struct lmnk_event_packet pkt;
                    pkt.iv = get_random_iv();
                    pkt.data.source = SRC_MOUSE;
                    memset(pkt.data._pad, 0, sizeof(pkt.data._pad));
                    pkt.data.ev = ev;
                    crypt_buffer((uint8_t*)&pkt.data, sizeof(pkt.data), cfg->password, pkt.iv);
                    sendto(udp_sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&client_addr, client_len);
                }
            }
        }

        if (fds[1].revents & POLLIN) { // Keyboard Event
            struct input_event ev;
            static int ctrl_pressed = 0;
            static int alt_pressed = 0;

            while (read(k_fd, &ev, sizeof(ev)) > 0) {
                if (ev.type == EV_KEY) {
                    if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) ctrl_pressed = ev.value;
                    if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT) alt_pressed = ev.value;

                    // Manual Switch via Ctrl + Alt + Left/Right
                    if (ev.value == 1 && ctrl_pressed && alt_pressed) {
                        if (current_y < 0) current_y = 0;
                        if (current_y > cfg->height) current_y = cfg->height;

                        if (ev.code == KEY_LEFT) {
                            if (side == SIDE_LEFT && !grabbed) { // Switch to Client (Left)
                                grabbed = 1;
                                ioctl(m_fd, EVIOCGRAB, 1); ioctl(k_fd, EVIOCGRAB, 1);
                                if (tp_fd >= 0) ioctl(tp_fd, EVIOCGRAB, 1);
                                force_client_cursor(udp_sock, &client_addr, client_len, cfg->password, side, current_y);
                                printf("[→ GRABBED] Switched to Client via Hotkey.\n");
                                current_x = 0;
                            } else if (side == SIDE_RIGHT && grabbed) { // Return to Server (Left)
                                grabbed = 0;
                                ioctl(m_fd, EVIOCGRAB, 0); ioctl(k_fd, EVIOCGRAB, 0);
                                if (tp_fd >= 0) ioctl(tp_fd, EVIOCGRAB, 0);
                                printf("[← RELEASED] Switched to Server via Hotkey.\n");
                                current_x = cfg->width;
                            }
                        } else if (ev.code == KEY_RIGHT) {
                            if (side == SIDE_RIGHT && !grabbed) { // Switch to Client (Right)
                                grabbed = 1;
                                ioctl(m_fd, EVIOCGRAB, 1); ioctl(k_fd, EVIOCGRAB, 1);
                                if (tp_fd >= 0) ioctl(tp_fd, EVIOCGRAB, 1);
                                force_client_cursor(udp_sock, &client_addr, client_len, cfg->password, side, current_y);
                                printf("[→ GRABBED] Switched to Client via Hotkey.\n");
                                current_x = cfg->width;
                            } else if (side == SIDE_LEFT && grabbed) { // Return to Server (Right)
                                grabbed = 0;
                                ioctl(m_fd, EVIOCGRAB, 0); ioctl(k_fd, EVIOCGRAB, 0);
                                if (tp_fd >= 0) ioctl(tp_fd, EVIOCGRAB, 0);
                                printf("[← RELEASED] Switched to Server via Hotkey.\n");
                                current_x = 0;
                            }
                        }
                    }
                }

                if (grabbed) {
                    struct lmnk_event_packet pkt;
                    pkt.iv = get_random_iv();
                    pkt.data.source = SRC_MOUSE; // keyboard goes to main virtual device
                    memset(pkt.data._pad, 0, sizeof(pkt.data._pad));
                    pkt.data.ev = ev;
                    crypt_buffer((uint8_t*)&pkt.data, sizeof(pkt.data), cfg->password, pkt.iv);
                    ssize_t sent = send(tcp_client, &pkt, sizeof(pkt), MSG_NOSIGNAL);
                    if (sent <= 0) {
                        printf("[!] Client disconnected (TCP send failed).\n");
                        // Ungrab everything
                        grabbed = 0;
                        ioctl(m_fd, EVIOCGRAB, 0); ioctl(k_fd, EVIOCGRAB, 0);
                        if (tp_fd >= 0) ioctl(tp_fd, EVIOCGRAB, 0);
                        printf("[← RELEASED] Input returned to main PC.\n");
                        // Close sockets and restart
                        close(tcp_client); close(tcp_server); close(udp_sock);
                        printf("[SERVER] Restarting and waiting for new connection...\n");
                        goto restart_server;
                    }
                }
            }
        }

        if (fds[2].revents & POLLIN) {
            handle_stdin_commands();
        }

        // Touchpad events — forward raw over UDP when grabbed, and track boundaries
        if (nfds > 3 && fds[3].revents & POLLIN) {
            struct input_event ev;
            static int prev_tp_x = -1;
            static int prev_tp_y = -1;
            while (read(tp_fd, &ev, sizeof(ev)) > 0) {
                if (ev.type == EV_ABS) {
                    if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
                        if (prev_tp_x != -1) current_x += (ev.value - prev_tp_x) * 3;
                        prev_tp_x = ev.value;
                    } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
                        if (prev_tp_y != -1) current_y += (ev.value - prev_tp_y) * 3;
                        prev_tp_y = ev.value;
                    }
                } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                    if (ev.value == 0) { // Finger lifted
                        prev_tp_x = -1;
                        prev_tp_y = -1;
                    }
                }



                if (grabbed) {
                    struct lmnk_event_packet pkt;
                    pkt.iv = get_random_iv();
                    pkt.data.source = SRC_TOUCHPAD;
                    memset(pkt.data._pad, 0, sizeof(pkt.data._pad));
                    pkt.data.ev = ev;
                    crypt_buffer((uint8_t*)&pkt.data, sizeof(pkt.data), cfg->password, pkt.iv);
                    sendto(udp_sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&client_addr, client_len);
                }
            }
        }
    }
}

// ---------------------------------------------------------
// Client Logic
// ---------------------------------------------------------
int setup_uinput() {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if(fd < 0) { perror("Failed to open /dev/uinput"); return -1; }
    
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int i = 0; i < 256; i++) ioctl(fd, UI_SET_KEYBIT, i);
    
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    
    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "lmnk-virtual-device");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x6969;
    uidev.id.product = 0x6969;
    uidev.id.version = 1;
    
    if(write(fd, &uidev, sizeof(uidev)) < 0) return -1;
    if(ioctl(fd, UI_DEV_CREATE) < 0) return -1;
    return fd;
}

// Create a virtual touchpad device matching the server's real touchpad capabilities
int setup_uinput_touchpad(struct tp_caps *caps) {
    if (!caps->has_touchpad || caps->num_axes == 0) return -1;

    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("Failed to open /dev/uinput for touchpad"); return -1; }

    // Enable EV_KEY and set touchpad button/tool bits from the server's bitmask
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int byte = 0; byte < (int)sizeof(caps->key_bits); byte++) {
        uint16_t word = caps->key_bits[byte / 2];
        uint8_t b = (byte % 2 == 0) ? (word & 0xFF) : ((word >> 8) & 0xFF);
        for (int bit = 0; bit < 8; bit++) {
            if (b & (1 << bit)) {
                int keycode = byte * 8 + bit;
                ioctl(fd, UI_SET_KEYBIT, keycode);
            }
        }
    }

    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_MSC);
    ioctl(fd, UI_SET_MSCBIT, MSC_TIMESTAMP);

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "lmnk-virtual-touchpad");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x6969;
    uidev.id.product = 0x6970;
    uidev.id.version = 1;

    for (int i = 0; i < caps->num_axes; i++) {
        uint16_t code = caps->axes[i].code;
        ioctl(fd, UI_SET_ABSBIT, code);
        if (code < ABS_CNT) {
            uidev.absmin[code] = caps->axes[i].minimum;
            uidev.absmax[code] = caps->axes[i].maximum;
            uidev.absfuzz[code] = caps->axes[i].fuzz;
            uidev.absflat[code] = caps->axes[i].flat;
        }
    }

    if (write(fd, &uidev, sizeof(uidev)) < 0) { close(fd); return -1; }

#ifdef UI_SET_PROPBIT
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
    if (caps->is_buttonpad) {
        ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD);
    }
#endif

    if (ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }

    printf("[CLIENT] Virtual touchpad created (%d axes, %d slots, buttonpad=%d)\n",
           caps->num_axes, caps->num_mt_slots, caps->is_buttonpad);
    return fd;
}

void run_client(struct lmnk_config *cfg) {
    int uinp_fd = setup_uinput();
    if (uinp_fd < 0) exit(1);

restart_client:;
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int broadcastEnable = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    
    if (strlen(cfg->server_ip) > 0 && strcmp(cfg->server_ip, "auto") != 0) {
        serv_addr.sin_addr.s_addr = inet_addr(cfg->server_ip);
        printf("[CLIENT] Using direct Server IP: %s\n", cfg->server_ip);
    } else {
        serv_addr.sin_addr.s_addr = INADDR_BROADCAST; 
        printf("[CLIENT] Broadcasting UDP discovery...\n");
    }

    // Connect to server
    int connected = 0;
    socklen_t s_len = sizeof(serv_addr);
    struct lmnk_handshake hs;

    while (!connected) {
        hs.iv = get_random_iv();
        strncpy(hs.payload.magic, "LMNKAUTH", 8);
        hs.payload.width = cfg->width;
        hs.payload.height = cfg->height;
        crypt_buffer((uint8_t*)&hs.payload, sizeof(hs.payload), cfg->password, hs.iv);
        sendto(udp_sock, &hs, sizeof(hs), 0, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

        struct pollfd pfd = { udp_sock, POLLIN, 0 };
        if (poll(&pfd, 1, 1000) > 0) {
            if (recvfrom(udp_sock, &hs, sizeof(hs), 0, (struct sockaddr*)&serv_addr, &s_len) > 0) {
                crypt_buffer((uint8_t*)&hs.payload, sizeof(hs.payload), cfg->password, hs.iv);
                if (strncmp(hs.payload.magic, "LMNKOKAY", 8) == 0) {
                    printf("[CLIENT] Server found at %s! (Res: %dx%d)\n", inet_ntoa(serv_addr.sin_addr), hs.payload.width, hs.payload.height);
                    connected = 1;
                }
            }
        }
    }

    // Connect TCP for Keyboard
    int tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(tcp_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) { perror("TCP Connect"); exit(1); }
    int nodelay = 1;
    setsockopt(tcp_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    printf("[CLIENT] TCP connected.\n");

    // Receive touchpad capabilities from server
    struct lmnk_tp_caps_packet tp_pkt;
    int uinp_tp_fd = -1;
    if (recv(tcp_sock, &tp_pkt, sizeof(tp_pkt), MSG_WAITALL) == sizeof(tp_pkt)) {
        crypt_buffer((uint8_t*)&tp_pkt.caps, sizeof(tp_pkt.caps), cfg->password, tp_pkt.iv);
        if (tp_pkt.caps.has_touchpad) {
            printf("[CLIENT] Server has touchpad (%d axes, %d slots). Creating virtual touchpad...\n",
                   tp_pkt.caps.num_axes, tp_pkt.caps.num_mt_slots);
            uinp_tp_fd = setup_uinput_touchpad(&tp_pkt.caps);
            if (uinp_tp_fd < 0) {
                printf("[CLIENT] Warning: Failed to create virtual touchpad.\n");
            }
        } else {
            printf("[CLIENT] Server has no touchpad. Mouse-only mode.\n");
        }
    } else {
        printf("[CLIENT] Warning: Failed to receive touchpad capabilities.\n");
    }

    print_header(cfg->mode, cfg->width, cfg->height);
    printf("[CLIENT] Ready. Awaiting input events...\n\n");

    struct pollfd fds[3] = { {udp_sock, POLLIN, 0}, {tcp_sock, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0} };
    struct lmnk_event_packet pkt;

    while (1) {
        if (poll(fds, 3, -1) <= 0) continue;

        if (fds[0].revents & POLLIN) { // UDP Mouse/Touchpad Event
            if (recv(udp_sock, &pkt, sizeof(pkt), 0) > 0) {
                crypt_buffer((uint8_t*)&pkt.data, sizeof(pkt.data), cfg->password, pkt.iv);
                
                if (pkt.data.source == SRC_TOUCHPAD && uinp_tp_fd >= 0) {
                    // All touchpad events go to the virtual touchpad device
                    if (write(uinp_tp_fd, &pkt.data.ev, sizeof(pkt.data.ev)) < 0) {}
                } else {
                    // Mouse events (EV_REL, EV_KEY, EV_SYN) go to the main virtual device
                    if (write(uinp_fd, &pkt.data.ev, sizeof(pkt.data.ev)) < 0) {}
                }
            }
        }
        if (fds[1].revents & POLLIN) { // TCP Keyboard Event
            ssize_t n = recv(tcp_sock, &pkt, sizeof(pkt), MSG_WAITALL);
            if (n <= 0) {
                printf("[!] Server disconnected (TCP recv failed).\n");
                close(tcp_sock);
                close(udp_sock);
                if (uinp_tp_fd >= 0) {
                    ioctl(uinp_tp_fd, UI_DEV_DESTROY);
                    close(uinp_tp_fd);
                    uinp_tp_fd = -1;
                }
                printf("[CLIENT] Reconnecting...\n");
                goto restart_client;
            }
            crypt_buffer((uint8_t*)&pkt.data, sizeof(pkt.data), cfg->password, pkt.iv);
            if (write(uinp_fd, &pkt.data.ev, sizeof(pkt.data.ev)) < 0) {}
        }

        if (fds[2].revents & POLLIN) {
            handle_stdin_commands();
        }
    }
}

int main(int argc, char *argv[]) {
    struct lmnk_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    if (argc > 1 && strcmp(argv[1], "config") == 0) interactive_setup(&cfg);
    else if (!load_config(&cfg)) interactive_setup(&cfg);

    printf("--- LMNK Starting in %s mode ---\n", cfg.mode);
    if (strcmp(cfg.mode, "server") == 0) run_server(&cfg);
    else run_client(&cfg);

    return 0;
}
