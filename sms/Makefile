ROM = hello.sms

all: $(ROM) game.bin

# In-game mini-game blob: 8KB image that runs from Z80 RAM. The header
# the 68K compiles against is regenerated whenever the blob changes.
game.o: game.asm
	wla-z80 -o game.o game.asm

game.linkfile: Makefile
	printf '[objects]\ngame.o\n' > game.linkfile

game.bin: game.o game.linkfile
	wlalink -d game.linkfile game.bin
	python3 ../tools/gen_z80_game.py

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
	rm -f hello.o $(ROM) linkfile font_1bpp.bin game.o game.bin game.linkfile

.PHONY: all install clean
