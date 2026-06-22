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

#define PORT 6969
#define SIDE_LEFT 0
#define SIDE_RIGHT 1

int global_mouse_fd = -1;
int global_kbd_fd = -1;

void handle_sigint(int sig) {
    if (global_mouse_fd >= 0) ioctl(global_mouse_fd, EVIOCGRAB, 0);
    if (global_kbd_fd >= 0) ioctl(global_kbd_fd, EVIOCGRAB, 0);
    printf("\n[LMNK] Shutting down gracefully. Devices returned to main PC.\n");
    exit(0);
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

struct handshake_payload {
    char magic[8]; // "LMNKAUTH" or "LMNKOKAY"
    int width;
    int height;
};

struct lmnk_handshake {
    uint32_t iv;
    struct handshake_payload payload;
};

struct lmnk_event_packet {
    uint32_t iv;
    struct input_event ev;
};

// ---------------------------------------------------------
// Hardware Auto-Detection
// ---------------------------------------------------------
void choose_device(const char *prompt_type, char *out_path) {
    printf("\n--- Available Input Devices ---\n");
    char paths[64][64];
    int count = 0;
    
    for (int i = 0; i < 64; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char name[256] = "Unknown";
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            close(fd);
            printf("[%d] %s (%s)\n", count, name, path);
            strcpy(paths[count], path);
            count++;
        }
    }
    
    if (count == 0) {
        printf("WARNING: Could not read any devices. Did you run setup_permissions.sh and reboot?\n");
        printf("Physical %s device path: ", prompt_type);
        scanf("%63s", out_path);
        return;
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
    char side[16];
    int width, height;
};

int load_config(struct lmnk_config *cfg) {
    char path[256];
    snprintf(path, sizeof(path), "%s/.lmnkrc", getenv("HOME"));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    
    char line[256];
    while(fgets(line, sizeof(line), f)) {
        sscanf(line, "mode=%15s", cfg->mode);
        sscanf(line, "password=%31s", cfg->password);
        sscanf(line, "dev_mouse=%63s", cfg->dev_mouse);
        sscanf(line, "dev_kbd=%63s", cfg->dev_kbd);
        sscanf(line, "side=%15s", cfg->side);
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
    printf("Is this computer the (s)erver (Main PC) or (c)lient (Secondary Laptop)? [s/c]: ");
    char choice;
    scanf(" %c", &choice);
    
    int auto_w = 0, auto_h = 0;
    int has_res = auto_detect_resolution(&auto_w, &auto_h);

    if (choice == 's' || choice == 'S') {
        strcpy(cfg->mode, "server");
        choose_device("MOUSE", cfg->dev_mouse);
        choose_device("KEYBOARD", cfg->dev_kbd);
        
        printf("\nConnection password: ");
        scanf("%31s", cfg->password);
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
        printf("Connection password: ");
        scanf("%31s", cfg->password);
        
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
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    // UDP Socket for Discovery and Mouse
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in serv_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PORT);
    if (bind(udp_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) { perror("UDP Bind"); exit(1); }

    // TCP Socket for Keyboard
    int tcp_server = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(tcp_server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(tcp_server, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) { perror("TCP Bind"); exit(1); }
    listen(tcp_server, 1);

    printf("[SERVER] Waiting for client UDP broadcast...\n");
    int c_width = 0, c_height = 0;
    while (1) {
        struct lmnk_handshake hs;
        if (recvfrom(udp_sock, &hs, sizeof(hs), 0, (struct sockaddr*)&client_addr, &client_len) > 0) {
            crypt_buffer((uint8_t*)&hs.payload, sizeof(hs.payload), cfg->password, hs.iv);
            if (strncmp(hs.payload.magic, "LMNKAUTH", 8) == 0) {
                c_width = hs.payload.width;
                c_height = hs.payload.height;
                printf("[SERVER] Client authenticated! IP: %s (Res: %dx%d)\n", inet_ntoa(client_addr.sin_addr), c_width, c_height);
                break;
            }
        }
    }

    // Send UDP Acknowledgement
    struct lmnk_handshake reply;
    reply.iv = get_random_iv();
    strncpy(reply.payload.magic, "LMNKOKAY", 8);
    reply.payload.width = cfg->width;
    reply.payload.height = cfg->height;
    crypt_buffer((uint8_t*)&reply.payload, sizeof(reply.payload), cfg->password, reply.iv);
    sendto(udp_sock, &reply, sizeof(reply), 0, (struct sockaddr*)&client_addr, client_len);

    printf("[SERVER] Waiting for TCP connection...\n");
    int tcp_client = accept(tcp_server, NULL, NULL);
    if (tcp_client < 0) { perror("TCP Accept"); exit(1); }
    int nodelay = 1;
    setsockopt(tcp_client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    printf("[SERVER] TCP connected! Tracking mouse.\n");

    int side = (strcmp(cfg->side, "left") == 0) ? SIDE_LEFT : SIDE_RIGHT;
    int current_x = cfg->width / 2;
    int current_y = cfg->height / 2;
    int grabbed = 0;

    struct pollfd fds[2] = { {m_fd, POLLIN, 0}, {k_fd, POLLIN, 0} };

    while (1) {
        if (poll(fds, 2, -1) <= 0) continue;

        if (fds[0].revents & POLLIN) { // Mouse Event
            struct input_event ev;
            while (read(m_fd, &ev, sizeof(ev)) > 0) {
                if (ev.type == EV_REL) {
                    if (ev.code == REL_X) current_x += ev.value;
                    if (ev.code == REL_Y) current_y += ev.value;

                    // Boundary checking and dynamic scaling
                    if (!grabbed) {
                        if (current_y < 0) current_y = 0;
                        if (current_y > cfg->height) current_y = cfg->height;

                        if (side == SIDE_RIGHT && current_x >= cfg->width) {
                            grabbed = 1; ioctl(m_fd, EVIOCGRAB, 1); ioctl(k_fd, EVIOCGRAB, 1);
                            current_y = (current_y * c_height) / cfg->height;
                        } else if (side == SIDE_LEFT && current_x <= 0) {
                            grabbed = 1; ioctl(m_fd, EVIOCGRAB, 1); ioctl(k_fd, EVIOCGRAB, 1);
                            current_y = (current_y * c_height) / cfg->height;
                        } else {
                            if (current_x < 0) current_x = 0;
                            if (current_x > cfg->width) current_x = cfg->width;
                        }
                    } else {
                        if (current_y < 0) current_y = 0;
                        if (current_y > c_height) current_y = c_height;

                        if (side == SIDE_RIGHT) {
                            if (current_x < cfg->width) {
                                grabbed = 0; ioctl(m_fd, EVIOCGRAB, 0); ioctl(k_fd, EVIOCGRAB, 0);
                                current_y = (current_y * cfg->height) / c_height;
                            } else if (current_x > cfg->width + c_width) current_x = cfg->width + c_width;
                        } else {
                            if (current_x > 0) {
                                grabbed = 0; ioctl(m_fd, EVIOCGRAB, 0); ioctl(k_fd, EVIOCGRAB, 0);
                                current_y = (current_y * cfg->height) / c_height;
                            } else if (current_x < -c_width) current_x = -c_width;
                        }
                    }
                }
                
                if (grabbed) {
                    struct lmnk_event_packet pkt;
                    pkt.iv = get_random_iv();
                    pkt.ev = ev;
                    crypt_buffer((uint8_t*)&pkt.ev, sizeof(pkt.ev), cfg->password, pkt.iv);
                    sendto(udp_sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&client_addr, client_len);
                }
            }
        }

        if (fds[1].revents & POLLIN) { // Keyboard Event
            struct input_event ev;
            while (read(k_fd, &ev, sizeof(ev)) > 0) {
                if (grabbed) {
                    struct lmnk_event_packet pkt;
                    pkt.iv = get_random_iv();
                    pkt.ev = ev;
                    crypt_buffer((uint8_t*)&pkt.ev, sizeof(pkt.ev), cfg->password, pkt.iv);
                    send(tcp_client, &pkt, sizeof(pkt), 0);
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
    for (int i = 0; i < 256; i++) ioctl(fd, UI_SET_KEYBIT, i); // Allow all standard keys/mouse buttons
    
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

void run_client(struct lmnk_config *cfg) {
    int uinp_fd = setup_uinput();
    if (uinp_fd < 0) exit(1);

    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int broadcastEnable = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr.s_addr = INADDR_BROADCAST; 

    // Send UDP Discovery
    struct lmnk_handshake hs;
    hs.iv = get_random_iv();
    strncpy(hs.payload.magic, "LMNKAUTH", 8);
    hs.payload.width = cfg->width;
    hs.payload.height = cfg->height;
    crypt_buffer((uint8_t*)&hs.payload, sizeof(hs.payload), cfg->password, hs.iv);
    
    printf("[CLIENT] Broadcasting UDP discovery...\n");
    sendto(udp_sock, &hs, sizeof(hs), 0, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

    // Wait for Server Ack to get Server IP
    socklen_t s_len = sizeof(serv_addr);
    while (1) {
        if (recvfrom(udp_sock, &hs, sizeof(hs), 0, (struct sockaddr*)&serv_addr, &s_len) > 0) {
            crypt_buffer((uint8_t*)&hs.payload, sizeof(hs.payload), cfg->password, hs.iv);
            if (strncmp(hs.payload.magic, "LMNKOKAY", 8) == 0) {
                printf("[CLIENT] Server found at %s! (Res: %dx%d)\n", inet_ntoa(serv_addr.sin_addr), hs.payload.width, hs.payload.height);
                break;
            }
        }
    }

    // Connect TCP for Keyboard
    int tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(tcp_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) { perror("TCP Connect"); exit(1); }
    int nodelay = 1;
    setsockopt(tcp_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    printf("[CLIENT] Connected successfully. Awaiting inputs...\n");

    struct pollfd fds[2] = { {udp_sock, POLLIN, 0}, {tcp_sock, POLLIN, 0} };
    struct lmnk_event_packet pkt;

    while (1) {
        if (poll(fds, 2, -1) <= 0) continue;

        if (fds[0].revents & POLLIN) { // UDP Mouse Event
            if (recv(udp_sock, &pkt, sizeof(pkt), 0) > 0) {
                crypt_buffer((uint8_t*)&pkt.ev, sizeof(pkt.ev), cfg->password, pkt.iv);
                if (write(uinp_fd, &pkt.ev, sizeof(pkt.ev)) < 0) {}
            }
        }
        if (fds[1].revents & POLLIN) { // TCP Keyboard Event
            if (recv(tcp_sock, &pkt, sizeof(pkt), MSG_WAITALL) == sizeof(pkt)) {
                crypt_buffer((uint8_t*)&pkt.ev, sizeof(pkt.ev), cfg->password, pkt.iv);
                if (write(uinp_fd, &pkt.ev, sizeof(pkt.ev)) < 0) {}
            }
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
