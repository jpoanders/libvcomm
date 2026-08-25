#!/bin/bash
# =============================================================================
# Exporta TODOS os blocos Mermaid de doc/DOCUMENTACAO_UML.md para PNG de alta
# resolução, organizados em pastas por seção e com nomes que dizem o que são.
#
# POR QUE ESTE SCRIPT EXISTE, e não apenas uma linha de `npx mmdc`:
#   o mermaid-cli só sabe numerar a saída na ordem em que os blocos aparecem
#   (DOCUMENTACAO_UML-7.png).  Esse nome não diz nada, e a numeração muda
#   inteira quando você insere um diagrama no meio do documento.  Aqui o número
#   é traduzido UMA vez, no array TARGETS abaixo, e o resultado sobrevive a
#   qualquer reexecução.
#
# Uso:
#   ./scripts/export-uml.sh                 # padrão: escala 4x, 2400px
#   SCALE=6 WIDTH=3000 ./scripts/export-uml.sh
# =============================================================================
set -euo pipefail

cd "$(dirname "$0")/.."

DOC="${DOC:-doc/DOCUMENTACAO_UML.md}"
OUT="${OUT:-doc/uml-png}"
PPTR="${PPTR:-doc/puppeteer.json}"
SCALE="${SCALE:-4}"
WIDTH="${WIDTH:-2400}"
THEME="${THEME:-neutral}"

# -----------------------------------------------------------------------------
# Mapa bloco -> "caminho relativo|título".
#
# A ORDEM É A ORDEM DOS BLOCOS ```mermaid NO DOCUMENTO.  Ao acrescentar um
# diagrama, insira a linha na posição correspondente — a verificação logo abaixo
# aborta se o mapa e o documento discordarem, em vez de exportar torto em
# silêncio.
# -----------------------------------------------------------------------------
TARGETS=(
    "1-arquitetura/1.1-componentes-camadas|Componentes e camadas"
    "1-arquitetura/1.2-pacotes-dependencias|Pacotes e grafo de includes"
    "1-arquitetura/1.3-implantacao-frota|Implantação da frota"
    "2-classes/2.1-nucleo-suporte|Classes: núcleo de suporte"
    "2-classes/2.2-observer-observed|Classes: Observer x Observed"
    "2-classes/2.3-camada-enlace|Classes: camada de enlace"
    "2-classes/2.4-camada-rede-transporte|Classes: camada de rede e transporte"
    "2-classes/2.4b-encapsulamento-cabecalhos|Encapsulamento de cabeçalhos"
    "2-classes/2.5-aplicacao|Classes: camada de aplicação"
    "2-classes/2.5b-instanciacao-templates|Instanciação dos templates"
    "3-comportamento/3.1-sequencia-bootstrap|Sequência: bootstrap da pilha"
    "3-comportamento/3.2-sequencia-transmissao|Sequência: transmissão"
    "3-comportamento/3.3-sequencia-recepcao-sigio|Sequência: recepção via SIGIO"
    "3-comportamento/3.4-sequencia-posse-buffer|Sequência: posse do Buffer"
    "3-comportamento/3.5-atividade-drain|Atividade: drain()"
    "4-anexos/4.1-estados-engine|Estados do Raw_Socket_Engine"
)

# --- verificações antes de gastar minutos de Chromium ------------------------
[ -f "$DOC" ]  || { echo "ERRO: $DOC não encontrado." >&2; exit 1; }
[ -f "$PPTR" ] || { echo "ERRO: $PPTR não encontrado (necessário no Ubuntu 23.10+)." >&2; exit 1; }
command -v npx >/dev/null || { echo "ERRO: npx não encontrado.  sudo apt install nodejs npm" >&2; exit 1; }

blocks=$(grep -c '^```mermaid$' "$DOC")
if [ "$blocks" -ne "${#TARGETS[@]}" ]; then
    echo "ERRO: $DOC tem $blocks blocos mermaid, mas o mapa TARGETS tem ${#TARGETS[@]}." >&2
    echo "      Atualize o array TARGETS em $0 antes de exportar." >&2
    exit 1
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

echo "==> renderizando $blocks diagramas (escala ${SCALE}x, viewport ${WIDTH}px)"
npx -y @mermaid-js/mermaid-cli \
    -i "$DOC" \
    -o "$STAGE/DOCUMENTACAO_UML.md" \
    -e png -s "$SCALE" -w "$WIDTH" -b white -t "$THEME" \
    -p "$PPTR"

# --- limpa apenas o que este script produz -----------------------------------
for d in 1-arquitetura 2-classes 3-comportamento 4-anexos; do
    rm -rf "${OUT:?}/$d"
done
rm -f "${OUT:?}"/*.png "${OUT:?}"/*.md

echo "==> organizando em $OUT/"
MD="$STAGE/DOCUMENTACAO_UML.md"
for i in "${!TARGETS[@]}"; do
    n=$((i + 1))
    path="${TARGETS[$i]%%|*}"
    title="${TARGETS[$i]#*|}"

    src="$STAGE/DOCUMENTACAO_UML-$n.png"
    [ -f "$src" ] || { echo "ERRO: $src não foi gerado." >&2; exit 1; }

    mkdir -p "$OUT/$(dirname "$path")"
    mv "$src" "$OUT/$path.png"

    # Reaponta o Markdown de imagens estáticas para o novo caminho e troca o
    # alt text genérico "diagram" pelo título real.
    sed -i "s|!\[diagram\](\./DOCUMENTACAO_UML-$n\.png)|![$title](./$path.png)|" "$MD"

    printf '    %2d  %s\n' "$n" "$path.png"
done

# O Markdown de imagens fica um nível mais fundo que o original (doc/uml-png/),
# então todo link relativo do documento precisa de um ../ a mais.
sed -i -e 's|](\.\./|](../../|g' -e 's|](puppeteer\.json)|](../puppeteer.json)|g' "$MD"
mv "$MD" "$OUT/DOCUMENTACAO_UML-imagens.md"

echo "==> pronto"
echo "    PNGs ...................... $OUT/"
echo "    versão com imagens ........ $OUT/DOCUMENTACAO_UML-imagens.md"
du -sh "$OUT"
