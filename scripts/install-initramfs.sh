#!/bin/bash
# Injeta build/student-app no initramfs e deixa a imagem pronta para a frota.
#
# REGRA QUE NÃO SE QUEBRA: o starter em ~/work/so2/ é material do professor e é
# READ-ONLY.  O repack-initramfs.sh escreve dentro da árvore onde ele está, então
# rodá-lo direto no starter suja o material original.  Trabalhe numa cópia.
set -euo pipefail

STARTER="${STARTER:-$HOME/work/so2/pratical-class-1/INE5424-x86_64-starter-6.15.5}"
WORKVM="${WORKVM:-build/vm}"
APP="${1:-build/student-app}"

# TODO(joao):
#   1. se $WORKVM não existir, copiar $STARTER para lá (cp -a) e avisar
#   2. validar que $APP existe, é x86-64 e é estático
#        file "$APP" | grep -q 'statically linked'
#   3. "$WORKVM/install-app.sh" "$(readlink -f "$APP")"
#        (ele já chama repack-initramfs.sh por dentro)
#   4. falhar com mensagem clara em qualquer erro — este script roda dentro do
#      make e o make PRECISA falhar quando algo dá errado
echo "TODO: implementar install-initramfs.sh"
exit 1
