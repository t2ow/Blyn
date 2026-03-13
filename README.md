# Blyn

Blyn is a simple and fast command line code editor built for 2026. It takes the best ideas from classic editors like Vim and Emacs and puts them into a package that is easy to pick up but powerful enough for serious work.

This project started as VaultEdit and has been completely rebuilt to be faster and more reliable. It now uses a dynamic line system, which means it can handle large files without any issues.

## Key features

Blyn uses different modes to keep your workspace clean.

Normal mode is where you start. You can move around using the arrow keys or the classic hjkl keys. You can also jump to the start or end of lines and search for text easily.

Insert mode is for typing. You can enter it by pressing i, a, or o. While you are typing, you can use shortcuts inspired by Emacs to move your cursor or delete text quickly.

Command mode lets you save your work and manage your files. Just type a colon followed by your command.

## Installation

Building Blyn is straightforward. You just need a terminal and the ncurses library.

To compile it, run:
make

If you want to be able to run it from anywhere by just typing blyn, you can install it system wide:
sudo make install

## How to use it

To open a file, just type:
blyn filename.txt

Once you are in the editor:

Press i to start typing.
Press ESC to return to normal mode for navigation.
Type :w to save your progress.
Type :q to exit.

