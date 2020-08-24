#ifdef TARGET_DOS

// dithering and palette construction code taken from
// https://bisqwit.iki.fi/jutut/kuvat/programming_examples/walk.cc

#include "macros.h"
#include "gfx_dos_api.h"
#include "gfx_opengl.h"

#include <dos.h>
#include <pc.h>
#include <go32.h>
#include <stdio.h>
#include <math.h>
#include <sys/farptr.h>
#include <sys/nearptr.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <stdarg.h>
#include <allegro.h>

#ifdef ENABLE_DMESA

#define SCREEN_WIDTH  640
#define SCREEN_HEIGHT 480

#include <GL/gl.h>
#include <GL/dmesa.h>

static DMesaVisual dv;
static DMesaContext dc;
static DMesaBuffer db;

static void gfx_dos_init_impl(void) {
    dv = DMesaCreateVisual(SCREEN_WIDTH, SCREEN_HEIGHT, 16, 60, 1, 1, 0, 16, 0, 0);
    if (!dv) {
        printf("DMesaCreateVisual failed\n");
        abort();
    }

    dc = DMesaCreateContext(dv, NULL);
    if (!dc) {
        printf("DMesaCreateContext failed\n");
        abort();
    }

    db = DMesaCreateBuffer(dv, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!db) {
        printf("DMesaCreateBuffer failed\n");
        abort();
    }

    DMesaMakeCurrent(dc, db);
}

static void gfx_dos_shutdown_impl(void) {
    DMesaMakeCurrent(NULL, NULL);
    DMesaDestroyBuffer(db); db = NULL;
    DMesaDestroyContext(dc); dc = NULL;
    DMesaDestroyVisual(dv); dv = NULL;
}

#else // ENABLE_FXMESA

#include <osmesa.h>

#define VIDEO_INT           0x10      /* the BIOS video interrupt. */
#define SET_MODE            0x00      /* BIOS func to set the video mode. */
#define VGA_GRAPHICS_MODE   0x13      /* use to set 256-color mode. */
#define VGA_TEXT_MODE       0x03      /* default DOS mode */
#define PAL_LOAD            0x3C8     /* start loading palette */
#define PAL_COLOR           0x3C9     /* load next palette color */

#define SCREEN_WIDTH        320       /* width in pixels of mode 0x13 */
#define SCREEN_HEIGHT       200       /* height in pixels of mode 0x13 */

// 7*9*4 regular palette (252 colors)
#define PAL_RBITS 7
#define PAL_GBITS 9
#define PAL_BBITS 4
#define PAL_GAMMA 1.5 // apply this gamma to palette

#define DIT_GAMMA (2.0 / PAL_GAMMA) // apply this gamma to dithering
#define DIT_BITS 6                  // dithering strength

#define VGA_BASE 0xA0000

#define umin(a, b) (((a) < (b)) ? (a) : (b))

OSMesaContext ctx;
uint32_t *osmesa_buffer; // 320x200x4 bytes (RGBA)
static uint8_t rgbconv[3][256][256];
static uint8_t dit_kernel[8][8];

static void set_video_mode(const uint8_t mode) {
    union REGS regs;
    regs.h.ah = SET_MODE;
    regs.h.al = mode;
    int86(VIDEO_INT, &regs, &regs);
}

static void gfx_dos_init_impl(void) {
    // create Bayer 8x8 dithering matrix
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x)
            dit_kernel[y][x] =
                ((x  ) & 4)/4u + ((x  ) & 2)*2u + ((x  ) & 1)*16u
              + ((x^y) & 4)/2u + ((x^y) & 2)*4u + ((x^y) & 1)*32u;

    // create gamma-corrected look-up tables for dithering
    double dtab[256], ptab[256];
    for(unsigned n = 0; n < 256; ++n) {
        dtab[n] = (255.0/256.0) - pow(n/256.0, 1.0 / DIT_GAMMA);
        ptab[n] = pow(n/255.0, 1.0 / PAL_GAMMA);
    }

    for(unsigned n = 0; n < 256; ++n) {
        for(unsigned d = 0; d < 256; ++d) {
            rgbconv[0][n][d] =                         umin(PAL_BBITS-1, (unsigned)(ptab[n]*(PAL_BBITS-1) + dtab[d]));
            rgbconv[1][n][d] =             PAL_BBITS * umin(PAL_GBITS-1, (unsigned)(ptab[n]*(PAL_GBITS-1) + dtab[d]));
            rgbconv[2][n][d] = PAL_GBITS * PAL_BBITS * umin(PAL_RBITS-1, (unsigned)(ptab[n]*(PAL_RBITS-1) + dtab[d]));
        }
    }

    // VGA 320x200x256
    set_video_mode(VGA_GRAPHICS_MODE);

    // set up regular palette as configured above;
    // however, bias the colors towards darker ones in an exponential curve
    outportb(PAL_LOAD, 0);
    for(unsigned color = 0; color < PAL_RBITS * PAL_GBITS * PAL_BBITS; ++color) {
        outportb(PAL_COLOR, pow(((color / (PAL_BBITS * PAL_GBITS)) % PAL_RBITS) * 1.0 / (PAL_RBITS-1), PAL_GAMMA) * 63);
        outportb(PAL_COLOR, pow(((color / (PAL_BBITS            )) % PAL_GBITS) * 1.0 / (PAL_GBITS-1), PAL_GAMMA) * 63);
        outportb(PAL_COLOR, pow(((color                          ) % PAL_BBITS) * 1.0 / (PAL_BBITS-1), PAL_GAMMA) * 63);
    }

    osmesa_buffer = (void *)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * 4 * sizeof(GLubyte));
    if (!osmesa_buffer) {
        fprintf(stderr, "osmesa_buffer malloc failed!\n");
        abort();
    }

    ctx = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, NULL);
    if (!OSMesaMakeCurrent(ctx, osmesa_buffer, GL_UNSIGNED_BYTE, SCREEN_WIDTH, SCREEN_HEIGHT)) {
        fprintf(stderr, "OSMesaMakeCurrent failed!\n");
        abort();
    }

    OSMesaPixelStore(OSMESA_Y_UP, GL_FALSE);
}

static void gfx_dos_shutdown_impl(void) {
    OSMesaDestroyContext(ctx);

    free(osmesa_buffer);
    osmesa_buffer = NULL;

    set_video_mode(VGA_TEXT_MODE);
}

#endif // ENABLE_FXMESA

#define FRAMETIME 33
#define FRAMESKIP 33

static bool init_done = false;
static bool do_render = true;
static volatile uint32_t tick = 0;
static uint32_t last = 0;

static void timer_handler(void) { ++tick; }
END_OF_FUNCTION(timer_handler)

static void gfx_dos_init(UNUSED const char *game_name, UNUSED bool start_in_fullscreen) {
    if (__djgpp_nearptr_enable() == 0) {
      printf("Could get access to first 640K of memory.\n");
      abort();
    }

    allegro_init();

    LOCK_VARIABLE(tick);
    LOCK_FUNCTION(timer_handler);
    install_timer();
    install_int(timer_handler, FRAMETIME);

    gfx_dos_init_impl();

    last = tick;

    init_done = true;
}

static void gfx_dos_set_keyboard_callbacks(UNUSED bool (*on_key_down)(int scancode), UNUSED bool (*on_key_up)(int scancode), UNUSED void (*on_all_keys_up)(void)) { }

static void gfx_dos_set_fullscreen_changed_callback(UNUSED void (*on_fullscreen_changed)(bool is_now_fullscreen)) { }

static void gfx_dos_set_fullscreen(UNUSED bool enable) { }

static void gfx_dos_main_loop(void (*run_one_game_iter)(void)) {
    const uint32_t now = tick;

    const uint32_t frames = now - last;
    if (frames) {
        // catch up but skip the first FRAMESKIP frames
        int skip = (frames > FRAMESKIP) ? FRAMESKIP : (frames - 1);
        for (uint32_t f = 0; f < frames; ++f, --skip) {
            do_render = (skip <= 0);
            run_one_game_iter();
        }
        last = now;
    }
}

static void gfx_dos_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = SCREEN_WIDTH;
    *height = SCREEN_HEIGHT;
}

static void gfx_dos_handle_events(void) { }

static bool gfx_dos_start_frame(void) {
    return do_render;
}

static void gfx_dos_swap_buffers_begin(void) {
#ifdef ENABLE_DMESA
    DMesaSwapBuffers(db);
#else
    if (osmesa_buffer != NULL) {
        _farsetsel(_dos_ds);

        // convert/dither RGBA result into palettized 8 bit
        for (unsigned y = 0; y < SCREEN_HEIGHT; ++y) {
            for (unsigned p = y * SCREEN_WIDTH, x = 0; x < SCREEN_WIDTH; ++x, ++p) {
                unsigned rgb = osmesa_buffer[p], d = dit_kernel[y&7][x&7]; // 0..63
                d = (d & (0x3F - (0x3F >> DIT_BITS))) << 2;
                _farnspokeb(VGA_BASE + p,
                    rgbconv[2][(rgb >> 0) & 0xFF][d]
                  + rgbconv[1][(rgb >> 8) & 0xFF][d]
                  + rgbconv[0][(rgb >>16) & 0xFF][d]);
            }
        }
    }
#endif
}

static void gfx_dos_swap_buffers_end(void) { }

static double gfx_dos_get_time(void) {
    return 0.0;
}

static void gfx_dos_shutdown(void) {
    if (!init_done) return;

    gfx_dos_shutdown_impl();
    // allegro_exit() should be in atexit()

    init_done = false;
}

struct GfxWindowManagerAPI gfx_dos_api =
{
    gfx_dos_init,
    gfx_dos_set_keyboard_callbacks,
    gfx_dos_set_fullscreen_changed_callback,
    gfx_dos_set_fullscreen,
    gfx_dos_main_loop,
    gfx_dos_get_dimensions,
    gfx_dos_handle_events,
    gfx_dos_start_frame,
    gfx_dos_swap_buffers_begin,
    gfx_dos_swap_buffers_end,
    gfx_dos_get_time,
    gfx_dos_shutdown
};

#endif
