TARGET = Seed3P4SpiAudio

CPP_SOURCES = src/main.cpp src/seed_audio_clock.cpp
C_INCLUDES += -I../protocol

# A normal checkout should place libDaisy here (for example as a submodule).
# build.ps1 also knows how to use the already installed Seed tool project.
LIBDAISY_DIR ?= libDaisy
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core

include $(SYSTEM_FILES_DIR)/Makefile
