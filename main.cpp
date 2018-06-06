#define SDL_MAIN_HANDLED

#include "mediastate.h"

int main(int argc, char *argv[])
{
    if (argc != 2)
        return -1;

    media_init();

    MediaState *s = NULL;

    media_open_input_file(&s, argv[1]);
    media_create_video_display(s, NULL);
    media_open_audio_device(s);

    media_play(s);

    media_state_free(&s);

    return 0;
}
