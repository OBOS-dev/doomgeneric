#include "doomgeneric.h"
#include "doomkeys.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include <sys/time.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <obos/syscall.h>
#include <obos/keycode.h>
#include <obos/error.h>

asm ("\
.intel_syntax noprefix;\
\
.global syscall;\
\
syscall:;\
    push rbp;\
    mov rbp, rsp;\
\
    mov eax, edi;\
    mov rdi, rsi;\
    mov rsi, rdx;\
    mov rdx, rcx;\
\
    syscall;\
\
    leave;\
    ret;\
\
.att_syntax prefix;");

bool obos_exit_event = false;

static int keyboard_fd = 0;
static int fb0_fd = 0;
static void* fb0;
static struct fb_mode {
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint16_t format;
    uint8_t bpp; // See OBOS_FB_FORMAT_*
} fb0_mode;

// Assume /dev/ps2k1 is the keyboard.
void DG_Init()
{
    keyboard_fd = open("/dev/ps2k1", O_RDONLY);
    if (keyboard_fd == -1)
    {
        perror("open");
        abort();
    }
    fb0_fd = open("/dev/fb0", O_RDWR);
    if (fb0_fd == -1)
    {
        perror("open");
        abort();
    }

    struct stat st = {};
    fstat(fb0_fd, &st);
    fb0 = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fb0_fd, 0);
    if (!fb0)
    {
        perror("mmap");
        abort();
    }

    obos_status status = (obos_status)syscall4(Sys_FdIoctl, fb0_fd, 1 /* query framebuffer info */, &fb0_mode, sizeof(struct fb_mode));
    if (obos_is_error(status))
    {
        fprintf(stderr, "Sys_FdIoctl: Status %d\n", status);
        abort();
    }
    printf("%s: Framebuffer is %dx%dx%d\n", __func__, fb0_mode.width, fb0_mode.height, fb0_mode.bpp);
    printf("%s: Mapped framebuffer at %p.\n", __func__, fb0);

    uint32_t new_priority = 4;
    syscall3(Sys_ThreadPriority, HANDLE_CURRENT, &new_priority, NULL);
}

void DG_DrawFrame()
{
    for (size_t i = 0; i < DOOMGENERIC_RESY; i++)
    {
        size_t offset = i*fb0_mode.pitch;
        memcpy((uint8_t*)fb0+offset, DG_ScreenBuffer+i*DOOMGENERIC_RESX, DOOMGENERIC_RESX*4);
    }
}

size_t i = 0;
void DG_SleepMs(uint32_t ms)
{
    syscall2(Sys_SleepMS, ms, NULL);
    i++;
}

uint32_t DG_GetTicksMs()
{
    return (uint32_t)i;
    struct timeval tv = {};
    gettimeofday(&tv, NULL);
    return tv.tv_usec/1000;
}

unsigned char numpad_to_key(uint8_t scancode)
{
    switch (scancode)
    {
        case SCANCODE_0: return KEYP_0;
        case SCANCODE_1: return KEYP_1;
        case SCANCODE_2: return KEYP_2;
        case SCANCODE_3: return KEYP_3;
        case SCANCODE_4: return KEYP_4;
        case SCANCODE_5: return KEYP_5;
        case SCANCODE_6: return KEYP_6;
        case SCANCODE_7: return KEYP_7;
        case SCANCODE_8: return KEYP_8;
        case SCANCODE_9: return KEYP_9;
        default: abort();
    }
}

int DG_GetKey(int* pressed, unsigned char* key)
{
    size_t nReady = 0;
    again:
    obos_status st = syscall4(Sys_FdIoctl, keyboard_fd, 1 /* query ready count */, &nReady, 8);
    if (obos_is_error(st) || nReady == 0)
        return 0;

    keycode code = 0;
    uint8_t scancode = 0;
    enum modifiers mod = 0;

    // Read a keycode.
    read(keyboard_fd, &code, sizeof(code));
    scancode = SCANCODE_FROM_KEYCODE(code);
    mod = MODIFIERS_FROM_KEYCODE(code);

    *pressed = ~mod & KEY_RELEASED;

    switch (scancode)
    {
        case SCANCODE_F1 ... SCANCODE_F12:
            *key = (scancode - SCANCODE_F1) + KEY_F1;
            break;
        case SCANCODE_TAB:
            *key = KEY_TAB;
            break;
        case SCANCODE_ENTER:
            *key = KEY_ENTER;
            break;
        case SCANCODE_ESC:
            *key = KEY_ESCAPE;
            break;
        case SCANCODE_CTRL:
            *key = KEY_FIRE;
            break;
        case SCANCODE_SPACE:
            *key = KEY_USE;
            break;
        case SCANCODE_LEFT_ARROW:
            if (mod & ALT)
                *key = KEY_STRAFE_L;
            else
                *key = KEY_LEFTARROW;
            break;
        case SCANCODE_RIGHT_ARROW:
            if (mod & ALT)
                *key = KEY_STRAFE_R;
            else
                *key = KEY_RIGHTARROW;
            break;
        case SCANCODE_UP_ARROW:
            *key = KEY_UPARROW;
            break;
        case SCANCODE_DOWN_ARROW:
            *key = KEY_DOWNARROW;
            break;
        case SCANCODE_BACKSPACE:
            *key = KEY_BACKSPACE;
            break;
        case SCANCODE_SHIFT:
            *key = KEY_RSHIFT;
            break;
        case SCANCODE_ALT:
            *key = KEY_LALT;
            break;
        case SCANCODE_EQUAL:
            *key = KEY_EQUALS;
            break;
        case SCANCODE_DASH:
            *key = (mod & NUMPAD) ? KEYP_MINUS : KEY_MINUS;
            break;
        case SCANCODE_0 ... SCANCODE_9:
            if (mod & NUMPAD)
                *key = numpad_to_key(scancode);
            else
                *key = '0' + (scancode-SCANCODE_0);
            break;
        case SCANCODE_FORWARD_SLASH:
            *key = '/';
            break;
        case SCANCODE_PLUS:
            *key = '+';
            break;
        case SCANCODE_STAR:
            *key = '*';
            break;
        case SCANCODE_DOT:
            *key = (mod & NUMPAD) ? KEYP_PERIOD : ((~mod & SHIFT) ? '.' : '>');
            break;
        case SCANCODE_HOME:
            *key = KEY_HOME;
            break;
        case SCANCODE_END:
            *key = KEY_END;
            break;
        case SCANCODE_PGUP:
            *key = KEY_PGUP;
            break;
        case SCANCODE_PGDOWN:
            *key = KEY_PGDN;
            break;
        case SCANCODE_INSERT:
            *key = KEY_INS;
            break;
        case SCANCODE_DELETE:
            *key = KEY_DEL;
            break;
        case SCANCODE_A ... SCANCODE_Z:
            *key = 'A' + (scancode-SCANCODE_A);
            break;
        case SCANCODE_SQUARE_BRACKET_LEFT:
            *key = (~mod & SHIFT) ? '[' : '{';
            break;
        case SCANCODE_SQUARE_BRACKET_RIGHT:
            *key = (~mod & SHIFT) ? ']' : '}';
            break;
        case SCANCODE_SEMICOLON:
            *key = (~mod & SHIFT) ? ';' : ':';
            break;
        case SCANCODE_COMMA:
            *key = (~mod & SHIFT) ? ',' : '<';
             break;
        case SCANCODE_APOSTROPHE:
            *key = (~mod & SHIFT) ? '\'' : '\"';
            break;
        case SCANCODE_BACKTICK:
            *key = (~mod & SHIFT) ? '`' : '~';
            break;
        case SCANCODE_UNDERSCORE:
            *key = (~mod & SHIFT) ? '_' : '-';
            break;
        case SCANCODE_BACKSLASH:
            *key = (~mod & SHIFT) ? '\\' : '|';
            break;
        default: goto again;
    }
    return 1;
}

void DG_SetWindowTitle(const char * title)
{
    (void)(title);
}

int main (int argc, char** argv)
{
    doomgeneric_Create(argc, argv);

    while (!obos_exit_event)
        doomgeneric_Tick();

    return 0;
}
