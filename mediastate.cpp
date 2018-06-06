#include "mediastate.h"
#include "demuxer.h"
#include "decoder.h"

int interrupt_cb(void *ctx);
int audio_decode_frame(MediaState* s, uint8_t *audio_buf, int buf_size);
void audio_callback(void* userdata, uint8_t *stream, int len);

void media_init()
{
    av_register_all();

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
}

MediaState *media_state_alloc()
{
    MediaState *s;
    s = (MediaState *)av_malloc(sizeof(MediaState));
    if (!s)
        return s;

    media_state_init(s);

    return s;
}

void media_state_init(MediaState *s)
{
    if (!s)
        return;

    *s = { 0 };

    s->audio_stream_index = -1;
    s->video_stream_index = -1;

    s->display_pix_fmt = AV_PIX_FMT_YUV420P;

    s->r.x = 100;
    s->r.y = 100;
    s->r.w = 640;
    s->r.h = 480;

    s->vol = SDL_MIX_MAXVOLUME * 0.7;

    s->frame_last_delay = 40e-3;
    s->delay = 40;

    packet_queue_init(&s->video_packet_queue);
    packet_queue_init(&s->audio_packet_queue);
}

void media_state_free(MediaState **ps)
{
    MediaState *s;

    if (!ps || !*ps)
        return;

    s = *ps;

    SDL_CloseAudio();

    if (s->display)
        SDL_DestroyWindow(s->display);
    if (s->render)
        SDL_DestroyRenderer(s->render);
    if (s->texture)
        SDL_DestroyTexture(s->texture);

    SDL_Quit();

    if (s->pkt.data)
        av_packet_unref(&s->pkt);

    if (s->wanted_frame) //avframe free
        av_frame_free(&s->wanted_frame);

    if (s->audio_out_frame) //avframe free
        av_frame_free(&s->audio_out_frame);

    if (s->audio_codec_ctx) //audio context
        avcodec_close(s->audio_codec_ctx);

    if (s->video_out_frame)
        av_frame_free(&s->video_out_frame);

    if (s->video_codec_ctx) //video context
        avcodec_close(s->video_codec_ctx);

    if (s->ic) //format context
        avformat_close_input(&s->ic);

    if (s->swr_ctx) //swr free
        swr_free(&s->swr_ctx);

    if (s->audio_buf) //buff free
        av_freep(&s->audio_buf);

    if (s->sws_ctx)
        sws_freeContext(s->sws_ctx);

    if (s->video_buf)
        av_freep(&s->video_buf);

    packet_queue_flush(&s->video_packet_queue);
    packet_queue_flush(&s->audio_packet_queue);

    av_free(s);

    *ps = NULL;
}

int media_open_input_file(MediaState **ps, const char *filename)
{
    MediaState *s = *ps;

    if (!s && !(s = media_state_alloc()))
        return -1;

    int ret;

    // open file and read information
    ret = avformat_open_input(&s->ic, filename, NULL, NULL);
    if (ret < 0) {
        printf("open file failed, the file path may be invalid!");
        goto clean; // open failed
    }

    s->ic->interrupt_callback.callback = interrupt_cb; //callback
    s->ic->interrupt_callback.opaque = s;

    // stream infomation
    ret = avformat_find_stream_info(s->ic, NULL);
    if (ret < 0) {
        printf("has no audio and video stream!");
        goto clean; // no stream information
    }

    // dump video information
    av_dump_format(s->ic, 0, filename, 0);

    //find video and audio stream
    s->audio_stream_index = -1;
    s->video_stream_index = -1;
    for (unsigned int i = 0; i < s->ic->nb_streams; i++) {
        AVStream *stream = s->ic->streams[i];
        AVCodecContext *c = stream->codec;
        AVCodec *codec = avcodec_find_decoder(c->codec_id);
        if (!codec) {
            ret = -1;
            printf("unsupported %d codec!", c->codec_type);
            goto clean;
        }

        ret = avcodec_open2(c, codec, NULL); //open
        if (ret < 0) {
            printf("cannot open %d codec!", c->codec_type);
            goto clean;
        }

        if (c->codec_type == AVMEDIA_TYPE_VIDEO) {
            s->video_stream_index = i;
            s->video_stream = stream;
            s->video_codec_ctx = c;
            s->video_codec = codec;
        } else if (c->codec_type == AVMEDIA_TYPE_AUDIO) {
            s->audio_stream_index = i;
            s->audio_stream = stream;
            s->audio_codec_ctx = c;
            s->audio_codec = codec;
        }
    }
    *ps = s;
    return 0;

clean:
    media_state_free(&s);
    *ps = NULL;
    return ret;
}

int media_create_video_display(MediaState *s, void *handle)
{
    if (!s || !s->video_codec_ctx)
        return -1;

    s->video_out_frame = av_frame_alloc();
    if (!s->video_out_frame)
        goto clean;

    //convert YUV
    s->sws_ctx = sws_getContext(s->video_codec_ctx->width, s->video_codec_ctx->height,
                                s->video_codec_ctx->pix_fmt,
                                s->video_codec_ctx->width, s->video_codec_ctx->height,
                                s->display_pix_fmt,
                                SWS_BICUBIC, NULL, NULL, NULL);
    if (!s->sws_ctx)
        goto clean;

    s->video_buf_size = avpicture_get_size(s->display_pix_fmt, s->video_codec_ctx->width, s->video_codec_ctx->height);

    s->video_buf = (uint8_t *)av_malloc(s->video_buf_size * sizeof(uint8_t));

    if (avpicture_fill((AVPicture *)s->video_out_frame, s->video_buf,
                       s->display_pix_fmt,
                       s->video_codec_ctx->width, s->video_codec_ctx->height) < 0)
        goto clean;

    s->r.x = 0;
    s->r.y = 0;
    s->r.w = s->video_codec_ctx->width;
    s->r.h = s->video_codec_ctx->height;

    if (handle)
        s->display = SDL_CreateWindowFrom(handle);
    else
        s->display = SDL_CreateWindow("Player",
                                      100, 100,
                                      s->r.w, s->r.h,
                                      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!s->display)
        goto clean;

    s->render = SDL_CreateRenderer(s->display, -1, 0);
    if (!s->render)
         goto clean;
    s->texture = SDL_CreateTexture(s->render, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, s->r.w, s->r.h);
    if (!s->texture)
         goto clean;

    return 0;

clean:
    if (s->video_out_frame)
        av_frame_free(&s->video_out_frame);
    if (s->sws_ctx)
        sws_freeContext(s->sws_ctx);
    if (s->video_buf)
        av_freep(&s->video_buf);

    if (s->display)
        SDL_DestroyWindow(s->display);
    if (s->render)
        SDL_DestroyRenderer(s->render);
    if (s->texture)
        SDL_DestroyTexture(s->texture);

    return -1;
}

int media_open_audio_device(MediaState *s)
{
    if (!s || !s->audio_codec_ctx)
        return -1;

    // Set audio settings from codec info
    SDL_AudioSpec wanted_spec, spec;
    if (s->audio_stream_index != -1) {
        wanted_spec.freq = s->audio_codec_ctx->sample_rate;
        wanted_spec.format = AUDIO_S16SYS;
        wanted_spec.channels = s->audio_codec_ctx->channels;
        wanted_spec.silence = 0;
        wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
        wanted_spec.callback = audio_callback;
        wanted_spec.userdata = s;

        if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
            printf("open audio device failed: %s", SDL_GetError());
            return -1;
        }

        s->audio_buf = (uint8_t *)av_malloc(MAX_AUDIO_FRAME_SIZE * 2 * sizeof(uint8_t));

        s->audio_out_frame = av_frame_alloc();
        s->wanted_frame = av_frame_alloc();

        s->wanted_frame->format = AV_SAMPLE_FMT_S16;
        s->wanted_frame->sample_rate = spec.freq;
        s->wanted_frame->channel_layout = av_get_default_channel_layout(spec.channels);
        s->wanted_frame->channels = spec.channels;

        SDL_PauseAudio(0);
    }

    return 0;
}

//if the network is poor, running frequently
int interrupt_cb(void *ctx)
{
   (MediaState *)ctx;
   return 0;
}

//decode audio data
int audio_decode_frame(MediaState* s, uint8_t *audio_buf, int buf_size)
{
    UNUSED(buf_size);

    if (s->quit || s->seek_req)
        return -1;

#if 1
    AVPacket pkt, *packet = &pkt;
    av_init_packet(packet);
    packet->data = NULL;
    packet->size = 0;

    if (packet_queue_get(&s->audio_packet_queue, packet, 0) <= 0) //get packet from queue
        return -1;

    //receive FLUSH data to flush codec, because of seeking
    if(strcmp((char *)packet->data, FLUSH_DATA) == 0) {
        avcodec_flush_buffers(s->audio_stream->codec);
        //av_packet_unref(packet);  //快进时free会崩溃
        return -1;
    }

    int ret, got_frame;
    AVFrame *frame = NULL;
    SwrContext *swr_ctx = NULL;

    frame = av_frame_alloc();
    if (!frame) {
        ret = -1;
        goto clean;
    }

    ret = avcodec_decode_audio4(s->audio_codec_ctx, frame, &got_frame, packet);
    if (ret < 0){
        goto clean;
    }

    if (!got_frame) {
        ret = -1;
        goto clean;
    }

    if (packet->pts != AV_NOPTS_VALUE) {
        s->audio_clock = av_q2d(s->audio_stream->time_base) * packet->pts;
    }

    if (frame->channels > 0 && frame->channel_layout == 0) {
        frame->channel_layout = av_get_default_channel_layout(frame->channels);
    } else if (frame->channels == 0 && frame->channel_layout > 0) {
        frame->channels = av_get_channel_layout_nb_channels(frame->channel_layout);
    }

    swr_ctx = swr_alloc_set_opts(NULL,
                                 s->wanted_frame->channel_layout,
                                 (AVSampleFormat)s->wanted_frame->format,
                                 s->wanted_frame->sample_rate,
                                 frame->channel_layout,
                                 (AVSampleFormat)frame->format,
                                 frame->sample_rate, 0, NULL);

    if (!swr_ctx || swr_init(swr_ctx) < 0) {
        ret = -1;
        printf("swr_init failed!\n");
        goto clean;
    }

    int dst_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, frame->sample_rate) + frame->nb_samples,
                                        frame->sample_rate,
                                        frame->sample_rate, AVRounding(1));

    int convert_len = swr_convert(swr_ctx, &audio_buf, dst_nb_samples,
                                  (const uint8_t **)frame->data,
                                  frame->nb_samples);//important!!! in front of all are for this, here
    if (convert_len < 0) {
        ret = -1;
        printf("swr_convert failed\n");
        goto clean;
    }

//[][]important!!! convert to audio clock
    int resampled_data_size = convert_len * s->wanted_frame->channels * av_get_bytes_per_sample((AVSampleFormat)s->wanted_frame->format);
    int n = 2 * s->audio_stream->codec->channels;
    s->audio_clock += (double)resampled_data_size / (double)(n * s->audio_stream->codec->sample_rate);
//[][]

    ret = resampled_data_size;

clean:
    av_packet_unref(packet);
    if (frame)
        av_frame_free(&frame);
    if (swr_ctx)
        swr_free(&swr_ctx);

    return ret;
#else
    int len1;
    int data_size = 0;

    while (1) {
        while (s->audio_pkt_size > 0) {
            int got_frame = 0;
            len1 = avcodec_decode_audio4(s->audio_codec_ctx, s->audio_out_frame,
                                         &got_frame, &s->pkt);
            if (len1 < 0){
                s->audio_pkt_size = 0;
                break;
            }

//            s->audio_pkt_data += len1;
            s->audio_pkt_size -= len1;
            data_size = 0;
            if (got_frame)
                data_size = av_samples_get_buffer_size(NULL,
                                                       s->audio_codec_ctx->channels,
                                                       s->audio_out_frame->nb_samples,
                                                       s->audio_codec_ctx->sample_fmt,
                                                       1);
            else
                break;

            if (s->audio_out_frame->channels > 0 && s->audio_out_frame->channel_layout == 0) {
                s->audio_out_frame->channel_layout =
                        av_get_default_channel_layout(s->audio_out_frame->channels);
            } else if (s->audio_out_frame->channels == 0 && s->audio_out_frame->channel_layout > 0) {
                s->audio_out_frame->channels =
                        av_get_channel_layout_nb_channels(s->audio_out_frame->channel_layout);
            }
            if (s->swr_ctx) {
                swr_free(&s->swr_ctx);
                s->swr_ctx = NULL;
            }
            s->swr_ctx = swr_alloc_set_opts(NULL,
                                            s->wanted_frame->channel_layout,
                                            (AVSampleFormat)s->wanted_frame->format,
                                            s->wanted_frame->sample_rate,
                                            s->audio_out_frame->channel_layout,
                                            (AVSampleFormat)s->audio_out_frame->format,
                                            s->audio_out_frame->sample_rate, 0, NULL);

            if (!s->swr_ctx || swr_init(s->swr_ctx) < 0) {
                printf("swr_init failed!\n");
                break;
            }
            int dst_nb_samples = av_rescale_rnd(swr_get_delay(s->swr_ctx, s->audio_out_frame->sample_rate) + s->audio_out_frame->nb_samples,
                                                s->audio_out_frame->sample_rate,
                                                s->audio_out_frame->sample_rate, AVRounding(1));

            int convert_len = swr_convert(s->swr_ctx, &audio_buf, dst_nb_samples,
                                          (const uint8_t **)s->audio_out_frame->data,
                                          s->audio_out_frame->nb_samples);//important!!! in front of all are for this, here
            if (convert_len < 0) {
                printf("swr_convert failed\n");
                break;
            }
//[][]important!!! convert to audio clock
            int resampled_data_size = convert_len * s->wanted_frame->channels * av_get_bytes_per_sample((AVSampleFormat)s->wanted_frame->format);
            int n = 2 * s->audio_stream->codec->channels;
            s->audio_clock += (double)resampled_data_size / (double)(n * s->audio_stream->codec->sample_rate);
//[][]
            return resampled_data_size;
        } //end while

        if (s->pkt.buf)
            av_packet_unref(&s->pkt); //free packet

        if (packet_queue_get(&s->audio_packet_queue, &s->pkt, 0) <= 0) //reget packet from queue
            return -1;

        //receive FLUSH data to flush codec, because of seeking
        if(strcmp((char*)s->pkt.data, FLUSH_DATA) == 0) {
            avcodec_flush_buffers(s->audio_stream->codec);
            //av_packet_unref(&s->pkt);
            continue;
        }

        if (s->pkt.pts != AV_NOPTS_VALUE) {
            s->audio_clock = av_q2d(s->audio_stream->time_base) * s->pkt.pts;
        }
//        s->audio_pkt_data = s->pkt.data;
        s->audio_pkt_size = s->pkt.size;
    }
#endif
}

// audio call back after every decode operation
void audio_callback(void *userdata, uint8_t *stream, int len)
{
    MediaState* s = (MediaState *)userdata;
    if (s && s->quit)
        return;

	if (s && s->seek_req)
		return;

    int send_data_size, audio_size;

    SDL_memset(stream, 0, len);

    while (len > 0) {
        //uint8_t audio_buff[MAX_AUDIO_FRAME_SIZE * 2];

        if (s->audio_buf_index >= s->audio_buf_size) {
            //数据已经全部发送，再去取
            audio_size = audio_decode_frame(s, s->audio_buf, sizeof(s->audio_buf));

            if (audio_size < 0) {
                //错误则静音一下
                s->audio_buf_size = 1024;
                SDL_memset(s->audio_buf, 0, s->audio_buf_size);
            } else {
                //解码这么多
                s->audio_buf_size = audio_size;
            }
            s->audio_buf_index = 0;
        }

        send_data_size = s->audio_buf_size - s->audio_buf_index;
        if (send_data_size > len)
            send_data_size = len;

        SDL_MixAudio(stream, s->audio_buf + s->audio_buf_index, send_data_size, s->vol);

        len -= send_data_size;
        stream += send_data_size;
        s->audio_buf_index += send_data_size;
    }
}

int media_play(MediaState *s)
{
    if (!s || !s->ic)
        return -1;

    SDL_Thread *demux = SDL_CreateThread(demux_callback, "demuxer", s);
    SDL_Thread *refresh = SDL_CreateThread(refresh_callback, "refresh", s);

    SDL_Event event;
    while(1) {
        if (s->quit) {
            break;
        }

        SDL_WaitEvent(&event);
        switch (event.type) {
            case REFRESH_EVENT: {
                decode_and_show(s);
                break;
            }
            case SDL_WINDOWEVENT: {
                SDL_GetWindowSize(s->display, &s->r.w, &s->r.h);
                break;
            }
            case SDL_KEYUP: {
                switch (event.key.keysym.sym) {
                    case SDLK_UP: {
                        s->vol += SDL_MIX_MAXVOLUME * 0.05;
                        if (s->vol > SDL_MIX_MAXVOLUME)
                            s->vol = SDL_MIX_MAXVOLUME;
                        break;
                    }
                    case SDLK_DOWN: {
                        s->vol -= SDL_MIX_MAXVOLUME * 0.05;
                        if (s->vol < 0)
                            s->vol = 0;
                        break;
                    }
                    case SDLK_LEFT: {
                        int64_t pos = s->audio_clock * AV_TIME_BASE;
                        media_seek(s, pos - 5 * AV_TIME_BASE);
                        break;
                    }
                    case SDLK_RIGHT: {
                        int64_t pos = s->audio_clock * AV_TIME_BASE;
                        media_seek(s, pos + 5 * AV_TIME_BASE);
                        break;
                    }
                    case SDLK_SPACE: {
                        int status = media_status(s);
                        if (status == MediaState::PausedState) {
                            media_pause(s, 0);
                        } else if (status == MediaState::PlayingState) {
                            media_pause(s, 1);
                        }
                        break;
                    }
                    case SDLK_ESCAPE: {
                        s->quit = 1;
                        break;
                    }
                }
                break;
            }
            case SDL_QUIT: {
                s->quit = 1;
                break;
            }
            default: {
                break;
            }
        }
    }

    SDL_Delay(100);

    return 0;
}

int media_stop(MediaState *s)
{
    if (!s)
        return -1;

    s->quit = 1;

    return 0;
}

int media_pause(MediaState *s, int on)
{
    if (!s)
        return -1;

    if (s->audio_stream_index > -1) {
        SDL_PauseAudio(on);
    } else {
        s->pause = on;
    }
    return 0;
}

int media_status(MediaState *s)
{
    if (!s || (s && !s->ic))
        return MediaState::StoppedState;

//    if(s->is_buffering)
//        return MediaState::BufferingState;

    if (s->audio_stream_index > -1) {
        if(SDL_AUDIO_PLAYING == SDL_GetAudioStatus())
            return MediaState::PlayingState;
    } else {
        if (s->pause == 0)
            return MediaState::PlayingState;
    }

    return MediaState::PausedState;
}

int media_seek(MediaState *s, int64_t pos)
{
    if (!s)
        return -1;

    if(!s->seek_req) {
        s->seek_pos = pos;
        s->seek_req = 1;
    }

    return 0;
}

int64_t media_duration(MediaState *s)
{
    if (!s)
        return 0;

    if (s->ic && s->ic->duration != AV_NOPTS_VALUE) {
        return s->ic->duration + (s->ic->duration <= INT64_MAX - 5000 ? 5000 : 0);
    }

    if (s->video_stream)
        return av_rescale_q(s->video_stream->duration, AVRational{ 1, AV_TIME_BASE }, s->video_stream->time_base);

    return 0;
}
