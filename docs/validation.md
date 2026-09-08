# Validação — 2026-09-08

## Testes automatizados

Build Release com GCC 16.2.1, C++20, libtiff 4.7.2 e OpenSSL 3.5.8.

- `image_core`: classes e manchas com valores conhecidos, continuidade de textura
  entre tiles, equivalência tile/strip, rejeição de 16 bits e exclusão por no-data.
- `image_geotiff`: preservação de escala, tiepoints e GeoKeys e alpha em cinza.
- `image_http`: schemas HTTP canônicos, autenticação e negação de origem cruzada,
  classificação assíncrona, upload binário, preview, integridade SHA256, erro de
  arquivo inválido, histórico, exclusão e desligamento gracioso.
- `image_runtime`: consumo de binding, diretórios DEV isolados, idempotência,
  saúde, prontidão e recusa a sinalizar PID alheio.

Todos passaram. A declaração semântica DRAFT também foi validada com o schema
local de `sister.participant/2.0.0`.

Navegador Chromium headless: demonstração concluída, canvas preenchido e layout
sem overflow horizontal em 1440 × 1100 e 390 × 844; nenhum erro JavaScript.

## Experimento de CPU e memória

Imagem local: `obce_gui/images/odm_orthophoto.tif`, cerca de 362 MiB,
18401 × 18238 pixels. Janela 512, 7 classes, 825 janelas classificadas.

| Versão experimental | Pico RSS | Tempo total |
|---|---:|---:|
| Exportação por linha, mmap padrão da libtiff | 377112 KiB | 2,98 s |
| Exportação por linha, leitura sem mmap | 9756 KiB | 2,94 s |

As duas versões produziram o **mesmo SHA256 de saída**. Esta comparação isola o
mapeamento de arquivo da libtiff; não é uma comparação integral com o executável
original do OBCE. A segunda execução usou cache de filesystem aquecido; tempos
não constituem benchmark estatístico. RSS é do processo CLI de classificação,
não do servidor HTTP, navegador ou page cache do sistema.

Relatório e saídas de `/usr/bin/time -v` estão neste diretório. A medição inclui
exportação e geração de preview; o tempo total externo inclui também hashes.

## Integração operacional

Inspeção pelo `sister-component` passou. Qualificação isolada e DEV Preview são
registrados na conclusão deste incremento. O LAB não é alterado pelos testes.
