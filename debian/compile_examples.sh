#!/bin/sh
set -e
echo Creating $HOME/allegro.tmp
cd ~
mkdir -p allegro.tmp
cd allegro.tmp
#cp /usr/share/doc/liballegro-doc/examples/* .
echo "Uncompressing example source from " /usr/share/doc/liballegro-doc/examples/*.tgz
tar zxf /usr/share/doc/liballegro-doc/examples/*tgz
#gunzip -v *.gz
echo "Compiling examples..."
for src in *.c; do
  gcc -g -O2 -o ${src%.c} $src `allegro-config --libs`
#  strip ${src%.c}
done
echo "Compiled example programs are now in ~/allegro.tmp"
