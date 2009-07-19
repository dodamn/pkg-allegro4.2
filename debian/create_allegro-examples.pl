#!/usr/bin/perl
# This small script creates a dialog-script with a program starting
# dialog for allegro example programs
open(fh,"<debian/tests.dsc");
while (<fh>) {
  if(/(.*) - (.*)/) {
    $prg = $1;
    $dsc = $2;
    $prg = $1;
    $dsc = $2;
    $prg =~ s/\.c.*//;
    $dsc =~ s/\'/\"/g;
    $db{$prg} = $dsc;
#    print "$prg $dsc
#    ";
  }
}
close(fh);

print '#!/bin/sh
DIALOG=${DIALOG=dialog}
tempfile=`tempfile 2>/dev/null` || tempfile=/tmp/test$$
trap "rm -f $tempfile" 0 1 2 5 15

$DIALOG --clear --title "Examples programs for the Allegro library" \
        --menu "\nHi, using this menu you can run some of example \
        programs and utilities distributed with the Allegro library.\
        Some of them may work on your system, some may not. Just try and \
        you could find some nice looking demos and useful tools.\n\n" \
        20 80 10 \
';
for (sort(keys %db)) {
  print "\'$_\' \'".$db{$_}.'\' \\
  ';
}
print ' 2> $tempfile

retval=$?

choice=`cat $tempfile`

case $retval in
  0)
    cd /usr/share/games/allegro-examples/
    /usr/lib/games/allegro-examples/$choice
    echo Press RETURN to continue...
    read dummy
    exec $0;;
  1)
    echo "Cancel pressed.";;
  255)
    echo "ESC pressed.";;
esac
'
