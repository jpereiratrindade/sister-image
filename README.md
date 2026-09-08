# SisTer Image

Classificação **não supervisionada de regiões de imagens TIFF/GeoTIFF**, com motor
C++20 e interface web. Projeto independente extraído do módulo raster do OBCE.

## Executar localmente

Requisitos: CMake >= 3.20, Ninja, compilador C++20, libtiff >= 4.5 e OpenSSL com headers,
Python 3 e `jsonschema` para testes e integração Infra. Bibliotecas header-only
estão versionadas em `vendor/`; o build não baixa dependências.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 2
ctest --test-dir build --output-on-failure
./scripts/dev.sh start
```

Abra **http://127.0.0.1:8096**. Para outra porta, use `SISTER_IMAGE_PORT=...` em
todas as ações da sessão. `./scripts/dev.sh stop` encerra o processo; `status`,
`health`, `readiness` e `restart` também estão disponíveis.

1. Envie um TIFF ou escolha **Experimentar com imagem de exemplo**.
2. Escolha **Classes de intensidade e textura** ou **Manchas homogêneas**.
3. Ajuste o tamanho das janelas e os parâmetros de classificação.
4. Aguarde o processamento e visualize o mapa.
5. Baixe o GeoTIFF e o relatório JSON com parâmetros e hashes SHA256.

Os arquivos persistem fora do código, por padrão em
`~/.local/state/sister-image/data`. A lista de execuções permite consultar e
excluir explicitamente imagens e resultados antigos.

## Escopo científico

- Características: média de intensidade, desvio padrão e diferença absoluta
  horizontal entre pixels válidos, por janela espacial.
- Classes: agrupamento determinístico tipo k-means, até 24 iterações, com
  normalização das três características e centros iniciais por quantis.
- Manchas: componentes conectados por vizinhança de quatro direções e distância
  entre características normalizadas. A união é transitiva entre vizinhos;
  não garante um limite de distância entre todos os pixels da mancha.
- As classes são relativas à imagem analisada. **Não são rótulos semânticos**
  como solo, vegetação ou água. Não há treinamento supervisionado por exemplos,
  perceptron de imagem inteira, CNN ou LLM neste produto.
- Os tons das manchas codificam sua intensidade média; manchas distintas podem
  compartilhar tons. O mapa é uma visualização, não um raster de IDs únicos.

Formatos: TIFF/BigTIFF uint8 contíguo, RGB/RGBA ou cinza/cinza+alpha, orientação
superior esquerda. Alpha deve ser não associado e declarado. Outros formatos,
16 bits, float, paleta e outras orientações são rejeitados explicitamente.
Pixels transparentes são ignorados; zeros e um valor adicional sem dados são
configuráveis. O valor adicional é comparado à intensidade luminosa em cinza.
Tags GDAL_NODATA não são interpretadas automaticamente.

A exportação BigTIFF usa compressão LZW, dimensões originais e as tags GeoTIFF
básicas de escala, tiepoints, transformação e GeoKeys do módulo original. Não
reprojeta, não ressampleia o mapa exportado e não copia metadados arbitrários.
Janelas ignoradas são zero; áreas inválidas dentro de uma janela válida recebem
a classe da janela. O preview usa no máximo 1024 pixels por lado.

## CPU e memória

- Upload TIFF binário transmitido diretamente ao disco, sem base64 e sem
  materializar o corpo inteiro da imagem na memória do servidor.
- Mapeamento integral de arquivo da libtiff desativado; alocações do decoder limitadas a 64 MiB por chamada.
- Leitura por scanline/tile; acumuladores contíguos por janela.
- Uma tarefa científica por instância; pool HTTP fixo de quatro threads e fila
  limitada, mantendo saúde e consulta disponíveis durante o processamento.
- Exportação por linha: buffer de pixels O(largura), substituindo o buffer
  O(largura × altura) do OBCE. Metadados e índices são O(número de janelas).
- Manchas usam grade contígua e fila BFS reutilizada; sem árvores ou alocações
  de vizinhos por janela. A textura mantém continuidade entre tiles.
- Sem GPU, runtime Python de ML, cópia integral RGB ou framework frontend.

Limites: upload 1 GiB; até 4 bilhões de pixels; lado até 1 milhão; buffer de
leitura até 64 MiB; 262144 janelas; 32 execuções retidas. A admissão bloqueia
quando o histórico já supera 8 GiB ou há menos de 6 GiB livres. A tarefa corrente
pode ultrapassar o limiar de histórico; isso é um gate de admissão, não quota
rígida de filesystem. Aumente a janela para reduzir o número de regiões.

Benchmark reprodutível (diretório de saída deve ser novo):

```bash
cmake --build build --target image_benchmark -j 2
/usr/bin/time -v ./build/image_benchmark /caminho/ortofoto.tif /tmp/image-benchmark-novo
```

O benchmark lê o arquivo por symlink e escreve somente no diretório indicado.
Veja [arquitetura e proveniência](docs/architecture.md) e [validação](docs/validation.md).

## Integração SisTer / sister-infra

`.sister/component.json` declara `component_id=image`, `system_id=sister_image`,
build CMake/Ninja, CTest, artefato, superfície de interação e runtime
`persistent-external`. O componente não contém configuração de gateway, domínio,
TLS ou auto-admissão. O manifesto HTTP tem seu endpoint interno derivado do
binding no início da execução.

```bash
../sister-infra/bin/sister-component inspect .
../sister-infra/bin/sister-component qualify .
../sister-infra/bin/sister-infra dev preview . --duration 30
```

Antes de iniciar o preview, encerre uma sessão local com `./scripts/dev.sh stop`.
O Infra atual exporta o binário qualificado para `build/`, o que requer que
esse executável não esteja em uso.

O preview recebe binding e diretórios exclusivos do Infra. Consome todos os
marcadores `SISTER_RUNTIME_*`, confere identidade de PID e não reutiliza dados
ou processos LAB. `start` não compila, `start/stop` são idempotentes e releases
não recebem estado persistente.

Endpoints canônicos: `/manifest`, `/health`, `/ready`, `/capabilities`,
`/identity` e `/echo`. Capacidade: `image.regions.classify`.

O binding HTTP segue `sister.subsystem/1.0.0`. A declaração semântica separada em
`contracts/participant.draft.json` segue `sister.participant/2.0.0`, explicitamente
**DRAFT e não normativa para runtime**, conforme o estado atual do ARC-01.

DEV local é ativado explicitamente por `scripts/dev.sh` ou pelo preview. Fora
desse modo, operações de domínio requerem `X-Sister-Proxy-Token`, comparado ao
segredo externo `SISTER_IMAGE_PROXY_TOKEN` (mínimo 32 caracteres). `/identity` e
`/echo` sempre exigem mediação autenticada, inclusive em DEV. Usa o binding
histórico de proxy-token do contrato HTTP; não implementa o perfil adicional
de asserções Ed25519/JWT do WP-05. Não possui elegibilidade produtiva.

A integração operacional em LAB exige inclusão na composição autoritativa,
binding pelo resolvedor e segredo provisionado pelo operador; o código não
altera essas autoridades. Consulte [integração](docs/integration.md).

## API de imagens

- `POST /api/classify?window=128&mode=classes&classes=5`: corpo TIFF binário,
  `Content-Type: image/tiff`; retorna 202 com ID da tarefa.
- `POST /api/demo`: executa uma imagem sintética reproduzível.
- `GET /api/jobs`: histórico e indicação de tarefa ativa.
- `GET /api/jobs/{id}`: status, parâmetros e resultado ou erro.
- `GET /api/jobs/{id}/map.tif`, `/preview.pgm`, `/report.json`: resultados concluídos.
- `DELETE /api/jobs/{id}`: exclui entrada e resultados de uma tarefa encerrada.

Parâmetros adicionais: `homogeneity=0.85`, `min_valid=0.25`,
`ignore_zero=true|false`, `nodata=0..255` (opcional).
O resultado é persistido atomicamente em JSON. Tarefas interrompidas são
marcadas como falhas ao reiniciar e nunca reexecutadas automaticamente.
