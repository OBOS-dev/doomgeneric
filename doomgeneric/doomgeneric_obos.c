#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_sound.h"
#include "m_misc.h"
#include "memio.h"
#include "mus2mid.h"
#include "obos_hda_ioctl.h"

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <assert.h>

#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <obos/syscall.h>
#include <obos/keycode.h>
#include <obos/error.h>

#include <SDL3_sound/SDL_sound.h>
#include <SDL3/SDL_audio.h>

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

static void initialize_sound();

static int keyboard_fd = 0;
static int audiodev = 0;
static int fb0_fd = 0;
static void* fb0;
static struct fb_mode {
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint16_t format;
    uint8_t bpp; // See OBOS_FB_FORMAT_*
} fb0_mode;

sound_module_t DG_sound_module;
music_module_t DG_music_module;

void reset_tty()
{ tcflow(STDIN_FILENO, TCION); }
static void play_music_nolock(bool playing);
void fatal_signal_handler(int sig)
{
    (void)(sig);
    printf("Got fatal signal. Quitting DOOM.\n");
    reset_tty();
    if (audiodev != -1)
        play_music_nolock(false);
    if (sig == SIGINT || sig == SIGQUIT || sig == SIGTERM)
        obos_exit_event = true;
    else
        exit(sig | (sig << 8));
    return;
}
// Assume /dev/ps2k1 is the keyboard.
void DG_Init()
{
    if (isatty(STDIN_FILENO))
    {
        tcflow(STDIN_FILENO, TCIOFF);
        atexit(reset_tty);
    }

    signal(SIGSEGV, fatal_signal_handler);
    signal(SIGFPE, fatal_signal_handler);
    signal(SIGILL, fatal_signal_handler);
    signal(SIGINT, fatal_signal_handler);
    signal(SIGQUIT, fatal_signal_handler);
    signal(SIGTERM, fatal_signal_handler);

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
    audiodev = open("/dev/audio", O_RDONLY);
    if (audiodev == -1)
    {
        perror("open(/dev/audio, O_RDONLY)");
        printf("NOTE: Sound will not be avaliable\n");
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

    initialize_sound();
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
            *key = 'a' + (scancode-SCANCODE_A);
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

struct song_handle {
    Sound_Sample* sample;
    pthread_t decode_thread;
    size_t offset;
};

#define MAX_PATHS 16
struct {
    stream_parameters stream_params;
    uintptr_t path_hnd;
    size_t stream_index;
    int pipe_fds[2];
    bool activated : 1;
    bool playing : 1;
} g_paths[MAX_PATHS] = {};
pthread_mutex_t g_audiodev_mutex;
pthread_t sound_thread = {};
int g_music_path_index = -1;
struct song_handle *volatile g_current_playing_song;
bool g_loop_song;
int use_libsamplerate = 0;
float libsamplerate_scale = 0.f;

bool initialized_sound = false;

boolean Init(void) 
{
    if (audiodev == -1) return false;
    assert(!initialized_sound);
    Sound_Init();
    return (initialized_sound = true);
}

void StopSong(void);
void Shutdown(void)
{
    if (audiodev == -1) return;
    if (initialized_sound)
        return;
    play_music_nolock(false);
    StopSong();
    Sound_Quit();
    close(audiodev);
    pthread_mutex_destroy(&g_audiodev_mutex);
}

void SetMusicVolume(int volume)
{
    if (audiodev == -1) return;
    if (g_music_path_index == -1)
        return;
    if (volume > 100)
        volume = 100;
    if (volume < 0)
        volume = 0;
    struct hda_path_byte_parameter param = {.path=g_paths[g_music_path_index].path_hnd,.par1=volume};
    pthread_mutex_lock(&g_audiodev_mutex);
    ioctl(audiodev, IOCTL_HDA_PATH_VOLUME, &param);
    pthread_mutex_unlock(&g_audiodev_mutex);
}

static void play_music_nolock(bool playing)
{
    if (audiodev == -1) return;
    if (g_music_path_index == -1) return;
    ioctl(audiodev, IOCTL_HDA_OUTPUT_STREAM_SELECT, &g_paths[g_music_path_index].stream_index);
    ioctl(audiodev, IOCTL_HDA_STREAM_PLAY, &playing);
    g_paths[g_music_path_index].playing = playing;
}

static void play_music(bool playing)
{
    if (audiodev == -1) return;
    if (g_music_path_index == -1) return;
    pthread_mutex_lock(&g_audiodev_mutex);
    play_music_nolock(playing);
    pthread_mutex_unlock(&g_audiodev_mutex);
}

void PauseMusic(void)
{
    play_music(false);
}

void ResumeMusic(void)
{
    play_music(true);
}

#define MAXMIDLENGTH (96 * 1024)
#define MID_HEADER_MAGIC "MThd"
#define MUS_HEADER_MAGIC "MUS\x1a"

static boolean ConvertMus(byte *musdata, int len, void** outbuf, size_t* outbuf_len)
{
    MEMFILE *instream = mem_fopen_read(musdata, len);
    MEMFILE *outstream = mem_fopen_write();

    int result = mus2mid(instream, outstream);
    if (result == 1)
        return false;

    void* buf = NULL;
    size_t buflen = 0;
    mem_get_buf(outstream, &buf, &buflen);
    *outbuf = malloc(buflen);
    memcpy(*outbuf, buf, buflen);
    *outbuf_len = buflen;

    mem_fclose(instream);
    mem_fclose(outstream);

    return true;
}

#include <SDL3_mixer/SDL_mixer.h>
#include <SDL3/SDL.h>

static size_t get_song_pipe_size()
{
    if (!g_current_playing_song)
        return 512;
    static uint8_t pcm_byte_table[] = {
        1,2,2,3,4
    };
    return g_paths[g_music_path_index].stream_params.channels *
           pcm_byte_table[g_paths[g_music_path_index].stream_params.format] * 
           g_paths[g_music_path_index].stream_params.sample_rate;
}

#define SAMPLE_SIZE get_song_pipe_size()

static void* decoder_thread(void* arg);

void *RegisterSong(void *data, int len)
{
    if (audiodev == -1) return NULL;
    void* buf = NULL;
    size_t buflen = 0;
    if (!ConvertMus(data, len, &buf, &buflen))
        return NULL;

    Sound_AudioInfo desired_fmt = {.format=SDL_AUDIO_S16LE,.channels=2,.rate=44100};
    Sound_Sample* sample = Sound_NewSampleFromMem(
        buf, buflen, 
        ".mid", 
        &desired_fmt, 
        SAMPLE_SIZE);
    if (!sample)
    {
        fprintf(stderr, "Sound_NewSampleFromMem returned error %s\n", Sound_GetError());
        return NULL;
    }

    struct song_handle *hnd = malloc(sizeof(struct song_handle));
    hnd->sample = sample;
    pthread_create(&hnd->decode_thread, NULL, decoder_thread, hnd);
    pthread_join(hnd->decode_thread, NULL);
    return hnd;
}

void UnRegisterSong(void *handle)
{
    if (audiodev == -1) return;
    struct song_handle *hnd = handle;
    pthread_cancel(hnd->decode_thread);
    pthread_join(hnd->decode_thread, NULL);
    Sound_FreeSample(hnd->sample);
    free(hnd);    
}

void PlaySong(void *handle, boolean looping)
{
    if (audiodev == -1) return;

    looping = false;

    // Make sure...
    StopSong();

    pthread_mutex_lock(&g_audiodev_mutex);
    
    struct song_handle *hnd = handle;
    g_current_playing_song = hnd;
    g_loop_song = looping;

    struct hda_path_setup_parameters path_setup_req = {};
    path_setup_req.path = g_paths[g_music_path_index].path_hnd;
    path_setup_req.stream_parameters.channels = hnd->sample->actual.channels;
    path_setup_req.stream_parameters.sample_rate = hnd->sample->actual.rate;
    switch (hnd->sample->actual.format) {
        case SDL_AUDIO_S8:
            path_setup_req.stream_parameters.format = FORMAT_PCM8;
            break;
        case SDL_AUDIO_S16:
            path_setup_req.stream_parameters.format = FORMAT_PCM16;
            break;
        case SDL_AUDIO_S32:
            path_setup_req.stream_parameters.format = FORMAT_PCM32;
            break;
        default: 
            pthread_mutex_unlock(&g_audiodev_mutex);
            return; // bail
    }
    ioctl(audiodev, IOCTL_HDA_PATH_SETUP, &path_setup_req);
    g_paths[g_music_path_index].stream_params = path_setup_req.stream_parameters;
    
    ioctl(audiodev, IOCTL_HDA_OUTPUT_STREAM_SELECT, &g_paths[g_music_path_index].stream_index);
    
    struct hda_stream_setup_user_parameters setup_params = {};
    setup_params.ring_buffer_pipe = g_paths[g_music_path_index].pipe_fds[0];
    setup_params.stream_params = g_paths[g_music_path_index].stream_params;
    setup_params.ring_buffer_size = get_song_pipe_size();
    ioctl(audiodev, IOCTL_HDA_STREAM_SETUP_USER, &setup_params);

    // IOCTL_PIPE_SET_SIZE
    size_t size = setup_params.ring_buffer_size;
    ioctl(g_paths[g_music_path_index].pipe_fds[0], 1, &size);

    Sound_SetBufferSize(g_current_playing_song->sample, get_song_pipe_size());

    bool start = true;
    ioctl(audiodev, IOCTL_HDA_STREAM_PLAY, &start);
    g_paths[g_music_path_index].playing = start;

    pthread_kill(sound_thread, SIGUSR1);

    pthread_mutex_unlock(&g_audiodev_mutex);
}

void StopSong(void)
{
    if (audiodev == -1) return;

    pthread_mutex_lock(&g_audiodev_mutex);
    
    ioctl(audiodev, IOCTL_HDA_OUTPUT_STREAM_SELECT, &g_paths[g_music_path_index].stream_index);
    
    bool stop = false;
    ioctl(audiodev, IOCTL_HDA_STREAM_PLAY, &stop);
    g_paths[g_music_path_index].playing = stop;
    
    ioctl(audiodev, IOCTL_HDA_STREAM_CLEAR_QUEUE, NULL);
    ioctl(audiodev, IOCTL_HDA_STREAM_SHUTDOWN, NULL);
    
    ioctl(audiodev, IOCTL_HDA_PATH_SHUTDOWN, &g_paths[g_music_path_index].path_hnd);

    g_loop_song = false;
    g_current_playing_song = NULL;

    pthread_kill(sound_thread, SIGUSR1);

    pthread_mutex_unlock(&g_audiodev_mutex);
}

boolean MusicIsPlaying(void)
{
    if (audiodev == -1) return false;

    if (g_music_path_index == -1) return false;
    return g_paths[g_music_path_index].playing;
}

void Poll(void)
{
    if (audiodev == -1) return;
    return;
}

static void music_tick()
{
    if (!g_current_playing_song)
        return;
    if (!g_loop_song || (~g_current_playing_song->sample->flags & SOUND_SAMPLEFLAG_EOF))
    {
        pthread_mutex_lock(&g_audiodev_mutex);
        char* buffer = g_current_playing_song->sample->buffer;
        buffer += g_current_playing_song->offset;
        g_current_playing_song->offset += get_song_pipe_size();
        pthread_mutex_unlock(&g_audiodev_mutex);
        write(g_paths[g_music_path_index].pipe_fds[1], buffer, get_song_pipe_size());
    }
}

static void sigusr1_handler(int sig)
{
    (void)(sig);
    return;
}

void* sound_tick(void* unused)
{
    (void)(unused);
    if (audiodev == -1)
        return NULL;
    uint32_t new_priority = 5;
    syscall3(Sys_ThreadPriority, HANDLE_CURRENT, &new_priority, NULL);
    signal(SIGUSR1, sigusr1_handler);
    while (!obos_exit_event)
    {
        // pthread_mutex_lock(&g_audiodev_mutex);
        if (g_current_playing_song)
            music_tick();
        // pthread_mutex_unlock(&g_audiodev_mutex);
    }
    return NULL;
}

static void* decoder_thread(void* arg)
{
    struct song_handle* hnd = arg;
    int32_t duration = Sound_GetDuration(hnd->sample);
    if (duration < 0)
    {
        printf("Sound_GetDuration returned error status\n");
        return NULL;
    }
    if (duration == 0)
    {
        printf("Sound_GetDuration unexpectedly returned zero\n");
        return NULL;
    }
    size_t size = 0;
    static uint8_t pcm_byte_table[] = {
        1,2,2,3,4
    };
    size = g_paths[g_music_path_index].stream_params.channels *
           pcm_byte_table[g_paths[g_music_path_index].stream_params.format] * 
           g_paths[g_music_path_index].stream_params.sample_rate;
    size *= duration;
    Sound_SetBufferSize(hnd->sample, size);
    Sound_DecodeAll(hnd->sample);
    return NULL;
}

static void initialize_sound()
{
    pthread_mutex_init(&g_audiodev_mutex, NULL);

    DG_music_module.Init = Init;
    DG_music_module.Shutdown = Shutdown;
    DG_music_module.SetMusicVolume = SetMusicVolume;
    DG_music_module.PauseMusic = PauseMusic;
    DG_music_module.ResumeMusic = ResumeMusic;
    DG_music_module.RegisterSong = RegisterSong;
    DG_music_module.UnRegisterSong = UnRegisterSong;
    DG_music_module.PlaySong = PlaySong;
    DG_music_module.StopSong = StopSong;
    DG_music_module.MusicIsPlaying = MusicIsPlaying;
    DG_music_module.Poll = Poll;

    if (audiodev == -1)
        return;

    size_t index = 0;
    ioctl(audiodev, IOCTL_HDA_CODEC_SELECT, &index);
    ioctl(audiodev, IOCTL_HDA_OUTPUT_STREAM_SELECT, &index);
    ioctl(audiodev, IOCTL_HDA_CODEC_SELECT_OUTPUT_GROUP, &index);
    ioctl(audiodev, IOCTL_HDA_OUTPUT_GROUP_SELECT_OUTPUT, &index);

    snddevice_t* music_devices = calloc(1, sizeof(snddevice_t));
    music_devices[0] = SNDDEVICE_PCSPEAKER;
    DG_music_module.sound_devices = music_devices;
    DG_music_module.num_sound_devices = 1;

    g_music_path_index = 0;
    struct hda_path_find_parameters path_find_req = {};
    path_find_req.same_stream = false;
    path_find_req.other_path_count = 0;
    ioctl(audiodev, IOCTL_HDA_PATH_FIND, &path_find_req);
    g_paths[g_music_path_index].path_hnd = path_find_req.found_path;
    ioctl(audiodev, IOCTL_HDA_OUTPUT_GROUP_SELECTED_OUTPUT, &g_paths[g_music_path_index].stream_index);
    syscall2(Sys_CreatePipe, g_paths[g_music_path_index].pipe_fds, SAMPLE_SIZE);
    g_paths[g_music_path_index].activated = true;

    atexit(Shutdown);

    int err = 0;
    if ((err = pthread_create(&sound_thread, NULL, sound_tick, NULL)) > 0)
    {
        fprintf(stderr, "pthread_create: %s\n", strerror(err));
        exit(-1);
    }
}

#include <pthread.h>

int main (int argc, char** argv)
{
    doomgeneric_Create(argc, argv);

    while (!obos_exit_event)
        doomgeneric_Tick();

    pthread_join(sound_thread, NULL);

    return 0;
}
