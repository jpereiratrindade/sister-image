# Arquitetura e proveniência

## Decisão 001 — fronteira exclusiva de imagens

O novo componente extrai os algoritmos raster de `obce_gui` e mantém uma cópia
local auditável. Não depende do checkout irmão para compilar ou qualificar.
Não carrega telas CSI, modelos de linguagem, SDL2 ou ImGui.

Fontes iniciais:

- `obce_gui`, commit `ee0153e73419b7807e79e9d2633a7df76821e229`;
- `obce`, commit `41a7ae24a9e2d7d1080e4a3cc364739e93d2457f`;
- caminhos e hashes exatos capturados em `upstream.json`, inclusive se os
  arquivos de trabalho diferissem do commit de referência;
- cpp-httplib 0.42.0 e nlohmann/json importados de cópias já disponíveis no
  workspace, com licenças mantidas. Ver `vendor/`.

## Decisão 002 — núcleo e adaptadores

`RasterWindowAnalysis.cpp` é a base numérica extraída. `service.cpp` orquestra
classificação, preview e proveniência sem depender de HTTP. `main.cpp` é o
adaptador HTTP, catálogo de capacidades e agendamento. `web/` apenas apresenta
parâmetros, estado e pixels calculados pelo servidor.

O subconjunto NormalityModel/RegimeStateMachine está preservado em `vendor/obce`
para compatibilidade da API raster herdada. O fluxo web desativa explicitamente
a trilha de anomalias e trabalha em uma única escala por execução; evita
normalização adaptativa e construção de regiões de anomalia que não são usadas
pelos dois modos de classificação.

## Decisão 003 — memória e processamento previsíveis

Mantêm-se doubles nos acumuladores para reduzir erro de soma. Características
são três valores contíguos. A classificação de manchas substitui mapas em
árvore e filas com nós por índices numa grade e um vetor de busca reutilizado.

A exportação mantém apenas uma linha de pixels e spans de janelas ativos.
Resultado completo não é copiado para a UI: a prévia PGM tem até 1024² bytes.
A API de download usa file streaming do cpp-httplib. Não existe cache global de
imagens nem base64. A thread científica única evita multiplicar as alocações
por uploads concorrentes. Os demais handlers permanecem disponíveis.

Além das estruturas próprias, libtiff e codecs mantêm seus buffers internos;
O(largura + janelas) descreve o algoritmo e não é garantia absoluta de RSS.
Limites de entrada e buffers são conferidos antes de alocações próprias.

## Decisão 004 — custódia e autorização

O subsistema controla apenas suas entradas, resultados e evidências. Não se
aprova para o ecossistema, não decide ações em outros participantes e não
modifica autoridade de instalação. A identidade semântica é separada do binding.
O contrato ARC-01 permanece DRAFT até promoção normativa externa.

Local DEV é opt-in. Modo proxy é o padrão do binário e exige segredo externo.
As rotas de identidade nunca fabricam um usuário local e negam solicitações
sem mediação. Origens de navegador divergentes são rejeitadas para operações
protegidas. Credenciais não ficam no código nem na interface.
