/* libScePad on SDL3 gamepads, with the keyboard as a second source. SDL events
 * are pumped by the window thread (gpu/shim/window.cpp); here state is only sampled, on the pad's
 * own host thread (host_thread).
 *
 * Gamepads map by position: the bottom face button is Cross, right Circle, left Square, top
 * Triangle (an Xbox pad's A B X Y). View/Back clicks the touchpad; a DualSense's own touchpad works.
 *
 * Two players (local co-op): the first gamepad connected is user 1's, the second user 2's
 * (runtime_services.c logs user 2 in when that pad first connects, as switching on a second
 * controller does on a PS4). A pad keeps its slot while connected; a pad that comes back fills the
 * first free slot. The keyboard always plays for user 1.
 *
 * Keyboard layout, by key position:
 *   WASD left stick, IJKL right stick, arrow keys d-pad (menus),
 *   Space/Enter Cross, Escape/Backspace/; Circle, F Square, H Triangle,
 *   Q L1, O R1, E L2, U R2 (Page Up/Page Down are L1/R1 too), X L3, M R3,
 *   Tab/P Options, T touchpad click. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/resogun_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PADS 2 /* users 1 and 2; the handle of user n's pad is n */

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
    /* Host-only sources, never shown to the game: a touchpad click from a button or key without a
     * touch surface (play_click turns it into a touch and a click), and the keyboard's sticks. */
    CLICK=0x80000000,
    LSTICK_UP=0x00010000, LSTICK_DOWN=0x00020000, LSTICK_LEFT=0x00040000, LSTICK_RIGHT=0x00080000,
    RSTICK_UP=0x00200000, RSTICK_DOWN=0x00400000, RSTICK_LEFT=0x00800000, RSTICK_RIGHT=0x01000000,
    STICK_DIRECTIONS=LSTICK_UP|LSTICK_DOWN|LSTICK_LEFT|LSTICK_RIGHT|RSTICK_UP|RSTICK_DOWN|RSTICK_LEFT|RSTICK_RIGHT,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened[PADS], sdl_ready;
static size_t reads, vibrations;
/* The slot of an open handle (0 for user 1), or -1. */
static int slot_of(int32_t handle) { return handle>=1 && handle<=PADS && opened[handle-1] ? handle-1 : -1; }
/* The host side runs on its own thread (host_thread): opening, closing and even reading an SDL
 * gamepad can block for hundreds of ms while a Bluetooth pad connects or drops (SDL holds its
 * joystick lock meanwhile), and on the game's thread that froze the frame. The thread samples every
 * millisecond and a read copies the latest sample, so the game's threads make no gamepad calls. */
static pthread_mutex_t host_lock=PTHREAD_MUTEX_INITIALIZER;
static PadData host_sample[PADS];     /* under host_lock */
static char gamepad_name[PADS][64]={"none","none"}; /* under host_lock */
static struct { uint8_t large, small; uint32_t serial; } rumble[PADS]; /* under host_lock */
static struct { uint8_t r, g, b; uint32_t serial; } light[PADS];       /* under host_lock */
static int reconnect_wanted;          /* atomic: the pad file's reconnect token */
static int second_seen;               /* atomic: user 2's slot has had a gamepad */
static int speaker_kind;              /* RUNTIME_PAD_SPEAKER_* of user 1's gamepad (host thread) */
static int speaker_wanted;            /* runtime_pad_speaker_route */
static int speaker_routed;            /* what the open DualSense was last told; -1: nothing yet */

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
/* Pad calls run on the game's own threads, so a slow one is a late frame: calls over 2 ms are
 * counted per kind (read, vibration, info, and the host thread's sample) and the first 20 printed.
 * One thread notes each kind; the shared count and start time are atomic. */
enum { SLOW_READ, SLOW_VIBRATION, SLOW_INFO, SLOW_HOST, SLOW_KINDS };
static size_t slow_calls[SLOW_KINDS], slow_shown;
static uint64_t slow_max_us[SLOW_KINDS], slow_epoch;
static void note_call(int kind, uint64_t start) {
    static const char *const names[SLOW_KINDS]={"read","vibration","info","host sample"};
    const uint64_t us=now_us()-start;
    uint64_t unset=0;
    __atomic_compare_exchange_n(&slow_epoch,&unset,start,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED);
    if (us<2000) return;
    ++slow_calls[kind];
    if (us>slow_max_us[kind]) slow_max_us[kind]=us;
    if (__atomic_fetch_add(&slow_shown,1,__ATOMIC_RELAXED)<20)
        printf("Runtime: slow pad %s %.1f ms at %.3f s\n",names[kind],us/1e3,(start-__atomic_load_n(&slow_epoch,__ATOMIC_RELAXED))/1e6);
}
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}

/* A click without a touch surface (Back/View, the T key, the pad file) is played like a finger in
 * the middle of the touchpad: the touch is reported CLICK_LEAD_US before the button, the button
 * stays at least CLICK_HOLD_US, and the touch lingers CLICK_TAIL_US after the release, as a real
 * finger would. */
enum { CLICK_LEAD_US=50000, CLICK_HOLD_US=50000, CLICK_TAIL_US=100000 };
typedef struct { bool active; uint64_t pressed, released; } Click;
static Click clicks[PADS];
static void play_click(PadData *d, Click *c) {
    const uint64_t now=d->timestamp;
    bool source=(d->buttons & CLICK)!=0;
    d->buttons&=~CLICK;
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) { /* a replay, or a pad without fingers */
        d->buttons&=~BTN_TOUCHPAD;
        source=true;
    }
    if (source && (!c->active || c->released)) { c->active=true; c->pressed=now; c->released=0; }
    else if (!source && c->active && !c->released) c->released=now;
    if (!c->active) return;
    const uint64_t down=c->pressed+CLICK_LEAD_US;
    uint64_t up=down+CLICK_HOLD_US;
    if (!c->released) up=UINT64_MAX;
    else if (c->released>up) up=c->released;
    if (now>=up && now-up>=CLICK_TAIL_US) { c->active=false; return; }
    if (d->touch_count<2) {
        d->touches[d->touch_count]=(PadTouch){.x=960,.y=471,.id=d->touch_count};
        ++d->touch_count;
    }
    if (now>=down && now<up) d->buttons|=BTN_TOUCHPAD;
}

/* Host thread: the open gamepads, closing those that are gone (all of them when the pad file asked
 * for a reconnect), and every 100 ms while a slot is empty opening a connected gamepad no slot has. */
static void host_gamepads(SDL_Gamepad **g, uint64_t now, uint64_t *next_scan, uint8_t *connected_count) {
    const bool reconnect=__atomic_exchange_n(&reconnect_wanted,0,__ATOMIC_ACQ_REL)!=0;
    bool empty=false;
    for (int i=0;i<PADS;++i) {
        if (g[i] && (reconnect || !SDL_GamepadConnected(g[i]))) {
            SDL_CloseGamepad(g[i]); g[i]=NULL;
            pthread_mutex_lock(&host_lock); strcpy(gamepad_name[i],"none"); pthread_mutex_unlock(&host_lock);
            printf("Runtime: gamepad %d closed\n",i+1);
        }
        if (!g[i]) empty=true;
    }
    if (!empty || now<*next_scan) return;
    *next_scan=now+100000;
    int count=0;
    SDL_JoystickID *ids=SDL_GetGamepads(&count);
    for (int n=0;ids && n<count;++n) {
        int slot=-1;
        for (int i=PADS-1;i>=0;--i) {
            if (g[i] && SDL_GetGamepadID(g[i])==ids[n]) { slot=-2; break; }
            if (!g[i]) slot=i;
        }
        if (slot<0 || !(g[slot]=SDL_OpenGamepad(ids[n]))) continue;
        ++connected_count[slot];
        if (slot==1) __atomic_store_n(&second_seen,1,__ATOMIC_RELEASE);
        const char *name=SDL_GetGamepadName(g[slot]);
        pthread_mutex_lock(&host_lock);
        snprintf(gamepad_name[slot],sizeof(gamepad_name[slot]),"%s",name ? name : "gamepad");
        ++light[slot].serial; /* the user's color goes on the new pad */
        pthread_mutex_unlock(&host_lock);
        printf("Runtime: gamepad %d connected: %s\n",slot+1,name ? name : "gamepad");
    }
    SDL_free(ids);
}
int runtime_pad_second_user(void) { return __atomic_load_n(&second_seen,__ATOMIC_ACQUIRE); }
/* Which controller speaker a gamepad has: a DualShock 4 or DualSense on USB shows up as a USB audio
 * device too (over Bluetooth its audio travels in HID reports, which is not done here). */
static int speaker_kind_of(SDL_Gamepad *g) {
    if (!g || SDL_GetGamepadConnectionState(g)!=SDL_JOYSTICK_CONNECTION_WIRED) return RUNTIME_PAD_SPEAKER_NONE;
    switch (SDL_GetGamepadType(g)) {
    case SDL_GAMEPAD_TYPE_PS4: return RUNTIME_PAD_SPEAKER_DS4;
    case SDL_GAMEPAD_TYPE_PS5: return RUNTIME_PAD_SPEAKER_DUALSENSE;
    default: return RUNTIME_PAD_SPEAKER_NONE;
    }
}
/* A DualSense's USB audio output plays on its headphone jack until an output report (SDL's
 * DS5EffectsState_t, report 0x02 after the id) selects another path: audio control byte 7 bits 4-5,
 * 3 = the speaker takes the right channel and the jack nothing, 0 = the jack takes both channels.
 * The speaker volume (byte 5) is set with it: the PS5 keeps it within 0x3d..0x64. */
static int dualsense_speaker(SDL_Gamepad *g, int on) {
    uint8_t effect[47]={0};
    effect[0]=on ? 0x80|0x20 : 0x80; /* valid flags: audio control, speaker volume */
    effect[5]=0x64;
    effect[7]=on ? 3<<4 : 0;
    return SDL_SendGamepadEffect(g,effect,sizeof(effect));
}
/* Host thread: keeps the open gamepad's speaker kind and a DualSense's audio path current. */
static void speaker_update(SDL_Gamepad *g, SDL_Gamepad **last) {
    if (g!=*last) {
        *last=g;
        __atomic_store_n(&speaker_routed,-1,__ATOMIC_RELEASE);
        const int kind=speaker_kind_of(g);
        __atomic_store_n(&speaker_kind,kind,__ATOMIC_RELEASE);
        if (kind) printf("Runtime: controller speaker available (%s on USB)\n",kind==RUNTIME_PAD_SPEAKER_DS4 ? "DualShock 4" : "DualSense");
    }
    const int want=__atomic_load_n(&speaker_wanted,__ATOMIC_ACQUIRE);
    if (!g || __atomic_load_n(&speaker_kind,__ATOMIC_ACQUIRE)!=RUNTIME_PAD_SPEAKER_DUALSENSE) return;
    const int routed=__atomic_load_n(&speaker_routed,__ATOMIC_ACQUIRE);
    if (want==routed || (!want && routed<0)) return; /* a pad never moved keeps its jack untouched */
    if (!dualsense_speaker(g,want)) printf("Runtime: DualSense audio path not set (%s)\n",SDL_GetError());
    else printf("Runtime: DualSense audio sent to its %s\n",want ? "speaker" : "headphone jack");
    __atomic_store_n(&speaker_routed,want,__ATOMIC_RELEASE); /* tried once per change, not every ms */
}
int runtime_pad_speaker(void) { return __atomic_load_n(&speaker_kind,__ATOMIC_ACQUIRE); }
void runtime_pad_speaker_route(int on) { __atomic_store_n(&speaker_wanted,on ? 1 : 0,__ATOMIC_RELEASE); }

/* PS4 button names for scripted input (RESOGUN_PAD_FILE). */
static const struct { const char *name; uint32_t ps; } ps_names[]={
    {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
    {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
    {"options",BTN_OPTIONS}, {"touchpad",CLICK},
    {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    {"lstick_up",LSTICK_UP}, {"lstick_down",LSTICK_DOWN}, {"lstick_left",LSTICK_LEFT}, {"lstick_right",LSTICK_RIGHT},
    {"rstick_up",RSTICK_UP}, {"rstick_down",RSTICK_DOWN}, {"rstick_left",RSTICK_LEFT}, {"rstick_right",RSTICK_RIGHT},
};
/* Button sources: a gamepad button or key and the PS4 button it presses. Gamepads map by
 * position; the keyboard layout is the one in the comment at the top. */
typedef struct { int source; uint32_t ps; } ButtonSource;
typedef struct { ButtonSource entries[32]; size_t count; } ButtonMap;
static const ButtonMap pad_map={{
    {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
    {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,CLICK},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
},15};
static const ButtonMap key_map={{
    {SDL_SCANCODE_W,LSTICK_UP}, {SDL_SCANCODE_S,LSTICK_DOWN}, {SDL_SCANCODE_A,LSTICK_LEFT}, {SDL_SCANCODE_D,LSTICK_RIGHT},
    {SDL_SCANCODE_I,RSTICK_UP}, {SDL_SCANCODE_K,RSTICK_DOWN}, {SDL_SCANCODE_J,RSTICK_LEFT}, {SDL_SCANCODE_L,RSTICK_RIGHT},
    {SDL_SCANCODE_UP,BTN_UP}, {SDL_SCANCODE_DOWN,BTN_DOWN}, {SDL_SCANCODE_LEFT,BTN_LEFT}, {SDL_SCANCODE_RIGHT,BTN_RIGHT},
    {SDL_SCANCODE_SPACE,BTN_CROSS}, {SDL_SCANCODE_RETURN,BTN_CROSS}, {SDL_SCANCODE_KP_ENTER,BTN_CROSS},
    {SDL_SCANCODE_ESCAPE,BTN_CIRCLE}, {SDL_SCANCODE_BACKSPACE,BTN_CIRCLE}, {SDL_SCANCODE_SEMICOLON,BTN_CIRCLE},
    {SDL_SCANCODE_F,BTN_SQUARE}, {SDL_SCANCODE_H,BTN_TRIANGLE},
    {SDL_SCANCODE_Q,BTN_L1}, {SDL_SCANCODE_PAGEUP,BTN_L1}, {SDL_SCANCODE_O,BTN_R1}, {SDL_SCANCODE_PAGEDOWN,BTN_R1},
    {SDL_SCANCODE_E,BTN_L2}, {SDL_SCANCODE_U,BTN_R2}, {SDL_SCANCODE_X,BTN_L3}, {SDL_SCANCODE_M,BTN_R3},
    {SDL_SCANCODE_TAB,BTN_OPTIONS}, {SDL_SCANCODE_P,BTN_OPTIONS}, {SDL_SCANCODE_T,CLICK},
},31};

static void key_axis(uint8_t *axis, bool negative, bool positive) {
    if (negative || positive) *axis=(uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0));
}

/* keys: SDL keyboard state. Returns the mapped sources held (0: the keyboard is idle). */
static uint32_t sample_keyboard(PadData *d, const bool *keys) {
    uint32_t pressed=0;
    for (size_t i=0;i<key_map.count;++i) {
        const int source=key_map.entries[i].source;
        if (keys[source]) pressed|=key_map.entries[i].ps;
    }
    d->buttons|=pressed & ~STICK_DIRECTIONS;
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    key_axis(&d->left_x,pressed & LSTICK_LEFT,pressed & LSTICK_RIGHT);
    key_axis(&d->left_y,pressed & LSTICK_UP,pressed & LSTICK_DOWN);
    key_axis(&d->right_x,pressed & RSTICK_LEFT,pressed & RSTICK_RIGHT);
    key_axis(&d->right_y,pressed & RSTICK_UP,pressed & RSTICK_DOWN);
    return pressed;
}

/* Host thread: a slot's gamepad as one PadData (no timestamp), with the keyboard in slot 0. User
 * 1's pad is always connected (the keyboard plays without a gamepad); user 2's while it has one. */
static void host_read(PadData *d, int slot, SDL_Gamepad *g, uint8_t connected_count) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=!slot || g; d->connected_count=connected_count ? connected_count : 1;
    if (resogun_gpu_overlay_captures_input()) return; /* a dialog is open: neutral input */
    const bool *k=!slot && SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (g) {
        for (size_t i=0;i<pad_map.count;++i)
            if (SDL_GetGamepadButton(g,(SDL_GamepadButton)pad_map.entries[i].source)) d->buttons|=pad_map.entries[i].ps;
        const uint32_t mapped=d->buttons;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (mapped & BTN_L2) d->l2=255;
        if (mapped & BTN_R2) d->r2=255;
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
    }
    /* The keyboard works with or without a gamepad: its buttons add to the pad's, and a stick
     * follows the keys only while one of them is held. */
    if (k) sample_keyboard(d,k);
}

/* Host thread: Select (Back/View/Share) or Start (Options/Menu) held alone for MENU_HOLD_US opens the
 * Display Settings panel (as F10); Select and Start down together ask whether to close the game (the
 * exit prompt, as F4). What each button presses (Select the touchpad click, Start Options) is held
 * back while it is down and played as a MENU_TAP_US tap when it is let go without a shortcut; after
 * a shortcut, or while an overlay had the input, it is dropped. */
enum { MENU_HOLD_US=2000000, MENU_TAP_US=60000 };
typedef struct {
    SDL_GamepadButton button;
    const char *name;
    uint64_t since, tap_until; /* down since (0: up); its tap plays until */
    bool used;                 /* this press fired a shortcut or met an overlay: no tap */
} MenuButton;
/* One button's state for this sample: true while it is down, its press held back from `d`. */
static bool menu_button(MenuButton *b, PadData *d, SDL_Gamepad *g, uint64_t now) {
    uint32_t bits=0;
    for (size_t i=0;i<pad_map.count;++i)
        if (pad_map.entries[i].source==(int)b->button) bits|=pad_map.entries[i].ps;
    if (g && SDL_GetGamepadButton(g,b->button)) {
        if (!b->since) { b->since=now; b->used=false; }
        if (resogun_gpu_overlay_captures_input()) b->used=true;
        d->buttons&=~bits;
        return true;
    }
    if (b->since) {
        if (!b->used) b->tap_until=now+MENU_TAP_US;
        b->since=0;
    }
    if (now<b->tap_until) d->buttons|=bits;
    return false;
}
static void menu_held(MenuButton *b, uint64_t now) {
    if (b->used || now-b->since<MENU_HOLD_US) return;
    b->used=true;
    if (resogun_display_settings_is_open()) return;
    resogun_display_settings_open();
    printf("Runtime: %s held; Display Settings opened\n",b->name);
}
/* Each player's pad has the shortcuts, on its own buttons. */
static void menu_buttons(PadData *d, int slot, SDL_Gamepad *g, uint64_t now) {
    static MenuButton select[PADS], start[PADS];
    MenuButton *se=&select[slot], *st=&start[slot];
    se->button=SDL_GAMEPAD_BUTTON_BACK; se->name="Select";
    st->button=SDL_GAMEPAD_BUTTON_START; st->name="Start";
    const bool select_down=menu_button(se,d,g,now), start_down=menu_button(st,d,g,now);
    if (select_down && start_down) {
        if (se->used && st->used) return;
        se->used=st->used=true;
        resogun_exit_prompt_open();
        puts("Runtime: Select+Start; asking whether to close the game");
    } else if (select_down) menu_held(se,now);
    else if (start_down) menu_held(st,now);
}

/* Game's pad read (sample): settings.ini title_back_exits. Back (Circle: B, Escape, Backspace, or
 * the pad file) pressed on the
 * title screen, where the game itself ignores it, asks whether to close the game (the overlay's
 * exit prompt); that press does not reach the game. The title screen is up while its movie plays
 * (runtime_pad_title_movie). */
static _Atomic int title_showing;
static void title_back(PadData *d) {
    static bool was_down;
    /* An overlay's input (the prompt answered No with Circle) counts as held: a new press opens it. */
    if (resogun_gpu_overlay_captures_input()) { was_down=true; return; }
    const bool down=(d->buttons & BTN_CIRCLE)!=0;
    const bool pressed=down && !was_down;
    was_down=down;
    if (!pressed || !atomic_load(&title_showing) || !resogun_title_back_exits()) return;
    d->buttons&=~BTN_CIRCLE;
    resogun_exit_prompt_open();
    puts("Runtime: Back on the title screen; asking whether to close the game");
}

/* Host thread: samples every millisecond (SDL_DelayNS waits on a high resolution timer) and
 * passes each user's vibration and light bar color on: when they change, and the vibration every
 * 500 ms while the motors run, as each rumble is sent for 1000 ms. */
static void *host_thread(void *arg) {
    (void)arg;
    SDL_Gamepad *g[PADS]={NULL}, *speaker_pad=NULL;
    uint64_t next_scan=0, rumble_sent[PADS]={0};
    uint32_t rumble_serial[PADS]={0}, light_serial[PADS]={0};
    uint8_t connected_count[PADS]={0};
    for (;;) {
        const uint64_t start=now_us();
        if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
        if (sdl_ready>0) host_gamepads(g,start,&next_scan,connected_count);
        speaker_update(g[0],&speaker_pad);
        PadData d[PADS];
        for (int i=0;i<PADS;++i) {
            host_read(&d[i],i,g[i],connected_count[i]);
            menu_buttons(&d[i],i,g[i],start);
        }
        pthread_mutex_lock(&host_lock);
        memcpy(host_sample,d,sizeof(d));
        __typeof__(rumble) r; __typeof__(light) l;
        memcpy(r,rumble,sizeof(r)); memcpy(l,light,sizeof(l));
        pthread_mutex_unlock(&host_lock);
        for (int i=0;i<PADS;++i) {
            if (!g[i]) continue;
            if (r[i].serial!=rumble_serial[i] || ((r[i].large || r[i].small) && start-rumble_sent[i]>=500000)) {
                SDL_RumbleGamepad(g[i],(uint16_t)(r[i].large*257u),(uint16_t)(r[i].small*257u),1000);
                rumble_serial[i]=r[i].serial; rumble_sent[i]=start;
            }
            if (l[i].serial!=light_serial[i]) {
                if (l[i].r || l[i].g || l[i].b) SDL_SetGamepadLED(g[i],l[i].r,l[i].g,l[i].b);
                light_serial[i]=l[i].serial;
            }
        }
        note_call(SLOW_HOST,start);
        SDL_DelayNS(1000000);
    }
    return NULL;
}

/* RESOGUN_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad up down left right) are held while listed; lx= ly= rx= ry= (0..255) override
 * the sticks; either prefixed with p2: (p2:cross, p2:lx=0) is user 2's; reconnect closes the
 * gamepads once per edit and the host thread opens them again, as after a real reconnect. An empty
 * file releases everything. */
static struct { uint32_t buttons; int stick[4]; } injected[PADS]={{0,{-1,-1,-1,-1}},{0,{-1,-1,-1,-1}}};
static int replay_armed;      /* 1 while a RESOGUN_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("RESOGUN_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    for (int u=0;u<PADS;++u) {
        injected[u].buttons=0;
        for (int i=0;i<4;++i) injected[u].stick[i]=-1;
    }
    char token[64];
    int reconnect=0;
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* RESOGUN_PAD_REPLAY */
        if (!strcmp(token,"reconnect")) reconnect=1;
        const int u=!strncmp(token,"p2:",3);
        const char *t=token+(u ? 3 : 0);
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!strcmp(t,ps_names[i].name)) injected[u].buttons|=ps_names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(t,sticks[i],3)) { int v=atoi(t+3); injected[u].stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    if (reconnect) __atomic_store_n(&reconnect_wanted,1,__ATOMIC_RELEASE); /* host_gamepads closes them */
    for (int u=0;u<PADS;++u) {
        const int *k=injected[u].stick;
        if (!u || injected[u].buttons || k[0]>=0 || k[1]>=0 || k[2]>=0 || k[3]>=0)
            printf("Runtime: pad file: user %d buttons 0x%x sticks %d %d %d %d\n",u+1,injected[u].buttons,k[0],k[1],k[2],k[3]);
    }
}
/* RESOGUN_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; RESOGUN_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in RESOGUN_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("RESOGUN_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("RESOGUN_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}

/* settings.ini skip_intro: the game starts with -nosplash (loader.c), which leaves its title screen
 * ("Press X to start"), the only screen that plays video/title.mp4. While the first title movie of
 * the run plays (AvPlayer reports it, gpu/shadps4/.../avplayer_state.cpp), Cross is tapped for
 * 100 ms every 500 ms from 1 s in, as the player would; the press closes the movie, which ends the
 * taps, so none reaches the main menu. Later title screens (after the attract demo, or Back from
 * the menu) are left alone. */
enum { TITLE_NONE, TITLE_PLAYING, TITLE_DONE };
static _Atomic int title_movie=TITLE_NONE;
static _Atomic uint64_t title_since;
void runtime_pad_title_movie(int playing) {
    atomic_store(&title_showing,playing!=0);
    if (!resogun_skip_intro()) { if (playing) puts("Intro: title screen"); return; }
    if (playing) {
        int none=TITLE_NONE;
        if (!atomic_compare_exchange_strong(&title_movie,&none,TITLE_PLAYING)) return;
        atomic_store(&title_since,now_us());
        puts("Intro: title screen; pressing Cross for the main menu (skip_intro=1)");
    } else if (atomic_exchange(&title_movie,TITLE_DONE)==TITLE_PLAYING) {
        puts("Intro: title screen left");
    }
}
static void title_press(PadData *d) {
    if (atomic_load(&title_movie)!=TITLE_PLAYING) return;
    const uint64_t t=now_us()-atomic_load(&title_since);
    if (t>=1000000 && (t-1000000)%500000<100000) d->buttons|=BTN_CROSS;
}

/* 3D from the pad while it is on (gpu/resogun_gpu.h): with L1+R1 held, D-pad Left/Right steps the
 * depth and Up/Down the convergence, repeated while held; the values are saved when both are let go.
 * The game sees neither shoulder nor the D-pad meanwhile (a direction still held afterwards stays
 * hidden until released). A lone shoulder reaches the game STEREO_JOIN_US late, the time to press
 * the other one; a tap shorter than that still arrives, as a STEREO_TAP_US press. Player 1's
 * pad only, with the pad lock held. */
enum { STEREO_JOIN_US=60000, STEREO_TAP_US=50000, STEREO_REPEAT_FIRST_US=400000, STEREO_REPEAT_US=150000 };
static void stereo_combo(PadData *d) {
    static uint64_t lone_since, tap_until, next_repeat;
    static uint32_t lone, tap, last_dir, dpad_hold;
    static bool active, changed;
    const uint32_t pair=BTN_L1|BTN_R1, dpad=BTN_UP|BTN_DOWN|BTN_LEFT|BTN_RIGHT;
    dpad_hold&=d->buttons;
    d->buttons&=~dpad_hold;
    if (!active && !lone && !tap && !resogun_gpu_stereo_available()) return;
    const uint64_t now=now_us();
    const uint32_t shoulders=d->buttons&pair, held=d->buttons&dpad;
    if (shoulders==pair || (active && shoulders)) {
        if (!active) { active=true; last_dir=0; }
        lone=tap=0;
        d->buttons&=~(pair|dpad);
        if (shoulders!=pair) return;
        const uint32_t dir=held&(0u-held);
        if (!dir) { last_dir=0; return; }
        if (dir!=last_dir) { last_dir=dir; next_repeat=now+STEREO_REPEAT_FIRST_US; }
        else if (now<next_repeat) return;
        else next_repeat=now+STEREO_REPEAT_US;
        resogun_gpu_stereo_adjust(dir==BTN_RIGHT ? 1 : dir==BTN_LEFT ? -1 : 0, dir==BTN_UP ? 1 : dir==BTN_DOWN ? -1 : 0);
        changed=true;
        return;
    }
    if (active) {
        active=false; dpad_hold=held; d->buttons&=~held;
        if (changed) { changed=false; resogun_gpu_stereo_save(); }
    }
    if (shoulders) {
        if (shoulders!=lone) { lone=shoulders; lone_since=now; }
        if (now-lone_since<STEREO_JOIN_US) d->buttons&=~shoulders;
        return;
    }
    if (lone && now-lone_since<STEREO_JOIN_US) { tap=lone; tap_until=now+STEREO_TAP_US; }
    lone=0;
    if (tap && now<tap_until) d->buttons|=tap;
    else tap=0;
}

/* After a dialog closes, buttons still held (the Cross that accepted a name) stay hidden until
 * released: the game would take them as a new press. A replay and the title screen's presses are
 * user 1's; the pad file presses for either user. */
static int hold_after_capture[PADS];
static void sample(PadData *d, int slot) {
    pthread_mutex_lock(&host_lock);
    *d=host_sample[slot];
    pthread_mutex_unlock(&host_lock);
    if (!d->connected_count) { /* before the host thread's first sample */
        d->left_x=d->left_y=d->right_x=d->right_y=128;
        d->orientation[3]=1.0f;
        d->connected=!slot; d->connected_count=1;
    }
    d->timestamp=now_us();
    if (resogun_gpu_overlay_captures_input()) {
        hold_after_capture[slot]=1; clicks[slot].active=false;
        if (!slot) title_back(d);
        return;
    }
    if (!slot) record_sample(d); /* the click sources as they are: a replay plays them again */
    read_inject();
    if (!slot) replay_sample(d);
    d->buttons|=injected[slot].buttons;
    if (injected[slot].buttons & BTN_L2) d->l2=255;
    if (injected[slot].buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected[slot].stick[i]>=0) *axes[i]=(uint8_t)injected[slot].stick[i];
    if (!slot) stereo_combo(d);
    if (!slot) title_back(d);
    play_click(d,&clicks[slot]);
    if (!slot) title_press(d);
    if (hold_after_capture[slot]) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture[slot]=0;
    }
}

static void start_host_thread(void) {
    pthread_t thread;
    if (pthread_create(&thread,NULL,host_thread,NULL)==0) pthread_detach(thread);
    else puts("Runtime: pad host thread failed to start; the pad reads neutral");
}
static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
/* scePadOpen(user, type, index, param): one standard (0) or special (2) port per user, index 0;
 * the handle is the user's id. User 2's pad opens without a gamepad too and reads disconnected. */
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<1 || user>PADS) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened[user-1]; opened[user-1]=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    static pthread_once_t host_started=PTHREAD_ONCE_INIT;
    pthread_once(&host_started,start_host_thread);
    printf("Runtime: pad opened for user %d (%s)\n",user,user==1 ? "first gamepad or keyboard" : "second gamepad");
    return user;
}
static ABI int32_t pad_close(int32_t handle) {
    const int slot=slot_of(handle);
    if (slot<0) return ERR_INVALID_HANDLE;
    opened[slot]=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    const int slot=slot_of(handle);
    if (slot<0) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    const uint64_t start=now_us();
    pthread_mutex_lock(&lock);
    sample(data,slot); ++reads;
    note_call(SLOW_READ,start);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    const int slot=slot_of(handle);
    if (slot<0) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->device_class=0;
    const uint64_t start=now_us();
    pthread_mutex_lock(&host_lock);
    const PadData *h=&host_sample[slot];
    info->connected=h->connected_count ? h->connected : !slot;
    info->connected_count=h->connected_count ? h->connected_count : 1;
    pthread_mutex_unlock(&host_lock);
    note_call(SLOW_INFO,start);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    const int slot=slot_of(handle);
    if (slot<0) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    const uint64_t start=now_us();
    pthread_mutex_lock(&host_lock); /* host_thread sends it */
    if (rumble[slot].large!=param[0] || rumble[slot].small!=param[1]) {
        rumble[slot].large=param[0]; rumble[slot].small=param[1]; ++rumble[slot].serial;
    }
    pthread_mutex_unlock(&host_lock);
    note_call(SLOW_VIBRATION,start);
    __atomic_add_fetch(&vibrations,1,__ATOMIC_RELAXED);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return slot_of(handle)>=0 ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

/* scePadSetLightBar(handle, {r,g,b}): the host thread puts the color on the gamepad's LED when it
 * has one (a DualShock 4's light bar, a DualSense's), so each player sees whose pad is whose. */
static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    const int slot=slot_of(handle);
    if (slot<0) return ERR_INVALID_HANDLE;
    if (!rgb) return ERR_INVALID_ARG;
    pthread_mutex_lock(&host_lock);
    if (light[slot].r!=rgb[0] || light[slot].g!=rgb[1] || light[slot].b!=rgb[2]) {
        light[slot].r=rgb[0]; light[slot].g=rgb[1]; light[slot].b=rgb[2]; ++light[slot].serial;
    }
    pthread_mutex_unlock(&host_lock);
    return 0;
}
static const RuntimeExport exports[]={
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_ok_handle},
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) {
    /* On the way out: a DualSense routed to its speaker gets its headphone jack back (the host
       thread sends it; up to 50 ms). */
    if (__atomic_load_n(&speaker_routed,__ATOMIC_ACQUIRE)==1) {
        runtime_pad_speaker_route(0);
        for (int i=0;i<50 && __atomic_load_n(&speaker_routed,__ATOMIC_ACQUIRE)==1;++i) SDL_Delay(1);
    }
    char name[PADS][sizeof(gamepad_name[0])];
    pthread_mutex_lock(&host_lock); memcpy(name,gamepad_name,sizeof(name)); pthread_mutex_unlock(&host_lock);
    printf("Runtime: pad reads=%zu, vibrations=%zu, gamepads=%s / %s; over 2 ms: reads %zu (max %.1f ms), "
           "vibrations %zu (max %.1f ms), info %zu (max %.1f ms), host samples %zu (max %.1f ms)\n",reads,vibrations,name[0],name[1],
           slow_calls[SLOW_READ],slow_max_us[SLOW_READ]/1e3,slow_calls[SLOW_VIBRATION],slow_max_us[SLOW_VIBRATION]/1e3,
           slow_calls[SLOW_INFO],slow_max_us[SLOW_INFO]/1e3,slow_calls[SLOW_HOST],slow_max_us[SLOW_HOST]/1e3);
}

/* resogun --gamepad: runs before any game state exists, so it uses its own SDL gamepad and leaves
 * the lock and statics above alone. */
static const char *label_name(SDL_GamepadButtonLabel label) {
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "a";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "b";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "x";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "triangle";
    default: return "unknown";
    }
}
int runtime_pad_describe(void) {
    SDL_Gamepad *g=NULL;
    const bool ready=SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    /* No event loop yet, so pads show up only after a pump: poll for about 300 ms. */
    for (int waited=0; ready && !g && waited<30; ++waited) {
        SDL_PumpEvents();
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) g=SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
        if (!g) SDL_Delay(10);
    }
    if (!g) puts("Gamepad: none");
    else {
        const char *type=SDL_GetGamepadStringForType(SDL_GetGamepadType(g));
        const char *name=SDL_GetGamepadName(g);
        printf("Gamepad: type=%s south=%s east=%s west=%s north=%s name=%s\n",
               type ? type : "unknown",
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_SOUTH)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_EAST)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_WEST)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_NORTH)),
               name ? name : "unknown");
        SDL_CloseGamepad(g);
    }
    if (ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return g!=NULL;
}
