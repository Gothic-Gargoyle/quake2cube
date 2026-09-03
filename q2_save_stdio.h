#ifndef QUAKE2CUBE_SAVE_STDIO_H
#define QUAKE2CUBE_SAVE_STDIO_H

#include <stdio.h>
#include <stddef.h>

FILE *Q2_SaveFOpen(
    const char *path,
    const char *mode);

size_t Q2_SaveFRead(
    void *ptr,
    size_t size,
    size_t nmemb,
    FILE *file);

size_t Q2_SaveFWrite(
    const void *ptr,
    size_t size,
    size_t nmemb,
    FILE *file);

int Q2_SaveFClose(
    FILE *file);

int Q2_SaveRemove(
    const char *path);

char *Q2_SaveFindFirst(
    char *path,
    unsigned musthave,
    unsigned canthave);

char *Q2_SaveFindNext(
    unsigned musthave,
    unsigned canthave);

void Q2_SaveFindClose(void);

#ifdef Q2_GAMECUBE_SAVE_SHIM

#define fopen   Q2_SaveFOpen
#define fread   Q2_SaveFRead
#define fwrite  Q2_SaveFWrite
#define fclose  Q2_SaveFClose
#define remove  Q2_SaveRemove

#endif



#ifdef Q2_GAMECUBE_SAVE_SHIM

/*
 * Raw persistence plumbing for Q2CF.
 *
 * q2_save_stdio.c owns the CARD/transaction backend.
 * q2_config_gamecube.c owns engine policy and serialization.
 */
int Q2_ConfigStorageGet(
    void *buffer,
    size_t capacity,
    size_t *size);

int Q2_ConfigStoragePut(
    const void *buffer,
    size_t size);


void Q2_ConfigApplyPersisted(void);

void Q2_ConfigSaveIfChanged(void);

#endif


#endif
