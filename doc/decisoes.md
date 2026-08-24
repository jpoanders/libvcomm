# Decisões de projeto — libvcomm, Etapa 1

Grupo M10 — INE5424 2026/2 — UFSC
João Pedro de Oliveira Anders · Artur Tribeck Ferreira Tomaz · André Filipe Martins

Este documento existe para a banca. Toda vez que a implementação se afasta do
que está escrito no `full_assignment.pdf`, o desvio aparece aqui com a razão.
Mostrar que o desvio foi deliberado vale mais do que uma implementação que
segue o PDF ao pé da letra e não compila.

---

## 1. Desvios em relação à API do enunciado

### 1.1 `Traits<NIC>` e `Traits<Protocol>` → `Traits<Ethernet>`

O PDF usa `Traits<NIC>::SEND_BUFFERS` e `Traits<Protocol>::ETHERNET_PROTOCOL_NUMBER`.
`NIC` e `Protocol` são templates de classe; um template não é um tipo, então
`Traits<NIC>` não compila. O EPOS resolve isso com bases não-template
(`NIC_Common`, `Protocol_Common`) e especializa `Traits` sobre elas.

**Decisão:** ancorar a especialização em `Ethernet`, que já é classe concreta e já
é base de `NIC`. Mesmo efeito, um único ponto de configuração.

### 1.2 `Protocol: private typename NIC::Observer`

O `typename` não pode aparecer numa lista de bases — em *base-clause* o nome já é
lido como tipo. **Decisão:** removido.

### 1.3 `Protocol::Observer` é `Concurrent_Observer`, não `Conditional_Data_Observer`

O PDF declara, dentro de `Protocol`:

```cpp
typedef Conditional_Data_Observer<Buffer<Ethernet::Frame>, Port> Observer;
```

mas o `Communicator` do mesmo PDF herda de `Concurrent_Observer` e se registra
com `_channel->attach(this, address)`. As duas coisas não encaixam: o observador
que o Protocol aceita teria de ser da família Conditional, e o Communicator é da
família Concurrent.

**Decisão:** quem manda é o `Communicator`. `Protocol::Observer` é
`Concurrent_Observer<Buffer, Port>` e `Protocol::_observed` é
`Concurrent_Observed<Buffer, Port>`.

E isso é o que a arquitetura pede, não só o que o compilador aceita:

| Fronteira | Família | Por quê |
|---|---|---|
| `NIC` → `Protocol` | `Conditionally_Data_Observed` | roda dentro do signal handler; **não pode bloquear** |
| `Protocol` → `Communicator` | `Concurrent_Observed` | do outro lado há uma thread de aplicação dormindo; **precisa do semáforo** (e `sem_post` é async-signal-safe) |

### 1.4 `Concurrent_Observer` não tem `rank`

`Concurrent_Observed::notify()` do PDF chama `obs->rank()`, mas o
`Concurrent_Observer` impresso não tem campo `_rank` nem construtor que o receba.
Do jeito que está, não compila.

**Decisão:** `Concurrent_Observer` ganhou `_rank` e o par `rank()`/`rank(C)`; o
`attach()` fixa o rank do observador. O resto dos métodos é transcrição literal.

### 1.5 `update()` com dois argumentos, não três

O PDF passa também um ponteiro para o observado
(`update(NIC::Observed * obs, prot, buf)`). Isso só serve quando um observador
acompanha vários observados — não é o caso em nenhuma camada desta biblioteca.

**Decisão:** `update(condição, dado)` em todas as camadas. Uma assinatura só,
usada de forma idêntica em `NIC`→`Protocol` e `Protocol`→`Communicator`.

### 1.6 `send()`/`receive()` de `Protocol` não são estáticos

O PDF os declara `static` mas o corpo usa `_nic`, que é membro de instância, e o
`Communicator` os chama como `_channel->send(...)`.
**Decisão:** métodos de instância.

### 1.7 `static Observed _observed` → membro de instância

O comentário do PDF diz "channel protocols are usually singletons". Um membro
estático de template exige definição fora da classe e impede dois protocolos no
mesmo processo. **Decisão:** membro de instância. A singularidade, se for
desejada, é responsabilidade de quem constrói a pilha.

### 1.8 `NIC()` público

O PDF marca o construtor como `protected` (no EPOS, a instância vem de
`Meta`/`Traits`). Sem uma factory, `protected` deixa a classe inutilizável.
**Decisão:** público.

### 1.9 `Channel::Address::BROADCAST` → `Address::broadcast()`

Membro estático de classe aninhada dentro de template exige definição fora da
classe. Uma função estática dá o mesmo resultado sem a cerimônia.

### 1.10 `send(Buffer *)` libera o buffer incondicionalmente

O enunciado não especifica quem libera o buffer após `send(Buffer *)`. A alternativa
natural seria: libera em caso de sucesso, devolve ao chamador em caso de erro.

**Decisão:** `send(Buffer *)` chama `free(buf)` em **todos** os caminhos, sucesso
e falha. Depois de chamar `send`, o chamador **não pode** tocar no buffer.

**Por quê.** O pool tem capacidade fixa (`SEND_BUFFERS + RECEIVE_BUFFERS`). Se a
responsabilidade de liberar em caso de erro for do chamador, um único `free`
esquecido num caminho de erro trava um slot permanentemente. Depois de 32
envios falhos sem `free`, o pool esgota e `alloc()` devolve `0` para sempre. Esse bug só aparece sob carga, tipicamente na demonstração.

Com a liberação incondicional:

- **Regra única de ownership:** `send` é transferência de posse. Sem lógica
  condicional para o chamador.
- **Vazamento impossível:** o buffer volta ao pool em qualquer caminho.
- **Coerência com `send(Address, prot, data, size)`:** o caminho simples já
  chama `alloc` + `send(buf)` internamente, então o ciclo de vida é autocontido.

**Consequência:** o chamador não pode retentar com o mesmo buffer. Se `send` falhar e
houver necessidade de retentativa, é preciso `alloc` novo e remontar o frame.

---

## 2. Decisões próprias

### 2.1 Recepção por sinal POSIX, não por thread

**Decisão:** a recepção de frames acontece dentro de um *signal handler*, armado
com `fcntl(F_SETOWN)` + `fcntl(F_SETSIG, SIGRTMIN)` + `O_ASYNC | O_NONBLOCK` e
tratado por um `sigaction` com `SA_SIGINFO`. Não há thread de recepção.

**Por quê.** É o que o enunciado manda:

> "os eventos de recepção de pacotes pelo kernel do SO devem ser imediatamente
> propagados às camadas superiores da pilha de protocolos. Essa propagação pode
> se dar tanto através da implementação de módulos específicos do protocolo para
> o kernel quanto através de **sinais POSIX**."

Uma thread bloqueada em `recvfrom` não é nenhum dos dois mecanismos nomeados.

E há uma razão de projeto por trás da regra: no EPOS, `handle()` é chamado do
**handler de interrupção de hardware** da NIC. O análogo fiel de interrupção em
POSIX é o sinal. Manter isso preserva a estrutura que a Etapa 2 vai reaproveitar
— e explica o resto do desenho, que de outro modo pareceria arbitrário:
`Conditional_Data_Observer::update` não pode bloquear porque roda em contexto de
interrupção; `Concurrent_Observer` usa semáforo porque `sem_post(3)` é
async-signal-safe; o pool de `Buffer` é pré-alocado porque `malloc` não é.

**Sinal de tempo real, não `SIGIO`.** Sinais padrão não enfileiram: dois frames
em rajada gerariam um sinal só. Os de tempo real enfileiram, e `SA_SIGINFO`
ainda entrega `si_fd` (`man 2 fcntl`, `F_SETSIG`).

**`O_NONBLOCK` e drenagem em laço.** O handler chama `recvfrom` repetidamente
até `EAGAIN`. O sinal é o gatilho; o laço é a garantia. Sair no primeiro frame
deixaria os demais parados no buffer do kernel até a chegada do próximo — e a
latência medida viraria ficção.

**Contrapartida documentada:** disposição de sinal é estado global do processo,
logo **uma Engine por processo**. Na Etapa 1 isso não incomoda (um processo = um
veículo = uma NIC). Quando um veículo virar vários processos, é a primeira
suposição a revisar.

**Ganho colateral:** a pergunta 3 do guia da aula prática ("how will a blocked
receiver terminate cleanly during automated tests?") deixa de existir. Não há
receptor bloqueado para acordar — desarmar é tirar o `O_ASYNC` com um `fcntl`.

> **Nota de honestidade:** o `practical_class_1_guide.md` §6 permite
> *"receive asynchronously **or in a dedicated receive thread**"*, contradizendo o
> enunciado. Adotamos o enunciado, que é o artefato avaliado.

### 2.2 EtherType `0x88B5`

*IEEE Local Experimental EtherType 1*, faixa reservada para uso local e
experimental (RFC 5342 §2.3.4; registro IANA "IEEE 802 Numbers"). Escolhido por
ser a faixa formalmente correta para um protocolo de disciplina, em vez de um
valor livre qualquer.

O valor é passado ao `socket()` como filtro do kernel. Isso importa na prática:
com uma VM ociosa, **8 de 11 frames** capturados no barramento eram IPv6 do
próprio kernel do guest (MLD, router solicitation — EtherType `0x86DD`, destino
`33:33:00:00:00:16`). O filtro do kernel elimina esse ruído antes da cópia para
o espaço de usuário; um receptor filtrado por `htons(0x88B5)` viu **só** os
frames do projeto.

> **Pendente:** confirmar `0x88B5` com Artur e André antes da apresentação.

### 2.3 Listas lock-free, sem alocação

O EPOS usa listas intrusivas para não alocar memória no caminho de recepção, que
lá roda em contexto de interrupção. **Pela decisão 2.1, aqui vale exatamente a
mesma restrição** — o caminho de recepção roda em contexto de sinal.

A primeira versão usava `std::deque`/`std::vector` com `std::mutex`, apoiada no
fato de o enunciado autorizar a C++ Standard Library. Isso **deixou de ser
legal**: `pthread_mutex_lock` não está na lista de funções async-signal-safe
(`man 7 signal-safety`), e travar de dentro de um handler que interrompeu a
própria thread que já segura o mutex é deadlock imediato; `push_back` pode
chamar `malloc`, que também não está na lista.

**Decisão:** estruturas lock-free de capacidade fixa, sem alocação.

| | |
|---|---|
| `List<T,CAP>` | anel SPSC com dois índices `std::atomic` — o handler só escreve `_tail`, a aplicação só escreve `_head` |
| `Ordered_List<T,C,CAP>` | vetor de `std::atomic<T*>`; `detach()` escreve *tombstone* em vez de compactar, porque compactar mexeria nos índices sob um percurso em curso |

`static_assert(is_always_lock_free)` nas duas: se `std::atomic` não fosse
lock-free na plataforma, a biblioteca padrão usaria mutex por baixo e o problema
voltaria em silêncio.

**Preço:** capacidade fixa. `insert()` devolve `false` quando enche, e isso é
`Statistics::rx_dropped` — não um erro a esconder.

Verificado empiricamente: produtor num handler de `SIGRTMIN` a 20 kHz,
consumidor no `main`, 3 s — **58.937 sinais, 176.811 itens, zero perdidos, FIFO
preservado.**

### 2.4 Barramento multicast próprio

O default do `run-vm.sh` é `230.0.0.1:1234`, compartilhado por toda a turma.
**Decisão:** `SO2_MCAST=239.10.10.10:15424` (faixa administrativamente escopada,
porta = número da disciplina).

> **Atenção, verificado em 22/08/2026:** `ip route get 239.10.10.10` responde
> `dev wlp0s20f3`. O barramento sai pela **WiFi**, para a rede do prédio — pode
> colidir com outro grupo e leva o tráfego do projeto para fora da máquina.
> Prender à loopback com `sudo ip route add 239.10.10.0/24 dev lo` antes de
> qualquer medição, e reconferir a rota depois de trocar de rede.

---

## 3. Limitações honestas da bancada

- **Uma Engine por processo** (consequência da decisão 2.1). A disposição de
  sinal é global ao processo, então duas Engines no mesmo processo disputariam o
  mesmo handler. Não afeta a Etapa 1.
- **Filas de capacidade fixa** (decisão 2.3). Sob rajada acima da capacidade a
  mensagem é descartada e contada em `rx_dropped`, em vez de a biblioteca
  crescer a fila alocando memória em contexto de sinal.

- **Sem `/dev/kvm`.** As VMs rodam em TCG (emulação pura). Toda latência medida
  é dominada por ruído de emulação. O número vai no slide **com essa ressalva**.
- **`virtio-net` do QEMU não faz padding para 60 bytes.** Medido: um frame de 20
  bytes chega ao `recvfrom()` do outro guest como 20. Ou seja,
  `tamanho_do_payload = bytes_recebidos - 14` funciona *nesta bancada* e quebra
  em hardware real e na Engine de memória compartilhada da Etapa 2. Por isso o
  tamanho verdadeiro deve viajar num campo do `Protocol::Header`.
- **Uma captura no host não prova recepção.** Ver o datagrama sair prova formato
  e tempo; a prova de que um guest processou a mensagem é o log da VM.

---

## 4. Pendências

- [ ] Diagramas em `doc/` (topologia da frota; camadas; sequência de uma mensagem
      de `send()` até a thread da aplicação acordar, marcando onde termina o
      contexto de sinal). O enunciado exige.
- [ ] Slides da Etapa 1 em `doc/`.
- [ ] Confirmar o EtherType com o grupo.
- [ ] Decidir e registrar: cada componente abre o próprio raw socket, ou um
      processo dono da NIC repassa aos filhos? (Com uma Engine por processo, a
      primeira opção é a que sai de graça.)
- [ ] Decidir e registrar a política de fila cheia: hoje `Concurrent_Observer::
      update()` ignora o `bool` de `insert()`, ou seja, descarta em silêncio.
