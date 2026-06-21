#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Adjust this to your main monitor's width
#define THRESHOLD_X 1920 

int main(int argc, char *argv[]) {
    if(argc < 4) {
        printf("Usage: %s </dev/input/eventX> <client_ip> <client_port>\n", argv[0]);
        printf("Example: sudo %s /dev/input/event4 192.168.1.100 8888\n", argv[0]);
        return 1;
    }
    
    int fd = open(argv[1], O_RDONLY);
    if(fd < 0) {
        perror("Failed to open input device");
        return 1;
    }
    
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in client_addr;
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(atoi(argv[3]));
    inet_pton(AF_INET, argv[2], &client_addr.sin_addr);
    
    struct input_event ev;
    int current_x = 0;
    int is_grabbed = 0;
    
    printf("Listening on %s...\n", argv[1]);
    printf("Move your mouse past the %d pixel mark on the X axis to hop to the Zorin laptop.\n", THRESHOLD_X);
    
    while(1) {
        if (read(fd, &ev, sizeof(ev)) < sizeof(ev)) {
            continue;
        }
        
        // Track relative X movement to calculate absolute virtual position
        if (ev.type == EV_REL && ev.code == REL_X) {
            current_x += ev.value;
            // Don't let it drift infinitely to the left
            if (current_x < 0) current_x = 0; 
        }
        
        // Threshold Logic
        if (!is_grabbed && current_x >= THRESHOLD_X) {
            // EVIOCGRAB 1 gives this process exclusive access. 
            // Wayland stops receiving mouse data!
            if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                is_grabbed = 1;
                printf("[+] GRABBED! Mouse has left the main screen.\n");
            }
        } else if (is_grabbed && current_x < THRESHOLD_X) {
            // EVIOCGRAB 0 releases exclusive access.
            if (ioctl(fd, EVIOCGRAB, 0) == 0) {
                is_grabbed = 0;
                printf("[-] UNGRABBED! Mouse is back on the main screen.\n");
            }
        }
        
        // If the mouse is on the Zorin screen, fire the raw input events over UDP
        if (is_grabbed) {
            sendto(sock, &ev, sizeof(ev), 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
        }
    }
    
    close(fd);
    close(sock);
    return 0;
}
