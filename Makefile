
PATH=$PATH:/usr/local/arm/armv7linaro/bin/
BIN_PATH = /home/user/dom3/rootfs

CROSS_COMPILE = arm-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc

CP = /usr/bin/sudo /bin/cp
DEL = /bin/rm -f

APP = howldet
MKFILE = Makefile

SRC = main.c

OBJS = $(SRC:.c=.o)
DEPS = $(SRC:.c=.d)

LIBDIR= -L/home/user/dom3/DSP/lib

CPFLAGS = -mfpu=neon -funsafe-math-optimizations -ftree-vectorize -fomit-frame-pointer \
        -I/home/user/dom3/DSP/include \
        -I/home/user/dom3/DSP/include/siglib \
        -DUSE_NEON


LIBS = -lm -lrt -lpthread -ldl -lasound -lNE10 -lsiglib -lnhl_part

LDFLAGS = -g -O2 -Wl,-EL $(LIBS) $(LIBDIR) -Wl,-rpath=/home/user/dom3/DSP/lib

.PHONY: all target
target: all

all: $(OBJS) $(MKFILE)
	$(CC) $(OBJS) -o $(APP) $(LDFLAGS)
	-$(CP) $(APP) $(BIN_PATH)
	

%.o: %.c $(MKFILE)
	@echo "Compiling '$<'"
	$(CC) -c $(CPFLAGS) -I . $< -o $@

%.d: %.c $(MKFILE)
	@echo "Building dependencies for '$<'"
	@$(CC) -E -MM -MQ $(<:.c=.o) $(CPFLAGS) $< -o $@
	@$(DEL) $(<:.c=.o)
	
clean:
	-$(DEL) $(OBJS:/=\)
	-$(DEL) $(DEPS:/=\)
	-$(DEL) $(APP:/=\)

	
.PHONY: dep
dep: $(DEPS) $(SRC)
	@echo "##########################"
	@echo "### Dependencies built ###"
	@echo "##########################"

-include $(DEPS)
