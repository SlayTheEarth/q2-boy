// SPDX-License-Identifier: GPL-2.0-or-later
// Host test: run in a fresh scratch directory with Q2_RUNTIME_DIR set to ".".
#define main pocket_boy_main
#include "../src/main.c"
#undef main
#include <assert.h>

static bool pcm_open, view_yielded;
static unsigned resumed;
static uint64_t test_clock = 1000000000ull;

uint64_t clock_ns(void) { return test_clock; }
void platform_audio_release(void) { pcm_open = false; }
int platform_audio_open(const char *device, const char *volume) {
    assert((!strcmp(device, "plughw:0,0") && !*volume) ||
           (!strcmp(device, "usb") && !strcmp(volume, "h70")));
    return pcm_open = true;
}
void platform_audio_select(int route, int gain) { (void)route; (void)gain; abort(); }
void platform_yield_to_player(void) { view_yielded = true; }
void platform_resume_from_player(void) { view_yielded = false; ++resumed; }

static void write_request(const char *path, const char *value) {
    assert(file_write(path, value, strlen(value)));
}

int main(void) {
    assert(has_cap("now audio2 gameview\n", "audio2"));
    assert(!has_cap("audio20 gameview\n", "audio2"));
    strcpy(app_dir, ".");
    data_dir = ".";
    write_request("q2app", "name=Pocket Boy\r\ngame=1\r\naudio=1\r\ngameview=1\r\n");
    bool was = true;
    assert(enable_manifest_audio(true, &was) && !was);
    char manifest[100];
    assert(read_text("q2app", manifest, sizeof manifest));
    assert(!strcmp(manifest, "name=Pocket Boy\r\ngame=1\r\naudio=2\r\ngameview=1\r\n"));
    assert(enable_manifest_audio(false, &was) && was);
    assert(read_text("q2app", manifest, sizeof manifest) && strstr(manifest, "audio=1\r\n"));

    assert(write_quit_option(true) && !access(QUIT_OPTION, F_OK));
    assert(write_quit_option(false) && access(QUIT_OPTION, F_OK));
    assert(write_quit_option(false));

    audio_bridge = audio2 = true;
    music_mode = true;
    assert(switch_audio2(AUDIO_GAME, false));
    audio2_poll(test_clock);
    assert(pending_action == AUDIO_GAME && !pcm_open);
    write_request(AUDIO_MODE, "usb h70\n");
    audio2_poll(test_clock);
    assert(!pending_action && !music_mode && pcm_open);
    request_audio(AUDIO_NOW_PLAYING, 0);
    char view[12], reply[12];
    assert(pending_action == AUDIO_MUSIC && music_mode && !pcm_open && view_yielded);
    assert(read_text(AUDIO_MODE, reply, sizeof reply) && !strcmp(reply, "music"));
    assert(read_text(NATIVE_VIEW, view, sizeof view) && !strcmp(view, "now"));
    assert(!unlink(AUDIO_MODE));
    write_request(NATIVE_VIEW, "open");
    audio2_poll(test_clock);
    assert(!pending_action && view_yielded);
    assert(!unlink(NATIVE_VIEW));
    audio2_poll(test_clock);
    assert(!view_yielded && resumed == 1 && pending_action == AUDIO_GAME);
    write_request(AUDIO_MODE, "plughw:0,0\n");
    audio2_poll(test_clock);
    assert(!pending_action && pcm_open && !music_mode && !prefer_music);

    request_audio(AUDIO_MUSIC, 0);
    assert(!pcm_open && pending_action == AUDIO_MUSIC);
    assert(!unlink(AUDIO_MODE));
    audio2_poll(test_clock);
    assert(music_mode && prefer_music && !pending_action);
    request_audio(AUDIO_NOW_PLAYING, 0);
    assert(view_yielded && !pending_action);
    test_clock += 6000000000ull;
    audio2_poll(test_clock); // Unanswered view is cancelled without running the game.
    assert(!view_yielded && resumed == 2 && access(NATIVE_VIEW, F_OK));
    assert(music_mode && !pending_action && !pcm_open);
    puts("PASS: audio manifest, quit option, audio takeover, Now Playing handoff, recovery");
}
