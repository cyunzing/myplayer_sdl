#include "demuxer.h"

int demux_callback(void *userdata)
{
    MediaState *s = (MediaState *)userdata;
    if (!s)
        return -1;

    int ret = 0;
    AVPacket packet;
    while (1) { //quit? -> read? -> write
        if (s->quit) {
            break;
        }

        //end of the file and queue is empty
        if (ret < 0) {
            if (s->audio_stream_index != -1 && !s->audio_packet_queue.first_pkt)
                break;
            if (s->video_stream_index != -1 && !s->video_packet_queue.first_pkt)
                break;
        }

        //seek part
        if (s->seek_req) {
            int stream_index = av_find_default_stream_index(s->ic);
            if (stream_index >= 0) {
                s->seek_pos = av_rescale_q(s->seek_pos, AVRational{ 1, AV_TIME_BASE }, s->ic->streams[stream_index]->time_base);
            }

            if (av_seek_frame(s->ic, stream_index, s->seek_pos, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY) < 0) {
                  printf("%s: error while seeking\n", s->ic->filename);
            } else {
                //avpacket for video or audio
                AVPacket packet;
                av_new_packet(&packet, 10);
                strcpy((char *)packet.data, FLUSH_DATA);

                if (s->audio_stream_index >= 0) { //audio
                    packet_queue_flush(&s->audio_packet_queue); //flush queue
                     //push FLUSH pkt in queue
                    packet_queue_put(&s->audio_packet_queue, &packet);
                }
                if (s->video_stream_index >= 0) { //video
                    packet_queue_flush(&s->video_packet_queue); //flush queue
                    //push FLUSH pkt in queue
                    packet_queue_put(&s->video_packet_queue, &packet);
                    s->video_clock = 0;
                }
            }
            s->seek_req = 0;
			s->seek_pos = 0;
        }

        //read but not all
        if (s->audio_packet_queue.size > MAX_AUDIO_SIZE || s->video_packet_queue.size > MAX_VIDEO_SIZE) {
            SDL_Delay(100);
            continue;
        }

        //read frame
        ret = av_read_frame(s->ic, &packet);
        if(ret > -1) { //read a frame, push into queue
            if(packet.stream_index == s->video_stream_index)
                packet_queue_put(&s->video_packet_queue, &packet);
            else if (packet.stream_index == s->audio_stream_index)
                packet_queue_put(&s->audio_packet_queue, &packet);
            else
                av_packet_unref(&packet);

            s->is_buffering = 1;
        }
    }

    //quit
    s->quit = 1;

    return 0;
}
