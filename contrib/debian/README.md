
Debian
====================
This directory contains files used to package nodexd/nodex-qt
for Debian-based Linux systems. If you compile nodexd/nodex-qt yourself, there are some useful files here.

## nodex: URI support ##


nodex-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install nodex-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your nodexqt binary to `/usr/bin`
and the `../../share/pixmaps/nodex128.png` to `/usr/share/pixmaps`

nodex-qt.protocol (KDE)

