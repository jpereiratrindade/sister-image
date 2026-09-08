# SisTer Image

Classificação **não supervisionada de regiões de imagens TIFF/GeoTIFF**, suporte a vetores **SHP, KML, KMZ e GeoJSON**, prévia da imagem original e ferramentas de **recorte de GeoTIFF por polígonos**, com motor C++20 e interface web harmonizada com a identidade visual SisTer.

## Executar localmente

Requisitos: CMake >= 3.20, Ninja, compilador C++20, libtiff >= 4.5 e OpenSSL com headers,
Python 3 e `jsonschema` para testes e integração Infra. Bibliotecas header-only
estão versionadas em `vendor/`; o build não baixa dependências.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
./scripts/dev.sh start
```

Abra **http://127.0.0.1:8096**. Para outra porta, use `SISTER_IMAGE_PORT=...` em
todas as ações da sessão. `./scripts/dev.sh stop` encerra o processo; `status`,
`health`, `readiness` e `restart` também estão disponíveis.

1. Envie um TIFF ou escolha **Experimentar com imagem de exemplo**.
2. Escolha **Classes de intensidade e textura** ou **Manchas homogêneas**.
3. Ajuste o tamanho das janelas e os parâmetros de classificação.
4. Visualize a **Imagem Original**, o **Mapa Classificado**, ou a **Sobreposição Vetorial**.
5. Carregue polígonos vetoriais (.shp, .kml, .kmz ou .zip) e execute o **Recorte por Shape**.
6. Baixe o GeoTIFF classificado, o GeoTIFF recortado, a prévia PGM original e o relatório JSON com parâmetros e hashes SHA256.

Os arquivos persistem fora do código, por padrão em
`~/.local/state/sister-image/data`. A lista de execuções permite consultar e
excluir explicitamente imagens e resultados antigos.

## Escopo científico e vetorial

- Características: média de intensidade, desvio padrão e diferença absoluta
  horizontal entre pixels válidos, por janela espacial.
- Classes: agrupamento determinístico tipo k-means, até 24 iterações, com
  normalização das três características e centros iniciais por quantis.
- Vetores e recortes: leitor C++ de Shapefile (.shp), KML e KMZ (Google Earth).
  Mapeamento de coordenadas espaciais para a grade de pixels GeoTIFF e recorte O(scanline)
  com re-cálculo automático das tags `ModelTiepointTag` e `ModelPixelScaleTag`.
- Manchas: componentes conectados por vizinhança de quatro direções e distância
  entre características normalizadas.
- As classes são relativas à imagem analisada. **Não são rótulos semânticos**
  como solo, vegetação ou água.
- Os tons das manchas codificam sua intensidade média; o mapa é uma visualização, não um raster de IDs únicos.

Formatos: TIFF/BigTIFF uint8 contíguo, RGB/RGBA ou cinza/cinza+alpha, orientação
superior esquerda. Prévia da imagem original gerada automaticamente em PGM (P5).

## CPU e memória

- Upload TIFF binário transmitido diretamente ao disco, sem base64 e sem
  materializar o corpo inteiro da imagem na memória do servidor.
- Mapeamento integral de arquivo da libtiff desativado; alocações do decoder limitadas a 64 MiB por chamada.
- Leitura e recorte por scanline/tile em streaming O(largura); acumuladores contíguos por janela.
- Uma tarefa científica por instância; pool HTTP fixo de quatro threads e fila
  limitada, mantendo saúde e consulta disponíveis durante o processamento.
- Sem GPU, runtime Python de ML, cópia integral RGB ou framework frontend.

Limites: upload 1 GiB; até 4 bilhões de pixels; lado até 1 milhão; buffer de
leitura até 64 MiB; 262144 janelas; 32 execuções retidas.

Benchmark reprodutível:

```bash
cmake --build build --target image_benchmark -j 2
/usr/bin/time -v ./build/image_benchmark /caminho/ortofoto.tif /tmp/image-benchmark-novo
```

Veja [arquitetura](docs/architecture.md) e [validação](docs/validation.md).

## API de imagens e vetores

- `POST /api/classify?window=128&mode=classes&classes=5`: corpo TIFF binário, `Content-Type: image/tiff`.
- `POST /api/demo`: executa uma imagem sintética reproduzível.
- `POST /api/shapes/parse`: recebe `.shp`, `.kml`, `.kmz`, `.zip` ou GeoJSON e retorna geometria GeoJSON.
- `POST /api/jobs/{id}/clip`: recorta GeoTIFF do job por máscara de polígono vetorial.
- `GET /api/jobs`: histórico e indicação de tarefa ativa.
- `GET /api/jobs/{id}`: status, parâmetros e resultado ou erro.
- `GET /api/jobs/{id}/map.tif`, `/preview.pgm`, `/original_preview.pgm`, `/clipped.tif`, `/clipped_preview.pgm`, `/report.json`: resultados concluídos.
- `DELETE /api/jobs/{id}`: exclui entrada e resultados de uma tarefa encerrada.
