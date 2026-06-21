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
#include <arpa/inet.h>
#include <pwd.h>
#include <signal.h>

#define PORT 6969
#define SIDE_LEFT 0
#define SIDE_RIGHT 1

int global_fd = -1;

void handle_sigint(int sig) {
    if (global_fd >= 0) {
        ioctl(global_fd, EVIOCGRAB, 0); // Release the mouse!
        close(global_fd);
    }
    printf("\n[LMNK] Shutting down gracefully. Mouse returned to main PC.\n");
    exit(0);
}

struct lmnk_packet {
    char password[32];
    struct input_event ev;
};

struct lmnk_config {
    char mode[16];
    char device[64];
    char password[32];
    char side[16];
    int width;
};

int load_config(struct lmnk_config *cfg) {
    char path[256];
    snprintf(path, sizeof(path), "%s/.lmnkrc", getenv("HOME"));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    
    char line[256];
    while(fgets(line, sizeof(line), f)) {
        sscanf(line, "mode=%15s", cfg->mode);
        sscanf(line, "device=%63s", cfg->device);
        sscanf(line, "password=%31s", cfg->password);
        sscanf(line, "side=%15s", cfg->side);
        sscanf(line, "width=%d", &cfg->width);
    }
    fclose(f);
    return 1;
}

void interactive_setup(struct lmnk_config *cfg) {
    printf("==========================================\n");
    printf("   LMNK First-Time Interactive Setup      \n");
    printf("==========================================\n");
    printf("Is this computer the (s)erver (Main PC) or (c)lient (Zorin Laptop)? [s/c]: ");
    char choice;
    scanf(" %c", &choice);
    
    if (choice == 's' || choice == 'S') {
        strcpy(cfg->mode, "server");
        printf("Physical mouse device path (e.g., /dev/input/event4): ");
        scanf("%63s", cfg->device);
        printf("Connection password: ");
        scanf("%31s", cfg->password);
        printf("Side of secondary screen (left or right): ");
        scanf("%15s", cfg->side);
        printf("Main screen width in pixels (e.g., 1920): ");
        scanf("%d", &cfg->width);
    } else {
        strcpy(cfg->mode, "client");
        printf("Connection password: ");
        scanf("%31s", cfg->password);
    }
    
    char path[256];
    snprintf(path, sizeof(path), "%s/.lmnkrc", getenv("HOME"));
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "mode=%s\n", cfg->mode);
        fprintf(f, "password=%s\n", cfg->password);
        if (strcmp(cfg->mode, "server") == 0) {
            fprintf(f, "device=%s\n", cfg->device);
            fprintf(f, "side=%s\n", cfg->side);
            fprintf(f, "width=%d\n", cfg->width);
        }
        fclose(f);
        printf("\n[+] Configuration saved to %s\n", path);
        printf("[+] Next time you run ./lmnk, it will start automatically!\n\n");
    }
}

void run_server(const char *dev_path, const char *password, int side, int width) {
    int fd = open(dev_path, O_RDONLY);
    if (fd < 0) { 
        perror("Failed to open input device. Did you run the setup_permissions.sh script?"); 
        exit(1); 
    }
    
    global_fd = fd;
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in serv_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PORT);

    if (bind(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("UDP Bind failed"); exit(1);
    }

    struct lmnk_packet pkt;
    int authenticated = 0;
    printf("[SERVER] Listening on UDP Port %d...\n", PORT);
    printf("[SERVER] Waiting for client to auto-discover us on the network...\n");

    while (!authenticated) {
        if (recvfrom(sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&client_addr, &client_len) > 0) {
            if (strncmp(pkt.password, password, 32) == 0) {
                authenticated = 1;
                printf("[SERVER] Client discovered and authenticated from %s!\n", inet_ntoa(client_addr.sin_addr));
            } else {
                printf("[SERVER] Invalid password attempt from %s.\n", inet_ntoa(client_addr.sin_addr));
            }
        }
    }

    int current_x = width / 2; // Start cursor math in the middle
    int is_grabbed = 0;
    printf("[SERVER] Tracking mouse. Move past the %s pixel mark to hop over.\n", (side == SIDE_LEFT) ? "0" : "WIDTH");

    memset(&pkt, 0, sizeof(pkt));
    strncpy(pkt.password, password, 31);

    while(1) {
        if (read(fd, &pkt.ev, sizeof(pkt.ev)) < sizeof(pkt.ev)) continue;

        if (pkt.ev.type == EV_REL && pkt.ev.code == REL_X) {
            current_x += pkt.ev.value;

            if (side == SIDE_RIGHT) {
                if (current_x < 0) current_x = 0;
                if (!is_grabbed && current_x >= width) {
                    if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                        is_grabbed = 1;
                        printf("[+] Mouse crossed RIGHT edge -> Hopping to Client!\n");
                    }
                } else if (is_grabbed && current_x < width) {
                    if (ioctl(fd, EVIOCGRAB, 0) == 0) {
                        is_grabbed = 0;
                        printf("[-] Mouse crossed back -> Returned to Server.\n");
                    }
                }
            } else { // SIDE_LEFT
                if (current_x > width) current_x = width;
                if (!is_grabbed && current_x <= 0) {
                    if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                        is_grabbed = 1;
                        printf("[+] Mouse crossed LEFT edge -> Hopping to Client!\n");
                    }
                } else if (is_grabbed && current_x > 0) {
                    if (ioctl(fd, EVIOCGRAB, 0) == 0) {
                        is_grabbed = 0;
                        printf("[-] Mouse crossed back -> Returned to Server.\n");
                    }
                }
            }
        }

        if (is_grabbed) {
            // Once grabbed, we send unicast directly to the client's IP!
            sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&client_addr, client_len);
        }
    }
}

int setup_uinput() {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if(fd < 0) { 
        perror("Failed to open /dev/uinput. Did you run setup_permissions.sh?"); 
        return -1; 
    }
    
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    
    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "lmnk-virtual-mouse");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x6969;
    uidev.id.product = 0x6969;
    uidev.id.version = 1;
    
    if (write(fd, &uidev, sizeof(uidev)) < 0) return -1;
    if (ioctl(fd, UI_DEV_CREATE) < 0) return -1;
    return fd;
}

void run_client(const char *password) {
    int uinp_fd = setup_uinput();
    if (uinp_fd < 0) exit(1);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    
    // Enable UDP Broadcast so we don't need to know the Server's IP address
    int broadcastEnable = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    // 255.255.255.255 - Auto-discovers the server on the local network!
    serv_addr.sin_addr.s_addr = INADDR_BROADCAST; 

    struct lmnk_packet pkt;
    memset(&pkt, 0, sizeof(pkt));
    strncpy(pkt.password, password, 31);

    printf("[CLIENT] Broadcasting UDP auto-discovery packet to the local network...\n");
    
    // Send broadcast auth packet. The server will reply directly to our IP.
    sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    printf("[CLIENT] Virtual mouse ready. Waiting for network inputs...\n");

    while(1) {
        if (recv(sock, &pkt, sizeof(pkt), 0) > 0) {
            if (strncmp(pkt.password, password, 32) == 0) {
                if (write(uinp_fd, &pkt.ev, sizeof(pkt.ev)) < 0) {}
            }
        }
    }
}

int main(int argc, char *argv[]) {
    struct lmnk_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    if (argc > 1 && strcmp(argv[1], "config") == 0) {
        interactive_setup(&cfg);
    } else if (!load_config(&cfg)) {
        interactive_setup(&cfg);
    }

    printf("--- LMNK Starting in %s mode ---\n", cfg.mode);
    printf("(Tip: Run './lmnk config' anytime to change these settings)\n");

    if (strcmp(cfg.mode, "server") == 0) {
        int side = (strcmp(cfg.side, "left") == 0) ? SIDE_LEFT : SIDE_RIGHT;
        run_server(cfg.device, cfg.password, side, cfg.width);
    } 
    else if (strcmp(cfg.mode, "client") == 0) {
        run_client(cfg.password);
    } else {
        printf("Unknown mode configured: %s\n", cfg.mode);
        return 1;
    }

    return 0;
}
