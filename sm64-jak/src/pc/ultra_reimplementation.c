#include <stdio.h>
#include <string.h>
#include "lib/src/libultra_internal.h"
#include "macros.h"
#include "platform.h"
#include "fs/fs.h"

#ifdef TARGET_WEB
#include <emscripten.h>
#endif

extern OSMgrArgs piMgrArgs;

u64 osClockRate = 62500000;

s32 osPiStartDma(UNUSED OSIoMesg *mb, UNUSED s32 priority, UNUSED s32 direction,
                 uintptr_t devAddr, void *vAddr, size_t nbytes,
                 UNUSED OSMesgQueue *mq) {
    memcpy(vAddr, (const void *) devAddr, nbytes);
    return 0;
}

void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msgBuf, s32 count) {
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msgBuf;
    return;
}

void osSetEventMesg(UNUSED OSEvent e, UNUSED OSMesgQueue *mq, UNUSED OSMesg msg) {
}
s32 osJamMesg(UNUSED OSMesgQueue *mq, UNUSED OSMesg msg, UNUSED s32 flag) {
    return 0;
}
s32 osSendMesg(UNUSED OSMesgQueue *mq, UNUSED OSMesg msg, UNUSED s32 flag) {
#ifdef VERSION_EU
    s32 index;
    if (mq->validCount >= mq->msgCount) {
        return -1;
    }
    index = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[index] = msg;
    mq->validCount++;
#endif
    return 0;
}
s32 osRecvMesg(UNUSED OSMesgQueue *mq, UNUSED OSMesg *msg, UNUSED s32 flag) {
#ifdef VERSION_EU
    if (mq->validCount == 0) {
        return -1;
    }
    if (msg != NULL) {
        *msg = *(mq->first + mq->msg);
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
#endif
    return 0;
}

uintptr_t osVirtualToPhysical(void *addr) {
    return (uintptr_t) addr;
}

void osCreateViManager(UNUSED OSPri pri) {
}
void osViSetMode(UNUSED OSViMode *mode) {
}
void osViSetEvent(UNUSED OSMesgQueue *mq, UNUSED OSMesg msg, UNUSED u32 retraceCount) {
}
void osViBlack(UNUSED u8 active) {
}
void osViSetSpecialFeatures(UNUSED u32 func) {
}
void osViSwapBuffer(UNUSED void *vaddr) {
}

OSTime osGetTime(void) {
    return 0;
}

void osWritebackDCacheAll(void) {
}

void osWritebackDCache(UNUSED void *a, UNUSED size_t b) {
}

void osInvalDCache(UNUSED void *a, UNUSED size_t b) {
}

u32 osGetCount(void) {
    static u32 counter;
    return counter++;
}

s32 osAiSetFrequency(u32 freq) {
    u32 a1;
    s32 a2;
    u32 D_8033491C;

#ifdef VERSION_EU
    D_8033491C = 0x02E6025C;
#else
    D_8033491C = 0x02E6D354;
#endif

    a1 = D_8033491C / (float) freq + .5f;

    if (a1 < 0x84) {
        return -1;
    }

    a2 = (a1 / 66) & 0xff;
    if (a2 > 16) {
        a2 = 16;
    }

    return D_8033491C / (s32) a1;
}

s32 osEepromProbe(UNUSED OSMesgQueue *mq) {
    return 1;
}

#if defined(JAKOPENGOAL) && !defined(TARGET_WEB)
/* Async EEPROM save: the N64 EEPROM (512 bytes) is mirrored in memory and
 * flushed to disk on a background thread. The game reads/writes the mirror
 * instantly and never blocks the main loop on file I/O — critical when the
 * install lives on a OneDrive-synced path, where a save right after a star
 * grab could stall the main thread ~5s (OneDrive locks the file, sm64's
 * write_eeprom_data retries the fopen 4x). Writes are coalesced. */
#include <SDL2/SDL.h>

static u8 s_eeprom[512];
static bool s_eeprom_loaded = false;
static bool s_eeprom_dirty = false;
static char s_eeprom_path[SYS_MAX_PATH] = "";
static SDL_mutex *s_eeprom_mtx = NULL;
static SDL_sem *s_eeprom_wake = NULL;

static int eeprom_writer_thread(UNUSED void *arg) {
    for (;;) {
        SDL_SemWait(s_eeprom_wake);
        u8 snapshot[512];
        SDL_LockMutex(s_eeprom_mtx);
        memcpy(snapshot, s_eeprom, 512);
        s_eeprom_dirty = false;
        SDL_UnlockMutex(s_eeprom_mtx);
        /* This fopen may block for seconds under OneDrive contention —
         * but it's off the main thread, so the game keeps running. */
        FILE *fp = fopen(s_eeprom_path, "wb");
        if (fp != NULL) {
            fwrite(snapshot, 1, 512, fp);
            fclose(fp);
        }
    }
    return 0;
}

static void eeprom_ensure_init(void) {
    if (s_eeprom_loaded) {
        return;
    }
    /* Resolve the save path once; fs_writepath is fixed after startup. */
    snprintf(s_eeprom_path, sizeof(s_eeprom_path), "%s", fs_get_write_path(SAVE_FILENAME));
    memset(s_eeprom, 0, sizeof(s_eeprom));
    FILE *fp = fopen(s_eeprom_path, "rb");
    if (fp != NULL) {
        (void)fread(s_eeprom, 1, 512, fp);
        fclose(fp);
    }
    s_eeprom_mtx = SDL_CreateMutex();
    s_eeprom_wake = SDL_CreateSemaphore(0);
    SDL_Thread *t = SDL_CreateThread(eeprom_writer_thread, "jak-eeprom-save", NULL);
    if (t != NULL) {
        SDL_DetachThread(t);
    }
    s_eeprom_loaded = true;
}

s32 osEepromLongRead(UNUSED OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes) {
    eeprom_ensure_init();
    SDL_LockMutex(s_eeprom_mtx);
    memcpy(buffer, s_eeprom + address * 8, nbytes);
    SDL_UnlockMutex(s_eeprom_mtx);
    return 0;
}

s32 osEepromLongWrite(UNUSED OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes) {
    eeprom_ensure_init();
    bool post = false;
    SDL_LockMutex(s_eeprom_mtx);
    memcpy(s_eeprom + address * 8, buffer, nbytes);
    if (!s_eeprom_dirty) {   /* coalesce bursts into one flush */
        s_eeprom_dirty = true;
        post = true;
    }
    SDL_UnlockMutex(s_eeprom_mtx);
    if (post) {
        SDL_SemPost(s_eeprom_wake);
    }
    return 0;  /* instant — the flush happens on the writer thread */
}

#else  /* original synchronous EEPROM I/O */

s32 osEepromLongRead(UNUSED OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes) {
    u8 content[512];
    s32 ret = -1;

#ifdef TARGET_WEB
    if (EM_ASM_INT({
        var s = localStorage.sm64_save_file;
        if (s && s.length === 684) {
            try {
                var binary = atob(s);
                if (binary.length === 512) {
                    for (var i = 0; i < 512; i++) {
                        HEAPU8[$0 + i] = binary.charCodeAt(i);
                    }
                    return 1;
                }
            } catch (e) {
            }
        }
        return 0;
    }, content)) {
        memcpy(buffer, content + address * 8, nbytes);
        ret = 0;
    }
#else
    fs_file_t *fp = fs_open(SAVE_FILENAME);
    if (fp == NULL) {
        return -1;
    }
    if (fs_read(fp, content, 512) == 512) {
        memcpy(buffer, content + address * 8, nbytes);
        ret = 0;
    }
    fs_close(fp);
#endif
    return ret;
}

s32 osEepromLongWrite(UNUSED OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes) {
    u8 content[512] = {0};
    if (address != 0 || nbytes != 512) {
        osEepromLongRead(mq, 0, content, 512);
    }
    memcpy(content + address * 8, buffer, nbytes);

#ifdef TARGET_WEB
    EM_ASM({
        var str = "";
        for (var i = 0; i < 512; i++) {
            str += String.fromCharCode(HEAPU8[$0 + i]);
        }
        localStorage.sm64_save_file = btoa(str);
    }, content);
    s32 ret = 0;
#else
    FILE *fp = fopen(fs_get_write_path(SAVE_FILENAME), "wb");
    if (fp == NULL) {
        return -1;
    }
    s32 ret = fwrite(content, 1, 512, fp) == 512 ? 0 : -1;
    fclose(fp);
#endif
    return ret;
}

#endif  /* JAKOPENGOAL async EEPROM */
