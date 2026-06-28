NAME = gameboy

CC_C   = gcc
CC_CXX = g++

# Detecta compilação para Windows via variável de ambiente (MSYS2/MinGW) ou
# nome do compilador passado explicitamente (ex: make CC_C=x86_64-w64-mingw32-gcc)
ifeq ($(OS),Windows_NT)
    WINDOWS := 1
else ifneq ($(findstring mingw,$(CC_C)),)
    WINDOWS := 1
else ifneq ($(findstring w64,$(CC_C)),)
    WINDOWS := 1
endif

BUILD_DIR        = build
# objetos do núcleo GB/GBC compartilhados entre frontends
BUILD_CORE_DIR   = $(BUILD_DIR)/core
# objetos dos frontends (main, sdl, state)
BUILD_APP_DIR    = $(BUILD_DIR)/app
# binários de teste/compat isolados do app principal
BUILD_TEST_DIR   = $(BUILD_DIR)/test
# ImGui + painéis de debug (C++ — compilador separado)
BUILD_UI_DIR     = $(BUILD_DIR)/ui
# núcleo GBA (inclui próprio subdiretório gba/)
BUILD_GBA_DIR    = $(BUILD_DIR)/gba

# -MMD -MP: gera arquivos .d de dependências automáticas para headers;
# sem isso, mudar um .h não recompila os .c que o incluem
CFLAGS   = -Wall -O2 -MMD -MP `pkg-config --cflags sdl3` -I . -I gb -I ui -I sm83 -I hw_schematic
CXXFLAGS = -Wall -O2 -MMD -MP `pkg-config --cflags sdl3` -I . -I gb -I ui -I imgui -I imgui/backends -I sm83 -I hw_schematic

ifdef WINDOWS
    NAME     = gameboy.exe
    GBA_NAME = meu-gba.exe
    # -lopengl32: equivalente Windows de -lGL; -lmingw32 requerido antes de SDL no MinGW
    # -mwindows: subsistema GUI — elimina a janela de console; remova para ver stdout/stderr
    LDFLAGS  = `pkg-config --libs sdl3` -lopengl32 -lmingw32 -mwindows
else
    LDFLAGS  = `pkg-config --libs sdl3` -lGL
endif

# ImGui: apenas os módulos necessários; imgui_demo.cpp é omitido intencionalmente
IMGUI_SRC = imgui/imgui.cpp \
            imgui/imgui_draw.cpp \
            imgui/imgui_tables.cpp \
            imgui/imgui_widgets.cpp \
            imgui/backends/imgui_impl_sdl3.cpp \
            imgui/backends/imgui_impl_opengl3.cpp

# Todos os fontes do frontend principal (GB/GBC com SDL3 + ImGui)
C_SRC = main.c sdl.c state.c \
        gb/cpu.c gb/memory.c gb/cart.c gb/gpu.c gb/sync.c gb/input.c gb/irq.c gb/dma.c \
        gb/hdma.c gb/timer.c gb/spu.c gb/debug.c gb/disasm.c gb/rtc.c

# Painéis de debug ImGui do emulador GB (C++ para compatibilidade com ImGui)
UI_SRC = ui/debug_ui.cpp ui/debug_ui_config.cpp ui/debug_ui_actions.cpp \
         ui/debug_ui_menus.cpp ui/debug_ui_panels.cpp ui/lcd_shader.cpp

# Simulação de netlist do SM83: dados estáticos + simulador + visualizador de die
SM83_C_SRC = sm83/sm83_netlist_data.c sm83/sm83_die_view.c sm83/sm83_signal_overlay.c \
             sm83/sm83_netlist_sim.c sm83/sm83_semantic_map.c

# Visualizador de esquemático de hardware (grafo, pinos, rastreamento de sinais)
HW_SCH_C_SRC = hw_schematic/hw_schematic_data.c hw_schematic/lcd_schematic_data.c \
               hw_schematic/hw_schematic_dataset.c \
               hw_schematic/hw_schematic_view.c \
               hw_schematic/hw_schematic_map.c hw_schematic/hw_schematic_trace.c \
               hw_schematic/hw_schematic_pins.c hw_schematic/hw_schematic_graph.c \
               hw_schematic/lcd_schematic_map.c

# Núcleo GB/GBC: compartilhado por todos os frontends e ferramentas de teste
# (exclui sdl.c e state.c que dependem de SDL3/ImGui)
CORE_C_SRC = gb/cpu.c gb/memory.c gb/cart.c gb/gpu.c gb/sync.c gb/input.c gb/irq.c gb/dma.c \
             gb/hdma.c gb/timer.c gb/spu.c gb/debug.c gb/disasm.c gb/rtc.c \
             $(SM83_C_SRC) $(HW_SCH_C_SRC)

# Núcleo headless: exclui hw_schematic_view.c (único módulo com chamadas OpenGL)
# Usado por compat_test e rom_tester que rodam sem janela/GPU
HEADLESS_CORE_C_SRC = $(filter-out hw_schematic/hw_schematic_view.c,$(CORE_C_SRC))

FRONTENDS_DIR = frontends

# Substituição de sufixo: mapeia cada .c/.cpp para o .o no diretório de build correto
CORE_OBJ         = $(CORE_C_SRC:%.c=$(BUILD_CORE_DIR)/%.o)
HEADLESS_CORE_OBJ = $(HEADLESS_CORE_C_SRC:%.c=$(BUILD_CORE_DIR)/%.o)
APP_OBJ  = $(BUILD_APP_DIR)/main.o $(BUILD_APP_DIR)/sdl.o $(BUILD_APP_DIR)/state.o
UI_FRONTEND_OBJ = $(UI_SRC:ui/%.cpp=$(BUILD_UI_DIR)/%.o)
IMGUI_OBJ       = $(IMGUI_SRC:%.cpp=$(BUILD_UI_DIR)/%.o)
UI_OBJ          = $(UI_FRONTEND_OBJ) $(IMGUI_OBJ)
OBJ      = $(APP_OBJ) $(CORE_OBJ) $(UI_OBJ)

# Arquivos .d gerados por -MMD; incluídos ao final para rastrear deps de headers
APP_DEP  = $(APP_OBJ:.o=.d)
CORE_DEP = $(CORE_OBJ:.o=.d)
UI_DEP   = $(UI_OBJ:.o=.d)

# Núcleo GBA: CPU ARM7TDMI (ARM + Thumb), memória, GPU, APU, DMA, timers, cartucho, IRQ
GBA_CORE_C_SRC = gba/gba.c gba/gba_cpu.c gba/gba_cpu_arm.c gba/gba_cpu_thumb.c \
                 gba/gba_memory.c gba/gba_gpu.c gba/gba_apu.c gba/gba_dma.c \
                 gba/gba_timer.c gba/gba_cart.c gba/gba_irq.c gba/gba_sync.c \
                 gba/gba_input.c gba/gba_debug.c gba/gba_disasm.c

GBA_CORE_OBJ = $(GBA_CORE_C_SRC:gba/%.c=$(BUILD_GBA_DIR)/%.o)
GBA_CORE_DEP = $(GBA_CORE_OBJ:.o=.d)
# Painéis de debug ImGui específicos do GBA (separados do GB para não misturar estados)
GBA_UI_SRC = ui/gba_debug_ui.cpp ui/gba_debug_ui_config.cpp ui/gba_debug_ui_panels.cpp
GBA_UI_OBJ = $(GBA_UI_SRC:ui/%.cpp=$(BUILD_UI_DIR)/%.o)
GBA_UI_DEP = $(GBA_UI_OBJ:.o=.d)

# GBA_NAME definido condicionalmente: .exe no Windows, sem extensão no Linux/macOS
ifndef WINDOWS
GBA_NAME = meu-gba
endif
GBA_OBJ  = $(BUILD_GBA_DIR)/gba_main.o $(GBA_CORE_OBJ) $(GBA_UI_OBJ) $(IMGUI_OBJ)

# gba_compat_test: roda ROMs de teste sem SDL (sem janela, saída via serial/memória)
GBA_COMPAT_NAME = gba_compat_test
GBA_COMPAT_OBJ  = $(BUILD_TEST_DIR)/gba_compat_test.o $(GBA_CORE_OBJ)

# gba_memory_test: invariants of debug peeks and byte-sized DMA I/O accesses
GBA_MEMORY_TEST_NAME = gba_memory_test
GBA_MEMORY_TEST_OBJ  = $(BUILD_TEST_DIR)/gba_memory_test.o $(GBA_CORE_OBJ)
GBA_MEMORY_TEST_DEP  = $(GBA_MEMORY_TEST_OBJ:.o=.d)

# Frontend minimalista: sem ImGui, útil para profiling e testes headless rápidos
SIMPLE_NAME = gameboy-simple
SIMPLE_OBJ  = $(BUILD_APP_DIR)/main_simple.o $(CORE_OBJ)

# Frontend com renderização vetorial (sem rasterização tile nativa)
VEC_NAME = gameboy-vector
VEC_OBJ  = $(BUILD_APP_DIR)/main_vector.o $(CORE_OBJ)

# Versão vetorial para GBA (protótipo, sem núcleo GBA completo)
GBA_VEC_NAME = gameboy-advance-vector
GBA_VEC_OBJ  = $(BUILD_APP_DIR)/main_gba_vector.o

# rom_tester: executa ROMs em lote e verifica saída esperada (CI/automação)
TESTER_NAME = rom_tester
TESTER_OBJ  = $(BUILD_TEST_DIR)/rom_tester.o $(HEADLESS_CORE_OBJ)

# compat_test: suite de compatibilidade GB/GBC (blargg, mooneye, etc.)
COMPAT_NAME = compat_test
COMPAT_OBJ  = $(BUILD_TEST_DIR)/compat_test.o $(HEADLESS_CORE_OBJ)

# sm83_netlist_validate: compara netlist simulada contra comportamento esperado do SM83
# usa apenas os módulos de simulação, sem GPU/APU/SDL
SM83_VALIDATE_NAME = sm83_netlist_validate
SM83_VALIDATE_OBJ  = $(BUILD_TEST_DIR)/sm83_netlist_validate.o \
                     $(BUILD_CORE_DIR)/sm83/sm83_netlist_data.o \
                     $(BUILD_CORE_DIR)/sm83/sm83_netlist_sim.o \
                     $(BUILD_CORE_DIR)/sm83/sm83_semantic_map.o
SM83_VALIDATE_DEP  = $(SM83_VALIDATE_OBJ:.o=.d)

SIMPLE_DEP  = $(SIMPLE_OBJ:.o=.d)
VEC_DEP     = $(VEC_OBJ:.o=.d)
GBA_VEC_DEP = $(GBA_VEC_OBJ:.o=.d)
TESTER_DEP = $(TESTER_OBJ:.o=.d)
COMPAT_DEP = $(COMPAT_OBJ:.o=.d)
GBA_DEP    = $(GBA_CORE_DEP) $(GBA_UI_DEP)
# DEP agrega todos os .d de todos os alvos; -include abaixo os carrega sem erro se ausentes
DEP        = $(APP_DEP) $(CORE_DEP) $(UI_DEP) $(SIMPLE_DEP) $(VEC_DEP) $(GBA_VEC_DEP) $(TESTER_DEP) $(COMPAT_DEP) $(GBA_DEP) $(SM83_VALIDATE_DEP) $(GBA_MEMORY_TEST_DEP)

$(NAME) : $(OBJ)
	$(info LD $@)
	$(CC_CXX) -o $@ $^ $(LDFLAGS)

$(SIMPLE_NAME) : $(SIMPLE_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ $(LDFLAGS) -lm

$(VEC_NAME) : $(VEC_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ $(LDFLAGS) -lm

$(GBA_VEC_NAME) : $(GBA_VEC_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ `pkg-config --libs sdl3` -lm

$(TESTER_NAME) : $(TESTER_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ -lpthread -lm

$(COMPAT_NAME) : $(COMPAT_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ -lpthread -lm

$(SM83_VALIDATE_NAME) : $(SM83_VALIDATE_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ -lm

$(GBA_NAME) : $(GBA_OBJ)
	$(info LD $@)
	$(CC_CXX) -o $@ $^ $(LDFLAGS) -lpthread -lm

$(GBA_COMPAT_NAME) : $(GBA_COMPAT_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ -lpthread -lm

$(GBA_MEMORY_TEST_NAME) : $(GBA_MEMORY_TEST_OBJ)
	$(info LD $@)
	$(CC_C) -o $@ $^ -lpthread -lm

-include $(DEP)

$(BUILD_CORE_DIR)/%.o: %.c
	$(info CC $@)
	mkdir -p $(dir $@)
	$(CC_C) -c $(CFLAGS) -o $@ $<

$(BUILD_CORE_DIR)/gb/%.o: gb/%.c
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

$(BUILD_TEST_DIR)/%.o: tests/%.c
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

# -I gba: expõe headers internos do GBA (gba.h, etc.) sem poluir o namespace global
$(BUILD_GBA_DIR)/%.o: gba/%.c
	$(info CC $@)
	mkdir -p $(dir $@)
	$(CC_C) -c $(CFLAGS) -I gba -o $@ $<

.PHONY : clean compat-run blargg-run mooneye-run game-smoke gba-game-smoke shootout-run shootout-list gba-compat-run gba-memory-test sm83-validate
clean:
	$(info CLEAN $(NAME))
	rm -rf $(BUILD_DIR)
	rm -f *.o *.d imgui/*.o imgui/*.d imgui/backends/*.o imgui/backends/*.d
	rm -f $(NAME) $(SIMPLE_NAME) $(VEC_NAME) $(GBA_VEC_NAME) $(TESTER_NAME) $(COMPAT_NAME) $(GBA_NAME) $(GBA_COMPAT_NAME) $(GBA_MEMORY_TEST_NAME) $(SM83_VALIDATE_NAME)

# --no-build: evita recompilar dentro do script Python; o Make já garantiu o binário
compat-run: $(COMPAT_NAME)
	./tests/compat/run_compat.py --no-build $(COMPAT_ARGS)

blargg-run: $(COMPAT_NAME)
	COMPAT_SUITE=blargg ./tests/compat/run_compat.py --no-build $(COMPAT_ARGS)

mooneye-run: $(COMPAT_NAME)
	./tests/compat/run_compat.py --no-build --manifest tests/compat/mooneye.tsv $(COMPAT_ARGS)

game-smoke: $(TESTER_NAME)
	./tests/games/run_game_smoke.py --no-build $(GAME_SMOKE_ARGS)

gba-game-smoke: $(GBA_COMPAT_NAME)
	./tests/games/run_gba_game_smoke.py --no-build $(GBA_GAME_SMOKE_ARGS)

gba-memory-test: $(GBA_MEMORY_TEST_NAME)
	./$(GBA_MEMORY_TEST_NAME)

shootout-run: $(COMPAT_NAME)
	./tests/shootout/run_shootout.py --no-build $(SHOOTOUT_ARGS)

# shootout-list não precisa compilar; apenas lista os ROMs disponíveis
shootout-list:
	./tests/shootout/run_shootout.py --list $(SHOOTOUT_ARGS)

gba-compat-run: $(GBA_COMPAT_NAME)
	./tests/gba_compat/run.sh

sm83-validate: $(SM83_VALIDATE_NAME)
	./$(SM83_VALIDATE_NAME)

# $V.SILENT suprime o eco de comandos quando V está vazio (make padrão silencioso)
$V.SILENT:
