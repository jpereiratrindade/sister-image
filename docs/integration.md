# Integração operacional

O componente é descoberto pelo `sister-component`; sua identidade não contém
endereço, porta ou domínio. Runtime instalado e DEV usam o mesmo entrypoint.

O runtime consome `SISTER_RESOLVED_DEPLOYMENT_FILE`, seleciona exatamente um
registro com `system_id=sister_image`, exige `runtime.transport=tcp`, listener
loopback e porta válida. PIDs, logs, dados e lock de lifecycle vêm dos diretórios
`SISTER_RUNTIME_STATE_DIR`, `SISTER_RUNTIME_RUN_DIR` e `SISTER_RUNTIME_DATA_DIR`.
DEV exige também `SISTER_RUNTIME_MODE=dev-preview`, `SISTER_RUNTIME_INSTANCE_ID`
e `SISTER_RUNTIME_CLEANUP_SCOPE=preview-only`.

Uma instância bloqueia o diretório de dados com flock; duas instâncias não
podem compartilhar dados. Antes de parar um PID, o adapter compara executável,
argumentos, tempo de início do processo e ID de instância. O processo recebe
SIGTERM para finalizar o trabalho; após o timeout operacional de 10 segundos,
o stop pode interrompê-lo. O próximo start marca tarefas incompletas como falhas.

Para a instalação decidir a participação em LAB:

1. Qualificar o repositório e seus artefatos com `sister-component qualify`.
2. Incluir `source` do projeto na composição autoritativa externa.
3. Incluir binding para `sister_image` no deployment autoritativo.
4. Provisionar o segredo de mediação no ambiente por autoridade externa.
5. Planejar pelo `sister-infra lab plan` e revisar a mudança concreta.
6. Aplicar somente no contexto autorizado da instalação.

Esses passos de instalação não são executados pelo subsistema. Os arquivos de
`~/.config/sister/workstation` e o LAB existente não são alterados por build,
testes ou preview deste repositório.

Há duas declarações distintas: `contracts/manifest.json` é o modelo do binding
HTTP normativo e seu endpoint é substituído pelo binding real ao iniciar;
`contracts/participant.draft.json` descreve identidade e capacidade sem
transporte e preserva o status DRAFT/não normativo do contrato ARC-01.

O runtime HTTP implementa proxy-token conforme `subsystem/1.0.0/interface.json`.
A extensão WP-05 Ed25519, admissão institucional e produção são requisitos de
promoção externos ainda não satisfeitos por este incremento. Compatibilidade
local e qualificação técnica não são aprovação institucional.
