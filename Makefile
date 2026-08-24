# =============================================================================
# libvcomm — INE5424 Etapa 1 — Grupo M10
#
# O enunciado exige que `make` na raiz compile E execute todos os testes de
# avaliação, e que a latência média saia automaticamente ao final.
#
# O que JÁ FUNCIONA:  app, test-support, test-stack, clean
# O que é ESQUELETO:  image, fleet, capture, stats  (procure por TODO)
#
# Quando fleet/capture/stats estiverem prontos, troque a linha
# .DEFAULT_GOAL abaixo para `check` — aí `make` puro roda a avaliação inteira,
# como o enunciado pede.
# =============================================================================

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude
LDFLAGS  := -pthread

# Binário que vai para dentro da VM: PRECISA ser estático x86_64, senão o
# install-app.sh recusa (o initramfs do starter não tem loader dinâmico).
APPFLAGS := -static

BUILD   := build
STARTER ?= $(HOME)/work/so2/pratical-class-1/INE5424-x86_64-starter-6.15.5

# Barramento virtual do grupo.  O default do run-vm.sh (230.0.0.1:1234) colide
# com outros grupos na mesma máquina — mantenha um endereço só nosso.
SO2_MCAST ?= 239.10.10.10:15424
export SO2_MCAST

VMS      := 1 2 3 4 5
VM_TIMEOUT ?= 20

LIB_SRC  := src/raw_socket_engine.cpp
APP_SRC  := app/main.cpp $(LIB_SRC)
HEADERS  := $(wildcard include/*.h include/engine/*.h)

.DEFAULT_GOAL := all

# -----------------------------------------------------------------------------
.PHONY: all
all: app test-support
	@echo
	@echo "  build ok. Próximo passo: 'make test-stack' e implemente até ficar verde."
	@echo "  (image/fleet/capture/stats ainda são esqueleto — veja os TODO no Makefile)"

# Alvo final da avaliação: é ISTO que o enunciado quer que `make` faça.
#
# VCOMM_REQUIRE_RAW=1 faz o test-engine FALHAR em vez de pular o nível 1 quando
# não há CAP_NET_RAW.  O alvo da avaliação não pode ficar verde tendo pulado o
# único teste que abre socket de verdade.  Rodando `make test-engine` sozinho a
# variável não está setada e o pulo continua amigável.
.PHONY: check
check: export VCOMM_REQUIRE_RAW := 1
check: app test-support test-stack test-engine image fleet capture stats

# -----------------------------------------------------------------------------
.PHONY: app
app: $(BUILD)/student-app

$(BUILD)/student-app: $(APP_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(APPFLAGS) $(APP_SRC) -o $@ $(LDFLAGS)
	@file $@ | grep -q 'statically linked' || { echo "ERRO: binário não é estático"; exit 1; }
	@file $@ | grep -q 'x86-64'            || { echo "ERRO: binário não é x86-64"; exit 1; }
	@echo "  ok: $@ é x86-64 estático"

# -----------------------------------------------------------------------------
.PHONY: test-support test-stack test-engine
test-support: $(BUILD)/test-support
	@echo
	./$(BUILD)/test-support

test-stack: $(BUILD)/test-stack
	@echo
	./$(BUILD)/test-stack

# Roda sem privilégio (nível 1 se pula sozinho).  Para exercitar o socket de
# verdade, uma vez:  sudo setcap cap_net_raw+ep $(BUILD)/test-engine
test-engine: $(BUILD)/test-engine
	@echo
	./$(BUILD)/test-engine

$(BUILD)/test-support: tests/test_support.cpp tests/check.h $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Itests tests/test_support.cpp -o $@ $(LDFLAGS)

$(BUILD)/test-stack: tests/test_stack.cpp tests/check.h $(APP_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Itests tests/test_stack.cpp $(LIB_SRC) -o $@ $(LDFLAGS)

$(BUILD)/test-engine: tests/test_engine.cpp tests/check.h $(LIB_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Itests tests/test_engine.cpp $(LIB_SRC) -o $@ $(LDFLAGS)

# -----------------------------------------------------------------------------
.PHONY: image
image: app
	@echo "TODO(joao): injetar o binário no initramfs."
	@echo "  Um binário só para as 5 VMs; o papel vem de SO2_VM_ID."
	@echo "  Passos: $(STARTER)/install-app.sh $(BUILD)/student-app"
	@echo "  Cuidado: o starter é READ-ONLY (material do professor)."
	@echo "  Copie a árvore para $(BUILD)/vm/ na primeira vez e trabalhe na cópia,"
	@echo "  senão o repack-initramfs.sh escreve no diretório do professor."
	@false

# -----------------------------------------------------------------------------
.PHONY: fleet
fleet: image
	@echo "TODO(joao): subir as 5 VMs em paralelo, com timeout, log por VM."
	@echo "  Ver scripts/run-fleet.sh"
	@false

# -----------------------------------------------------------------------------
.PHONY: capture
capture:
	@echo "TODO(joao): capturar o barramento no HOST enquanto a frota roda."
	@echo "  O tráfego mcast do QEMU passa pela loopback do host."
	@echo "  Ver scripts/capture.sh"
	@false

# -----------------------------------------------------------------------------
.PHONY: stats
stats:
	@echo "TODO(joao): extrair os pares request/response da captura e calcular"
	@echo "  count, mean, min, max e um percentil."
	@echo "  RÓTULO HONESTO: diga se é round-trip ou estimativa de via única."
	@echo "  E diga que a bancada roda em TCG (sem /dev/kvm) — o número tem viés."
	@echo "  Ver scripts/analyze-capture.sh"
	@false

# -----------------------------------------------------------------------------
$(BUILD):
	@mkdir -p $(BUILD)

.PHONY: clean
clean:
	rm -rf $(BUILD)/student-app $(BUILD)/test-support $(BUILD)/test-stack \
	       $(BUILD)/test-engine \
	       $(BUILD)/logs $(BUILD)/captures

.PHONY: help
help:
	@echo "make app           compila o binário estático da VM"
	@echo "make test-support  testa as classes de apoio (deve passar hoje)"
	@echo "make test-stack    testa a pilha (falha até você implementar)"
	@echo "make test-engine   testa a Engine (nível 1 precisa de CAP_NET_RAW)"
	@echo "                   prova do erro de RX: sudo scripts/test-engine-veth.sh"
	@echo "make image         injeta no initramfs          [TODO]"
	@echo "make fleet         sobe as 5 VMs                [TODO]"
	@echo "make capture       captura o barramento         [TODO]"
	@echo "make stats         calcula a latência           [TODO]"
	@echo "make check         a avaliação inteira          [TODO]"
