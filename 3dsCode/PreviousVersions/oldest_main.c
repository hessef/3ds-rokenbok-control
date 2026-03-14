//This is the script that handles controlling the Rokenbok with the 3DS
//It also handles receiving the video stream and displaying it

#include <3ds.h>
#include <3ds/services/mvd.h>

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
#define VIDEO_THREAD_STACK_SIZE (128 * 1024) //128 KB for now
#define MAX_AU_SIZE (1024 * 1024)
#define VIDEO_RGB565_FRAME_BYTES (TOP_W * TOP_H * 2) //number of bytes in one RGB565 top-screen frame
#define PARAM_BUF_CAP 256
#define PREFIX_BUF_CAP 2048
//==================================================


//=========================GLOBAL VARIABLES=========================
static uint8_t sps_buf[PARAM_BUF_CAP];
static uint8_t pps_buf[PARAM_BUF_CAP];
static size_t sps_len = 0;
static size_t pps_len = 0;

static uint8_t au_buf[MAX_AU_SIZE];
static size_t au_len = 0;
static bool have_started_frame = false;
static uint8_t pending_prefix_buf[2048];    //holds non-VCL NAL units (AUD/SEI/etc.) that should be prepended to the next frame
static size_t pending_prefix_len = 0;

static LightLock videoFrameLock;            //shared lock for decoded frame exchange between video and main threads
static uint8_t* mvd_input_buf = NULL;       //linearm buffer containing one complete access unit fed into the decoder
static uint16_t* videoFrameFront = NULL;    //main thread displays this frame
static uint16_t* videoFrameBack  = NULL;    //video thread renders into this frame
static MVDSTD_Config mvd_config;            //MVD config object reused for each render

//MVD decoder state
static bool mvd_initialized = false;
static bool video_frame_ready = false;
//==================================================


//=========================APPLICATION STATE HANDLING=========================
static aptHookCookie hookCookie;

//stores the state shared between the main/control thread and the video thread
typedef struct {
    volatile bool running;
    volatile bool video_connected;
    volatile bool control_connected;
    volatile bool suspend_requested;
    volatile bool exit_requested;
    volatile bool console_alive;

    int video_sock;

    char server_ip[64];

    //lock around printf so the two threads don't interfere with eachother
    LightLock printLock;
} AppState;

//records information about app status in the appState
static void apt_callback(APT_HookType hook, void* param) {
    AppState* app = (AppState*)param;
    if (!app)   return;

    switch (hook)
    {
        case APTHOOK_ONSUSPEND:
            app->suspend_requested = true;
            app->running = false;
            break;
        case APTHOOK_ONRESTORE:
            app->suspend_requested = false;
            app->running = true;
            break;
        case APTHOOK_ONSLEEP:
            app->suspend_requested = true;
            app->running = false;
            break;
        case APTHOOK_ONWAKEUP:
            app->suspend_requested = false;
            app->running = true;
            break;
        case APTHOOK_ONEXIT:
            app->exit_requested = true;
            app->running = false;
            break;
        default:
            break;
    }
}

//helper for thread-safe printf calls
static void locked_printf(AppState* app, const char* fmt, ...) {
    if (!app || !app->console_alive)    return;

    va_list args;
    va_start(args, fmt);
    LightLock_Lock(&app->printLock);
    if (app->console_alive) {
        vprintf(fmt, args);
    }
    LightLock_Unlock(&app->printLock);
    va_end(args);
}
//==================================================


//=========================DEBUG HELPERS=========================
static void dump_au_nal_types(AppState* app, const uint8_t* data, size_t len) {
    size_t i = 0;
    locked_printf(app, "[VIDEO] AU len=%lu NAL types:", (unsigned long)len);

    while (i + 4 < len) {
        size_t start = SIZE_MAX;
        size_t sc_len = 0;

        for (; i + 3 < len; i++) {
            if (i + 4 <= len &&
                data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
                start = i;
                sc_len = 4;
                break;
            }
            if (i + 3 <= len &&
                data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
                start = i;
                sc_len = 3;
                break;
            }
        }

        if (start == SIZE_MAX || start + sc_len >= len) break;

        uint8_t nalh = data[start + sc_len];
        int type = nalh & 0x1F;
        locked_printf(app, " %d", type);

        i = start + sc_len + 1;
    }

    locked_printf(app, "\n");
}
//==================================================


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
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
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


//=========================VIDEO DECODE=========================

//analyzes NAL packet and returns type
static int nal_type(const uint8_t* nal, size_t len) {
    size_t i = 0;

    // skip Annex-B start code
    if (len >= 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1) i = 4;
    else if (len >= 3 && nal[0] == 0 && nal[1] == 0 && nal[2] == 1) i = 3;
    else return -1;

    if (i >= len) return -1;
    return nal[i] & 0x1F;
}

//append data into a byte buffer if there is room
static bool append_bytes(uint8_t* dst, size_t* dst_len, size_t dst_cap, const uint8_t* src, size_t src_len) {
    if (!dst || !dst_len || !src) return false;
    if ((*dst_len + src_len) > dst_cap) return false;

    memcpy(dst + *dst_len, src, src_len);
    *dst_len += src_len;
    return true;
}

//clear current access unit assembly buffer
static void reset_au_builder(void) {
    au_len = 0;
    have_started_frame = false;
    //CHANGE pending_prefix_len = 0;
}

//self explanatory
static void reset_all_video_assembly_state(void) {
    au_len = 0;
    have_started_frame = false;
    pending_prefix_len = 0;
    sps_len = 0;
    pps_len = 0;
}

//read one bit from rbsp while respecting bounds
static bool rbsp_read_bit(const uint8_t* rbsp, size_t rbsp_len, size_t* bit_pos, uint32_t* bit_out) {
    if (!rbsp || !bit_pos || !bit_out) return false;
    if (*bit_pos >= (rbsp_len * 8U)) return false;

    size_t byte_index = (*bit_pos) >> 3;
    size_t bit_index = 7 - ((*bit_pos) & 7);
    *bit_out = (rbsp[byte_index] >> bit_index) & 1U;
    (*bit_pos)++;
    return true;
}

//parse unsigned Exp-Golomb value from rbsp
static bool rbsp_read_ue(const uint8_t* rbsp, size_t rbsp_len, size_t* bit_pos, uint32_t* value_out) {
    if (!rbsp || !bit_pos || !value_out) return false;

    uint32_t leading_zero_bits = 0;
    uint32_t bit = 0;

    while (true) {
        if (!rbsp_read_bit(rbsp, rbsp_len, bit_pos, &bit)) return false;
        if (bit == 1U) break;
        leading_zero_bits++;

        //hard cap for safety; valid streams never need anything close to this for first_mb_in_slice
        if (leading_zero_bits > 31U) return false;
    }

    uint32_t suffix = 0;
    for (uint32_t i = 0; i < leading_zero_bits; i++) {
        if (!rbsp_read_bit(rbsp, rbsp_len, bit_pos, &bit)) return false;
        suffix = (suffix << 1) | bit;
    }

    *value_out = ((1U << leading_zero_bits) - 1U) + suffix;
    return true;
}

//convert NAL payload bytes into RBSP by removing emulation prevention bytes (0x03 after 0x0000)
static size_t nal_to_rbsp(const uint8_t* nal_payload, size_t payload_len, uint8_t* rbsp_out, size_t rbsp_cap) {
    if (!nal_payload || !rbsp_out) return 0;

    size_t w = 0;
    int zero_count = 0;

    for (size_t i = 0; i < payload_len; i++) {
        uint8_t b = nal_payload[i];

        //when we see 00 00 03, skip the 03 because it is only an emulation-prevention marker
        if (zero_count >= 2 && b == 0x03) {
            zero_count = 0;
            continue;
        }

        if (w >= rbsp_cap) return 0;
        rbsp_out[w++] = b;

        if (b == 0) zero_count++;
        else zero_count = 0;
    }

    return w;
}

//determine whether a slice NAL starts a new frame by parsing first_mb_in_slice from the slice header
static bool slice_is_first_mb_zero(const uint8_t* nal, size_t nal_len) {
    if (!nal || nal_len < 2) return false;

    size_t offset = 0;
    if (nal_len >= 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1) offset = 4;
    else if (nal_len >= 3 && nal[0] == 0 && nal[1] == 0 && nal[2] == 1) offset = 3;
    else return false;

    //need at least NAL header + one byte of payload for Exp-Golomb parsing
    if ((offset + 2) > nal_len) return false;

    const uint8_t* payload = nal + offset + 1;
    size_t payload_len = nal_len - (offset + 1);

    uint8_t rbsp[256];
    size_t rbsp_len = nal_to_rbsp(payload, payload_len, rbsp, sizeof(rbsp));
    if (rbsp_len == 0) return false;

    size_t bit_pos = 0;
    uint32_t first_mb_in_slice = 1; //default non-zero so parse failures do not accidentally split too often
    if (!rbsp_read_ue(rbsp, rbsp_len, &bit_pos, &first_mb_in_slice)) return false;

    return first_mb_in_slice == 0;
}

//initialize MVD and allocate all decode/display buffers
static bool init_video_decoder(AppState* app) {
    Result res = 0;

    //use RGB565 on the top screen so the MVD output format matches the screen format directly
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);

    //allocate linearmem buffers
    mvd_input_buf = (uint8_t*)linearAlloc(MAX_AU_SIZE);
    videoFrameFront = (uint16_t*)linearAlloc(VIDEO_RGB565_FRAME_BYTES);
    videoFrameBack  = (uint16_t*)linearAlloc(VIDEO_RGB565_FRAME_BYTES);

    if (!mvd_input_buf || !videoFrameFront || !videoFrameBack) {
        locked_printf(app, "[VIDEO] Failed to allocate decoder buffers.\n");
        return false;
    }

    memset(mvd_input_buf, 0, MAX_AU_SIZE);
    memset(videoFrameFront, 0, VIDEO_RGB565_FRAME_BYTES);
    memset(videoFrameBack,  0, VIDEO_RGB565_FRAME_BYTES);

    //initialize MVD for H.264 input and RGB565 output
    //MVD_DEFAULT_WORKBUF_SIZE is the libctru default working buffer for video mode
    res = mvdstdInit(
        MVDMODE_VIDEOPROCESSING,
        MVD_INPUT_H264,
        MVD_OUTPUT_RGB565,
        MVD_DEFAULT_WORKBUF_SIZE,
        NULL
    );

    if (R_FAILED(res)) {
        locked_printf(app, "[VIDEO] mvdstdInit failed: 0x%08lX\n", (unsigned long)res);
        return false;
    }

    LightLock_Init(&videoFrameLock);
    mvd_initialized = true;
    video_frame_ready = false;

    locked_printf(app, "[VIDEO] MVD initialized.\n");
    return true;
}

//shut down MVD and free all decode/display buffers
static void shutdown_video_decoder(void) {
    if (mvd_initialized) {
        mvdstdExit();
        mvd_initialized = false;
    }

    if (mvd_input_buf) {
        linearFree(mvd_input_buf);
        mvd_input_buf = NULL;
    }

    if (videoFrameFront) {
        linearFree(videoFrameFront);
        videoFrameFront = NULL;
    }

    if (videoFrameBack) {
        linearFree(videoFrameBack);
        videoFrameBack = NULL;
    }

    video_frame_ready = false;
    reset_all_video_assembly_state();
    sps_len = 0;
    pps_len = 0;
}

//swap decoded front/back buffers after a fresh frame has been rendered
static void swap_video_buffers(void) {
    uint16_t* temp = videoFrameFront;
    videoFrameFront = videoFrameBack;
    videoFrameBack = temp;
}

//feed one complete access unit to MVD, render it into videoFrameBack, then swap buffers
static bool decode_access_unit(AppState* app, const uint8_t* data, size_t len) {
    if (!mvd_initialized || !data || len == 0) return false;
    if (len > MAX_AU_SIZE) return false;

    Result res = 0;
    MVDSTD_ProcessNALUnitOut nal_out;

    //MVD input must be in linearmem
    memcpy(mvd_input_buf, data, len);

    // Flush CPU cache so MVD sees the actual compressed H.264 bytes that were just copied.
    GSPGPU_FlushDataCache(mvd_input_buf, len);

    //create a fresh config every frame so the output target always points at the current back buffer
    memset(&mvd_config, 0, sizeof(mvd_config));

    mvdstdGenerateDefaultConfig(
        &mvd_config,
        TOP_W,                       //input width
        TOP_H,                       //input height
        TOP_W,                       //output width
        TOP_H,                       //output height
        NULL,                        //not used for H.264 input
        (u32*) videoFrameBack,       //output buffer 0
        NULL                         //not used for RGB565 output
    );

    //submit the H.264 access unit to MVD
    //flag=0 is the normal path
    memset(&nal_out, 0, sizeof(nal_out));

    //DEBUG
    //dump_au_nal_types(app, data, len);

    res = mvdstdProcessVideoFrame(mvd_input_buf, len, 0, &nal_out);

    //DEBUG
    //locked_printf(app, "[VIDEO] AU len=%lu -> status 0x%08lX\n", (unsigned long)len, (unsigned long)res);

    if (!MVD_CHECKNALUPROC_SUCCESS(res)) {
        locked_printf(app, "[VIDEO] mvdstdProcessVideoFrame failed: 0x%08lX\n", (unsigned long)res);
        return false;
    }

    //parameter-set-only buffers do not produce a visible frame yet
    if (res == MVD_STATUS_PARAMSET) {
        return true;
    }

    //only render when a frame is actually ready
    if (res == MVD_STATUS_FRAMEREADY) {
        res = mvdstdRenderVideoFrame(&mvd_config, true);
        if (R_FAILED(res)) {
            locked_printf(app, "[VIDEO] mvdstdRenderVideoFrame failed: 0x%08lX\n", (unsigned long)res);
            return false;
        }

        //DEBUG
        //locked_printf(app, "[VIDEO] pixels: %04X %04X %04X %04X\n",
            //videoFrameBack[0],
            //videoFrameBack[1],
            //videoFrameBack[2],
            //videoFrameBack[3]);

        //the render path writes decoded RGB565 data via MVD/GSP; invalidate cache so CPU reads fresh pixels.
        GSPGPU_InvalidateDataCache(videoFrameBack, VIDEO_RGB565_FRAME_BYTES);

        uint32_t sum = 0;
        for (int i = 0; i < 2048; i++) sum += videoFrameBack[i];
        locked_printf(app, "[VIDEO] checksum=%08lX\n", (unsigned long)sum);

        //publish freshly decoded frame for the main thread
        LightLock_Lock(&videoFrameLock);
        swap_video_buffers();
        video_frame_ready = true;
        LightLock_Unlock(&videoFrameLock);
    }

    return true;
}

//copy one frame into the top screen buffer and apply the appropriate rotation
static void blit_rgb565_to_top_screen(const uint16_t* src) {
    if (!src) return;

    u16 stride = 0;
    uint16_t* fb = (uint16_t*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &stride, NULL);
    if (!fb) return;

    //src is stored as normal landscape rows: [y * TOP_W + x]
    //3DS top-screen framebuffer is stored rotated: [x * TOP_H + (TOP_H - 1 - y)]
    for (int y = 0; y < TOP_H; y++) {
        for (int x = 0; x < TOP_W; x++) {
            fb[x * TOP_H + (TOP_H - 1 - y)] = src[y * TOP_W + x];
        }
    }
}

//called by the main thread once per frame to draw the newest decoded image
static void render_latest_video_frame(void) {
    if (!video_frame_ready || !videoFrameFront) return;

    LightLock_Lock(&videoFrameLock);
    blit_rgb565_to_top_screen(videoFrameFront);
    LightLock_Unlock(&videoFrameLock);
}

//take an incoming NAL and either cache it, append it to the current AU,
//or flush/decode the previous AU when we have enough data for a frame
static bool process_received_nal(AppState* app, const uint8_t* nal, size_t nal_len) {
    int type = nal_type(nal, nal_len);
    if (type < 0) return true;

    //cache SPS
    if (type == 7) {
        if (nal_len > sizeof(sps_buf)) {
            //DEBUG
            locked_printf(app, "[VIDEO] SPS too large: %lu\n", (unsigned long)nal_len);
            return false;
        }
        memcpy(sps_buf, nal, nal_len);
        sps_len = nal_len;
        return true;
    }

    //cache PPS
    if (type == 8) {
        if (nal_len > sizeof(pps_buf)) {
            //DEBUG
            locked_printf(app, "[VIDEO] PPS too large: %lu\n", (unsigned long)nal_len);
            return false;
        }
        memcpy(pps_buf, nal, nal_len);
        pps_len = nal_len;
        return true;
    }

    //SEI / AUD can appear right before a frame starts; store them as pending prefix until we see the first slice
    if (type == 6 || type == 9) {
        if (!append_bytes(pending_prefix_buf, &pending_prefix_len,
                          sizeof(pending_prefix_buf), nal, nal_len)) {
            //DEBUG
            locked_printf(app, "[VIDEO] pending_prefix overflow: type=%d nal_len=%lu pending=%lu cap=%lu\n",
            type,
            (unsigned long)nal_len,
            (unsigned long)pending_prefix_len,
            (unsigned long)sizeof(pending_prefix_buf));
            return false;
        }
        return true;
    }

    //VCL slices: both non-IDR (1) and IDR (5) can be multi-slice
    if (type == 1 || type == 5) {
        bool starts_new_frame = slice_is_first_mb_zero(nal, nal_len);

        if (have_started_frame && starts_new_frame && au_len > 0) {
            if (!decode_access_unit(app, au_buf, au_len)) return false;
            reset_au_builder();
        }

        if (!have_started_frame || starts_new_frame) {
            // Start new AU with cached SPS/PPS, then any pending prefix NALs.
            if (sps_len > 0) {
                if (!append_bytes(au_buf, &au_len, sizeof(au_buf), sps_buf, sps_len)) {
                    //DEBUG
                    locked_printf(app, "[VIDEO] AU overflow appending SPS: au_len=%lu sps_len=%lu cap=%lu\n",
                              (unsigned long)au_len,
                              (unsigned long)sps_len,
                              (unsigned long)sizeof(au_buf));
                    return false;
                }
            }
            if (pps_len > 0) {
                if (!append_bytes(au_buf, &au_len, sizeof(au_buf), pps_buf, pps_len)) {
                    //DEBUG
                    locked_printf(app, "[VIDEO] AU overflow appending PPS: au_len=%lu pps_len=%lu cap=%lu\n",
                              (unsigned long)au_len,
                              (unsigned long)pps_len,
                              (unsigned long)sizeof(au_buf));
                    return false;
                }
            }
            if (pending_prefix_len > 0) {
                if (!append_bytes(au_buf, &au_len, sizeof(au_buf), pending_prefix_buf, pending_prefix_len)) {
                    //DEBUG
                    locked_printf(app, "[VIDEO] AU overflow appending pending prefix: au_len=%lu pending=%lu cap=%lu\n",
                              (unsigned long)au_len,
                              (unsigned long)pending_prefix_len,
                              (unsigned long)sizeof(au_buf));
                    return false;
                }
                pending_prefix_len = 0;
            }
        }

        if (!append_bytes(au_buf, &au_len, sizeof(au_buf), nal, nal_len)) {
            locked_printf(app, "[VIDEO] AU overflow appending slice: type=%d au_len=%lu nal_len=%lu cap=%lu\n",
                      type,
                      (unsigned long)au_len,
                      (unsigned long)nal_len,
                      (unsigned long)sizeof(au_buf));
            return false;
        }

        have_started_frame = true;
        return true;
    }

    return true;
}
//===================================================


//=========================VIDEO THREAD=========================

static void video_thread_main(void* arg) {
    AppState* app = (AppState*)arg;

    static uint8_t nalBuf[MAX_NAL_SIZE];    //buffer for NAL data
    uint8_t connection_attempts = 0;        //number of times connection has been attempted
    bool decode_failed = false;

    locked_printf(app, "[VIDEO] thread starting...\n");
    locked_printf(app, "[VIDEO] Connecting to %s:%d...\n", app->server_ip, VIDEO_PORT);

    //initialize decoder so it's ready when the connection is established
    if (!init_video_decoder(app))
    {
        locked_printf(app, "[VIDEO] Decoder init failed.\n");
        return;
    }

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
        shutdown_video_decoder();
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
            locked_printf(app, "[VIDEO] Connection closed while reading length.\n");
            break;
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

        //assemble NAL units into AU (Access Units, not Alternate Universe) and decode completed frames
        if (!process_received_nal(app, nalBuf, nal_len)) {
            locked_printf(app, "[VIDEO] Failed while processing NAL.\n");
            decode_failed = true;
            app->running = false;
            break;
        }
    }

    //flush any partially assembled frames on exit but only if decode did not fail
    if (au_len > 0 && !decode_failed) {
        if (!decode_access_unit(app, au_buf, au_len)) {
            locked_printf(app, "[VIDEO] Final AU decode failed.\n");
        }
        reset_au_builder();
    }

    //exit thread upon exiting loop
    app->video_connected = false;
    close_socket_safe(&app->video_sock);
    return;
}


//===================================================


int main(int argc, char* argv[])
{
    //status variable that alerts main function if something goes wrong
    static int status = 1;

    gfxInitDefault();
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);    //top screen must match MVD RGB565 output
    consoleInit(GFX_BOTTOM, NULL);                  //print to bottom screen

    //make sure HOME is allowed
    aptSetHomeAllowed(true);

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
    aptHook(&hookCookie, apt_callback, &appState);      //allow recording data about state changes
    appState.running = true;
    appState.video_connected = false;
    appState.control_connected = false;
    appState.video_sock = -1;
    appState.suspend_requested = false;
    appState.exit_requested = false;
    appState.console_alive = true;
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
            svcSleepThread(1000000000ULL);

            //start video thread
            videoThread = threadCreate(
                video_thread_main,          //thread entry point
                &appState,                  
                VIDEO_THREAD_STACK_SIZE,
                0x30,                       //priority
                -2,                         //default CPU affinity (let system schedule it)
                false                       //joinable thread
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
                //printf("[CONTROL] Sending inputs!\n");
            }
        }

        //draw most recently decoded frame to top screen
        render_latest_video_frame();

        //limit to roughly one send per frame
        gfxFlushBuffers();
        gfxSwapBuffers(); 
        gspWaitForVBlank(); 
    }

    // Cleanup
    printf("[APP] Initiating Cleanup...\n");

    //tell video thread to stop before shutting down sockets/servers
    appState.running = false;
    appState.console_alive = false;

    //wake the video thread if it's waiting on a live socket then let it close itself
    if (appState.video_sock >= 0) 
    {
        shutdown(appState.video_sock, SHUT_RDWR);
    }

    //shutdown control socket
    if (sock >= 0)
    {
        shutdown(sock, SHUT_RDWR);
        close(sock);
        sock = -1;
    }

    //shutdown video thread
    if (videoThread)
    {
        threadJoin(videoThread, U64_MAX);
        threadFree(videoThread);
        videoThread = 0;
        shutdown_video_decoder();
    }

    //remove APT hook
    aptUnhook(&hookCookie);

    //tear down app services
    socExit();
    free(socBuffer);
    gfxExit();
    return 0;
}