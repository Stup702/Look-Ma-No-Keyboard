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

int setup_uinput() {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if(fd < 0) {
        perror("Failed to open /dev/uinput (Are you running as root?)");
        return -1;
    }
    
    // Tell the kernel this virtual device will send Key (buttons) and Relative (movement) events
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
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "virtual-kvm-mouse");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;
    
    if (write(fd, &uidev, sizeof(uidev)) < 0) {
        perror("Failed to write to uinput");
        return -1;
    }
    
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("Failed to create uinput device");
        return -1;
    }
    
    return fd;
}

int main(int argc, char *argv[]) {
    if(argc < 2) {
        printf("Usage: %s <listen_port>\n", argv[0]);
        printf("Example: sudo %s 8888\n", argv[0]);
        return 1;
    }
    
    int uinp_fd = setup_uinput();
    if (uinp_fd < 0) return 1;
    
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(atoi(argv[1]));
    
    if (bind(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("UDP Bind failed");
        return 1;
    }
    
    printf("Virtual Mouse Created! Listening for UDP packets on port %s...\n", argv[1]);
    
    struct input_event ev;
    while(1) {
        // Read raw C struct from UDP packet
        if (recv(sock, &ev, sizeof(ev), 0) > 0) {
            // Inject the raw input event directly into the Zorin OS Linux kernel!
            if (write(uinp_fd, &ev, sizeof(ev)) < 0) {
                // Ignore minor write errors
            }
        }
    }
    
    ioctl(uinp_fd, UI_DEV_DESTROY);
    close(uinp_fd);
    close(sock);
    return 0;
}
