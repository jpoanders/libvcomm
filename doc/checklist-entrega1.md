# Checklist de entrega — Etapa 1 (Grupo M10)

> Estrutura e ordem seguem literalmente `full_assignment.pdf` (requisitos
> globais, depois seção "1. Preparação do Cenário de Comunicação entre
> Sistemas Autônomos"), para que a correspondência com o enunciado fique
> direta. Cada bullet do PDF vira uma seção; os sub-itens são a verificação
> concreta de cada um. Estado marcado em 23/08/2026, com base no código do
> zip enviado e na execução real de `make test-support` / `make test-stack`.
> Revisado em 24/08/2026 após os commits `feat: implement protocol and
> communication`, `refactor: update project comments and docs to english` e
> `minor communicator fix` — ver notas `[atualizado 24/08]` abaixo.
> Revisado novamente em 24/08/2026 após o commit `feat: implement nic handle,
> constructor and destructor` (PR #6, `feature/nic_handle`) — ver notas
> `[atualizado 24/08 #2]`.
> Revisado em 24/08/2026 após o merge `9426f3b` (`feat: fix nic buffer and add
> timeout to updated()`) — ver notas `[atualizado 24/08 #3]`.

---

## ⛔ Bloqueadores da entrega — estado em `9426f3b`

Verificado por execução real, não por leitura de código:

| # | Bloqueador | Evidência |
|---|---|---|
| B1 | **`make app` não compila.** `Communicator::update()` continua devolvendo `void`, mas a base `Concurrent_Observer::update()` passou a devolver `bool` no último commit — retorno conflitante em função virtual | `include/communicator.h:109` × `include/observer.h:163` |
| B2 | **`make test-stack` não compila.** `Concurrent_Observer::updated(timeout_ms)` chama `_semaphore.p(timeout_ms)`, e `Semaphore` só tem `void p()` — o overload com prazo nunca foi escrito | `include/observer.h:188` × `include/sem.h:42` |
| B3 | **A mensagem não chega à aplicação nem quando compilar.** `Communicator::send()` endereça `Address::broadcast()`, cujo `Port` é **0**; `main()` registra o `Communicator` na porta **1000**. `notify(0, buf)` não encontra observador, o buffer é liberado e `receive()` nunca acorda | `include/communicator.h:84` × `app/main.cpp:93` |

B1 e B2 são mecânicos. B3 é uma decisão de projeto pendente: emissor e receptor
precisam concordar numa porta (uma porta de serviço fixa, ou `broadcast(port)`
carregando a porta do remetente).

> O enunciado exige que `make` compile **e execute** os testes da avaliação.
> Enquanto B1 e B2 existirem, a entrega não é avaliável — nem o binário da VM é
> gerado.

---

## Requisitos globais do projeto

### "O desenvolvimento deve ser feito na linguagem de programação C++ utilizando apenas a C Standard Library (libc) e a C++ Standard Library em plataforma POSIX nativa."

- [x] Código em C++ (C++17)
- [x] Sem bibliotecas externas além de libc/libstdc++ (`Makefile` não linka nada além de `-pthread`)
- [x] Plataforma POSIX nativa (raw socket, `fcntl`, `sigaction`, `sem_t` — tudo POSIX)

### "Cada sistema autônomo (e.g. veículo) deve ser modelado como um macro-objeto que tem associado a si uma máquina virtual (VM) QEMU."

- [x] Cada veículo = uma VM QEMU (`run-vm.sh`, um processo QEMU por veículo)
- [ ] As 5 VMs sobem e rodam simultaneamente de fato (`scripts/run-fleet.sh` — hoje TODO)

### "Cada componente de cada um dos sistemas autônomos (e.g. sensor, fusor, modelo de ML) deve ser modelado como um macro-objeto que tem associado a si um processo do sistema operacional virtualizado."

- [ ] Componentes do veículo (sensor, powertrain, ECU) implementados como processos POSIX separados (`fork()` em `app/main.cpp` — hoje é um processo único por VM, TODO)

### "A comunicação via rede se dará sempre por broadcast no nível físico, com alcance limitado pelo domínio de colisão inerente da respectiva tecnologia [...]. Portanto, domínios de colisão serão modelados simplesmente pela conexão de VMs em redes virtuais (VLANs) distintas ou, alternativamente, através da filtragem, na camada mais baixa da pilha de protocolos, de mensagens não pertinentes ao domínio."

- [x] Destino sempre broadcast (`Ethernet::BROADCAST`, usado em `engine_send()`)
- [x] Domínio de colisão isolado — barramento multicast próprio do grupo (`239.10.10.10:15424`, não o default compartilhado da turma)
- [ ] Confirmar rota de rede antes de medir/apresentar (`design-decisions.md` já alerta que o barramento pode estar saindo pela WiFi em vez de ficar preso à loopback — `ip route get 239.10.10.10`, checado por `make doctor`)

### "A biblioteca de comunicação deve apresentar uma API unificada para todos os agentes, independente de serem sistemas autônomos ou componentes dos mesmos. As mensagens trocadas entre os agentes têm tamanho máximo conhecido e menor do que a MTU da rede, logo, nunca serão fragmentadas e remontadas."

- [ ] `Communicator` funcional como API única (send/receive) — a classe está escrita (`include/communicator.h`), mas **não instancia**: `update()` devolve `void` contra a base que agora devolve `bool` (bloqueador **B1**) **[atualizado 24/08 #3]**
- [x] Tamanho máximo de mensagem garantido menor que a MTU por construção (`Message::MAX_SIZE = 1024 < Ethernet::MTU = 1500`)
- [ ] Confirmado em execução ponta a ponta — **regrediu**: além de continuar sem teste automatizado que exercite `Communicator::send`→`receive`, a pilha agora não compila (**B1**, **B2**) e, mesmo compilando, o descasamento de porta (**B3**) impede a entrega à aplicação. `test_stack.cpp` segue sem casos para `handle()` ou `Communicator` **[atualizado 24/08 #3]**

### "Os eventos assíncronos da pilha de protocolos de comunicação em desenvolvimento neste projeto devem ser tratados pelo prisma do padrão de projeto Observer X Observed [...]. Em particular, a recepção de mensagens deve ser sempre assíncrona, sem protocolos de rendezvous, e os eventos de recepção de pacotes pelo kernel do SO devem ser imediatamente propagados às camadas superiores da pilha de protocolos. Essa propagação pode se dar tanto através da implementação de módulos específicos do protocolo para o kernel quanto através de sinais POSIX."

- [x] Padrão Observer x Observed implementado (`Conditionally_Data_Observed`, `Concurrent_Observer`/`Concurrent_Observed`) — as verificações vivem em `test-stack`, que **hoje não compila** (**B2**); o último resultado conhecido foi 23/23 em `e64ca49` **[atualizado 24/08 #3]**
- [x] Decisão de mecanismo tomada e justificada: propagação via **sinais POSIX** (`doc/design-decisions.md` §2.1)
- [x] **Implementação da propagação por sinal** (`signal_handler`, `drain()`, `engine_start()`/`engine_stop()` em `src/raw_socket_engine.cpp`) — implementado **[atualizado 24/08]**
- [x] Ligação com o `NIC`: `NIC()` chama `Engine::engine_start()` no construtor e `Engine::engine_stop()` no destrutor, e `NIC::handle()` está implementado (aloca buffer da metade de recepção, copia o frame, `Observed::notify()`, `rx_dropped` em falha) **[atualizado 24/08 #2]**
- [x] **Política de fila cheia fechada em código** (era o último item aberto de `design-decisions.md` §4): `Concurrent_Observer::update()` passou a devolver `bool`, `Concurrent_Observed::notify()` só conta como notificado quem aceitou, e fila cheia virou indistinguível de "ninguém quis" — `Protocol::update()` libera o buffer e a `NIC` conta `rx_dropped`. Antes o buffer vazava **e** a mensagem se perdia **[atualizado 24/08 #3]**
- [ ] Encerramento limpo do receptor bloqueado (questão 3 do guia, lado da aplicação): `Concurrent_Observer::updated(timeout_ms)` foi acrescentado com essa finalidade, mas o `Semaphore::p(timeout_ms)` que ele chama não existe (**B2**) **[atualizado 24/08 #3]**

### "Os grupos serão constituídos por até 3 alunos e podem incluir alunos de ambas as turmas desde que os mesmos estejam presentes em todas as apresentações de andamento do projeto, em todos os atendimentos extraclasse e também na apresentação final."

- [ ] Confirmar presença dos 3 integrantes garantida em todas as apresentações de andamento
- [ ] Confirmar presença garantida em todos os atendimentos extraclasse
- [ ] Confirmar presença garantida na apresentação final

### "[...] os grupos submeterão, via Moodle, um link para um commit específico em seu próprio repositório Git, com todas as especificações e diagramas de projeto localizados em uma pasta chamada 'doc'."

- [ ] Link do commit submetido via Moodle
- [x] Pasta `doc/` existe no repositório
- [ ] Especificações em `doc/` — `design-decisions.md` existe e cobre §1.1–1.10, §2.1–2.5, §3 e §4, mas o último commit **fechou três decisões só no comentário do código e citou seções que não existem**: `§2.6` (partição do pool), `1.11` (`update()` devolvendo `bool`) e `§3` para o contrato de `unmarshal()` — §3 é "Honest limitations of the bench" e não registra esse contrato. Além disso, §4 ainda lista como aberta a política de fila cheia, que já foi resolvida em código **[atualizado 24/08 #3]**
- [x] Diagramas em `doc/` (`DOCUMENTACAO_UML.md` + `doc/uml-png/` — conjunto completo de diagramas Mermaid por engenharia reversa, com matriz de fidelidade código×diagrama; ressincronizado com `9426f3b`) **[atualizado 24/08 #3]**

### "Este commit deve, preferencialmente, ser resultado do merge de um branch da etapa com o branch main. Independentemente do merge, os artefatos a serem avaliados devem, obrigatoriamente, estar sempre no branch main."

- [x] Repositório Git inicializado — confirmado (`.git/` presente, 6 PRs mergeados em `main` mais o merge direto `9426f3b`) **[atualizado 24/08 #3]**
- [x] Branch da etapa criado e mergeado com `main` — feature branches (`feature/nic_buffer`, `feature/marshalling`, `feature/raw_socket_engine`, `feature/protocol_communicator`, `refactor/translate_to_english`, `feature/nic_handle`) mergeadas **[atualizado 24/08 #3]**
- [x] Artefatos avaliados presentes no `main` — confirmado, branch atual é `main` **[atualizado 24/08]**
- [ ] `main` em estado compilável no commit submetido — **hoje não está** (**B1**, **B2**). O commit enviado ao Moodle precisa compilar **[atualizado 24/08 #3]**

### "Na raiz da árvore de desenvolvimento, deve existir um Makefile capaz de acionar a compilação e a execução de todos os testes pertinentes a avaliação simplesmente com o comando make."

- [x] Makefile na raiz existe
- [ ] `make` compila o binário da aplicação — **`make app` falha** em `9426f3b` (**B1** e **B2**); nenhum binário estático é gerado, logo `make image`/`fleet` estão inalcançáveis mesmo depois de implementados **[atualizado 24/08 #3]**
- [ ] `make` roda os testes de apoio e de pilha — `test-support`: **29/29 ok**; `test-engine`: **6/6 ok** (nível 1 pulado, sem `CAP_NET_RAW`); `test-stack`: **não compila** (**B2**) **[atualizado 24/08 #3]**
- [ ] `make` sozinho aciona a avaliação **inteira** (`image`, `fleet`, `capture`, `stats` — hoje são `@false`, TODO)
- [ ] `.DEFAULT_GOAL` trocado para `check`, para que `make` puro (sem argumento) rode tudo

### "Os slides usados nas apresentações de andamento de cada uma das etapas do projeto devem estar na mesma pasta 'doc' no Git."

- [ ] Slides da Etapa 1 em `doc/` — pendente

### "As apresentações devem conter uma avaliação simples de desempenho com latência média observada durante os testes, a qual deve ser computada automaticamente ao final da sequência executada pelo make."

- [ ] Captura do barramento durante o teste (`scripts/capture.sh` — TODO)
- [ ] Cálculo de latência média (count, mean, min, max) automatizado (`scripts/analyze-capture.sh` — TODO)
- [ ] Rótulo correto (round-trip vs. via única) e ressalva de que a bancada roda em QEMU TCG sem `/dev/kvm`
- [ ] Número de latência presente nos slides

---

## Etapa 1 — Preparação do Cenário de Comunicação entre Sistemas Autônomos

### "[...] uma versão minimalista da API deve ser implementada para suportar a modelagem do cenário de comunicação entre sistemas autônomos [...]. A comunicação entre sistemas autônomos, ou seja, entre as VMs que abstraem veículos, deve ser feita com raw sockets para a transferência de frames Ethernet (sem IP) através da classe Engine referenciada na especificação da API."

- [x] Raw socket (`AF_PACKET`/`SOCK_RAW`) isolado na classe `Engine` (`Raw_Socket_Engine`)
- [x] Construtor da Engine (`socket` / `if_nametoindex` / `SIOCGIFHWADDR` / `bind`) — implementado
- [x] Envio (`engine_send()` → `sendto()`) — implementado
- [x] Recepção por sinal (`signal_handler`, `drain()`, `engine_start()`/`engine_stop()`) — implementado em `src/raw_socket_engine.cpp` **[atualizado 24/08]**
- [x] `NIC::send`/`unmarshal`/`handle` (a ponte entre Engine e o resto da pilha) — completo; recepção síncrona (`receive()`) permanece intencionalmente fora da API, redundante com o par `handle()`/`notify()` **[atualizado 24/08 #2]**
- [x] **Pool de buffers particionado** — `alloc()` só usa `[0, SEND_BUFFERS)` = 16 posições e `handle()` só usa `[SEND_BUFFERS, BUFFER_SIZE)` = as outras 16. Antes eram 32 compartilhadas, e uma rajada de transmissão podia deixar `handle()` sem buffer — que roda em contexto de sinal e não pode esperar, logo o quadro se perdia. Agora a profundidade garantida de recepção é um número declarável, não uma corrida. Preço: nenhuma metade empresta da outra **[atualizado 24/08 #3]**
- [x] **Contrato de `unmarshal()` fechado** — devolve *bytes do frame menos o cabeçalho*, padding incluído, e **não** "bytes de payload": a camada de enlace não tem campo de comprimento próprio. Quem precisa do tamanho verdadeiro lê `Protocol::Header::_length`, que viaja no frame **[atualizado 24/08 #3]**
- [x] `NIC(const char* iface = Traits<Ethernet>::INTERFACE)` — parâmetro acrescentado para que um teste na máquina hospedeira (sem `eth0`) aponte a mesma pilha para outra interface sem recompilar **[atualizado 24/08 #3]**
- [x] `Protocol` completo (construtor, destrutor, `send`, `receive`, `attach`, `detach`, `update`) — implementado em `include/protocol.h` **[atualizado 24/08]**
- [ ] `Communicator` completo — escrito, mas não compila (**B1**) **[atualizado 24/08 #3]**
- [ ] Frames sem IP confirmados em execução real — bloqueado por **B1**/**B2** (não há binário) e por **B3** (a porta não casa). Sem teste automatizado nem execução em VMs **[atualizado 24/08 #3]**

### "Os testes devem modelar ao menos 5 veículos implementados como VMs QEMU e cujos componentes (e.g. sensores, powertrain, ECUs) devem ser implementados como processos POSIX."

- [ ] Ao menos 5 VMs QEMU nos testes (`scripts/run-fleet.sh` — TODO)
- [ ] Componentes de cada veículo implementados como processos POSIX (`fork()` em `app/main.cpp` — TODO; hoje é um processo único por VM)

### "As VMs devem se comunicar via uma rede virtual privada."

- [x] Barramento multicast próprio do grupo, não o default compartilhado (`SO2_MCAST=239.10.10.10:15424`)
- [ ] Confirmar que o tráfego está de fato isolado (rota presa à loopback, não vazando pela interface física)

### Mensagens — "M = {.\*}"

- [x] Mensagem é um array de bytes cru, sem campos adicionais (`Message`, `MAX_SIZE = 1024`)
- [x] Nenhum campo de `origin`, `timestamp` etc. adicionado precocemente (reservado para etapas futuras)

### Identificadores — "Os endereços na classe NIC (i.e. NIC::Address) são endereços Ethernet (a.k.a. MAC Address) [...]. Além disso, é importante ter-se em mente que os requisitos do projeto exigem que o endereço destino na camada de enlace seja o de broadcast (FF:FF:FF:FF:FF:FF)."

- [x] `NIC::Address` é `Ethernet::Address` (MAC de 6 bytes)
- [x] MAC lido do kernel via `SIOCGIFHWADDR`, nunca hard-coded
- [x] Endereço de destino sempre `FF:FF:FF:FF:FF:FF` (`Ethernet::BROADCAST`)
- [x] Ressalva registrada: uma máquina com mais de uma NIC teria mais de um MAC (não se aplica à Etapa 1, mas fica documentado)

---

## Legenda

- `[x]` — confirmado pronto (por leitura de código e/ou execução real de teste)
- `[ ]` — pendente, ou não confirmável ainda (depende de algo pendente acima)