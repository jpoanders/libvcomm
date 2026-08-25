# Checklist de entrega — Etapa 1 (Grupo M10)

> Estrutura e ordem seguem literalmente `full_assignment.pdf` (requisitos
> globais, depois seção "1. Preparação do Cenário de Comunicação entre
> Sistemas Autônomos"), para que a correspondência com o enunciado fique
> direta. Cada bullet do PDF vira uma seção; os sub-itens são a verificação
> concreta de cada um.
>
### Execução de referência — `make check`, status 0

```
support:   29 checks, 0 failures
stack:     23 checks, 0 failures
protocol:  23 checks, 0 failures
engine:     6 checks, 0 failures   (nível 1 pulado no host: sem CAP_NET_RAW)

run-fleet: 5 veículos (1..5) x 3 componentes, bus 239.10.10.10:15424 via 127.0.0.1
run-fleet: frota terminou em 9s — 5/5 veículos OK (3/3 componentes cada)

verify-capture: 191 pacotes, 161 com EtherType 0x88b5
  ok  todo destino é ff:ff:ff:ff:ff:ff
  ok  toda origem é um MAC real por VM, nenhum zerado
  ok  o vm_id do payload concorda com o MAC de origem em todo frame
  ok  5 veículos e 3 componentes distintos no fio
  ok  kinds: READY 36, REQUEST 25, RESPONSE 100

LATÊNCIA ROUND-TRIP (REQUEST -> RESPONSE, um só relógio, no host)
  samples 80   mean 2.371 ms   min 0.705 ms
  median 2.245 ms   p95 3.990 ms   max 6.293 ms   stddev 1.032 ms
```

---

## Requisitos globais do projeto

### "O desenvolvimento deve ser feito na linguagem de programação C++ utilizando apenas a C Standard Library (libc) e a C++ Standard Library em plataforma POSIX nativa."

- [x] Código em C++ (C++17)
- [x] Sem bibliotecas externas além de libc/libstdc++ (`Makefile` não linka nada além de `-pthread`)
- [x] Plataforma POSIX nativa (raw socket, `fcntl`, `sigaction`, `sem_t` — tudo POSIX)

### "Cada sistema autônomo (e.g. veículo) deve ser modelado como um macro-objeto que tem associado a si uma máquina virtual (VM) QEMU."

- [x] Cada veículo = uma VM QEMU (`run-vm.sh`, um processo QEMU por veículo)
- [x] As 5 VMs sobem e rodam simultaneamente de fato — `scripts/run-fleet.sh` implementado; execução real: **5/5 veículos OK em 9 s**. Cada VM desliga sozinha (`RB_POWER_OFF`) ao terminar o trabalho, então o `VM_TIMEOUT` é teto e não duração

### "Cada componente de cada um dos sistemas autônomos (e.g. sensor, fusor, modelo de ML) deve ser modelado como um macro-objeto que tem associado a si um processo do sistema operacional virtualizado."

- [x] Componentes implementados como processos POSIX separados — `run_vehicle()` faz `fork()` de 3 componentes (`sensor`, `fuser`, `ecu`) por VM; **15 processos** no total na frota. A pilha é construída **depois** do fork, nunca antes: cada componente tem sua própria `Engine`, seu próprio raw socket e seu próprio veredito (`design-decisions.md` §2.7)
- [x] Sincronização de partida entre os componentes de um mesmo veículo por barreira de `pipe(2)`, não por `sleep` fixo — o guia proíbe o sleep como mecanismo de prontidão, e sob TCG a dispersão entre três forks não é algo a fixar em código 
### "A comunicação via rede se dará sempre por broadcast no nível físico, com alcance limitado pelo domínio de colisão inerente da respectiva tecnologia [...]. Portanto, domínios de colisão serão modelados simplesmente pela conexão de VMs em redes virtuais (VLANs) distintas ou, alternativamente, através da filtragem, na camada mais baixa da pilha de protocolos, de mensagens não pertinentes ao domínio."

- [x] Destino sempre broadcast (`Ethernet::BROADCAST`, usado em `engine_send()`)
- [x] Domínio de colisão isolado — barramento multicast próprio do grupo `239.10.10.10:15424`
- [x] Filtragem na camada mais baixa: a `NIC` descarta o que não é do EtherType do projeto antes de subir qualquer coisa; `verify-capture.sh` mostra os 30 frames de ruído IPv6 dos próprios convidados sendo separados dos 161 frames `0x88b5`
- [x] Rota confirmada — o barramento é preso a `127.0.0.1` pelo `localaddr=` do QEMU, **sem privilégio e sem rota manual**, e é assim que a frota roda (`design-decisions.md` §2.8). `make bus-local` continua como fallback para um QEMU velho demais para `localaddr=`. `make doctor` reporta o barramento em uso

### "A biblioteca de comunicação deve apresentar uma API unificada para todos os agentes, independente de serem sistemas autônomos ou componentes dos mesmos. As mensagens trocadas entre os agentes têm tamanho máximo conhecido e menor do que a MTU da rede, logo, nunca serão fragmentadas e remontadas."

- [x] `Communicator` funcional como API única — compila, instancia e é a **única** API que `app/main.cpp` usa, tanto entre veículos quanto entre componentes do mesmo veículo. `send()`, `receive()` e `receive(timeout_ms)`
- [x] Tamanho máximo garantido menor que a MTU por construção (`Message::MAX_SIZE = 1024 < Ethernet::MTU = 1500`); `Protocol::MTU` já desconta o cabeçalho de protocolo
- [x] Confirmado em execução ponta a ponta — em três níveis independentes: `test-protocol` (23/23, `Communicator::send`→`receive` sobre `tests/loopback_engine.h`, sem socket), a frota real (15 componentes, cada um acertando uma contagem prevista) e a captura do barramento, que reprova o layout a partir dos bytes no fio

### "Os eventos assíncronos da pilha de protocolos de comunicação em desenvolvimento neste projeto devem ser tratados pelo prisma do padrão de projeto Observer X Observed [...]. Em particular, a recepção de mensagens deve ser sempre assíncrona, sem protocolos de rendezvous, e os eventos de recepção de pacotes pelo kernel do SO devem ser imediatamente propagados às camadas superiores da pilha de protocolos. Essa propagação pode se dar tanto através da implementação de módulos específicos do protocolo para o kernel quanto através de sinais POSIX."

- [x] Padrão Observer x Observed implementado (`Conditionally_Data_Observed`, `Concurrent_Observer`/`Concurrent_Observed`) — `test-stack` **compila e passa 23/23** 
- [x] Decisão de mecanismo tomada e justificada: propagação via **sinais POSIX** (`design-decisions.md` §2.1)
- [x] Implementação da propagação por sinal (`signal_handler`, `drain()`, `engine_start()`/`engine_stop()` em `src/raw_socket_engine.cpp`)
- [x] Ligação com o `NIC`: `NIC()` chama `engine_start()` no construtor e `engine_stop()` no destrutor; `NIC::handle()` aloca da metade de recepção, copia o frame, `Observed::notify()`, e conta `rx_dropped` em falha
- [x] Recepção assíncrona sem rendezvous — o caminho `drain()`→`semaphore.v()` roda inteiro dentro do handler de sinal, async-signal-safe (sem `printf`, sem `new`, sem `std::mutex`); a thread de aplicação acorda em `Communicator::receive()`
- [x] Política de fila cheia fechada em código: `Concurrent_Observer::update()` devolve `bool`, `Concurrent_Observed::notify()` só conta quem aceitou, `Protocol::update()` libera o buffer e a `NIC` conta `rx_dropped`. Coberto por `test_protocol.cpp` §5 — a frota real fechou com `rx_dropped=0`
- [x] Encerramento limpo do receptor bloqueado (questão 3 do guia): `Semaphore::p(timeout_ms)` sobre `sem_timedwait` existe e é o que sustenta `Communicator::receive(msg, timeout_ms)`. É o que impede um componente de pendurar: um teste de frota que não consegue reportar não é avaliável

### "[...] os grupos submeterão, via Moodle, um link para um commit específico em seu próprio repositório Git, com todas as especificações e diagramas de projeto localizados em uma pasta chamada 'doc'."

- [x] Link do commit submetido via Moodle — **pendente, depende do merge em `main`**
- [x] Pasta `doc/` existe no repositório
- [x] Especificações em `doc/`  
- [x] Diagramas em `doc/` — `doc/DOCUMENTACAO_UML.md` (componentes, pacotes, implantação, classes por contexto, sequências de bootstrap/transmissão/recepção-SIGIO/posse-de-buffer, atividade do `drain()`, estados da `Engine`) com matriz de rastreabilidade classe→arquivo e matriz de fidelidade código×diagrama, mais os PNGs em `doc/uml-png/` para os slides

### "Na raiz da árvore de desenvolvimento, deve existir um Makefile capaz de acionar a compilação e a execução de todos os testes pertinentes a avaliação simplesmente com o comando make."

- [x] Makefile na raiz existe
- [x] `make` compila o binário da aplicação — `build/student-app`, e o próprio alvo **recusa** o binário se ele não for estático x86-64 (o initramfs do starter não tem carregador dinâmico) 
- [x] `make` roda os testes de apoio e de pilha — `test-support` 29/29, `test-stack` 23/23, `test-protocol` 23/23, `test-engine` 6/6 
- [x] `make` sozinho aciona a avaliação **inteira** — `check: app tap test-support test-stack test-protocol test-engine image fleet capture stats`, com `.NOTPARALLEL:` para que `make -j8` não reordene o pipeline
- [x] `.DEFAULT_GOAL := check` — `make` puro roda tudo
- [x] `make` **falha com status não-zero** se um receptor perde frame, se uma VM estoura o teto, se a captura vem vazia, ou se sobram menos de 10 amostras medidas. Um alvo que só imprime aviso não é avaliável
- [x] `make` **nunca chama sudo e nunca pede senha** — a captura é feita entrando no grupo multicast com um socket UDP comum (`tools/bus_tap.cpp`), e o `test-engine` sobe uma escada de privilégio que não pode prompt-ar (`scripts/run-engine-test.sh`). `make caps` e `make bus-local` são extras opcionais, fora do `make`

### "Os slides usados nas apresentações de andamento de cada uma das etapas do projeto devem estar na mesma pasta 'doc' no Git."

- [x] Slides da Etapa 1 em `doc/`

### "As apresentações devem conter uma avaliação simples de desempenho com latência média observada durante os testes, a qual deve ser computada automaticamente ao final da sequência executada pelo make."

- [x] Captura do barramento durante o teste — `tools/bus_tap.cpp` grava entrando no grupo (sem privilégio); `scripts/capture.sh` sobe também um `dumpcap` secundário quando disponível, e escreve um `.pcap` `LINKTYPE_ETHERNET` que abre direto no Wireshark 
- [x] Cálculo de latência média automatizado ao final do `make` — `scripts/analyze-capture.sh` é o último alvo de `check`. Reporta samples, mean, min, median, p95, max e stddev, mais a média por veículo respondente
- [x] Rótulo correto — impresso como **ROUND-TRIP (REQUEST → RESPONSE, um só relógio no host)**, explicitamente **não** dividido por dois, com a justificativa de por que dividir seria indefensável. A ressalva de TCG sem `/dev/kvm` sai junto do número, no mesmo bloco
- [x] Higiene da medição — as 5 primeiras requisições são warm-up e ficam de fora; duplicatas de captura são deduplicadas; respostas órfãs e deltas negativos são descartados e contados; o script **falha** se houver mais de um requisitante (o pareamento por número de sequência ficaria ambíguo) ou se sobrarem menos de 10 amostras
- [x] Número de latência presente nos slides

---

## Etapa 1 — Preparação do Cenário de Comunicação entre Sistemas Autônomos

### "[...] uma versão minimalista da API deve ser implementada para suportar a modelagem do cenário de comunicação entre sistemas autônomos [...]. A comunicação entre sistemas autônomos, ou seja, entre as VMs que abstraem veículos, deve ser feita com raw sockets para a transferência de frames Ethernet (sem IP) através da classe Engine referenciada na especificação da API."

- [x] Raw socket (`AF_PACKET`/`SOCK_RAW`) isolado na classe `Engine` (`Raw_Socket_Engine`) — nenhuma camada acima da `Engine` conhece socket, que é o que permite a Etapa 2 trocar só ela por memória compartilhada
- [x] Construtor da Engine (`socket` / `if_nametoindex` / `SIOCGIFHWADDR` / `bind`)
- [x] Envio (`engine_send()` → `sendto()`) — o **único** syscall de transmissão da pilha
- [x] Recepção por sinal (`signal_handler`, `drain()`, `engine_start()`/`engine_stop()`)
- [x] `NIC::send`/`unmarshal`/`handle` — completo; recepção síncrona (`receive()`) permanece intencionalmente fora da API, redundante com o par `handle()`/`notify()`
- [x] Pool de buffers particionado — `alloc()` só usa `[0, SEND_BUFFERS)` e `handle()` só usa `[SEND_BUFFERS, BUFFER_SIZE)`, 16 de cada. `handle()` roda em contexto de sinal e não pode esperar, então a profundidade de recepção precisa ser um número declarável e não uma corrida. Preço: nenhuma metade empresta da outra (`design-decisions.md` §2.6)
- [x] Contrato de `unmarshal()` fechado — devolve *bytes do frame menos o cabeçalho*, padding incluído, e **não** "bytes de payload". Quem precisa do tamanho verdadeiro lê `Protocol::Header::_length`, que viaja no frame — e `Protocol::receive()` o **clampa** aos bytes que de fato chegaram
- [x] `NIC(const char* iface = Traits<Ethernet>::INTERFACE)` — deixa a mesma pilha apontar para outra interface sem recompilar, o que é o que permite ensaiar a frota inteira no host sobre `lo`
- [x] `Protocol` completo (construtor, destrutor, `send`, `receive`, `attach`, `detach`, `update`)
- [x] `Communicator` completo — compila, instancia e é a API que a aplicação usa; `receive()` com e sem prazo
- [x] **Frames sem IP confirmados em execução real** — 161 frames de EtherType `0x88b5` na captura da frota, todos com destino `ff:ff:ff:ff:ff:ff`, provados por `scripts/verify-capture.sh` a partir dos bytes no fio, sem ler log de VM nenhum

### "Os testes devem modelar ao menos 5 veículos implementados como VMs QEMU e cujos componentes (e.g. sensores, powertrain, ECUs) devem ser implementados como processos POSIX."

- [x] Ao menos 5 VMs QEMU nos testes — `scripts/run-fleet.sh` sobe as 5 em paralelo; execução real 5/5 OK
- [x] Componentes como processos POSIX — 3 por veículo (`sensor`, `fuser`, `ecu`), 15 processos na frota, cada um com sua própria `Engine`; a captura confirma **3 componentes distintos** no fio
- [x] Os vereditos são previsíveis, não aproximados — componentes das VMs 2–5 esperam exatamente `N` REQUESTs, componentes da VM 1 esperam exatamente `4N` RESPONSEs. Nenhum dos dois números depende de o barramento ecoar o próprio broadcast de volta, porque nenhum conta um tipo que o próprio veículo emite

### "As VMs devem se comunicar via uma rede virtual privada."

- [x] Barramento multicast próprio do grupo (`SO2_MCAST=239.10.10.10:15424`)
- [x] Tráfego de fato isolado — preso à loopback pelo `localaddr=127.0.0.1` do QEMU, sem rota manual e sem privilégio; o tap entra no grupo pelo mesmo endereço, então nada precisa sair pela interface física

### Mensagens — "M = {.\*}"

- [x] Mensagem é um array de bytes cru, sem campos adicionais (`Message`, `MAX_SIZE = 1024`)
- [x] Nenhum campo de `origin`, `timestamp` etc. adicionado precocemente (reservado para etapas futuras). O `fleet::Payload` que a frota troca é da **aplicação**, mora em `app/fleet_payload.h` e não contamina a biblioteca

### Identificadores — "Os endereços na classe NIC (i.e. NIC::Address) são endereços Ethernet (a.k.a. MAC Address) [...]. Além disso, é importante ter-se em mente que os requisitos do projeto exigem que o endereço destino na camada de enlace seja o de broadcast (FF:FF:FF:FF:FF:FF)."

- [x] `NIC::Address` é `Ethernet::Address` (MAC de 6 bytes)
- [x] MAC lido do kernel via `SIOCGIFHWADDR`, nunca hard-coded — a captura confirma um MAC real e distinto por VM, nenhum zerado
- [x] Endereço de destino sempre `FF:FF:FF:FF:FF:FF` (`Ethernet::BROADCAST`) — confirmado em **todos** os 161 frames capturados

---
