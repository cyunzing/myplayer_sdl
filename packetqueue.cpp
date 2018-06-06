#include "packetqueue.h"

// queue init
void packet_queue_init(PacketQueue *q)
{
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
#if USE_MUTE
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
#endif
}

void packet_queue_flush(PacketQueue *q)
{
#if USE_MUTE
    SDL_LockMutex(q->mutex);
#endif
    AVPacketList *pkt = NULL, *pkt1 = NULL;

    for(pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        if(pkt1->pkt.data != (uint8_t *)"FLUSH") {

        }
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }

    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
#if USE_MUTE
    SDL_UnlockMutex(q->mutex);
#endif
}

// push packet into queue
int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pktl;

    if (av_dup_packet(pkt) < 0)
        return -1;

    pktl = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if (!pktl)
        return -1;

    pktl->pkt = *pkt;
    pktl->next = NULL;
#if USE_MUTE
    SDL_LockMutex(q->mutex);
#endif
    if (!q->last_pkt) // if the queue is empty, the new one will be the first
        q->first_pkt = pktl;
    else // or else push into rear
        q->last_pkt->next = pktl;

    q->last_pkt = pktl;
    q->nb_packets++;
    q->size += pkt->size;
#if USE_MUTE
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
#endif
    return 0;
}

// pop up packet from queue
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1 = NULL;
    int ret;
#if USE_MUTE
    SDL_LockMutex(q->mutex);
#endif
    for (;;) {
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt) {
                q->last_pkt = NULL;
            }
            q->nb_packets--;
            q->size -= pkt1->pkt.size;

            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
#if USE_MUTE
            SDL_CondWait(q->cond, q->mutex);
#endif
        }
    }
#if USE_MUTE
    SDL_UnlockMutex(q->mutex);
#endif
    return ret;
}
