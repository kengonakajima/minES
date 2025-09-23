PORTAUDIO_HOME ?= $(CURDIR)/pa
PORTAUDIO_INCLUDE ?= $(PORTAUDIO_HOME)/include
PORTAUDIO_LIB ?= $(PORTAUDIO_HOME)/libportaudio.a
PORTAUDIO_FRAMEWORKS ?= -framework CoreAudio -framework AudioToolbox -framework AudioUnit \
                        -framework CoreServices -framework CoreFoundation -framework Carbon

CXX ?= clang++
CPPFLAGS += -I$(PORTAUDIO_INCLUDE)
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
LDFLAGS +=
LDLIBS +=

BINARIES := echoback cancel_file

all: $(BINARIES)

clean:
	$(RM) $(BINARIES) *.o

.PHONY: all clean

echoback: echoback.o
	$(CXX) $(CXXFLAGS) $^ $(PORTAUDIO_LIB) $(PORTAUDIO_FRAMEWORKS) -lpthread -o $@

echoback.o: echoback.cc suppressor.h

cancel_file: cancel_file.o
	$(CXX) $(CXXFLAGS) $^ -o $@

cancel_file.o: cancel_file.cc suppressor.h
