#ifndef Q2_SAVE_SYMBOLS_GAMECUBE_H
#define Q2_SAVE_SYMBOLS_GAMECUBE_H

#include <stdint.h>

int Q2_SaveSymbolsBegin(void);
void Q2_SaveSymbolsEnd(void);

uint32_t Q2_SaveFunctionToId(
    const void *address
);

void *Q2_SaveFunctionFromId(
    uint32_t id
);

uint32_t Q2_SaveMMoveToId(
    const void *address
);

void *Q2_SaveMMoveFromId(
    uint32_t id
);

#endif
