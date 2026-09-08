# Arquitetura e proveniência SisTer Image

## Decisão 001 — fronteira exclusiva de imagens e vetores

O componente SisTer Image consolida os algoritmos de análise raster e processamento vetorial sob a marca e identidade SisTer. Não depende de frameworks externos, GPU ou bibliotecas de runtime instáveis.

Fontes e inclusões:
- Módulo `sister_image` C++20 com `RasterWindowAnalysis.cpp`, `VectorShape.cpp` (Shapefile, KML, KMZ), `RasterClip.cpp` (recorte GeoTIFF por polígonos);
- `cpp-httplib` e `nlohmann/json` versionados em `vendor/`.

## Decisão 002 — núcleo, vetores e adaptadores

`RasterWindowAnalysis.cpp` e `RasterClip.cpp` formam a base numérica O(scanline). `service.cpp` orquestra a classificação, geração de prévias (imagem original e mapa classificado), recortes e proveniência SHA256 sem depender do servidor HTTP. `main.cpp` é o adaptador HTTP e catálogo de capacidades. `web/` apresenta a interface com o sistema de design SisTer (slate/emerald/cyan), abas de controle, alternância de visualização e renderização vetorial sobre o canvas.

## Decisão 003 — memória e otimização CPU/RAM

Mantêm-se leituras e escritas em streaming de scanlines O(largura). A prévia da imagem original é gerada em tempo de ingestão em PGM P5 reduzida (até 1024² pixels) para visualização rápida no navegador sem carregar a imagem completa na memória do cliente. O recorte por polígono calcula a bounding box espacial, transforma coordenadas e aplica o algoritmo de Winding Number por pixel em streaming, preservando e recalculando `ModelTiepointTag` e `ModelPixelScaleTag`.

Alocações do decoder libtiff permanecem limitadas a 64 MiB por chamada com mmap desativado.

## Decisão 004 — custódia e autorização

O subsistema controla apenas suas entradas, resultados e evidências. A identidade semântica é declarada em `contracts/participant.draft.json` e o binding HTTP em `contracts/manifest.json`.

Local DEV é opt-in. Modo proxy exige segredo externo `SISTER_IMAGE_PROXY_TOKEN`.
