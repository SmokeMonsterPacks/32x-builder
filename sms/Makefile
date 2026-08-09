ROM = hello.sms

all: $(ROM)

font_1bpp.bin: gen_font.py
	python3 gen_font.py

hello.o: hello.asm font_1bpp.bin
	wla-z80 -o hello.o hello.asm

linkfile: Makefile
	printf '[objects]\nhello.o\n' > linkfile

$(ROM): hello.o linkfile
	wlalink -d linkfile $(ROM)
	python3 verify.py $(ROM)

install: $(ROM)
	cp $(ROM) ../rom/sms-diag.sms

clean:
	rm -f hello.o $(ROM) linkfile font_1bpp.bin

.PHONY: all install clean
