# sm83/ — Simulador transistor-level do SM83

Esta pasta contém um simulador experimental do SM83 em nível de transistor. Ela é **completamente separada do emulador** — não afeta precisão de emulação, timing ou compatibilidade de jogos. É ativada apenas quando explicitamente habilitada (`sm83_sim_enabled`) e só enquanto a execução está pausada ou em step.

A fonte autorizada do estado da CPU é sempre `gb->cpu`, nunca este simulador.

---

## O que é o SM83

O SM83 é o processador customizado da Sharp/Nintendo usado no Game Boy. É parecido com um Z80/8080 simplificado mas com diferenças significativas. Este módulo usa um **netlist extraído do die físico do SM83** (via fotografia e traçado manual) para simular o comportamento transistor por transistor do chip real.

A abordagem é a mesma do projeto [Perfect6502](https://github.com/trebonian/visual6502) e do [VisualZ80](https://github.com/gdevic/Z80Explorer) — em vez de emular instrução por instrução, o simulador resolve o estado lógico de cada fio (net) do circuito com base na topologia real dos transistores.

---

## Arquivos

| Arquivo | Função |
|---|---|
| `sm83_netlist_data.c/h` | Dados estáticos auto-gerados: 9.250 transistores, 15.440 nets, 66.749 nós e 86.726 arcos extraídos do netlist KiCad do die. Gerado por `tools/jelib_extractor.py`. **Não editar manualmente.** |
| `sm83_netlist_sim.c/h` | O simulador em si. Modela cada transistor N/P como uma chave: gate HIGH → conduz (NMOS), gate LOW → conduz (PMOS). Propaga estados por relaxação iterativa até convergir. Detecta conflitos (HIGH_STRONG + LOW_STRONG no mesmo net). |
| `sm83_semantic_map.c/h` | Mapeia IDs numéricos de nets (`net@N`) para sinais do emulador com nível de confiança: `CONFIRMED`, `PROBABLE`, `PROXY` ou `UNKNOWN`. Usado para validação e auditoria — comparar o que o simulador acha com o que `gb->cpu` reporta. |
| `sm83_node_map.h` | Mapa **visual**: nome de instância → posição normalizada no die. Usado para saber *onde* cada célula lógica está fisicamente no chip, não qual net elétrico ela carrega. |
| `sm83_die_view.c/h` | Modelo de visualização: transformadas de coordenada, visibilidade por layer, culling de viewport e hit-test para o painel ImGui. Não contém lógica de simulação. |
| `sm83_signal_overlay.c/h` | Lê `gb->cpu` a cada step/pause e converte o estado dos registradores em sinais visuais sobrepostos no die (cor + fade animado). É o que faz os fios "acenderem" no visualizador. |

---

## Como funciona o simulador

```
sm83_sim_init(&sim)              — aloca e zera
sm83_sim_seed_from_gb(&sim, gb)  — copia sinais conhecidos de gb->cpu para as nets
sm83_sim_step(&sim, 64)          — propaga até estabilizar (max 64 passes)
sm83_sim_net_state(&sim, id)     — consulta estado de uma net (HIGH/LOW/FLOAT/CONFLICT)
sm83_sim_net_source(&sim, id)    — consulta por que a net tem aquele valor
sm83_sim_shutdown(&sim)          — libera
```

Estados possíveis de uma net: `UNKNOWN`, `FLOAT`, `LOW_WEAK`, `HIGH_WEAK`, `LOW` (driven), `HIGH` (driven), `CONFLICT`.

---

## Relação com o restante do emulador

- O simulador **lê** `gb->cpu` para semear as nets iniciais, mas nunca escreve de volta.
- O painel ImGui em `ui/debug_ui_panels.cpp` chama `sm83_overlay_update()` e renderiza o die.
- A validação em `tests/sm83_netlist_validate.c` compara a simulação contra comportamento esperado do SM83 e pode ser rodada via `make sm83-validate`.
