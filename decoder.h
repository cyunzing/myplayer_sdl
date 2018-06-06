#ifndef DECODER_H
#define DECODER_H

#include "mediastate.h"

int refresh_callback(void *);

int decode_and_show(MediaState *s);

int decode_callback(void *);

#endif // DECODER_H
