#!/bin/sh
#
# A simple script which puts a component tree dump into the HTML
# documentation for each machine.
#

if [ ! -x gxemul ]; then
	echo "GXemul hasn't been built yet?"
	echo "Remember to launch this script from the main gxemul"
	echo "directory, not from the doc directory."
	exit
fi

for a in doc/machines/*.SKEL; do
	MACHINE=`echo $a|cut -d _ -f 2 | cut -d . -f 1`
	echo Generating final HTML documentation for machine $MACHINE...
	
	sed s/XMACHINEX/$MACHINE/g < doc/machine_template.html > doc/machines/machine_$MACHINE.html
 	echo "<!-- This file is AUTOMATICALLY GENERATED! Do not edit. -->" >> doc/machines/machine_$MACHINE.html
	./gxemul -WW@D$MACHINE >> doc/machines/machine_$MACHINE.html
	cat doc/machines/machine_$MACHINE.html.SKEL >> doc/machines/machine_$MACHINE.html
done

