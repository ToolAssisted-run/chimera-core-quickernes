# Builds libquicknes, the native half of the miniHawk core package, from this
# repository's own sources.
#   make -j4 && make install    (installs into ../natives)
CORE_ROOT = ../..

CXX = g++
RM = rm
CP = cp

CXXFLAGS = -I$(CORE_ROOT)/source/quickerNES/core/ -I$(CORE_ROOT)/extern/jaffarCommon/include -Wall -Wfatal-errors -Werror \
	-std=c++20 -O3 -fomit-frame-pointer -flto -fvisibility=internal -fvisibility-inlines-hidden \
	-D_GNU_SOURCE -D__INLINE__=inline -D_QUICKERNES_DETECT_JOYPAD_READS -D_QUICKERNES_ENABLE_TRACEBACK_SUPPORT -D_QUICKERNES_SUPPORT_ARKANOID_INPUTS -D_QUICKERNES_PRINT_CART_INFO

UNAME := $(shell uname)
ifeq ($(UNAME), Linux)
	TARGET = libquicknes.so
else
	TARGET = libquicknes.dll
endif

LDFLAGS = -shared -s $(CXXFLAGS)

DEST = ../natives

SRCS = \
	$(CORE_ROOT)/source/quickerNES/core/apu/vrc7/emu2413_state.cpp \
	$(CORE_ROOT)/source/quickerNES/core/apu/vrc7/emu2413.cpp  \
	$(CORE_ROOT)/source/quickerNES/core/apu/vrc7/apu_vrc7.cpp \
	$(CORE_ROOT)/source/quickerNES/core/apu/fme7/apu_fme7.cpp \
	$(CORE_ROOT)/source/quickerNES/core/apu/namco/apu_namco.cpp \
	$(CORE_ROOT)/source/quickerNES/core/apu/effectsBuffer.cpp \
	$(CORE_ROOT)/source/quickerNES/core/apu/blipBuffer.cpp \
	$(CORE_ROOT)/source/quickerNES/core/apu/NESEffectsBuffer.cpp \
	$(CORE_ROOT)/source/quickerNES/core/apu/vrc6/apu_vrc6.cpp \
	$(CORE_ROOT)/source/quickerNES/core/apu/oscs.cpp \
	$(CORE_ROOT)/source/quickerNES/core/apu/apu.cpp \
	$(CORE_ROOT)/source/quickerNES/core/apu/buffer.cpp \
	$(CORE_ROOT)/source/quickerNES/core/apu/multiBuffer.cpp \
	$(CORE_ROOT)/source/quickerNES/core/emu.cpp \
	$(CORE_ROOT)/source/quickerNES/core/mappers/mapper.cpp \
	$(CORE_ROOT)/source/quickerNES/core/cpu.cpp \
	$(CORE_ROOT)/source/quickerNES/core/ppu/ppu.cpp \
	$(CORE_ROOT)/source/quickerNES/core/ppu/ppuRendering.cpp \
	$(CORE_ROOT)/source/quickerNES/core/ppu/ppuImpl.cpp \
	bizinterface.cpp

OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

%.o: %.cpp
	$(CXX) -c -o $@ $< $(CXXFLAGS)

$(TARGET) : $(OBJS)
	$(CXX) -o $@ $(LDFLAGS) $(OBJS)

clean:
	$(RM) -f $(OBJS)
	$(RM) -f $(TARGET)

install:
	$(CP) $(TARGET) $(DEST)
