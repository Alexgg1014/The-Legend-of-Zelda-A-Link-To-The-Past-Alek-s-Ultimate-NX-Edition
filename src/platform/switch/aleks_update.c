/* aleks_update.c -- in-game updater. See aleks_update.h for the contract.
 *
 * Own TU because <switch.h>, <curl/curl.h> and <mbedtls/sha256.h> bring
 * typedefs that clash with the engine headers (same reason aleks_net.c and
 * aleks_ra_badge.c are separate).
 *
 * Lineage: TMC port_update_common.c + port_update_check.c (Switch section),
 * carried over with the file names changed and one structural difference:
 * the canonical NRO is the launch path hbloader hands us in argv[0], written
 * into the journal, instead of a compile-time constant. Everything the TMC
 * code learned the hard way is kept -- read the comments on JsonChangelog and
 * install_worker before "simplifying" either. */
#include <switch.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/sha256.h>

#include "aleks_update.h"
#include "aleks_update_manifest.h"
#include "aleks_version.h"

/* ---- fixed local names --------------------------------------------------
 * Nothing below is ever derived from the manifest. The only path that is not
 * a literal is the launched NRO itself, and that comes from the loader. */
#define Z3_UPDATE_ROOT       "sdmc:/switch/Zelda3/"
#define Z3_UPDATE_DIR        Z3_UPDATE_ROOT "update/"
#define Z3_UPDATE_STAGED_NRO Z3_UPDATE_DIR "Zelda3-ALEKS-NX.new.nro"
#define Z3_UPDATE_MANIFEST   Z3_UPDATE_DIR "update_manifest.json"
#define Z3_UPDATE_JOURNAL    Z3_UPDATE_DIR "update_state.dat"
#define Z3_UPDATE_LOG        Z3_UPDATE_DIR "update.log"

/* Project-controlled metadata on the release repo's default branch. Not a
 * GitHub release scrape: the closed schema pins size and SHA-256 before a
 * single NRO byte is fetched. */
#define Z3_UPDATE_MANIFEST_URL "https://raw.githubusercontent.com/Alexgg1014/" \
  "The-Legend-of-Zelda-A-Link-To-The-Past-Alek-s-Ultimate-NX-Edition/main/update_manifest.json"

#define Z3_UPDATE_PATH_MAX      256u

/* ---- transport (aleks_net.c) ------------------------------------------ */
extern void Port_Net_Init(void);
extern void Port_Net_Exit(void);
extern long Port_Net_HttpGetBinaryBounded(const char *url, size_t max_bytes,
                                          unsigned char **out_body, size_t *out_len);
extern long Port_Net_HttpDownloadFile(const char *url, const char *dest, size_t expected_bytes,
                                      volatile unsigned *progress_permille);
/* main.c */
extern void StartupLog(const char *fmt, ...);

/* ---- types ------------------------------------------------------------- */
typedef enum {
  JOURNAL_IDLE = 0,
  JOURNAL_DOWNLOADED = 1,
  JOURNAL_INSTALLING = 2,
  JOURNAL_INSTALLED = 3,
  JOURNAL_ROLLED_BACK = 4,
} JournalState;

typedef struct {
  uint32_t schema;
  uint32_t state;
  uint64_t targetSize;
  uint64_t backupSize;
  char targetVersion[32];
  char targetSha256[65];
  char backupSha256[65];
  /* The NRO being replaced and its .bak sibling. Local, loader-derived,
   * checksummed with the rest so a corrupt journal can never point the
   * installer somewhere else. */
  char targetPath[Z3_UPDATE_PATH_MAX];
  char backupPath[Z3_UPDATE_PATH_MAX];
  uint32_t checksum;
} Journal;


/* ---- state ------------------------------------------------------------- */
static volatile int sStatus;              /* AleksUpdateStatus */
static char sLaunchPath[Z3_UPDATE_PATH_MAX];
static char sBackupPath[Z3_UPDATE_PATH_MAX];
static bool sLaunchPathOk;
static char sLatest[32];
static AleksManifest sManifest;
static volatile unsigned sProgressPermille;
static char sError[128];
static bool sStarted, sNetHeld;

typedef struct { Thread t; volatile int done; bool started; } BgTask;
static BgTask sBg;

/* ---- logging ----------------------------------------------------------- */
/* Update state is rare and safety-critical, so it always gets a real SD log,
 * release builds included -- this is what a "the update broke my game"
 * report has to come with. */
static void ulog(const char *fmt, ...) {
  FILE *f = fopen(Z3_UPDATE_LOG, "a");
  va_list ap;
  if (!f) return;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fflush(f);
  fclose(f);
}

static void SetError(const char *text) {
  snprintf(sError, sizeof sError, "%s", text ? text : "Update failed");
  sStatus = ALEKS_UPDATE_FAILED;
  ulog("[UPDATE] failure: %s\n", sError);
}

/* ---- durable SD writes ------------------------------------------------- */
/* libnx buffers through the FS service: a file written seconds before an
 * abnormal exit is not guaranteed to have reached the card. Every write that
 * the next boot depends on ends with a commit. */
static void CommitSd(void) { fsdevCommitDevice("sdmc"); }

static int FlushClose(FILE *f) {
  int ok = f && fflush(f) == 0;
  int fd = f ? fileno(f) : -1;
  if (ok && fd >= 0) (void)fsync(fd);
  if (!f || fclose(f) != 0) ok = 0;
  if (ok) CommitSd();
  return ok;
}

static int EnsureDirectories(void) {
  if (mkdir(Z3_UPDATE_ROOT, 0777) != 0 && access(Z3_UPDATE_ROOT, F_OK) != 0) return 0;
  return mkdir(Z3_UPDATE_DIR, 0777) == 0 || access(Z3_UPDATE_DIR, F_OK) == 0;
}

static int WriteDurable(const char *path, const void *data, size_t size) {
  FILE *f;
  if (!path || (!data && size)) return 0;
  f = fopen(path, "wb");
  if (!f) return 0;
  if (size && fwrite(data, 1, size, f) != size) { fclose(f); return 0; }
  return FlushClose(f);
}

static int CopyDurable(const char *from, const char *to) {
  static unsigned char buffer[64 * 1024];  /* workers are serialised */
  FILE *in, *out; size_t n; int ok = 1;
  if (!from || !to || !strcmp(from, to)) return 0;
  in = fopen(from, "rb"); if (!in) return 0;
  out = fopen(to, "wb"); if (!out) { fclose(in); return 0; }
  while ((n = fread(buffer, 1, sizeof buffer, in)) != 0)
    if (fwrite(buffer, 1, n, out) != n) { ok = 0; break; }
  if (ferror(in)) ok = 0;
  fclose(in);
  return FlushClose(out) && ok;
}

static int FileSize(const char *path, uint64_t *outSize) {
  struct stat st;
  if (!path || stat(path, &st) != 0 || st.st_size < 0) return 0;
  if (outSize) *outSize = (uint64_t)st.st_size;
  return 1;
}

static int FileSha256(const char *path, char outHex[65]) {
  static const char hex[] = "0123456789abcdef";
  static unsigned char buf[64 * 1024];
  unsigned char digest[32];
  mbedtls_sha256_context ctx; FILE *f; size_t n; int ok = 1;
  if (!path || !outHex) return 0;
  f = fopen(path, "rb"); if (!f) return 0;
  mbedtls_sha256_init(&ctx);
  if (mbedtls_sha256_starts_ret(&ctx, 0) != 0) ok = 0;
  while (ok && (n = fread(buf, 1, sizeof buf, f)) != 0)
    if (mbedtls_sha256_update_ret(&ctx, buf, n) != 0) ok = 0;
  if (ferror(f) || (ok && mbedtls_sha256_finish_ret(&ctx, digest) != 0)) ok = 0;
  mbedtls_sha256_free(&ctx); fclose(f);
  if (!ok) return 0;
  for (unsigned i = 0; i < sizeof digest; ++i) {
    outHex[i * 2] = hex[digest[i] >> 4];
    outHex[i * 2 + 1] = hex[digest[i] & 15];
  }
  outHex[64] = 0;
  return 1;
}

/* A real NRO of exactly the promised size and hash: NRO0 magic at 0x10 and
 * a sane header size field, then the full SHA-256. */
static int LooksLikeNro(const char *path) {
  unsigned char header[0x20]; FILE *f; uint64_t size; uint32_t nroSize;
  if (!FileSize(path, &size) || size < sizeof header) return 0;
  f = fopen(path, "rb"); if (!f) return 0;
  if (fread(header, 1, sizeof header, f) != sizeof header) { fclose(f); return 0; }
  fclose(f);
  if (memcmp(header + 0x10, "NRO0", 4) != 0) return 0;
  nroSize = (uint32_t)header[0x18] | ((uint32_t)header[0x19] << 8) |
            ((uint32_t)header[0x1a] << 16) | ((uint32_t)header[0x1b] << 24);
  return nroSize >= 0x80u && (uint64_t)nroSize <= size;
}

static int VerifyNro(const char *path, uint64_t expectedSize, const char expectedSha256[65]) {
  char actual[65]; uint64_t size;
  if (!expectedSha256 || !FileSize(path, &size) || size != expectedSize) return 0;
  if (!LooksLikeNro(path)) return 0;
  return FileSha256(path, actual) && !strcmp(actual, expectedSha256);
}

/* ---- journal ----------------------------------------------------------- */
static uint32_t Hash32(uint32_t hash, const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  while (len--) hash = (hash ^ *p++) * 16777619u;
  return hash;
}

static uint32_t JournalChecksum(const Journal *j) {
  Journal copy = *j; copy.checksum = 0;
  return Hash32(2166136261u, &copy, sizeof copy);
}

static void InitJournal(Journal *j, const AleksManifest *m, JournalState state) {
  memset(j, 0, sizeof *j);
  j->schema = Z3_UPDATE_SCHEMA;
  j->state = (uint32_t)state;
  if (m) {
    j->targetSize = m->nroSize;
    snprintf(j->targetVersion, sizeof j->targetVersion, "%s", m->version);
    snprintf(j->targetSha256, sizeof j->targetSha256, "%s", m->nroSha256);
  }
  snprintf(j->targetPath, sizeof j->targetPath, "%s", sLaunchPath);
  snprintf(j->backupPath, sizeof j->backupPath, "%s", sBackupPath);
}

static int WriteJournal(const Journal *j) {
  Journal copy;
  if (!j || !EnsureDirectories()) return 0;
  copy = *j; copy.checksum = JournalChecksum(&copy);
  return WriteDurable(Z3_UPDATE_JOURNAL, &copy, sizeof copy);
}

static int IsSdNroPath(const char *p);

static int ReadJournal(Journal *out) {
  FILE *f; size_t n; int extra;
  if (!out) return 0;
  f = fopen(Z3_UPDATE_JOURNAL, "rb");
  if (!f) return 0;
  n = fread(out, 1, sizeof *out, f); extra = fgetc(f); fclose(f);
  return n == sizeof *out && extra == EOF && out->schema == Z3_UPDATE_SCHEMA &&
         out->checksum == JournalChecksum(out) && out->state <= JOURNAL_ROLLED_BACK &&
         out->targetSize >= 4096 && out->targetSize <= Z3_UPDATE_NRO_MAX &&
         memchr(out->targetSha256, '\0', sizeof out->targetSha256) != NULL &&
         strlen(out->targetSha256) == 64 &&
         memchr(out->targetVersion, '\0', sizeof out->targetVersion) != NULL &&
         memchr(out->targetPath, '\0', sizeof out->targetPath) != NULL &&
         memchr(out->backupPath, '\0', sizeof out->backupPath) != NULL &&
         IsSdNroPath(out->targetPath) && out->backupPath[0] != 0;
}

/* ---- launch path ------------------------------------------------------- */
static int EndsWithNoCase(const char *s, const char *suffix) {
  size_t n = strlen(s), m = strlen(suffix);
  if (n < m) return 0;
  for (size_t i = 0; i < m; ++i)
    if (tolower((unsigned char)s[n - m + i]) != tolower((unsigned char)suffix[i])) return 0;
  return 1;
}

/* An absolute SD path to a .nro. hbloader passes "sdmc:/..."; a launcher
 * that strips the device gives "/...", which the devoptab resolves to the
 * same card. Anything else (romfs:, relative, no extension) is refused. */
static int IsSdNroPath(const char *p) {
  size_t n = p ? strlen(p) : 0;
  if (n < 6 || n >= Z3_UPDATE_PATH_MAX) return 0;
  if (strncmp(p, "sdmc:/", 6) != 0 && p[0] != '/') return 0;
  return EndsWithNoCase(p, ".nro");
}

#define Z3_UPDATE_CANONICAL_NRO Z3_UPDATE_ROOT "Zelda3-ALEKS-NX.nro"

static char sArgv0[Z3_UPDATE_PATH_MAX];

void AleksUpdate_SetLaunchPath(const char *argv0) {
  sLaunchPathOk = false;
  sLaunchPath[0] = sBackupPath[0] = 0;
  snprintf(sArgv0, sizeof sArgv0, "%s", argv0 ? argv0 : "(null)");
  if (!IsSdNroPath(argv0)) {
    /* No usable loader path (an emulator hands us a host path, or nothing).
     * Fall back to the agreed canonical name if -- and only if -- that file
     * exists.  Same fixed-path model as TMC; still never a remote value. */
    if (access(Z3_UPDATE_CANONICAL_NRO, F_OK) != 0) return;
    argv0 = Z3_UPDATE_CANONICAL_NRO;
  }
  snprintf(sLaunchPath, sizeof sLaunchPath, "%s", argv0);
  /* "x.nro" -> "x.bak", next to it. */
  snprintf(sBackupPath, sizeof sBackupPath, "%s", argv0);
  memcpy(sBackupPath + strlen(sBackupPath) - 4, ".bak", 4);
  sLaunchPathOk = true;
}

/* ---- background runner ------------------------------------------------- */
static void bg_entry(void *arg) {
  void (*fn)(void) = (void (*)(void))arg;
  fn();
  sBg.done = 1;
}

static int bg_start(void (*fn)(void)) {
  sBg.done = 0;
  /* 256 KB: curl over the libnx ssl service plus a 64 KB hashing buffer in
   * static storage. Low priority so the game thread always wins. */
  if (R_FAILED(threadCreate(&sBg.t, bg_entry, (void *)fn, NULL, 256 * 1024, 0x3B, -2)))
    return 0;
  if (R_FAILED(threadStart(&sBg.t))) { threadClose(&sBg.t); return 0; }
  sBg.started = true;
  return 1;
}

static bool bg_busy(void) { return sBg.started && !sBg.done; }

static void bg_join(void) {
  if (!sBg.started) return;
  threadWaitForExit(&sBg.t);
  threadClose(&sBg.t);
  sBg.started = false;
  sBg.done = 0;
}

/* ---- romfs release ----------------------------------------------------- *
 * romfs is a 14 KB .bps read once during first-run asset extraction, before
 * the main loop -- but while it is mounted libnx keeps the NRO file open.
 * Unmount before overwriting the file. Idempotent so main()'s own exit-time
 * call is harmless after an install. */
static bool sRomfsReleased;
void AleksSwitch_RomfsRelease(void) {
  if (sRomfsReleased) return;
  sRomfsReleased = true;
  romfsExit();
}

/* ---- workers ----------------------------------------------------------- */
static void check_worker(void) {
  unsigned char *body = NULL; size_t len = 0; long status; AleksManifest m;
  ulog("[UPDATE] manifest request url=%s\n", Z3_UPDATE_MANIFEST_URL);
  status = Port_Net_HttpGetBinaryBounded(Z3_UPDATE_MANIFEST_URL, Z3_UPDATE_MANIFEST_MAX, &body, &len);
  ulog("[UPDATE] manifest http=%ld bytes=%u\n", status, (unsigned)len);
  if (status < 200 || status >= 300 || !body) {
    /* Offline is silent to the player: the game is wholly usable and the
     * UPDATE page just offers CHECK AGAIN. */
    if (body) free(body);
    sStatus = ALEKS_UPDATE_UNKNOWN;
    return;
  }
  if (!AleksManifest_Parse((const char *)body, len, &m)) {
    free(body);
    SetError("Update manifest rejected");
    return;
  }
  (void)EnsureDirectories();
  (void)WriteDurable(Z3_UPDATE_MANIFEST, body, len);
  free(body);
  sManifest = m;
  snprintf(sLatest, sizeof sLatest, "%s", m.version);
  if (AleksManifest_IsNewerVersion(m.version, ALEKS_NX_VERSION)) {
    sStatus = ALEKS_UPDATE_AVAILABLE;
    ulog("[UPDATE] local=%s latest=%s -> AVAILABLE\n", ALEKS_NX_VERSION, m.version);
  } else {
    sStatus = ALEKS_UPDATE_UP_TO_DATE;
    ulog("[UPDATE] local=%s latest=%s -> up to date\n", ALEKS_NX_VERSION, m.version);
  }
}

static void download_worker(void) {
  long status; Journal j;
  if (!EnsureDirectories()) { SetError("Cannot create update directory"); return; }
  (void)remove(Z3_UPDATE_STAGED_NRO);  /* fixed staging path, never the launched NRO */
  sStatus = ALEKS_UPDATE_DOWNLOADING;
  sProgressPermille = 0;
  ulog("[UPDATE] download begin version=%s size=%llu url=%s\n", sManifest.version,
       (unsigned long long)sManifest.nroSize, sManifest.nroUrl);
  status = Port_Net_HttpDownloadFile(sManifest.nroUrl, Z3_UPDATE_STAGED_NRO,
                                     (size_t)sManifest.nroSize, &sProgressPermille);
  ulog("[UPDATE] download http=%ld\n", status);
  if (status < 200 || status >= 300) { SetError("Download failed"); return; }
  CommitSd();  /* durability is a separate boundary from libcurl's close */
  sStatus = ALEKS_UPDATE_VERIFYING;
  if (!VerifyNro(Z3_UPDATE_STAGED_NRO, sManifest.nroSize, sManifest.nroSha256)) {
    SetError("Downloaded NRO failed verification"); return;
  }
  InitJournal(&j, &sManifest, JOURNAL_DOWNLOADED);
  if (!WriteJournal(&j)) { SetError("Cannot persist update journal"); return; }
  sStatus = ALEKS_UPDATE_READY;
  sProgressPermille = 1000;
  ulog("[UPDATE] staged file verified and journalled for %s\n", j.targetPath);
}

static int restore_backup(const Journal *j) {
  if (!VerifyNro(j->backupPath, j->backupSize, j->backupSha256) ||
      !CopyDurable(j->backupPath, j->targetPath) ||
      !VerifyNro(j->targetPath, j->backupSize, j->backupSha256)) {
    ulog("[UPDATE] rollback FAILED\n");
    return 0;
  }
  ulog("[UPDATE] rollback restored verified backup\n");
  return 1;
}

/* In-process install.
 *
 * Overwriting the running NRO is safe: hbloader reads it into memory in full
 * at launch, and once romfs is unmounted nothing holds the file open. The
 * bytes on the card are not backing anything live.
 *
 * Order, unchanged from the TMC helper: nothing overwrites the launched NRO
 * until a verified backup exists, and any failure restores it. The hashing
 * and copying of a ~9 MB file several times over the SD takes seconds, which
 * is why this is on the background runner and not the frame loop. */
static void install_worker(void) {
  Journal j; uint64_t currentSize = 0; char currentHash[65];
  sStatus = ALEKS_UPDATE_INSTALLING;
  if (!ReadJournal(&j) || j.state != JOURNAL_DOWNLOADED) {
    SetError("Update journal missing or not staged"); return;
  }
  if (strcmp(j.targetPath, sLaunchPath) != 0) {
    /* Staged for a different NRO than the one running now. Refuse rather
     * than guess; the journal is discarded so the next check starts clean. */
    (void)remove(Z3_UPDATE_JOURNAL);
    SetError("Staged update is for another NRO"); return;
  }
  if (!VerifyNro(Z3_UPDATE_STAGED_NRO, j.targetSize, j.targetSha256)) {
    SetError("Staged update failed verification"); return;
  }
  if (!LooksLikeNro(j.targetPath) ||
      !FileSize(j.targetPath, &currentSize) || !FileSha256(j.targetPath, currentHash)) {
    SetError("Current game NRO missing or invalid"); return;
  }
  /* Only now is an old .bak eligible for overwrite. */
  if (!CopyDurable(j.targetPath, j.backupPath) ||
      !VerifyNro(j.backupPath, currentSize, currentHash)) {
    SetError("Could not create verified backup"); return;
  }
  j.backupSize = currentSize;
  snprintf(j.backupSha256, sizeof j.backupSha256, "%s", currentHash);
  j.state = JOURNAL_INSTALLING;
  if (!WriteJournal(&j)) { SetError("Could not persist install journal"); return; }

  AleksSwitch_RomfsRelease();
  if (!CopyDurable(Z3_UPDATE_STAGED_NRO, j.targetPath) ||
      !VerifyNro(j.targetPath, j.targetSize, j.targetSha256)) {
    ulog("[UPDATE] install copy or verification failed; attempting rollback\n");
    if (restore_backup(&j)) {
      j.state = JOURNAL_ROLLED_BACK;
      (void)WriteJournal(&j);
      SetError("Install failed; previous version restored");
    } else {
      SetError("Install failed; copy the .bak over the .nro by hand");
    }
    return;
  }
  j.state = JOURNAL_INSTALLED;
  if (!WriteJournal(&j))
    ulog("[UPDATE] installed target valid but final journal write failed\n");
  (void)remove(Z3_UPDATE_STAGED_NRO);
  ulog("[UPDATE] install complete version=%s path=%s\n", j.targetVersion, j.targetPath);
  sStatus = ALEKS_UPDATE_INSTALLED_RESTART;
}

/* ---- boot-time journal recovery --------------------------------------- *
 * A power cut mid-install leaves INSTALLING in the journal. On the next boot
 * the launched NRO is either the new one (hbloader loaded it, so it was
 * complete) or the old one (the copy never finished and the .bak is intact).
 * Either way, finish the bookkeeping so the UPDATE page tells the truth. */
static void RecoverJournal(void) {
  Journal j;
  if (!ReadJournal(&j)) return;
  if (j.state == JOURNAL_INSTALLING && strcmp(j.targetPath, sLaunchPath) == 0) {
    if (VerifyNro(j.targetPath, j.targetSize, j.targetSha256)) {
      j.state = JOURNAL_INSTALLED;
      (void)WriteJournal(&j);
      ulog("[UPDATE] recovery: target already installed and verified\n");
    } else if (restore_backup(&j)) {
      j.state = JOURNAL_ROLLED_BACK;
      (void)WriteJournal(&j);
      ulog("[UPDATE] recovery: interrupted install rolled back\n");
    } else {
      ulog("[UPDATE] recovery: could not verify or restore; leaving journal\n");
    }
  }
}

/* ---- public API -------------------------------------------------------- */
void AleksUpdate_Start(void) {
  Journal j;
  if (sStarted) return;   /* once per process: a game reset must not re-query */
  sStarted = true;
  sLatest[0] = sError[0] = 0;
  sProgressPermille = 0;
  (void)EnsureDirectories();
  StartupLog("UPDATE: argv0='%s'", sArgv0);
  if (!sLaunchPathOk) {
    sStatus = ALEKS_UPDATE_UNAVAILABLE;
    StartupLog("UPDATE: launch path unusable and no %s; in-game update disabled",
               Z3_UPDATE_CANONICAL_NRO);
    ulog("[UPDATE] argv0='%s' unusable, no canonical NRO; disabled\n", sArgv0);
    return;
  }
  StartupLog("UPDATE: local %s, target %s", ALEKS_NX_VERSION, sLaunchPath);
  RecoverJournal();
  /* A verified staging journal survives a relaunch: do not download again
   * or discard it just because GitHub is unreachable right now. */
  if (ReadJournal(&j) && j.state == JOURNAL_DOWNLOADED &&
      strcmp(j.targetPath, sLaunchPath) == 0 &&
      VerifyNro(Z3_UPDATE_STAGED_NRO, j.targetSize, j.targetSha256)) {
    snprintf(sLatest, sizeof sLatest, "%s", j.targetVersion);
    sStatus = ALEKS_UPDATE_READY;
    ulog("[UPDATE] found verified staged update %s at startup\n", sLatest);
    return;
  }
  Port_Net_Init();
  sNetHeld = true;
  sStatus = ALEKS_UPDATE_CHECKING;
  if (!bg_start(check_worker)) {
    sStatus = ALEKS_UPDATE_UNKNOWN;
    ulog("[UPDATE] could not spawn worker -> UNKNOWN\n");
  }
}

void AleksUpdate_Shutdown(void) {
  bg_join();
  if (sNetHeld) { Port_Net_Exit(); sNetHeld = false; }
}

int AleksUpdate_RequestCheck(void) {
  if (sStatus == ALEKS_UPDATE_READY || sStatus == ALEKS_UPDATE_UNAVAILABLE ||
      sStatus == ALEKS_UPDATE_INSTALLED_RESTART || bg_busy())
    return 0;
  bg_join();
  if (!sNetHeld) { Port_Net_Init(); sNetHeld = true; }
  sStatus = ALEKS_UPDATE_CHECKING;
  sLatest[0] = sError[0] = 0;
  sProgressPermille = 0;
  ulog("[UPDATE] manual check\n");
  if (!bg_start(check_worker)) { sStatus = ALEKS_UPDATE_UNKNOWN; return 0; }
  return 1;
}

int AleksUpdate_RequestDownload(void) {
  if ((sStatus != ALEKS_UPDATE_AVAILABLE && sStatus != ALEKS_UPDATE_FAILED) ||
      sManifest.nroSize == 0 || !AleksManifest_IsHttpsUrl(sManifest.nroUrl) || bg_busy())
    return 0;
  bg_join();
  if (!sNetHeld) { Port_Net_Init(); sNetHeld = true; }
  sStatus = ALEKS_UPDATE_DOWNLOADING;
  sError[0] = 0;
  sProgressPermille = 0;
  if (!bg_start(download_worker)) { SetError("Cannot start download task"); return 0; }
  return 1;
}

int AleksUpdate_RequestInstall(void) {
  if (sStatus != ALEKS_UPDATE_READY || bg_busy()) return 0;
  bg_join();
  sError[0] = 0;
  if (!bg_start(install_worker)) { SetError("Cannot start install task"); return 0; }
  return 1;
}

AleksUpdateStatus AleksUpdate_Status(void) { return (AleksUpdateStatus)sStatus; }
const char *AleksUpdate_LatestVersion(void) { return sLatest; }
unsigned AleksUpdate_ChangelogCount(void) { return sManifest.changelogCount; }
const char *AleksUpdate_ChangelogLine(unsigned i) {
  return i < sManifest.changelogCount ? sManifest.changelog[i] : "";
}
unsigned AleksUpdate_ProgressPermille(void) { return sProgressPermille; }
const char *AleksUpdate_Error(void) { return sError; }
bool AleksUpdate_HasNews(void) {
  int s = sStatus;
  return s == ALEKS_UPDATE_AVAILABLE || s == ALEKS_UPDATE_READY ||
         s == ALEKS_UPDATE_INSTALLED_RESTART;
}
