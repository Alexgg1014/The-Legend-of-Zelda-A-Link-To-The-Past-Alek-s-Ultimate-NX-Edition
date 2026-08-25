/*
 * aleks_lang.h -- optional translated dialogue, extracted from the user's own
 * translation ROM.
 *
 * Ported from the Esteban 3DS donor's TranslationExtractor.java.  The donor
 * lifts the dialogue, dictionary, font and width table straight out of a
 * translation hack of the US ROM and appends them to the asset container as an
 * extra language, so the engine's existing `Language =` setting and
 * ZeldaSetLanguage select it with no second text engine.  That is exactly what
 * this does.
 *
 * ONE DELIBERATE DIFFERENCE from the donor: the donor rewrites
 * zelda3_assets.dat.  This does NOT.  The base asset file is the thing that
 * currently boots on hardware, and a language is optional, so the new entries
 * are built in memory and the asset array pointers are repointed at them.  A
 * broken or missing translation therefore cannot damage the file the game
 * needs to start.
 *
 * NOTHING HERE IS REQUIRED.  With no languages/ directory, no language ROM and
 * no cached pack, every function is a no-op and the game boots English.
 */
#pragma once
#include "types.h"

/*
 * Scan languages/, build or load each pack, and register the languages into
 * the kDialogue / kDialogueFont / kDialogueMap asset arrays.
 *
 * Call AFTER the base assets are loaded and BEFORE ZeldaSetLanguage.  Failure
 * of any individual language is contained: it is logged and skipped.
 */
void AleksLang_Init(void);

/* How many languages the selector should offer.  Always >= 1: index 0 is the
 * built-in English that ships inside zelda3_assets.dat. */
int AleksLang_Count(void);

/* The `Language =` value for entry i; NULL for index 0 (built-in English),
 * which is what ZeldaSetLanguage wants for the default. */
const char *AleksLang_CodeAt(int i);

/* The name to draw in the settings row, e.g. "ENGLISH", "ESPANOL". */
const char *AleksLang_DisplayAt(int i);

/* Index of the currently configured language, or 0 when its pack is missing
 * -- the caller can then fall back without inspecting anything. */
int AleksLang_CurrentIndex(void);
