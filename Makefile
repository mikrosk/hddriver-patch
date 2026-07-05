CC      = m68k-atari-mintelf-gcc
LD      = m68k-atari-mintelf-gcc
LIBCMINI = $(shell $(CC) -print-sysroot)/opt/libcmini
CFLAGS  = -std=c99 -Wall -Wextra -O2 -fomit-frame-pointer -I$(LIBCMINI)/include
LDFLAGS = -nostdlib $(LIBCMINI)/lib/crt0.o -L$(LIBCMINI)/lib -s
LIBS    = -lcmini -lgcc

OBJS		= main.o endian_utils.o

TARGET     	= dmapatch
PRG			= DMAPATCH.PRG
INPUT      	= ./test/HDDRIVER.ORG
OUTPUT     	= ./test/HDDRIVER.SYS
BUILD_DIR  	= build

HD_IMG     	?= hd.img
CLEAN_IMG	?= hd-clean.img
HATARI     	?= hatari-hrdb
TOS_ROM    	?= $(HOME)/ATARI/TOS/tos206uk.img
TIME       	?= $(shell date +%H%M%S)
DEBUG_SCRIPT = breakpoints.txt
#BREAK_ADDR = 0xA8B2
BREAK_ADDR 	= 0xDA42

all: $(PRG)

$(PRG): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

$(OUTPUT): $(TARGET) $(INPUT)
	mkdir -p output
	./$(TARGET) $(INPUT) $(OUTPUT)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

run:
	echo "" > $(DEBUG_SCRIPT)
	echo 'a $(BREAK_ADDR)' >> $(DEBUG_SCRIPT)
	$(HATARI) \
		-w -m --machine ste --tos $(TOS_ROM) \
		--fullscreen \
		--harddrive "$(shell pwd)/output/" --gemdos-drive G \
		--acsi 0=$(HD_IMG) \
		--parse $(DEBUG_SCRIPT) \
		--fast-boot TRUE --fast-forward TRUE --timer-d TRUE \
		--confirm-quit false

deploy:
	echo "" > $(DEBUG_SCRIPT)
	echo 'a $(BREAK_ADDR)' >> $(DEBUG_SCRIPT)
	cp -f $(CLEAN_IMG) $(HD_IMG)
	mkdir -p output/AUTO
	cp copier/AUTOCOPY.TOS output/AUTO/
	$(HATARI) \
		--fast-boot 1 --fast-forward 0 --timer-d 1 \
		-w -m --machine ste --tos $(TOS_ROM) \
		--fullscreen \
		--harddrive "$(shell pwd)/output/" --gemdos-drive G \
		--acsi 0=$(HD_IMG) \
		--parse $(DEBUG_SCRIPT) \
		--confirm-quit false \
		--auto "G:\AUTO\AUTOCOPY.TOS"


clean:
	rm -f $(OBJS) $(TARGET) $(DEBUG_SCRIPT) $(OUTPUT) $(PRG)

