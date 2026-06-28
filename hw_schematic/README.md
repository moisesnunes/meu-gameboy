# hw_schematic/ — Visualizador do esquemático de hardware do DMG

Esta pasta implementa um visualizador interativo do esquemático elétrico real do Game Boy DMG (revisão DMG-CPU-06), com animação ao vivo dos sinais do emulador sobre os fios do circuito.

Assim como o `sm83/`, este módulo é **puramente visual** — não altera emulação, timing ou compatibilidade. É renderizado pelo painel "HW Schematic" na UI de debug.

---

## O que é e por que existe

O esquemático é o diagrama elétrico completo da placa do DMG: mostra o SoC (U1), a RAM (U2/U3), o conector de cartucho, o LCD, o amplificador de áudio, o clock de cristal e todas as conexões entre eles.

A ideia é que, enquanto um jogo roda, você veja **em tempo real** quais fios estão ativos — barramento de endereços piscando durante fetch, dados se propagando da RAM, strobes de /RD e /WR, interrupções, etc. É uma forma de entender o hardware físico olhando para o circuito real, não para código.

A fonte do esquemático é o projeto [gb-schematics](https://github.com/Gekkio/gb-schematics) de Joonas Javanainen (Gekkio), licença CC BY 4.0. Os dados são extraídos do KiCad via `tools/kicad_extractor.py`.

---

## Arquivos

| Arquivo | Função |
|---|---|
| `hw_schematic_data.c/h` | Dados estáticos auto-gerados: 15 componentes, 411 segmentos de fio, 98 nets, 180 labels e 19 junções. Coordenadas normalizadas [0,1] x [0,1] a partir do papel A4 KiCad. Gerado por `tools/kicad_extractor.py`. **Não editar manualmente.** |
| `hw_schematic_map.c/h` | Mapeamento semântico: associa cada net do KiCad (ex: `"A0"`, `"PHI"`) a um subsistema do emulador (`HwSignalKind`, `HwComponentKind`) com nível de confiança (`CONFIRMED`, `PROBABLE`, `PROXY`, `UNKNOWN`). Não afeta emulação. |
| `hw_schematic_trace.c/h` | Camada de projeção de eventos: recebe `gb_hw_trace_event` do ring buffer do emulador e traduz para intensidades de fade por net e por componente. O hot path é O(nets+componentes), chamado uma vez por frame. |
| `hw_schematic_view.c/h` | Transformadas de coordenada (pan/zoom), carregamento da textura de background (imagem do esquemático ou da placa real), cache de projeção por frame. Único módulo com chamadas OpenGL — excluído dos builds headless. |
| `hw_schematic_pins.c/h` | Tabela dos 80 pinos físicos do U1 (SoC): mapeia número de pino → net_id. Usado para tooltips e highlight de wire-to-pin. |
| `hw_schematic_graph.c/h` | Grafo de adjacência sobre `hw_wires[]`: constrói uma lista de vizinhos por segmento de fio para que a animação de "fluxo" se propague fisicamente pelas conexões reais em vez de acender todos os segmentos de uma net de uma vez. BFS limitado estaticamente. |
| `nanosvg.h` / `nanosvgrast.h` | Bibliotecas header-only para parsear e rasterizar SVG (usadas para carregar a imagem de background quando não há JPG/PNG disponível). |

---

## Pipeline de dados

```
Emulador (gb->cpu, bus, IRQ)
    │
    ▼
debug.h ring buffer (gb_hw_trace_event)
    │
    ▼
hw_schematic_trace.c  — projeta evento → net_fade[], comp_fade[]
    │
    ▼
hw_schematic_view.c   — coordenadas de tela, culling
    │
    ▼
debug_ui_panels.cpp   — desenha fios, componentes, labels com cor/alpha animados
```

---

## Grupos de animação

As nets são divididas em grupos para colorização distinta:

| Grupo | Nets |
|---|---|
| `addr` | A0..A15 — barramento de endereços |
| `data` | D0..D7 — barramento de dados |
| `wram_data` | MD0..MD7 — dados internos da WRAM |
| `wram_addr` | MA0..MA12 — endereços internos da WRAM |
| `clock` | PHI — clock de 4 MHz |
| `audio` | SO1, SO2, VEE |
| `lcd` | sinais de pixel e controle do LCD |
| `irq` | linhas de interrupção |
| `power` | VCC, GND, VIN, VDD |
| `serial` | SCK, SIN, SOUT |
| `bus_ctrl` | /RD, /WR, /CS |

---

## Confiança do mapeamento

Nem todos os nets do KiCad têm mapeamento certo ainda:

- **CONFIRMED** — verificado contra schematics ou fotografias do die
- **PROBABLE** — inferido pelo nome do net ou posição no barramento
- **PROXY** — agrupamento visual útil, mas não é o net físico direto
- **UNKNOWN** — presente no esquemático, sem mapeamento ainda

Nets com confiança `PROXY` ou `UNKNOWN` recebem intensidade visual menor na animação.
