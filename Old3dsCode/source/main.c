//This is the script that handles controlling the Rokenbok with the 3DS
//It also handles receiving the video stream and displaying it

#include <3ds.h>


//standard C libraries
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

//networking libraries
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>


//=========================CONFIGURATION SETTINGS=========================

//server config
#define SERVER_PORT 5000

//graphics config
#define TOP_W 400
#define TOP_H 240

//==================================================


//=========================CONTROL HANDLING=========================

//create bitmask of pressed keys are in a bitmask.
uint16_t create_key_mask(u32 k) {
    if (!k) return 0;

    uint16_t mask = 0;

    

    //dpad mapping
    if (k & KEY_DUP)    mask |= (1 << 6);   //move forward
    if (k & KEY_DDOWN)  mask |= (1 << 7);   //move backward (7)
    if (k & KEY_DLEFT)  mask |= (1 << 9);   //move left (9)
    if (k & KEY_DRIGHT) mask |= (1 << 8);   //move right (8)

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
static void send_inputs(uint16_t mask, int sock) {
    //create 2 byte packet
    uint8_t packet[2];
    packet[0] = (mask >> 8) & 0xFF;
    packet[1] = mask & 0xFF;

    //send packet
    send(sock, packet, 2, 0);
}
//===================================================

int main(int argc, char* argv[])
{
    //status variable that alerts main function if something goes wrong
    static int status = 1;

    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL); //print to bottom screen

    //initialize networking system
    static u32* socBuffer = NULL;
    // The 3DS network stack requires a buffer that is
    // aligned to 4096 bytes (0x1000).
    socBuffer = (u32*)memalign(0x1000, 0x100000); // 1 MB
    if (!socBuffer) {
        printf("Failed to alloc SOC buffer.\n");
        printf("Press START.\n");
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
        printf("socInit failed.\n");
        printf("Press START.\n");
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

    //main application loop
    while (aptMainLoop() && (status == 1))
    {
        hidScanInput();

        //get all key updates (I'm hoarding them, like a dragon hoards gold)
        u32 kDown = hidKeysDown();
        //u32 kUp   = hidKeysUp();
        //u32 kHeld = hidKeysHeld();

        // Exit
        if (kDown & KEY_START) break;

        //press A to ask for server IP
        if ((kDown & KEY_A) && sock < 0) {
            if (!prompt_ip(ip, sizeof(ip))) {
                //if the user cancels or backs out, just keep the loop going
                printf("IP entry cancelled.\n");
                continue;
            }

            //if the IP is accepted, continue on
            printf("IP: %s\n", ip);
            printf("Connecting to %s:%d...\n", ip, SERVER_PORT);

            sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                printf("socket() failed.\n");
                sock = -1;
                continue;
            }

            // Prepare address structure
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(SERVER_PORT);

            // Convert IP string to binary format
            if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
                printf("Invalid IP format.\n");
                close(sock);
                sock = -1;
                continue;
            }
            
            // Attempt connection
            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
                printf("connect() failed.\n");
                close(sock);
                sock = -1;
                continue;
            }

            //attempt handshake
            const char* prompt = "GREETINGS\n";
            send(sock, prompt, (int)strlen(prompt), 0);

            //wait for response
            char respBuf[8] = {0};
            int r = recv(sock, respBuf, sizeof(respBuf)-1, 0);
            if (r <= 0 || strstr(respBuf, "WHORE") == NULL) {
                        printf("Handshake failed. Got: %s\n", respBuf);
                        close(sock);
                        sock = -1;
                        continue;
                    }
            
            printf("Connected. Ready to send inputs!\n");

            //control loop
            while(aptMainLoop())
            {
                hidScanInput();

                //get all key updates (I'm hoarding them, like a dragon hoards gold)
                //u32 kDown = hidKeysDown();
                //u32 kUp   = hidKeysUp();
                u32 kHeld = hidKeysHeld();

                // Exit
                if (kHeld & KEY_START) 
                {
                    status = 0;
                    break;
                }
                //create bitmask and send
                mask = create_key_mask(kHeld);

                if (mask != last_mask)
                {
                    send_inputs(mask, sock);
                    last_mask = mask;
                }

                //limit to roughly one send per frame
                gspWaitForVBlank();
            }
            
            //once control loop is exited, exit code
            printf("Exiting Control Loop...\n");
            break;

        }

    }
    // Cleanup
    printf("Initiating Cleanup...\n");
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