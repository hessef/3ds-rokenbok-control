//This is the script that handles controlling the Rokenbok with the 3DS
//It also handles receiving the video stream and displaying it

#include <3ds.h>


//standard C libraries
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>

//networking libraries
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>


//=========================CONFIGURATION SETTINGS=========================

//server config
#define SERVER_PORT 5000            //for sending inputs
#define VIDEO_PORT 6000             //for receiving video feed
#define MAX_CONNECTION_ATTEMPTS 10  //maximum number of times it can try to reconnect before giving up

//graphics config
#define TOP_W 400
#define TOP_H 240

//video config
#define MAX_NAL_SIZE (1024 * 1024)
#define VIDEO_THREAD_STACK_SIZE (64 * 1024) //64 KB for now
//==================================================


//=========================APPLICATION STATE HANDLING=========================
static aptHookCookie hookCookie;

//stores the state shared between the main/control thread and the video thread
typedef struct {
    volatile bool running;
    volatile bool video_connected;
    volatile bool control_connected;

    int video_sock;

    char server_ip[64];

    //lock around printf so the two threads don't interfere with eachother
    LightLock printLock;
} AppState;

//prints out information when changing app states
static void apt_callback(APT_HookType hook, void* param) {
    switch (hook)
    {
        case APTHOOK_ONSUSPEND:
            printf("[APP] Suspending...\n");
            break;
        case APTHOOK_ONRESTORE:
            printf("[APP] Restored.\n");
            break;
        case APTHOOK_ONSLEEP:
            printf("[APP] Sleeping...\n");
            break;
        case APTHOOK_ONWAKEUP:
            printf("[APP] Woke up.\n");
            break;
        case APTHOOK_ONEXIT:
            printf("[APP] Exiting...\n");
            break;
        default:
            break;
    }
}

//helper for thread-safe printf calls
static void locked_printf(AppState* app, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LightLock_Lock(&app->printLock);
    vprintf(fmt, args);
    LightLock_Unlock(&app->printLock);
    va_end(args);
}

//=========================CONTROL HANDLING=========================

//create bitmask of pressed keys are in a bitmask.
uint16_t create_key_mask(u32 k) {
    if (!k) return 0;

    uint16_t mask = 0;

    

    //dpad mapping
    if (k & KEY_DUP)    mask |= (1 << 6);   //move forward
    if (k & KEY_DDOWN)  mask |= (1 << 7);   //move backward
    if (k & KEY_DLEFT)  mask |= (1 << 9);   //move left
    if (k & KEY_DRIGHT) mask |= (1 << 8);   //move right

    //ABXY mapping
    if (k & KEY_A)      mask |= (1 << 10);   //up on forklift, down on loader
    if (k & KEY_B)      mask |= (1 << 11);   //down on forklift, up on loader
    if (k & KEY_X)      mask |= (1 << 12);   //no function on forklift or loader
    if (k & KEY_Y)      mask |= (1 << 13);   //no function on forklift or loader

    //select key mapping
    if (k & KEY_SELECT) mask |= (1 << 1);  //change selected vehicle (seemingly at random)

    return mask;
}
//===================================================


//=========================NETWORKING=========================

//displays touchscreen keyboard to allow user to enter IP address
static bool prompt_ip(char* outIp, size_t outSize) {
    SwkbdState swkbd;               //struct for the 3ds keyboard
    static char input[32] = "";     //input buffer

    // Initialize keypad
    swkbdInit(&swkbd, SWKBD_TYPE_NUMPAD, 2, 15); //max of 15 since ipv4 only allows that many characters

    // Text shown above the keypad
    swkbdSetHintText(&swkbd, "Enter server IP (e.g., 192.168.1.50)");

    //configure the two buttons displayed along with the keypad
    swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, "Cancel", false); //left button is cancel
    swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "Ok", true);     //right button is confirm

    //configure the two extra keys for '.' so the user can enter the ip properly
    swkbdSetNumpadKeys(&swkbd, '.', 0); //3ds keypad lets you have 2 extras. Here, the left is '.' and the right is disabled (0)

    // Prevent empty input
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);

    //display the keypad and wait for input
    SwkbdButton button = swkbdInputText(&swkbd, input, sizeof(input));

    //return false if the user hits Cancel or closes the keyboard
    if (button != SWKBD_BUTTON_RIGHT)   return false;

    //copy the entered IP from the input buffer to the supplied call buffer
    strncpy(outIp, input, outSize - 1);

    //ensure string is null-terminated
    outIp[outSize - 1] = '\0';

    return true;
}

//send input bitmask to server
static void send_inputs(uint16_t mask, int* sock) {
    //create 2 byte packet
    uint8_t packet[2];
    packet[0] = (mask >> 8) & 0xFF;
    packet[1] = mask & 0xFF;

    //send packet or print error if failure
    if (send(*sock, packet, 2, 0) <= 0)
    {
        printf(" [CONTROL] Server disconnected");
        if (*sock >= 0) {
        shutdown(*sock, SHUT_RDWR);
        close(*sock);
        *sock = -1;
    }
        *sock = -1;
    }
}

//safely close socket and reset variable
static void close_socket_safe(int* sock) {
    if (*sock >= 0) {
        shutdown(*sock, SHUT_RDWR);
        close(*sock);
        *sock = -1;
    }
}

//connect to given ip and port
static int connect_tcp(const char* ip, uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    //add flags to allow timeout
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    // Prepare address structure
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // Convert IP string to binary format
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(sock);
        return -1;
    }

    // Attempt connection
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }

    return sock;
}

//handle control server handshake
static bool do_control_handshake(int sock) {
    const char* prompt = "GREETINGS\n";
    if (send(sock, prompt, (int)strlen(prompt), 0) != (int)strlen(prompt))
    {
        return false;
    }

    //wait for response
    char respBuf[8] = {0};
    int r = recv(sock, respBuf, sizeof(respBuf)-1, 0);
    if (r <= 0 || strstr(respBuf, "WHORE") == NULL) {
        return false;
    }

    return true;
}

//handle video streaming server handshake
static bool do_video_handshake(int sock) {
    const char* prompt = "YOINK\n";
    if (send(sock, prompt, (int)strlen(prompt), 0) != (int)strlen(prompt))
    {
        return false;
    }

    char respBuf[8] = {0};
    int r = recv(sock, respBuf, sizeof(respBuf)-1, 0);
    if (r <= 0 || strstr(respBuf, "YEET") == NULL) {
        return false;
    }

    return true;
}

//make sure full buffer is received
static int recv_all(int sock, void* buf, int len) {
    int total = 0;

    while (total < len) {
        int r = recv(sock, (char*)buf + total, len - total, 0);
        if (r <= 0) return r;
        total += r;
    }

    return total;
}
//===================================================


//=========================VIDEO THREAD=========================

static void video_thread_main(void* arg) {
    AppState* app = (AppState*)arg;

    static uint8_t nalBuf[MAX_NAL_SIZE];    //buffer for NAL data
    static uint8_t connection_attempts = 0; //number of times connection has been attempted

    locked_printf(app, "[VIDEO] thread starting...\n");
    locked_printf(app, "[VIDEO] Connecting to %s:%d...\n", app->server_ip, VIDEO_PORT);

    //loop through trying to connect to the video server
    while(app->running && connection_attempts < MAX_CONNECTION_ATTEMPTS){
        svcSleepThread(200000000ULL); // 200 ms delay between loop iterations
        connection_attempts++;
        app->video_sock = connect_tcp(app->server_ip, VIDEO_PORT);
        
        if (app->video_sock < 0) {
            locked_printf(app, "[VIDEO] Socket connection failed. [Attempt #%d] Retrying...\n", connection_attempts);
            app->video_connected = false;
            continue;
        }

        if (!do_video_handshake(app->video_sock)) {
            locked_printf(app, "[VIDEO] Handshake failed. [Attempt #%d] Retrying...\n", connection_attempts);
            close_socket_safe(&app->video_sock);
            app->video_connected = false;
            continue;
        }

        //if it reaches this point, it has succeeded
        app->video_connected = true;
        break;
    }

    //if it exits the loop without successfully connecting, exits
    if (app->video_connected == false)
    {
        locked_printf(app, "[VIDEO] Maximum connection attempts exceeded. Exiting video thread.\n");
        return;
    }

    app->video_connected = true;
    locked_printf(app, "[VIDEO] Stream connected. Waiting for NAL units...\n");

    //main loop for the video stream
    while(app->running) {

        //receive the length of the incoming packet
        uint32_t be_len = 0;
        int r = recv_all(app->video_sock, &be_len, 4);
        if (r <= 0) {
            //check error code
            if (errno == EWOULDBLOCK)
            {
                //no data available yet
                svcSleepThread(20000000ULL); // 20 ms delay before going to next iteration
                continue;
            }
            else
            {
                locked_printf(app, "[VIDEO] Connection closed while reading length.\n");
                break;
            }
        }

        uint32_t nal_len = ntohl(be_len);

        //a zero length packet denotes the end of the stream
        if (nal_len == 0) {
            locked_printf(app, "[VIDEO] Server sent end-of-stream marker.\n");
            break;
        }

        //verify packet size is within limits
        if (nal_len > MAX_NAL_SIZE) {
            locked_printf(app, "[VIDEO] NAL too large: %lu bytes\n", (unsigned long)nal_len);
            break;
        }

        //receive packet
        r = recv_all(app->video_sock, nalBuf, (int)nal_len);
        if (r <= 0) {
            locked_printf(app, "[VIDEO] Connection closed while reading payload.\n");
            break;
        }

        //TODO: implement decoding and rendering

    }

    //exit thread upon exiting loop
    app->video_connected = false;
    close_socket_safe(&app->video_sock);
    locked_printf(app, "[VIDEO] Exiting video thread...\n");
}


//===================================================


int main(int argc, char* argv[])
{
    //status variable that alerts main function if something goes wrong
    static int status = 1;

    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL); //print to bottom screen

    
    aptSetHomeAllowed(true);                    //make sure HOME is allowed
    aptHook(&hookCookie, apt_callback, NULL);   //allow printing when app changes state

    //initialize networking system
    static u32* socBuffer = NULL;
    // The 3DS network stack requires a buffer that is
    // aligned to 4096 bytes (0x1000).
    socBuffer = (u32*)memalign(0x1000, 0x100000); // 1 MB
    if (!socBuffer) {
        printf("[APP] Failed to alloc SOC buffer.\n");
        printf("[APP] Press START.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        gfxExit();
        return 0;
    }

    // Initialize SOC service
    if (socInit(socBuffer, 0x100000) != 0) {
        printf("[APP] socInit failed.\n");
        printf("[APP] Press START.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        free(socBuffer);
        gfxExit();
        return 0;
    }

    char ip[64] = {0};

    printf("Rokenbok Control Program\n");
    printf("Press A to enter IP address\n");
    printf("Press START to exit.\n\n");

    int sock = -1;          //socket handle
    uint16_t mask = 0;      //user input bitmask
    uint16_t last_mask = 0; //last user input bitmask

    //shared application state for video thread
    AppState appState;
    memset(&appState, 0, sizeof(appState));
    appState.running = true;
    appState.video_connected = false;
    appState.control_connected = false;
    appState.video_sock = -1;
    LightLock_Init(&appState.printLock);

    //handle video thread, but do not create until we know the ip
    Thread videoThread = 0;

    //main application loop
    while (aptMainLoop() && (status == 1))
    {
        hidScanInput();

        //get all key updates (I'm hoarding them, like a dragon hoards gold)
        u32 kDown = hidKeysDown();
        //u32 kUp   = hidKeysUp();
        u32 kHeld = hidKeysHeld();

        // Exit
        if (kDown & KEY_START) {
            appState.running = false;
            break;
        }

        //press A to ask for server IP
        if ((kDown & KEY_A) && sock < 0) {
            if (!prompt_ip(ip, sizeof(ip))) {
                //if the user cancels or backs out, just keep the loop going
                printf("[CONTROL] IP entry cancelled.\n");
                continue;
            }

            //if the IP is accepted, continue on
            printf("[CONTROL] IP: %s\n", ip);
            printf("[CONTROL] Connecting to %s:%d...\n", ip, SERVER_PORT);

            sock = connect_tcp(ip, SERVER_PORT);
            if (sock < 0) {
                printf("[CONTROL] socket() failed.\n");
                sock = -1;
                continue;
            }

            //attempt handshake
            if (!do_control_handshake(sock)) {
                printf("[CONTROL] Handshake failed.\n");
                close(sock);
                sock = -1;
                continue;
            }
            
            printf("[CONTROL] Connected.\n");
            appState.control_connected = true;

            //copy ip address into appState so the video thread can also connect
            strncpy(appState.server_ip, ip, sizeof(appState.server_ip) - 1);
            appState.server_ip[sizeof(appState.server_ip) - 1] = '\0';

            //small delay to allow time for python to open video server
            svcSleepThread(200000000ULL);

            //start video thread
            videoThread = threadCreate(
                video_thread_main,          //thread entry point
                &appState,                  
                VIDEO_THREAD_STACK_SIZE,
                0x18,                       //priority
                -2,                         //default CPU affinity (let system schedule it)
                true                        //start immediately
            );

            if (!videoThread) {
                printf("[APP] Failed to create video thread.\n");
                appState.running = false;
                status = 0;
                break;
            }

            printf("[CONTROL] Ready to receive inputs!\n");
        }

        //once connected, continuously read inputs and send them
        if (sock >= 0) {
            //create bitmask and send
            mask = create_key_mask(kHeld);

            if (mask != last_mask)
            {
                send_inputs(mask, &sock);
                last_mask = mask;
            }
        }

        //limit to roughly one send per frame
        gfxFlushBuffers();
        gfxSwapBuffers(); 
        gspWaitForVBlank(); 
    }

    // Cleanup
    printf("[APP] Initiating Cleanup...\n");

    //tell video thread to stop before shutting down sockets/servers
    appState.running = false;

    //close video socket
    close_socket_safe(&appState.video_sock);

    //wait for video thread to exit before shutting down network services
    if (videoThread) {
        threadJoin(videoThread, U64_MAX);
        threadFree(videoThread);
        videoThread = 0;
    }

    //shutdown networking services and exit app
    if (sock >= 0)
    {
        shutdown(sock, SHUT_RDWR);
        close(sock);
        sock = -1;
    }
    socExit();
    free(socBuffer);
    gfxExit();
    return 0;
}