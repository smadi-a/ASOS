# user/DOOM/Makefile — Build DOOM for ASOS.
#
# Compiles the linuxdoom-1.10 source against libasos.a with ASOS-specific
# i_video/i_system/i_sound/i_net replacements.  Produces doom.elf linked
# at 0x400000 using user/user.ld.
#
# Usage:
#   make          Build doom.elf
#   make clean    Remove build artefacts

# ── Toolchain ────────────────────────────────────────────────────────────

CC  := gcc
LD  := ld

# ── Paths ────────────────────────────────────────────────────────────────

DOOM_SRC   := linuxdoom-1.10
LIBC_DIR   := ../libc
LIBC_INC   := $(LIBC_DIR)/include
LIBC_LIB   := $(LIBC_DIR)/libasos.a
LIBC_CRT   := $(LIBC_DIR)/start.o
USER_LD    := ../user.ld

# Build output directory.
O := asos

# ── Flags ────────────────────────────────────────────────────────────────

# ASOS user-space CFLAGS matching user/Makefile.
CFLAGS := \
    -ffreestanding -nostdlib -nostdinc -fno-builtin \
    -fno-stack-protector -mno-red-zone \
    -mno-mmx -mno-sse -mno-sse2 \
    -Wall -O2 \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -Wno-unused-variable -Wno-unused-const-variable \
    -DASOS \
    -I$(LIBC_INC) \
    -Iinclude \
    -I$(DOOM_SRC) \
    -include $(DOOM_SRC)/asos_compat.h

LDFLAGS := -T $(USER_LD) -nostdlib

# ── Source files ─────────────────────────────────────────────────────────

# All original DOOM .c files EXCEPT the platform-specific i_* files
# and i_main.c (we provide our own).
DOOM_SRCS := \
    $(DOOM_SRC)/doomdef.c \
    $(DOOM_SRC)/doomstat.c \
    $(DOOM_SRC)/dstrings.c \
    $(DOOM_SRC)/tables.c \
    $(DOOM_SRC)/f_finale.c \
    $(DOOM_SRC)/f_wipe.c \
    $(DOOM_SRC)/d_main.c \
    $(DOOM_SRC)/d_net.c \
    $(DOOM_SRC)/d_items.c \
    $(DOOM_SRC)/g_game.c \
    $(DOOM_SRC)/m_menu.c \
    $(DOOM_SRC)/m_misc.c \
    $(DOOM_SRC)/m_argv.c \
    $(DOOM_SRC)/m_bbox.c \
    $(DOOM_SRC)/m_fixed.c \
    $(DOOM_SRC)/m_swap.c \
    $(DOOM_SRC)/m_cheat.c \
    $(DOOM_SRC)/m_random.c \
    $(DOOM_SRC)/am_map.c \
    $(DOOM_SRC)/p_ceilng.c \
    $(DOOM_SRC)/p_doors.c \
    $(DOOM_SRC)/p_enemy.c \
    $(DOOM_SRC)/p_floor.c \
    $(DOOM_SRC)/p_inter.c \
    $(DOOM_SRC)/p_lights.c \
    $(DOOM_SRC)/p_map.c \
    $(DOOM_SRC)/p_maputl.c \
    $(DOOM_SRC)/p_plats.c \
    $(DOOM_SRC)/p_pspr.c \
    $(DOOM_SRC)/p_setup.c \
    $(DOOM_SRC)/p_sight.c \
    $(DOOM_SRC)/p_spec.c \
    $(DOOM_SRC)/p_switch.c \
    $(DOOM_SRC)/p_mobj.c \
    $(DOOM_SRC)/p_telept.c \
    $(DOOM_SRC)/p_tick.c \
    $(DOOM_SRC)/p_saveg.c \
    $(DOOM_SRC)/p_user.c \
    $(DOOM_SRC)/r_bsp.c \
    $(DOOM_SRC)/r_data.c \
    $(DOOM_SRC)/r_draw.c \
    $(DOOM_SRC)/r_main.c \
    $(DOOM_SRC)/r_plane.c \
    $(DOOM_SRC)/r_segs.c \
    $(DOOM_SRC)/r_sky.c \
    $(DOOM_SRC)/r_things.c \
    $(DOOM_SRC)/w_wad.c \
    $(DOOM_SRC)/wi_stuff.c \
    $(DOOM_SRC)/v_video.c \
    $(DOOM_SRC)/st_lib.c \
    $(DOOM_SRC)/st_stuff.c \
    $(DOOM_SRC)/hu_stuff.c \
    $(DOOM_SRC)/hu_lib.c \
    $(DOOM_SRC)/s_sound.c \
    $(DOOM_SRC)/z_zone.c \
    $(DOOM_SRC)/info.c \
    $(DOOM_SRC)/sounds.c

# ASOS-specific replacements for the platform interface.
ASOS_SRCS := \
    $(DOOM_SRC)/i_video_asos.c \
    $(DOOM_SRC)/i_system_asos.c \
    $(DOOM_SRC)/i_sound_asos.c \
    $(DOOM_SRC)/i_net_asos.c \
    $(DOOM_SRC)/i_main.c

# Object files.
DOOM_OBJS := $(patsubst $(DOOM_SRC)/%.c, $(O)/%.o, $(DOOM_SRCS))
ASOS_OBJS := $(patsubst $(DOOM_SRC)/%.c, $(O)/%.o, $(ASOS_SRCS))
ALL_OBJS  := $(DOOM_OBJS) $(ASOS_OBJS)

# The compat header declares a vfprintf shim buffer.
# We need one translation unit to define it.
COMPAT_OBJ := $(O)/asos_compat.o

# ── Targets ──────────────────────────────────────────────────────────────

.PHONY: all clean

all: $(O)/doom.elf
	@echo "DOOM for ASOS built: $(O)/doom.elf"

clean:
	rm -rf $(O)

$(O):
	mkdir -p $(O)

# Compile original DOOM sources.
$(O)/%.o: $(DOOM_SRC)/%.c | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile the compat shim (defines the vfprintf buffer).
$(COMPAT_OBJ): | $(O)
	@echo 'char _vfprintf_buf[512];' | \
	    $(CC) $(CFLAGS) -D_ASOS_COMPAT_IMPL -x c -c - -o $@

# Link everything into doom.elf.
$(O)/doom.elf: $(ALL_OBJS) $(COMPAT_OBJ) $(LIBC_CRT) $(LIBC_LIB) $(USER_LD)
	$(LD) $(LDFLAGS) -o $@ \
	    $(LIBC_CRT) $(ALL_OBJS) $(COMPAT_OBJ) $(LIBC_LIB)
