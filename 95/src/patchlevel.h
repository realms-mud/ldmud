#ifndef __PATCHLEVEL_H__
#define __PATCHLEVEL_H__ 1

/* $Format: "#define RELEASE_DATE \"$ProjectDate$\""$ */
#define RELEASE_DATE "Sat, 22 May 1999 04:34:51 +0200"

/* $Format: "#define PROJ_VERSION \"$ProjectVersion$\""$ */
#define PROJ_VERSION "3-2-dev.95"

/* $Format: "#define GAME_VERSION \"$ReleaseVersion$\""$ */
#define GAME_VERSION "3.2.7"

/* $Format: "#define LONG_VERSION \"$ReleaseVersion$-$ReleaseType$.$ProjectMinorVersion$\""$ */
#define LONG_VERSION "3.2.7-dev.95"

/* $Format: "#define LOCAL_LEVEL \"\""$ */
#define LOCAL_LEVEL ""

/* TODO: Add something like the perl local patchlevel management. */
/* TODO: Add LPC defines for the various version number parts */
/* TODO: Make the necessary mods for PRCS */

/* $Format: "#define IS_RELEASE() (!strcmp(\"$ReleaseType$\", \"rel\"))"$ */
#define IS_RELEASE() (!strcmp("dev", "rel"))

#endif /* __PATCHLEVEL_H__ */
