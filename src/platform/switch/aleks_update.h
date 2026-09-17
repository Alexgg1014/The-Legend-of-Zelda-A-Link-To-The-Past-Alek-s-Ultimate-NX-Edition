/* aleks_update.h -- in-game updater for ALEKS Ultimate NX (Switch only).
 *
 * Ported from the TMC port's updater (port_update_check.c +
 * port_update_common.c, production since TMC v1.0.0), which settled on an
 * in-process install after the separate helper NRO proved unusable from a
 * forwarder (hbloader aborts with 2001-0106 while reusing the address space).
 *
 * The transaction, in order, and what each step can NOT do:
 *   CHECK     GET the project-controlled manifest (closed schema: version,
 *             https URL, size, SHA-256, changelog). Never touches the SD
 *             beyond caching the manifest itself.
 *   DOWNLOAD  Stream the NRO to a fixed staging path under update/. Size is
 *             enforced during the transfer, then the file is reopened and
 *             hashed. Never touches the installed NRO.
 *   INSTALL   Verified backup (.bak) of the running NRO first; only then the
 *             staged file is copied over it and re-verified. Any failure
 *             restores the backup. A journal survives a power cut at any
 *             point so the next boot can finish or roll back.
 *
 * What gets replaced is the NRO this process was launched from (hbloader's
 * argv[0]) -- a local, loader-supplied path. No remote value ever names a
 * path: the manifest supplies bytes and a hash, nothing else. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ALEKS_UPDATE_UNKNOWN = 0,       /* not checked, or the check failed quietly */
  ALEKS_UPDATE_UNAVAILABLE,       /* not launched from an updatable NRO path */
  ALEKS_UPDATE_CHECKING,
  ALEKS_UPDATE_UP_TO_DATE,
  ALEKS_UPDATE_AVAILABLE,
  ALEKS_UPDATE_DOWNLOADING,
  ALEKS_UPDATE_VERIFYING,
  ALEKS_UPDATE_READY,             /* staged + verified + journalled */
  ALEKS_UPDATE_INSTALLING,
  ALEKS_UPDATE_INSTALLED_RESTART, /* done; the running image is the old one */
  ALEKS_UPDATE_FAILED,
} AleksUpdateStatus;

/* main(): record argv[0] BEFORE any argv shifting. Safe to pass NULL. */
void AleksUpdate_SetLaunchPath(const char *argv0);

/* Kick off the background check once per process. Returns immediately.
 * Brings the network up (refcounted with RetroAchievements). */
void AleksUpdate_Start(void);
/* Join any worker. Call before SDL/network teardown. */
void AleksUpdate_Shutdown(void);

/* Manual re-check after a finished check. Returns 1 only when a worker began. */
int AleksUpdate_RequestCheck(void);
/* Stage the manifest's NRO. Non-destructive on failure. */
int AleksUpdate_RequestDownload(void);
/* Replace the launched NRO with the verified staged file. */
int AleksUpdate_RequestInstall(void);

AleksUpdateStatus AleksUpdate_Status(void);
/* Latest version from the manifest, e.g. "1.2.0"; "" if unknown. */
const char *AleksUpdate_LatestVersion(void);
unsigned AleksUpdate_ChangelogCount(void);
const char *AleksUpdate_ChangelogLine(unsigned index);
/* 0..1000 while downloading. */
unsigned AleksUpdate_ProgressPermille(void);
const char *AleksUpdate_Error(void);
/* True when the status is something a player should be told about at a
 * glance (available / ready / installed). */
bool AleksUpdate_HasNews(void);

/* Unmount romfs once.  The install does it before overwriting the launched
 * NRO (libnx keeps the file open while romfs is mounted); main() calls it at
 * exit.  Second and later calls are no-ops. */
void AleksSwitch_RomfsRelease(void);

#ifdef __cplusplus
}
#endif
