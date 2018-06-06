#ifndef PACKETQUEUE_H
#define PACKETQUEUE_H

#define FLUSH_DATA "FLUSH"

#define USE_MUTE 1
#define UNUSED (void *)

#ifdef __cplusplus
extern "C"{
#endif

#include <libavformat/avformat.h>
#include <SDL2/SDL.h>

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

void packet_queue_init(PacketQueue *q);

void packet_queue_flush(PacketQueue *q);

int packet_queue_put(PacketQueue *q, AVPacket *pkt);

int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);

#ifdef __cplusplus
}
#endif

#endif // PACKETQUEUE_H
