#ifndef MEDIASTATE_H
#define MEDIASTATE_H

#define MAX_AUDIO_FRAME_SIZE 192000//1 second of 48khz 32bit audio
#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_SIZE (5 * 16 * 1024)
#define MAX_VIDEO_SIZE (5 * 256 * 1024)

#define REFRESH_EVENT (SDL_USEREVENT + 1)
#define BREAK_EVENT (SDL_USEREVENT + 2)

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include "packetqueue.h"


typedef struct MediaState {
    AVFormatContext *ic;
    AVPacket pkt;

    //audio
    int audio_stream_index;
    AVStream *audio_stream;
    AVCodecContext *audio_codec_ctx;
    AVCodec *audio_codec;
    PacketQueue audio_packet_queue;

//    uint8_t *audio_pkt_data;
    int audio_pkt_size;
    struct SwrContext* swr_ctx;
    AVFrame *audio_out_frame;
    AVFrame *wanted_frame;

    uint8_t *audio_buf;
    unsigned int audio_buf_size;
    unsigned int audio_buf_index;
    int is_buffering;
    int seek_req;
    int64_t seek_pos;

    //video
    int video_stream_index;
    AVStream *video_stream;
    AVCodecContext *video_codec_ctx;
    AVCodec *video_codec;
    PacketQueue video_packet_queue;

    uint8_t *video_buf;
    unsigned int video_buf_size;
    struct SwsContext *sws_ctx;
    AVFrame *video_out_frame;

    AVPixelFormat display_pix_fmt;

    //sync
    double audio_clock;
    double video_clock;
    double frame_last_pts; 			//前一帧显示时间
    double frame_last_delay; 	//当前帧和前一帧的延时，前面两个相减的结果
    uint32_t delay;

    SDL_Rect r;
    SDL_Window *display;
    SDL_Renderer *render;
    SDL_Texture *texture;

    //other
    int vol;
    int quit;
    int pause;

    enum State {
        PlayingState = 0,
        PausedState,
        StoppedState,
        BufferingState
    };

} MediaState;

void media_init();

MediaState *media_state_alloc();

void media_state_init(MediaState *s);

void media_state_free(MediaState **s);

int media_open_input_file(MediaState **s, const char *filename);

int media_create_video_display(MediaState *s, void *handle);

int media_open_audio_device(MediaState *s);

int media_play(MediaState *s);

int media_stop(MediaState *s);

int media_pause(MediaState *s, int on);

int media_status(MediaState *s);

int media_seek(MediaState *s, int64_t pos);

#ifdef __cplusplus
}
#endif

#endif // MEDIASTATE_H
