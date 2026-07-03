/*
 * ArcadeOS – Device Filesystem (devfs)
 *
 * Provides /dev/tty and /dev/null.
 */

#ifndef DEVFS_H
#define DEVFS_H

/* Initialize devfs and register its devices with the VFS */
void devfs_init(void);

#endif /* DEVFS_H */
