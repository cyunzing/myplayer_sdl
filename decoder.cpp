#include "decoder.h"

double get_frame_pts(MediaState *s, AVFrame *src, double pts)
{
    double frame_delay;

    if (pts != 0) {
        /* if we have pts, set video clock to it */
        s->video_clock = pts;
    } else {
        /* if we aren't given a pts, set it to the clock */
        pts = s->video_clock;
    }
    /* update the video clock */
    frame_delay = av_q2d(s->video_stream->codec->time_base);
    /* if we are repeating a frame, adjust clock accordingly */
    frame_delay += src->repeat_pict * (frame_delay * 0.5);
    s->video_clock += frame_delay;

    return pts;
}

int refresh_callback(void *userdata)
{
    MediaState *s = (MediaState *)userdata;
    if (!s)
        return -1;

    SDL_Event event;

    while(1) {
        if (s->quit)
            break;

        if (s->audio_stream_index == -1 && s->pause) {//pause
            SDL_Delay(10);
            continue;
        }
        if (SDL_AUDIO_PAUSED == SDL_GetAudioStatus()) {//pause
            SDL_Delay(10);
            continue;
        }

        event.type = REFRESH_EVENT;
        SDL_PushEvent(&event);

        SDL_Delay(s->delay);
    }

    return 0;
}

int decode_and_show(MediaState *s)
{
    int ret, got_picture;
    AVFrame *frame = NULL;
    AVPacket pkt, *packet = &pkt;
    double video_pts, audio_pts;
    double diff, frame_delay;

    av_init_packet(packet);
    packet->data = NULL;
    packet->size = 0;

    if (packet_queue_get(&s->video_packet_queue, packet, 0) <= 0) { //!block
        //no data
        return -1;
    }

    //receive FLUSH data to flush codec, because of seeking
    if (strcmp((char *)packet->data, FLUSH_DATA) == 0) {
        avcodec_flush_buffers(s->video_stream->codec);
        av_packet_unref(packet);
        return 0;
    }

    frame = av_frame_alloc();
    ret = avcodec_decode_video2(s->video_codec_ctx, frame, &got_picture, packet);
    if (ret < 0) {
        printf("decode error\n");
        av_frame_free(&frame);
        av_packet_unref(packet);
        return -1;
    }

//sync video and audio
    if (s->audio_stream_index != -1) {
        if (packet->dts == AV_NOPTS_VALUE && frame->opaque && *(uint64_t*)frame->opaque != AV_NOPTS_VALUE) {
            video_pts = *(uint64_t *)frame->opaque;
        } else if (packet->dts != AV_NOPTS_VALUE) {
            video_pts = packet->dts;
        } else {
            video_pts = 0.0;
        }

        video_pts *= av_q2d(s->video_stream->time_base);
        video_pts = get_frame_pts(s, frame, video_pts);

        frame_delay = video_pts - s->frame_last_pts;
        if (frame_delay <= 0 || frame_delay >= 1.0)
            frame_delay = s->frame_last_delay;

        s->frame_last_delay = frame_delay;
        s->frame_last_pts = video_pts;

        audio_pts = s->audio_clock;
        diff = video_pts - audio_pts;
        if (diff <= -frame_delay) // 慢了，delay设为0
            frame_delay /= 2;
        else if (diff >= frame_delay) // 快了，加倍delay
            frame_delay *= 2;

        s->delay = (frame_delay) * 1000 + 0.5;
    } else {
        if (packet->pts != AV_NOPTS_VALUE) {
            s->video_clock = av_q2d(s->video_stream->time_base) * packet->pts;
        }

        s->delay = 1000 / av_q2d(s->video_stream->r_frame_rate);
    }
//sync end

    sws_scale(s->sws_ctx,
              (uint8_t const * const *)frame->data,
              frame->linesize, 0, s->video_codec_ctx->height,
              s->video_out_frame->data, s->video_out_frame->linesize);

    double ratio = (double)s->video_codec_ctx->width / s->video_codec_ctx->height;
    double tmp = (double)s->r.w / s->r.h;

    SDL_Rect r;
    if (tmp > ratio) {
        r.h = s->r.h;
        r.w = s->r.h * ratio;
    } else {
        r.w = s->r.w;
        r.h = s->r.w / ratio;
    }
    r.x = (s->r.w - r.w) / 2;
    r.y = (s->r.h - r.h) / 2;

    SDL_UpdateTexture(s->texture, NULL, s->video_out_frame->data[0], s->video_out_frame->linesize[0]);
    SDL_RenderClear(s->render);
    SDL_RenderCopy(s->render, s->texture, NULL, &r);
    SDL_RenderPresent(s->render);

    av_frame_free(&frame);
    av_packet_unref(packet);

    return 0;
}

int decode_callback(void *userdata)
{
    MediaState *s = (MediaState *)userdata;
    if (!s)
        return -1;

    AVPacket pkt1, *packet = &pkt1;

    int ret, got_picture, numBytes;

    double video_pts = 0; //video pts
    double audio_pts = 0; //audio pts

    //decode video
    AVFrame *frame, *out_frame;
    uint8_t *out_buffer_rgb; //rgb buffer after decode
    struct SwsContext *img_convert_ctx;  //convert for video format

    frame = av_frame_alloc();
    out_frame = av_frame_alloc();

    //convert YUV
    img_convert_ctx = sws_getContext(s->video_codec_ctx->width, s->video_codec_ctx->height,
                                     s->video_codec_ctx->pix_fmt,
                                     s->video_codec_ctx->width, s->video_codec_ctx->height,
                                     s->display_pix_fmt,
                                     SWS_BICUBIC, NULL, NULL, NULL);

    numBytes = avpicture_get_size(s->display_pix_fmt, s->video_codec_ctx->width, s->video_codec_ctx->height);

    out_buffer_rgb = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
    avpicture_fill((AVPicture *)out_frame, out_buffer_rgb,
                   s->display_pix_fmt,
                   s->video_codec_ctx->width, s->video_codec_ctx->height);

    while(1) {
        if (s->quit) {
            break;
        }
        if (s->audio_stream_index == -1 && s->pause) {
            SDL_Delay(10);
			continue;
		}
        if (SDL_AUDIO_PAUSED == SDL_GetAudioStatus()) { //pause
            SDL_Delay(10);
            continue;
        }
        if (packet_queue_get(&s->video_packet_queue, packet, 0) <= 0) { //!block
            SDL_Delay(10); //no data
            continue;
        }

        //receive FLUSH data to flush codec, because of seeking
        if (strcmp((char*)packet->data, FLUSH_DATA) == 0) {
            avcodec_flush_buffers(s->video_stream->codec);
            av_packet_unref(packet);
            continue;
        }

        ret = avcodec_decode_video2(s->video_codec_ctx, frame, &got_picture, packet);

        if (ret < 0) {
            printf("decode error\n");
            av_packet_unref(packet);
            continue;
        }

//sync video and audio
        if (s->audio_stream_index != -1) {
            if (packet->dts == AV_NOPTS_VALUE
                    && frame->opaque
                    && *(uint64_t*)frame->opaque != AV_NOPTS_VALUE) {
                video_pts = *(uint64_t *)frame->opaque;
            } else if (packet->dts != AV_NOPTS_VALUE) {
                video_pts = packet->dts;
            } else {
                video_pts = 0;
            }

            video_pts *= av_q2d(s->video_stream->time_base);
            video_pts = get_frame_pts(s, frame, video_pts);

            while(1) {
                if (s->quit) {
                    break;
                }

                audio_pts = s->audio_clock;

                //in demuxer.cpp, we set video_clock 0 for seeking
                //so we should update video pts
                //if not when we seek backwards, it will running here forever
                video_pts = s->video_clock;
                if (video_pts <= audio_pts)
                    break;

                int delayTime = (video_pts - audio_pts) * 1000;
                delayTime = delayTime > 5 ? 5 : delayTime;
                SDL_Delay(delayTime);
            }
        } else {
            if (packet->pts != AV_NOPTS_VALUE) {
                s->video_clock = av_q2d(s->video_stream->time_base) * packet->pts;
            }

            SDL_Delay(1000 / av_q2d(s->video_stream->r_frame_rate));
        }
//sync end

        if (got_picture) {
            sws_scale(img_convert_ctx,
                      (uint8_t const * const *) frame->data,
                      frame->linesize, 0, s->video_codec_ctx->height,
                      out_frame->data, out_frame->linesize);
        }

        av_packet_unref(packet);
    }
    av_free(frame);
    av_free(out_frame);
    av_free(out_buffer_rgb);

    return 0;
}
