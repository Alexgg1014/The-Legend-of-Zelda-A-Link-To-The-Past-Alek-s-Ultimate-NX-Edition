/*
 * aleks_ra_badge.c -- achievement badge fetch/cache worker.
 *
 * Ported from the final ALEKS TMC (platforms/switch/switch_ra_badge.c).  Kept
 * from the donor verbatim in spirit: single in-flight job, disk cache under
 * ra_badges/, HTTPS-only, bounded download, PNG signature checked before the
 * bytes are trusted, atomic cache write.
 *
 * TWO CHANGES, both because this port's toast draws on the game thread:
 *   - the worker DECODES the PNG (stb_image) so the draw never does;
 *   - the thread helper is local instead of pulling in the donor's parallel
 *     module, which carries extraction machinery this build has no use for.
 */
#include "../../aleks_ra_badge.h"

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION_ALREADY_PROVIDED 1
#include "../../third_party/stb/stb_image.h"

extern long Port_Net_HttpGetBinaryBounded(const char *url, size_t max_bytes,
                                          unsigned char **out_body, size_t *out_len);

#define RA_BADGE_MAX_BYTES (256u * 1024u)
#define RA_BADGE_DIR "ra_badges"

typedef struct {
  char name[8];
  char url[512];
  void *pixels;          /* decoded RGBA8, stb_image-owned */
  int w, h;
  int result;            /* 1 ready, <0 failure */
} BadgeJob;

static BadgeJob sJob;

/* ---- minimal background thread ----------------------------------------- */
typedef struct { Thread t; volatile int done; int started; } BgTask;
static BgTask sBg;

static void bg_entry(void *arg) {
  void (*fn)(void) = (void (*)(void))arg;
  fn();
  sBg.done = 1;
}

static void badge_worker(void);

static int bg_start(void) {
  sBg.done = 0;
  /* 128 KB is ample: curl plus one PNG decode.  Low priority (0x3B) so the
   * game thread and the OS always win -- the donor's rule. */
  if (R_FAILED(threadCreate(&sBg.t, bg_entry, (void *)badge_worker, NULL,
                            128 * 1024, 0x3B, -2)))
    return 0;
  if (R_FAILED(threadStart(&sBg.t))) { threadClose(&sBg.t); return 0; }
  sBg.started = 1;
  return 1;
}

static void bg_join(void) {
  if (!sBg.started) return;
  threadWaitForExit(&sBg.t);
  threadClose(&sBg.t);
  sBg.started = 0;
  sBg.done = 0;
}

/* ---- helpers (donor) ---------------------------------------------------- */
static int safe_name(const char *in, char out[8]) {
  size_t n = 0;
  if (!in) return 0;
  while (in[n] && n < 7) {
    unsigned char c = (unsigned char)in[n];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    out[n] = (char)c;
    n++;
  }
  out[n] = 0;
  return n > 0 && in[n] == 0;
}

static int is_png(const unsigned char *data, size_t size) {
  static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
  return data && size >= sizeof sig && memcmp(data, sig, sizeof sig) == 0;
}

static unsigned char *read_cache(const char *path, size_t *out_size) {
  FILE *f = fopen(path, "rb");
  long len;
  unsigned char *data;
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  len = ftell(f);
  if (len <= 0 || (unsigned long)len > RA_BADGE_MAX_BYTES ||
      fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  data = (unsigned char *)malloc((size_t)len);
  if (!data || fread(data, 1, (size_t)len, f) != (size_t)len ||
      !is_png(data, (size_t)len)) { free(data); fclose(f); return NULL; }
  fclose(f);
  *out_size = (size_t)len;
  return data;
}

static void write_cache(const char *path, const unsigned char *data, size_t size) {
  char temp[64];
  FILE *f;
  int wrote, closed;
  snprintf(temp, sizeof temp, "%s.tmp", path);
  f = fopen(temp, "wb");
  if (!f) return;
  wrote = fwrite(data, 1, size, f) == size;
  closed = fclose(f) == 0;
  if (wrote && closed) { remove(path); rename(temp, path); }
  else remove(temp);
}

/* Decode into the job.  Frees the PNG bytes either way. */
static void decode_into_job(unsigned char *png, size_t size) {
  int w = 0, h = 0, comp = 0;
  stbi_uc *px = stbi_load_from_memory(png, (int)size, &w, &h, &comp, 4);
  free(png);
  if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); sJob.result = -4; return; }
  sJob.pixels = px;
  sJob.w = w;
  sJob.h = h;
  sJob.result = 1;
}

static void badge_worker(void) {
  char path[64];
  unsigned char *data = NULL;
  size_t size = 0;
  long status;

  snprintf(path, sizeof path, RA_BADGE_DIR "/%s.png", sJob.name);
  data = read_cache(path, &size);
  if (data) { decode_into_job(data, size); return; }

  if (strncmp(sJob.url, "https://", 8) != 0) { sJob.result = -1; return; }
  status = Port_Net_HttpGetBinaryBounded(sJob.url, RA_BADGE_MAX_BYTES, &data, &size);
  if (status < 200 || status >= 300 || !is_png(data, size)) {
    free(data);
    sJob.result = -2;
    return;
  }
  mkdir(RA_BADGE_DIR, 0777);
  write_cache(path, data, size);
  decode_into_job(data, size);
}

/* ---- public ------------------------------------------------------------- */
void AleksRA_Badge_Request(const char *badge_name, const char *url) {
  if (sBg.started) return;                       /* one in flight at a time */
  if (!safe_name(badge_name, sJob.name)) return;
  if (!url || strlen(url) >= sizeof sJob.url) return;
  snprintf(sJob.url, sizeof sJob.url, "%s", url);
  sJob.pixels = NULL;
  sJob.w = sJob.h = 0;
  sJob.result = 0;
  if (!bg_start()) sJob.result = -3;
}

int AleksRA_Badge_TakeReady(const char **badge_name, void **pixels,
                            int *w, int *h) {
  if (pixels) *pixels = NULL;
  if (!sBg.started || !sBg.done) return 0;
  bg_join();
  if (sJob.result != 1 || !sJob.pixels) {
    if (sJob.pixels) { stbi_image_free(sJob.pixels); sJob.pixels = NULL; }
    return 0;
  }
  if (badge_name) *badge_name = sJob.name;
  if (pixels) *pixels = sJob.pixels;
  if (w) *w = sJob.w;
  if (h) *h = sJob.h;
  sJob.pixels = NULL;                            /* ownership moves out */
  return 1;
}

void AleksRA_Badge_FreeReady(void *pixels) {
  if (pixels) stbi_image_free(pixels);
}

void AleksRA_Badge_Shutdown(void) {
  bg_join();
  if (sJob.pixels) { stbi_image_free(sJob.pixels); sJob.pixels = NULL; }
}
