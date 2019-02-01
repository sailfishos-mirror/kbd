#ifndef _PATHS_H
#define _PATHS_H
/*
 * All data is in subdirectories of DATADIR, by default /usr/lib/kbd
 * The following five subdirectories are defined:
 */
#define KEYMAPDIR "keymaps"
#define VIDEOMODEDIR "videomodes"

/*
 * Default keymap, and where the kernel copy of it lives.
 */
#ifdef __sparc__
#define DEFMAP "sunkeymap.map"
#define KERNDIR "/usr/src/linux/drivers/sbus/char"
#else
#define DEFMAP "defkeymap.map"
#define KERNDIR "/usr/src/linux/drivers/tty/vt"
#endif

#endif /* _PATHS_H */
