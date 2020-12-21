# In case of windows, modification is not necessary anymore.

# Uncomment for MinGW
CFLAGS = -D MinGW_NOFORK
CC=i686-w64-mingw32-gcc
DLI=win-iconv-0.0.8
LI=libiconv.a
PLI=$(DLI)/$(LI)
LDFLASG=$(PLI)

# Uncomment to add debug info
# CFLAGS += -ggdb -O0

# The following options are only used by developers
# CFLAGS += -Wall -W
# CFLAGS += -D DEBUGMSG

# The following options for release
CFLAGS  += -O2
LDFLASG += -s

PRG=smsd_mi

EXE=$(PRG)

ifneq (,$(findstring MinGW_NOFORK,$(CFLAGS)))
EXE=$(PRG).exe
endif

all: $(EXE)

$(EXE): $(PRG).o $(PLI)
	$(CC) $< $(LDFLASG) -o $@
	
ifneq (,$(findstring MinGW_NOFORK,$(CFLAGS)))
$(PLI):
	make -C $(DLI) $(LI)
endif

clean:
	rm -f *.o $(EXE) *~
