# Validação — 2026-09-08

## Testes automatizados

Build Release com C++20, libtiff e OpenSSL.

- `image_core`: classes e manchas com valores conhecidos, leitor de Shapefile (SHP), KML, KMZ, GeoJSON e teste de recorte de GeoTIFF por polígonos.
- `image_geotiff`: preservação de escala, tiepoints e GeoKeys em exportação e recorte.
- `image_http`: schemas HTTP canônicos, prévia da imagem original PGM (`original_preview.pgm`), parse de vetores (`/api/shapes/parse`), recorte assíncrono (`/api/jobs/{id}/clip`), autenticação, upload binário streaming, integridade SHA256 e exclusão de tarefas.
- `image_runtime`: consumo de binding, diretórios DEV isolados, idempotência, saúde e prontidão.

Todos os 4 testes passaram com 100% de sucesso.

## Experimento de CPU e memória

- Upload streaming direto para disco sem buffer integral.
- Decoder libtiff limitado a 64 MiB com mmap desativado.
- Recorte vetorial O(scanline) com buffer de linha O(largura).
