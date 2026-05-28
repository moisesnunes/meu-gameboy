NAME = gameboy

CC_C   = gcc
CC_CXX = g++

BUILD_DIR        = build
BUILD_CORE_DIR   = $(BUILD_DIR)/core
BUILD_APP_DIR    = $(BUILD_DIR)/app
BUILD_TEST_DIR   = $(BUILD_DIR)/test
BUILD_UI_DIR     = $(BUILD_DIR)/ui
BUILD_GBA_DIR    = $(BUILD_DIR)/gba

CFLAGS   = -Wall -O2 -MMD -MP `pkg-config --cflags sdl3` -I . -I ui -I sm83 -I hw_schematic
CXXFLAGS = -Wall -O2 -MMD -MP `pkg-config --cflags sdl3` -I . -I ui -I imgui -I imgui/backends -I sm83 -I hw_schematic
LDFLAGS  = `pkg-config --libs sdl3` -lGL

IMGUI_SRC = imgui/imgui.cpp \
            imgui/imgui_draw.cpp \
            imgui/imgui_tables.cpp \
            imgui/imgui_widgets.cpp \
            imgui/backends/imgui_impl_sdl3.cpp \
            imgui/backends/imgui_impl_opengl3.cpp

C_SRC = main.c cpu.c memory.c cart.c gpu.c sync.c sdl.c input.c irq.c dma.c \
        hdma.c timer.c spu.c debug.c disasm.c rtc.c state.c

UI_SRC = ui/debug_ui.cpp ui/debug_ui_config.cpp ui/debug_ui_actions.cpp \
         ui/debug_ui_menus.cpp ui/debug_ui_panels.cpp

SM83_C_SRC = sm83/sm83_netlist_data.c sm83/sm83_die_view.c sm83/sm83_signal_overlay.c \
             sm83/sm83_netlist_sim.c sm83/sm83_semantic_map.c

HW_SCH_C_SRC = hw_schematic/hw_schematic_data.c hw_schematic/hw_schematic_view.c \
               hw_schematic/hw_schematic_map.c hw_schematic/hw_schematic_trace.c

CORE_C_SRC = cpu.c memory.c cart.c gpu.c sync.c input.c irq.c dma.c \
             hdma.c timer.c spu.c debug.c disasm.c rtc.c \
             $(SM83_C_SRC) $(HW_SCH_C_SRC)

FRONTENDS_DIR = frontends

CORE_OBJ = $(CORE_C_SRC:%.c=$(BUILD_CORE_DIR)/%.o)
APP_OBJ  = $(BUILD_APP_DIR)/main.o $(BUILD_APP_DIR)/sdl.o $(BUILD_APP_DIR)/state.o
UI_FRONTEND_OBJ = $(UI_SRC:ui/%.cpp=$(BUILD_UI_DIR)/%.o)
IMGUI_OBJ       = $(IMGUI_SRC:%.cpp=$(BUILD_UI_DIR)/%.o)
UI_OBJ          = $(UI_FRONTEND_OBJ) $(IMGUI_OBJ)
OBJ      = $(APP_OBJ) $(CORE_OBJ) $(UI_OBJ)

APP_DEP  = $(APP_OBJ:.o=.d)
CORE_DEP = $(CORE_OBJ:.o=.d)
UI_DEP   = $(UI_OBJ:.o=.d)

# GBA core sources
GBA_CORE_C_SRC = gba/gba.c gba/gba_cpu.c gba/gba_cpu_arm.c gba/gba_cpu_thumb.c \
                 gba/gba_memory.c gba/gba_gpu.c gba/gba_apu.c gba/gba_dma.c \
                 gba/gba_timer.c gba/gba_cart.c gba/gba_irq.c gba/gba_sync.c \
                 gba/gba_input.c gba/gba_debug.c gba/gba_disasm.c

GBA_CORE_OBJ = $(GBA_CORE_C_SRC:gba/%.c=$(BUILD_GBA_DIR)/%.o)
GBA_CORE_DEP = $(GBA_CORE_OBJ:.o=.d)
GBA_UI_SRC = ui/gba_debug_ui.cpp
GBA_UI_OBJ = $(GBA_UI_SRC:ui/%.cpp=$(BUILD_UI_DIR)/%.o)
GBA_UI_DEP = $(GBA_UI_OBJ:.o=.d)

GBA_NAME = meu-gba
GBA_OBJ  = $(BUILD_GBA_DIR)/gba_main.o $(GBA_CORE_OBJ) $(GBA_UI_OBJ) $(IMGUI_OBJ)

GBA_COMPAT_NAME = gba_compat_test
GBA_COMPAT_OBJ  = $(BUILD_TEST_DIR)/gba_compat_test.o $(GBA_CORE_OBJ)

SIMPLE_NAME = gameboy-simple
SIMPLE_OBJ  = $(BUILD_APP_DIR)/main_simple.o $(CORE_OBJ)

HW_NAME = gameboy-hardware
HW_OBJ  = $(BUILD_APP_DIR)/main_hardware.o $(CORE_OBJ)

VEC_NAME = gameboy-vector
VEC_OBJ  = $(BUILD_APP_DIR)/main_vector.o $(CORE_OBJ)

TESTER_NAME = rom_tester
TESTER_OBJ  = $(BUILD_TEST_DIR)/rom_tester.o $(CORE_OBJ)

COMPAT_NAME = compat_test
COMPAT_OBJ  = $(BUILD_TEST_DIR)/compat_test.o $(CORE_OBJ)

SM83_VALIDATE_NAME = sm83_netlist_validate
SM83_VALIDATE_OBJ  = $(BUILD_TEST_DIR)/sm83_netlist_validate.o \
                     $(BUILD_CORE_DIR)/sm83/sm83_netlist_data.o \
                     $(BUILD_CORE_DIR)/sm83/sm83_netlist_sim.o \
                     $(BUILD_CORE_DIR)/sm83/sm83_semantic_map.o
SM83_VALIDATE_DEP  = $(SM83_VALIDATE_OBJ:.o=.d)

SIMPLE_DEP = $(SIMPLE_OBJ:.o=.d)
HW_DEP     = $(HW_OBJ:.o=.d)
VEC_DEP    = $(VEC_OBJ:.o=.d)
TESTER_DEP = $(TESTER_OBJ:.o=.d)
COMPAT_DEP = $(COMPAT_OBJ:.o=.d)
GBA_DEP    = $(GBA_CORE_DEP) $(GBA_UI_DEP)
DEP        = $(APP_DEP) $(CORE_DEP) $(UI_DEP) $(SIMPLE_DEP) $(HW_DEP) $(VEC_DEP) $(TESTER_DEP) $(COMPAT_DEP) $(GBA_DEP) $(SM83_VALIDATE_DEP)

$(NAME) : $(OBJ)
	$(info LD $@)
	$(CC_CXX) -o $@ $^ $(LDFLAGS)

$(SIMPLE_NAME) : $(SIMPLE_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ `pkg-config --libs sdl3` -lGL -lm

$(HW_NAME) : $(HW_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ `pkg-config --libs sdl3` -lGL -lm

$(VEC_NAME) : $(VEC_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ `pkg-config --libs sdl3` -lGL -lm

$(TESTER_NAME) : $(TESTER_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ -lpthread -lGL -lm

$(COMPAT_NAME) : $(COMPAT_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ -lpthread -lGL -lm

$(SM83_VALIDATE_NAME) : $(SM83_VALIDATE_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ -lm

$(GBA_NAME) : $(GBA_OBJ)
	$(info LD $@)
	$(CC_CXX) -o $@ $^ $(LDFLAGS) -lpthread -lm

$(GBA_COMPAT_NAME) : $(GBA_COMPAT_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ -lpthread -lm

-include $(DEP)

$(BUILD_CORE_DIR)/%.o: %.c
	$(info CC $@)
	mkdir -p $(dir $@)
	$(CC_C) -c $(CFLAGS) -o $@ $<

$(BUILD_APP_DIR)/%.o: %.c
	$(info CC $@)
	mkdir -p $(dir $@)
	$(CC_C) -c $(CFLAGS) -o $@ $<

$(BUILD_APP_DIR)/%.o: frontends/%.c
	$(info CC $@)
	mkdir -p $(dir $@)
	$(CC_C) -c $(CFLAGS) -o $@ $<

$(BUILD_TEST_DIR)/%.o: %.c
	$(info CC $@)
	mkdir -p $(dir $@)
	$(CC_C) -c $(CFLAGS) -o $@ $<

$(BUILD_UI_DIR)/%.o: ui/%.cpp
	$(info CXX $@)
	mkdir -p $(dir $@)
	$(CC_CXX) -c $(CXXFLAGS) -o $@ $<

$(BUILD_UI_DIR)/imgui/%.o: imgui/%.cpp
	$(info CXX $@)
	mkdir -p $(dir $@)
	$(CC_CXX) -c $(CXXFLAGS) -o $@ $<

$(BUILD_UI_DIR)/imgui/backends/%.o: imgui/backends/%.cpp
	$(info CXX $@)
	mkdir -p $(dir $@)
	$(CC_CXX) -c $(CXXFLAGS) -o $@ $<

$(BUILD_GBA_DIR)/%.o: gba/%.c
	$(info CC $@)
	mkdir -p $(dir $@)
	$(CC_C) -c $(CFLAGS) -I gba -o $@ $<

.PHONY : clean compat-run mooneye-run game-smoke shootout-run shootout-list gba-compat-run sm83-validate
clean:
	$(info CLEAN $(NAME))
	rm -rf $(BUILD_DIR)
	rm -f *.o *.d imgui/*.o imgui/*.d imgui/backends/*.o imgui/backends/*.d
	rm -f $(NAME) $(SIMPLE_NAME) $(HW_NAME) $(VEC_NAME) $(TESTER_NAME) $(COMPAT_NAME) $(GBA_NAME) $(GBA_COMPAT_NAME) $(SM83_VALIDATE_NAME)

compat-run: $(COMPAT_NAME)
	./tests/compat/run.sh

mooneye-run: $(COMPAT_NAME)
	COMPAT_MANIFEST=tests/compat/mooneye.tsv ./tests/compat/run.sh

game-smoke: $(TESTER_NAME)
	./tests/games/run_game_smoke.py --no-build $(GAME_SMOKE_ARGS)

shootout-run: $(COMPAT_NAME)
	./tests/shootout/run_shootout.py --no-build $(SHOOTOUT_ARGS)

shootout-list:
	./tests/shootout/run_shootout.py --list $(SHOOTOUT_ARGS)

gba-compat-run: $(GBA_COMPAT_NAME)
	./tests/gba_compat/run.sh

sm83-validate: $(SM83_VALIDATE_NAME)
	./$(SM83_VALIDATE_NAME)

$V.SILENT:
