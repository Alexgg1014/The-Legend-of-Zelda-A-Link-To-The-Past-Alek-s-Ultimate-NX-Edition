/*
 * The few SDL entry points config.c uses for string compares.  Nothing is
 * linked against SDL here -- only its headers are needed -- so these stand in
 * with the same semantics SDL documents.
 */
#include <ctype.h>
#include <stddef.h>

int SDL_strncasecmp(const char *a, const char *b, size_t n) {
  while (n--) {
    int ca = tolower((unsigned char)*a++), cb = tolower((unsigned char)*b++);
    if (ca != cb) return ca - cb;
    if (!ca) return 0;
  }
  return 0;
}

int SDL_strcasecmp(const char *a, const char *b) {
  for (;;) {
    int ca = tolower((unsigned char)*a++), cb = tolower((unsigned char)*b++);
    if (ca != cb) return ca - cb;
    if (!ca) return 0;
  }
}

/* The engine's fatal path and the one SDL lookup the key parser uses.  A test
 * that trips Die() has found a real problem, so it aborts loudly rather than
 * carrying on with a half-parsed config. */
#include <stdio.h>
#include <stdlib.h>

void Die(const char *error) {
  printf("\n  FATAL  Die(\"%s\")\n", error ? error : "(null)");
  exit(3);
}

/* Key names are irrelevant to the config tests; returning 0 (SDLK_UNKNOWN) is
 * what SDL does for a name it does not know. */
int SDL_GetKeyFromName(const char *name) { (void)name; return 0; }
