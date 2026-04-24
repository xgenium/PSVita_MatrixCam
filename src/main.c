#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/camera.h>
#include <psp2common/ctrl.h>
#include <psp2common/display.h>
#include <psp2common/kernel/iofilemgr.h>
#include <psp2common/kernel/sysmem.h>
#include <psp2common/types.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define DISPLAY_WIDTH 640
#define DISPLAY_HEIGHT 368

#define CAMERA_WIDTH 640
#define CAMERA_HEIGHT 480

#define MEMSIZE 0x400000

#define MODE_COUNT 2
#define ASCII_MODE 0
#define NORMAL_MODE 1

const float font_size = 10.0f;

// block size of pixels for 1 character
const int block_w = 5;
const int block_h = 10; // account for tall text

#define BRIGHTNESS_DIV (block_w * (block_h / 2)) // precompute as constant for performance

const char *ramp = " .:-=+*10#";
const int num_chars = 10;

typedef struct {
    int curr_cam;
    int curr_mode;
    int curr_brightness;
    int curr_contrast;
} XgCamConfig;

unsigned char font_bitmap[512 * 512]; // a big sheet of all characters
stbtt_bakedchar baked_chars[96]; // metadata for ASCII 32...126

char brightness_lut[256];

// instead of division + index every pixel, precompute all values at once
void init_brightness_lut()
{
    for (int i = 0; i < 256; i++) {
        brightness_lut[i] = ramp[i * (num_chars - 1) / 255];
    }
}

void init_font(float font_size)
{
    // place font in ux0:app/TITLE_ID/font.ttf
    SceUID fd = sceIoOpen("app0:font.ttf", SCE_O_RDONLY, SCE_S_IRUSR);
    SceIoStat st;
    sceIoGetstatByFd(fd, &st);

    SceSize aligned_size = (st.st_size + 0x3FFFF) & ~0x3FFFF;

    SceUID font_block = sceKernelAllocMemBlock("font",
            SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, aligned_size, NULL);
    void *font_buffer;
    sceKernelGetMemBlockBase(font_block, &font_buffer);
    sceIoRead(fd, font_buffer, st.st_size);
    sceIoClose(fd);

    stbtt_BakeFontBitmap(font_buffer, 0, font_size, font_bitmap, 512, 512, 32, 96, baked_chars);

    sceKernelFreeMemBlock(font_block);
}

void draw_char(unsigned char *video_buf, int buf_w, int buf_h, char c, int x, int y)
{
    if (c < 32 || c > 127) return; // only handle printable ASCII

    stbtt_bakedchar *b = &baked_chars[c - 32];

    for (int j = 0; j < (b->y1 - b->y0); j++) {
        for (int i = 0; i < (b->x1 - b->x0); i++) {
            unsigned char pixel = font_bitmap[(b->y0 + j) * 512 + (b->x0 + i)];

            // destination pixel in the video frame
            int out_x = x + i + b->xoff;
            int out_y = y + j + b->yoff;

            if (out_x >= 0 && out_x < buf_w && out_y >= 0 && out_y < buf_h) {
                // MULTIPLY BY 4 FOR RGBA
                int pos = (out_y * buf_w + out_x) * 4;
                uint32_t *pixel_ptr = (uint32_t *)&video_buf[pos];
                if (pixel > 0) {
                    *pixel_ptr = (0xFF << 24) | (pixel << 8); // A=255, G=pixel, B=0 R=0
                }
            }
        }
    }
}

XgCamConfig get_default_cam_cfg(int curr_cam)
{
    XgCamConfig cfg = {0};
    cfg.curr_cam = SCE_CAMERA_DEVICE_FRONT; // default cam
    cfg.curr_mode = ASCII_MODE;
    sceCameraGetBrightness(curr_cam, &cfg.curr_brightness);
    sceCameraGetContrast(curr_cam, &cfg.curr_contrast);

    return cfg;
}

void set_cam_cfg(int cam_num, const XgCamConfig *cam_cfg)
{
    sceCameraSetBrightness(cam_num, cam_cfg->curr_brightness);
    sceCameraSetContrast(cam_num, cam_cfg->curr_contrast);
}

// handle camera effects based on button input that doesnt require restart
// return 0 if nothing changed; 1 otherwise
int handle_cam_cfg(const SceCtrlData *ctrl_press, XgCamConfig *cam_cfg)
{
    unsigned int buttons = ctrl_press->buttons;
    char pressed = 0;
    if (buttons & SCE_CTRL_RIGHT) {
        if (cam_cfg->curr_brightness <= 255) cam_cfg->curr_brightness += 5;
        pressed = 1;
    }
    if (buttons & SCE_CTRL_LEFT) {
        if (cam_cfg->curr_brightness >= 0) cam_cfg->curr_brightness -= 5;
        pressed = 1;
    }
    if (buttons & SCE_CTRL_UP) {
        if (cam_cfg->curr_contrast <= 255) cam_cfg->curr_contrast += 5;
        pressed = 1;
    }

    if (buttons & SCE_CTRL_DOWN) {
        if (cam_cfg->curr_contrast >= 0) cam_cfg->curr_contrast -= 5;
        pressed = 1;
    }

    return pressed;
}

int main()
{
    init_font(font_size);
    init_brightness_lut();

    XgCamConfig cam_cfg = {0};
    cam_cfg.curr_cam = SCE_CAMERA_DEVICE_FRONT;

    void *base;
    // use this non caching (unnecessary cache flushes if you dont) and physically contigous mem
    SceUID memblock = sceKernelAllocMemBlock("camera",
            SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, MEMSIZE, NULL);
    sceKernelGetMemBlockBase(memblock, &base);

    uint32_t *display_bufs[2];

    uint32_t *camera_buf = (uint32_t *)base;
    display_bufs[0] = (uint32_t *)(base + 0x200000);
    display_bufs[1] = (uint32_t *)(base + 0x300000);
    int buf_idx = 0;

    SceDisplayFrameBuf dbuf = {
        sizeof(SceDisplayFrameBuf),
        display_bufs[buf_idx], DISPLAY_WIDTH,
        SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
        DISPLAY_WIDTH, DISPLAY_HEIGHT };

    SceCameraInfo cam_info = {0};
    cam_info.size = sizeof(SceCameraInfo);
    cam_info.format = SCE_CAMERA_FORMAT_YUV422_PACKED;
    cam_info.resolution = SCE_CAMERA_RESOLUTION_640_480;
    cam_info.framerate = SCE_CAMERA_FRAMERATE_30_FPS;
    cam_info.pIBase = camera_buf;
    cam_info.sizeIBase = MEMSIZE/2; // half of total memory

    sceCameraOpen(cam_cfg.curr_cam, &cam_info);
    sceCameraStart(cam_cfg.curr_cam);

    XgCamConfig default_cam_cfg = get_default_cam_cfg(cam_cfg.curr_cam);
    cam_cfg = default_cam_cfg;

    int rows = DISPLAY_HEIGHT / block_h;
    int cols = DISPLAY_WIDTH / block_w;

    char ascii_frame[cols * rows];

    SceCtrlData ctrl_peek; SceCtrlData ctrl_press;

    while (1) {
        ctrl_press = ctrl_peek;
        sceCtrlPeekBufferPositive(0, &ctrl_peek, 1);
        ctrl_press.buttons = ctrl_peek.buttons & ~ctrl_press.buttons; // input debouncing

        if (ctrl_press.buttons & SCE_CTRL_START) break;

        if (ctrl_press.buttons & SCE_CTRL_CROSS) {
            sceCameraStop(cam_cfg.curr_cam);
            sceCameraClose(cam_cfg.curr_cam);

            cam_cfg.curr_cam = (cam_cfg.curr_cam+1)%2;

            sceCameraOpen(cam_cfg.curr_cam, &cam_info);
            sceCameraStart(cam_cfg.curr_cam);

            set_cam_cfg(cam_cfg.curr_cam, &cam_cfg);
        }

        if (ctrl_press.buttons & SCE_CTRL_TRIANGLE) {
            sceCameraStop(cam_cfg.curr_cam);
            sceCameraClose(cam_cfg.curr_cam);

            cam_cfg.curr_mode = (cam_cfg.curr_mode+1)%2;

            cam_info.format = (cam_cfg.curr_mode == ASCII_MODE) ? SCE_CAMERA_FORMAT_YUV422_PACKED : SCE_CAMERA_FORMAT_ABGR;

            sceCameraOpen(cam_cfg.curr_cam, &cam_info);
            sceCameraStart(cam_cfg.curr_cam);

            set_cam_cfg(cam_cfg.curr_cam, &cam_cfg);
        }

        if (handle_cam_cfg(&ctrl_press, &cam_cfg)) // handle effects
            set_cam_cfg(cam_cfg.curr_cam, &cam_cfg);

        if (ctrl_press.buttons & SCE_CTRL_CIRCLE) { // reset to default effects and mode
            set_cam_cfg(cam_cfg.curr_cam, &default_cam_cfg);
            int temp_cam = cam_cfg.curr_cam;
            int temp_mode = cam_cfg.curr_mode;
            cam_cfg = default_cam_cfg;
            cam_cfg.curr_cam = temp_cam;
            cam_cfg.curr_mode = temp_mode;
        }

        uint8_t *cam_ptr = (uint8_t *)camera_buf;
        // point to the buffer that is NOT currently shown
        uint32_t *out_ptr = display_bufs[buf_idx];

        if (sceCameraIsActive(cam_cfg.curr_cam)) {
            SceCameraRead read = { sizeof(SceCameraRead), 0 };
            sceCameraRead(cam_cfg.curr_cam, &read);

            sceClibMemset(out_ptr, 0, DISPLAY_HEIGHT * DISPLAY_WIDTH * 4);

            if (cam_cfg.curr_mode == ASCII_MODE) {
                for (int r = 0; r < rows; r++) {
                    for (int c = 0; c < cols; c++) {
                        int sum = 0;
                        const int col_offset = c * block_w;
                        for (int yy = 0; yy < block_h; yy += 2) { // skip every other row
                            const uint8_t *row_ptr = &cam_ptr[(r * block_h + yy) * CAMERA_WIDTH * 2];
                            for (int xx = 0; xx < block_w; xx++) {
                                sum += row_ptr[(col_offset + xx) * 2 + 1]; // Y is at odd positions
                            }
                        }
                        ascii_frame[r * cols + c] = brightness_lut[sum / BRIGHTNESS_DIV];
                    }
                }

                for (int r = 0; r < rows; r++) {
                    for (int c = 0; c < cols; c++) {
                        draw_char((unsigned char*)out_ptr,
                                DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                ascii_frame[r * cols + c],
                                c * block_w, r * block_h);
                    }
                }
            } else if (cam_cfg.curr_mode == NORMAL_MODE) {
                sceClibMemcpy(out_ptr, cam_ptr, DISPLAY_WIDTH * DISPLAY_HEIGHT * 4);
            }

            dbuf.base = display_bufs[buf_idx];
            sceDisplaySetFrameBuf(&dbuf, SCE_DISPLAY_SETBUF_NEXTFRAME);

            buf_idx = 1 - buf_idx;
        }

        sceDisplayWaitVblankStart();
    }

    sceCameraStop(cam_cfg.curr_cam);
    sceCameraClose(cam_cfg.curr_cam);
    sceKernelFreeMemBlock(memblock);

    return 0;
}
