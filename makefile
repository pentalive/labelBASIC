labelbasic: interp.c
	cc -o labelbasic interp.c -lm

install: /usr/bin/labelbasic
	cp labelbasic /usr/bin/labelbasic
	cp lb         /usr/bin/lb
