/* Single source of the ALEKS Ultimate NX version.
 *
 * The Switch Makefile reads this define for the NACP (APP_VERSION), and the
 * updater compares it against the manifest, so the two can never disagree.
 * Plain three-part semver, no "v" prefix, no suffix. */
#ifndef ALEKS_VERSION_H_
#define ALEKS_VERSION_H_

#define ALEKS_NX_VERSION "1.2.1"

#endif  /* ALEKS_VERSION_H_ */
