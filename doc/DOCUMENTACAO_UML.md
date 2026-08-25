# Documentação UML — libvcomm

**Biblioteca de comunicação para sistemas autônomos críticos**
INE5424 — Sistemas Operacionais II — UFSC — 2026/2 — Grupo M10 — Etapa 1

> Documento gerado por **engenharia reversa** do código-fonte, conferido contra
> `main` em **`9426f3b`** (*fix nic buffer and add timeout to updated()* —
> incluído). Todos os diagramas refletem o estado real do
> repositório, incluindo os pontos ainda não implementados: marcados com o
> estereótipo `«TODO»` e consolidados na
> [§4.4 Matriz de fidelidade](#44-matriz-de-fidelidade-código-vs-diagrama).
> Diagramas em sintaxe **Mermaid.js**; instruções de exportação para PNG na
> [§5](#5-exportação-para-png).

---

## Índice

| Seção | Conteúdo |
|---|---|
| [1. Arquitetura de Alto Nível](#1-arquitetura-de-alto-nível) | Componentes, pacotes e implantação |
| [2. Diagramas de Classes](#2-diagramas-de-classes-detalhados) | Cinco contextos arquiteturais |
| [3. Diagramas Comportamentais](#3-diagramas-comportamentais) | Sequência e atividade |
| [4. Anexos](#4-anexos) | Estados, legenda, rastreabilidade, fidelidade |
| [5. Exportação para PNG](#5-exportação-para-png) | CLI e extensões |

---

## Visão geral do sistema

`libvcomm` é uma pilha de comunicação em camadas, **header-only**, escrita em
C++17 no estilo do sistema operacional EPOS. Cada veículo é uma VM QEMU; cada
componente do veículo é um processo POSIX. As VMs conversam por **quadros
Ethernet crus em broadcast**, sem IP.

Dois princípios governam todo o desenho e explicam praticamente cada decisão
registrada neste documento:

1. **Isolamento de syscall.** Somente a classe `Raw_Socket_Engine` faz chamadas
   de sistema de rede. `NIC`, `Protocol` e `Communicator` não sabem que sockets
   existem. É essa promessa que permite trocar apenas o *Engine* na Etapa 2
   (memória compartilhada).
2. **Contexto de interrupção.** A recepção é propagada por **sinal POSIX
   (`SIGIO`)** — o análogo fiel da interrupção de hardware do EPOS. Todo o
   trecho entre `drain()` e `semaphore.v()` executa **dentro do tratador de
   sinal** e está sujeito a *async-signal-safety* (`man 7 signal-safety`): sem
   `printf`, sem `malloc`/`new`, sem `std::mutex`, sem `std::string`.

A parametrização por *policy* (`NIC<Engine>`, `Protocol<NIC>`,
`Communicator<Channel>`) é o mecanismo estrutural que sustenta o princípio 1; o
padrão **Observer × Observed em duas famílias** é o que sustenta o princípio 2.

---

# 1. Arquitetura de Alto Nível

## 1.1 Diagrama de Componentes — camadas e fronteiras

Cada camada recebe a camada inferior **já construída** e nunca a instancia. As
duas fronteiras horizontais destacadas são as invariantes arquiteturais do
sistema.

```mermaid
flowchart TB
    subgraph L5["Camada 5 — Aplicação"]
        direction LR
        APP["app/main.cpp «artefato»<br/>papel definido por SO2_VM_ID<br/>run_sender · run_receiver"]
        MSG["Message «value object»<br/>vetor de bytes · MAX_SIZE = 1024 B"]
    end

    subgraph L4["Camada 4 — API unificada do agente · FRONTEIRA DE CONTEXTO"]
        COMM["Communicator&lt;Channel&gt; «template»<br/>send() · receive() «bloqueante»"]
    end

    subgraph L3["Camada 3 — Rede / Transporte"]
        PROT["Protocol&lt;NIC_T&gt; «template»<br/>multiplexação por Port"]
    end

    subgraph L2["Camada 2 — Enlace portável"]
        direction LR
        NIC["NIC&lt;Engine&gt; «template»<br/>pool de buffers · marshalling · notificação"]
        ETH["Ethernet<br/>formatos de fio<br/>Address · Header · Frame · Statistics"]
    end

    subgraph L1["Camada 1 — Engine «policy» · FRONTEIRA DE SYSCALL"]
        ENG["Raw_Socket_Engine «policy — Etapa 1»<br/>socket · ioctl · bind · sigaction<br/>fcntl · sendto · recvfrom"]
    end

    subgraph SUPG["Núcleo de suporte — transversal às camadas 2 a 4"]
        SUP["Buffer · List · Ordered_List · Semaphore · Traits<br/>sem alocação dinâmica · capacidade fixa"]
    end

    subgraph OS["Sistema Operacional"]
        direction LR
        KSOCK["Kernel Linux<br/>AF_PACKET · SOCK_RAW · EtherType 0x88B5"]
        KSIG["Entrega de SIGIO<br/>O_ASYNC + F_SETOWN"]
    end

    APP -->|"send(Message*) / receive(Message*)"| COMM
    APP -.->|"«usa»"| MSG
    COMM -->|"_channel->send(from, to, data, size)"| PROT
    PROT -->|"alloc() · send(buf) · free(buf) · unmarshal()"| NIC
    NIC -->|"Engine::engine_send(frame, size)"| ENG
    NIC -.->|"«usa formatos»"| ETH
    ENG -->|"sendto(2)"| KSOCK

    KSOCK -.->|"quadro disponível"| KSIG
    KSIG -.->|"SIGIO → drain() → recvfrom(2)"| ENG
    ENG -.->|"handle(frame, size) «callback ascendente»"| NIC
    NIC -.->|"Observed::notify(EtherType, buf)"| PROT
    PROT -.->|"Observed::notify(Port, buf)"| COMM
    COMM -.->|"semaphore.v() desbloqueia receive()"| APP

    NIC -.->|"«usa»"| SUP
    PROT -.->|"«usa»"| SUP
    COMM -.->|"«usa»"| SUP
```

**As duas fronteiras.** *Camada 1* é a **fronteira de syscall**: nada acima dela
conhece sockets, e é essa promessa que permite trocar apenas o *Engine* na Etapa
2 — a substituição está modelada em
[§2.5](#instanciação-da-pilha-concreta--libvcommh), onde `Raw_Socket_Engine` e o
futuro `Shared_Memory_Engine` aparecem como realizações do mesmo contrato.
*Camada 4* é a **fronteira de contexto**: o `Communicator` é o último objeto
tocado dentro do tratador de `SIGIO` — `v()` deposita e retorna —, e o primeiro
tocado de volta na thread da aplicação, quando `p()` acorda.

**Leitura das setas.** As **contínuas** descem: é o caminho de transmissão,
chamada síncrona comum, na thread da aplicação. As **pontilhadas** sobem: é o
caminho de recepção, notificação assíncrona originada no tratador de `SIGIO`.
Toda a subida — de `handle()` até `semaphore.v()` — executa **dentro do
tratador**, e é por isso que a fronteira de contexto está desenhada exatamente
no `Communicator`: `v()` deposita e retorna, e quem acorda do outro lado, já na
thread da aplicação, é o `p()` de `receive()`.

## 1.2 Diagrama de Pacotes — grafo real de dependências

Grafo extraído das diretivas `#include` do projeto. Note que
`engine/raw_socket_engine.h` depende **apenas** de `ethernet.h` — é o que
garante que um *Engine* alternativo seja plugável.

```mermaid
flowchart LR
    subgraph PKG_APP["app/"]
        MAIN["main.cpp"]
    end

    subgraph PKG_INC["include/ — biblioteca header-only"]
        direction TB
        UMB["libvcomm.h<br/>«umbrella» + typedefs da pilha concreta"]
        subgraph PKG_LAYERS["Camadas"]
            direction TB
            H_COMM["communicator.h"]
            H_PROT["protocol.h"]
            H_NIC["nic.h"]
        end
        subgraph PKG_SUP["Núcleo de suporte"]
            direction TB
            H_OBS["observer.h"]
            H_LIST["list.h"]
            H_SEM["sem.h"]
            H_BUF["buffer.h"]
            H_MSG["message.h"]
            H_ETH["ethernet.h"]
            H_TRA["traits.h"]
        end
    end

    subgraph PKG_ENG["include/engine/ + src/"]
        H_ENG["raw_socket_engine.h"]
        C_ENG["raw_socket_engine.cpp"]
    end

    subgraph PKG_EXT["Bibliotecas externas"]
        direction TB
        STD["Biblioteca padrão C++<br/>&lt;atomic&gt; · &lt;cstddef&gt; · &lt;cstring&gt; · &lt;csignal&gt;"]
        POSIX["POSIX / Linux<br/>&lt;semaphore.h&gt; · &lt;cerrno&gt; · &lt;arpa/inet.h&gt;<br/>&lt;sys/socket.h&gt; · &lt;netpacket/packet.h&gt; · &lt;fcntl.h&gt;"]
    end

    MAIN --> UMB
    UMB --> H_COMM
    UMB --> H_PROT
    UMB --> H_NIC
    UMB --> H_ENG
    UMB --> H_MSG
    UMB --> H_OBS
    UMB --> H_SEM
    UMB --> H_LIST
    UMB --> H_BUF
    UMB --> H_ETH
    UMB --> H_TRA

    H_COMM --> H_OBS
    H_COMM --> H_MSG
    H_PROT --> H_OBS
    H_PROT --> H_ETH
    H_PROT --> H_TRA
    H_NIC --> H_OBS
    H_NIC --> H_BUF
    H_NIC --> H_ETH
    H_NIC --> H_TRA
    H_OBS --> H_LIST
    H_OBS --> H_SEM
    H_ETH --> H_TRA
    H_ENG --> H_ETH
    C_ENG --> H_ENG

    H_BUF --> STD
    H_LIST --> STD
    H_MSG --> STD
    H_ETH --> STD
    H_PROT --> STD
    H_ENG --> STD
    H_SEM --> POSIX
    H_NIC --> POSIX
    C_ENG --> POSIX
```

> **Como ler.** Toda seta é uma diretiva `#include` real, na direção
> *inclui → incluído*. O grafo é acíclico e `traits.h` é sua única folha
> interna: `traits.h` é o ponto exclusivo de configuração da biblioteca
> (EtherType, interface, tamanho do pool) e **não depende de nada** — nenhum
> arquivo de configuração depende de código.
>
> A aresta que sustenta a Etapa 2 é a ausência de arestas: `raw_socket_engine.h`
> alcança apenas `ethernet.h` e `<csignal>`. Ele não conhece `nic.h`, e é por
> isso que um *Engine* alternativo é plugável.

## 1.3 Diagrama de Implantação — a frota

```mermaid
flowchart TB
    subgraph HOST["«device» Máquina hospedeira — Linux + QEMU/KVM"]
        direction LR
        MK["«artefato» Makefile<br/>alvos: app · test-* · image<br/>fleet · capture · stats · check"]
        FLEET["«artefato» scripts/run-fleet.sh<br/>sobe as 5 VMs em paralelo"]
        CAP["«artefato» scripts/capture.sh<br/>dumpcap / tshark — evidência de tráfego"]
        MK -->|"«invoca»"| FLEET
        MK -->|"«invoca»"| CAP
    end

    BUS(["«meio compartilhado» Barramento da frota<br/>backend QEMU socket,mcast = 239.10.10.10:15424<br/>modela uma célula de rádio"])

    subgraph VM1["«execution environment» VM 1 — Veículo 1 «SENDER»"]
        direction TB
        P1["«processo» /student/app<br/>SO2_VM_ID = 1"]
        N1["«device» eth0 · virtio-net-pci<br/>MAC 02:00:00:00:00:01"]
        P1 ---|"«deploy»"| N1
    end

    subgraph VM2["«execution environment» VM 2 — Veículo 2 «RECEIVER»"]
        direction TB
        P2["«processo» /student/app<br/>SO2_VM_ID = 2"]
        N2["«device» eth0<br/>MAC 02:00:00:00:00:02"]
        P2 ---|"«deploy»"| N2
    end

    subgraph VMN["«execution environment» VMs 3, 4 e 5 — Veículos «RECEIVER»"]
        direction TB
        PN["«processo» /student/app<br/>SO2_VM_ID = 3..5"]
        NN["«device» eth0<br/>MAC 02:00:00:00:00:0N"]
        PN ---|"«deploy»"| NN
    end

    LOGS["«artefato» build/logs/vm-N.log — um por VM<br/>RESULT vm=N received=X expected=Y"]

    FLEET -.->|"«instancia»"| VM1
    FLEET -.->|"«instancia»"| VM2
    FLEET -.->|"«instancia»"| VMN

    N1 -->|"quadros EtherType 0x88B5<br/>dst = ff:ff:ff:ff:ff:ff"| BUS
    BUS -->|"broadcast"| N2
    BUS -->|"broadcast"| NN
    CAP -.->|"«observa»"| BUS

    VM1 -.->|"«produz»"| LOGS
    VM2 -.-> LOGS
    VMN -.-> LOGS
```

> **Onde há IP e onde não há.** Dentro das VMs **não existe pilha IP**: o
> `Raw_Socket_Engine` emite quadros Ethernet crus com EtherType 0x88B5 e destino
> `ff:ff:ff:ff:ff:ff`. O endereço `239.10.10.10:15424` é do **backend do QEMU no
> hospedeiro** (`-netdev socket,mcast=…`), que encapsula esses quadros em UDP
> multicast para que as cinco VMs compartilhem um único domínio de colisão. Os
> dois níveis não se misturam — e é essa distinção que a captura em `lo` mostra.

**Restrição de implantação relevante.** `Raw_Socket_Engine::_instance` é um
ponteiro **estático**, usado como *trampolim* do tratador de sinal — e a
disposição de sinais é estado global do processo. Consequência: **um Engine por
processo**. Na Etapa 1 isso não incomoda (1 VM = 1 veículo = 1 NIC); é a
primeira premissa a revisitar quando um veículo virar vários processos.

---

# 2. Diagramas de Classes Detalhados

> **Convenções de notação.** Visibilidade `+` público, `-` privado, `#`
> protegido. Membro **sublinhado** = estático (marcado `$` no fonte Mermaid);
> método em *itálico* = virtual puro. Abreviações de tipo: `uint` =
> `unsigned int`, `ushort` = `unsigned short`, `uchar` = `unsigned char`. Todo
> parâmetro de template aparece no estereótipo — `«template T, C, CAP»` — e a
> notação `Classe~T~` é usada apenas no nome quando há um parâmetro dominante.
> Classes **aninhadas** em C++ são agrupadas num pacote que nomeia o hospedeiro
> — aninhamento é relação entre *tipos*, não entre objetos, e por isso **não** há
> composição do hospedeiro para elas: `*--` só aparece onde existe de fato um
> membro por valor. Instanciação de template é dependência `«bind»`, do tipo
> ligado para o template — nunca realização. Detalhes em
> [§4.3](#43-legenda-de-notação).

## 2.1 Contexto: Núcleo de Suporte

Classes de infraestrutura, todas **livres de alocação dinâmica** e com
capacidade fixa — requisito imposto pela execução em contexto de sinal.

```mermaid
classDiagram
    direction TB

    class Traits~T~ {
        <<template T>>
        +bool debugged$
    }

    class Traits_Ethernet {
        <<template specialization>>
        +const char* INTERFACE$
        +ushort PROTOCOL_NUMBER$
        +uint SEND_BUFFERS$
        +uint RECEIVE_BUFFERS$
        +bool debugged$
    }

    class Buffer~T~ {
        <<template T>>
        -T _frame
        -uint _size
        -atomic_bool _in_use
        +Buffer()
        +T* frame()
        +uint size()
        +void size(uint s)
        +bool lock()
        +void unlock()
        +bool in_use()
    }

    class List~T~ {
        <<template T, CAP = 32>>
        -T* _slot[CAP]
        -atomic_uint _head
        -atomic_uint _tail
        +uint CAPACITY$
        +List()
        +bool insert(T* e)
        +T* remove()
        +bool empty()
        +uint size()
    }

    class Ordered_List~T~ {
        <<template T, C, CAP = 16>>
        -atomic_ptr _slot[CAP]
        -atomic_uint _size
        +uint CAPACITY$
        +Ordered_List()
        +Iterator begin()
        +Iterator end()
        +bool insert(T* e)
        +void remove(T* e)
        +uint size()
        +bool empty()
    }

    class Iterator {
        <<nested em Ordered_List>>
        -const atomic_ptr* _p
        -const atomic_ptr* _e
        +Iterator(p, e)
        +T* operator_deref()
        +T* operator_arrow()
        +Iterator operator_inc()
        +bool operator_eq(Iterator i)
        +bool operator_neq(Iterator i)
        -void skip()
    }

    class Semaphore {
        -sem_t _sem
        +Semaphore(int init)
        +void p()
        +void v()
        +bool try_p()
        +int value()
    }

    class Message {
        -uchar _data[MAX_SIZE]
        -uint _size
        +uint MAX_SIZE$
        +Message()
        +Message(const void* data, uint size)
        +void* data()
        +uint size()
        +void size(uint s)
        +uint set(const void* data, uint size)
    }

    Traits_Ethernet ..> Traits : «bind» T = Ethernet
    Iterator ..> Ordered_List : «nested» percorre saltando lápides

    note for List "CAP = 32, potência de 2: o wraparound é um AND, não um módulo.<br/>Uma posição fica sempre vazia para distinguir cheio de vazio → CAPACITY = CAP-1 = 31.<br/>Produtor = tratador de sinal (insert); consumidor = thread da aplicação (remove)."
    note for Ordered_List "CAP = 16, CAPACITY = 16. detach() não compacta: escreve 0 na posição (lápide).<br/>Compactar deslocaria índices sob uma travessia em curso dentro do tratador.<br/>_size é marca d'água: só cresce; size() conta os vivos, ignorando lápides."
    note for Buffer "lock() usa test-and-set atômico (exchange acq_rel).<br/>Desde 9426f3b o pool da NIC é PARTICIONADO: envio e recepção têm 16 posições<br/>cada e não disputam mais as mesmas — ver a nota da NIC em §2.3."
    note for Semaphore "sem_post(3) é async-signal-safe — é o que torna legal<br/>notificar de dentro do tratador. sem_wait() retorna EINTR: o laço de p() relança."
    note for Message "Etapa 1: M = {.*}, só bytes. As Etapas 2 a 6 acrescentam<br/>origem, timestamp, tipo e MAC — o espaço está reservado, o código não."
```

**Por que capacidade fixa.** `pthread_mutex_lock` e `malloc` **não** constam da
lista de funções *async-signal-safe*. Como a recepção acontece dentro de um
tratador de sinal, `List` e `Ordered_List` foram construídas sobre
`std::atomic` *lock-free* — os `static_assert` de `list.h` fazem o compilador
provar isso na plataforma alvo. O preço é não poder crescer: as duas estruturas
respondem "não" quando enchem, em vez de alocar.

## 2.2 Contexto: Padrão Observer × Observed

O coração do desacoplamento. **Duas famílias**, e a diferença entre elas é a
razão de ambas existirem.

```mermaid
classDiagram
    direction TB

    class Conditionally_Data_Observed {
        <<template T, C>>
        #Observers _observers
        +Conditionally_Data_Observed()
        +void attach(Observer* o, C c)
        +void detach(Observer* o, C c)
        +bool notify(C c, T* d)
    }

    class Conditional_Data_Observer {
        <<template T, C — abstrata>>
        #C _rank
        +Conditional_Data_Observer()
        +Conditional_Data_Observer(C c)
        +void update(C c, T* d)*
        +C rank()
        +void rank(C c)
    }

    class Concurrent_Observed {
        <<template D, C>>
        #Observers _observers
        +Concurrent_Observed()
        +void attach(Concurrent_Observer* o, C c)
        +void detach(Concurrent_Observer* o, C c)
        +bool notify(C c, D* d)
    }

    class Concurrent_Observer {
        <<template D, C>>
        -Semaphore _semaphore
        -List _data
        -C _rank
        +Concurrent_Observer()
        +bool update(C c, D* d)
        +D* updated()
        +D* updated(uint timeout_ms)
        +C rank()
        +void rank(C c)
    }

    class List~D~ {
        <<template D, CAP = 32>>
        +bool insert(D* e)
        +D* remove()
    }
    class Semaphore {
        +void p()
        +void v()
    }

    Conditionally_Data_Observed "1" o-- "0..16" Conditional_Data_Observer : notifica se rank == c
    Concurrent_Observed "1" o-- "0..16" Concurrent_Observer : notifica se rank == c
    Concurrent_Observer "1" *-- "1" List : _data
    Concurrent_Observer "1" *-- "1" Semaphore : _semaphore
    Concurrent_Observed ..> Concurrent_Observer : «friend» mútuo

    note for Conditionally_Data_Observed "_observers é um Ordered_List&lt;Observer, C&gt; por valor (CAP = 16) — ver §2.1.<br/>notify() percorre e chama update() em quem tem rank == c;<br/>retorna false se ninguém quis — é esse false que autoriza liberar o buffer."
    note for Conditional_Data_Observer "SÍNCRONO. update() executa no contexto de quem notifica —<br/>o TRATADOR DE SINAL. NÃO PODE BLOQUEAR: quem bloqueia dentro de um<br/>tratador congela a thread interrompida, que nada tem a ver com isso.<br/>Fronteira NIC → Protocol."
    note for Concurrent_Observed "Mesma estrutura de _observers, mas notify() aqui só conta como<br/>notificado quem ACEITOU: se update() devolve false, notified não muda.<br/>Fila cheia virou indistinguível de 'ninguém quis' — e é o que autoriza<br/>o Protocol a liberar o buffer em vez de vazá-lo.<br/>Regra que isso impõe: no máximo UM observador por rank pode aceitar,<br/>senão dois liberariam o mesmo buffer."
    note for Concurrent_Observer "DESACOPLADO por semáforo. update() apenas enfileira e faz v();<br/>quem chamou updated() dormia em p() e acorda com o ponteiro.<br/>Fronteira Protocol → Communicator (tratador → thread da aplicação).<br/>update() devolve FALSE quando a fila está cheia, e o v() só acontece<br/>depois de um insert() bem-sucedido — updated() nunca acorda em fila vazia.<br/>«TODO» updated(timeout_ms) chama um Semaphore::p(timeout_ms) que não existe — ver §4.4."
```

### Por que duas famílias — e não uma

| Fronteira | Família | Justificativa |
|---|---|---|
| `NIC` → `Protocol` | `Conditionally_Data_Observed` | executa dentro do tratador de sinal; **não pode bloquear** |
| `Protocol` → `Communicator` | `Concurrent_Observed` | do outro lado há uma thread de aplicação adormecida; **precisa do semáforo** — e `sem_post` é *async-signal-safe* |

O **rank** é a *condição*: para a `NIC` o rank é o EtherType; para o `Protocol`
é a `Port`. `notify(c, d)` só chama os observadores cujo `rank == c` — é isso, e
somente isso, que "condicional" significa aqui. O retorno `false` de `notify()`
significa *ninguém quis este quadro* e é o que autoriza a liberação do buffer.

**As duas famílias divergiram em `9426f3b`, e a diferença importa.** Em
`Concurrent_Observed::notify()` o `update()` do observador passou a devolver
`bool`, e só conta como notificado quem **aceitou** — de modo que fila cheia é
indistinguível de "ninguém quis" e o `Protocol` libera o buffer nos dois casos.
Em `Conditionally_Data_Observed::notify()` isso **não** aconteceu: `update()`
segue `void` e `notified` vira `true` só por ter havido a chamada. Consequência
prática, e é a origem do item 4 da [§4.4](#44-matriz-de-fidelidade-código-vs-diagrama):
quando a fila do `Communicator` enche, a `NIC` continua contando `rx_packets`,
porque do ponto de vista dela *houve* um `Protocol` interessado no EtherType.

Essa assimetria também impõe uma regra de posse: **no máximo um observador por
rank pode aceitar**, senão dois liberariam o mesmo buffer. Cada processo desta
biblioteca liga um `Communicator` por porta, então a regra vale por construção —
mas vale por construção, não por verificação.

## 2.3 Contexto: Camada de Enlace

Herança **múltipla e mista** é o traço central desta camada: a `NIC` herda
`Ethernet` (formatos) e `Conditionally_Data_Observed` (papel de observado) em
público, e o `Engine` em **privado**.

```mermaid
classDiagram
    direction TB

    namespace Classes_aninhadas_de_Ethernet {
        class Ethernet_Address {
            -uchar _addr[6]
            +Address()
            +Address(Null)
            +Address(uchar a0..a5)
            +Address(const uchar* raw)
            +uchar* bytes()
            +bool operator_eq(const Address& a)
            +bool operator_neq(const Address& a)
            +bool operator_bool()
            +char* to_string(char* buf)
        }
        class Ethernet_Header {
            +Ethernet_Address dst
            +Ethernet_Address src
            +ushort prot
        }
        class Ethernet_Frame {
            +uchar data[MTU]
        }
        class Ethernet_Statistics {
            +uint tx_packets
            +uint tx_bytes
            +uint rx_packets
            +uint rx_bytes
            +uint rx_dropped
        }
    }

    class Ethernet {
        +uint MTU$
        +uint HEADER_SIZE$
        +Ethernet_Address BROADCAST$
    }

    class Raw_Socket_Engine {
        <<policy — Etapa 1>>
        -int _sockfd
        -uint _ifindex
        -Ethernet_Address _address
        -ushort _protocol
        -volatile sig_atomic_t _armed
        -volatile sig_atomic_t _rx_error
        -volatile sig_atomic_t _rx_errors
        -Raw_Socket_Engine* _instance$
        #Raw_Socket_Engine(const char* iface, ushort prot)
        #int engine_send(const Ethernet_Frame* frame, uint size)
        #bool engine_start()
        #void engine_stop()
        #const Ethernet_Address& engine_address()
        #bool engine_valid()
        #uint engine_rx_errors()
        #int engine_rx_error()
        #void handle(Ethernet_Frame* frame, uint size)*
        -void signal_handler(int signo)$
        -void drain()
    }

    class NIC~Engine~ {
        <<template Engine>>
        +uint BUFFER_SIZE$
        -Ethernet_Statistics _statistics
        -Buffer _buffer[BUFFER_SIZE]
        -bool _armed
        +NIC(const char* iface = Traits::INTERFACE)
        +bool valid()
        +int send(Address dst, ushort prot, const void* data, uint size)
        +Buffer* alloc(Address dst, ushort prot, uint size)
        +int send(Buffer* buf)
        +void free(Buffer* buf)
        +int unmarshal(Buffer* buf, Address* src, Address* dst, void* data, uint size)
        +const Address& address()
        +const Statistics& statistics()
        -void handle(Ethernet_Frame* frame, uint size)
    }

    class Buffer~T~ {
        <<template T>>
        +bool lock()
        +void unlock()
    }
    class Conditionally_Data_Observed {
        <<template T, C>>
        +bool notify(C c, T* d)
    }
    class Traits_Ethernet {
        <<template specialization>>
    }

    Ethernet_Frame --|> Ethernet_Header : «herança» — layout de fio
    Ethernet_Header "1" *-- "2" Ethernet_Address : dst, src

    NIC --|> Ethernet : «public» formatos
    NIC --|> Conditionally_Data_Observed : «public» papel de observado
    NIC --|> Raw_Socket_Engine : «private» parâmetro Engine (ligado em §2.5)
    NIC "1" *-- "32" Buffer : _buffer — pool pré-alocado
    NIC "1" *-- "1" Ethernet_Statistics : _statistics
    NIC ..> Traits_Ethernet : «configura» INTERFACE, PROTOCOL_NUMBER
    Raw_Socket_Engine ..> Ethernet_Frame : lê e escreve

    note for Ethernet "Este pacote reúne as classes ANINHADAS de Ethernet — Address, Header,<br/>Frame e Statistics. Em C++ são tipos internos, NÃO membros: por isso não há<br/>composição entre Ethernet e elas, só entre quem de fato guarda um objeto.<br/>static_assert garante Header = 14 B e Address = 6 B: se algum campo ganhar<br/>padding, o compilador avisa antes da VM. O EtherType viaja em ordem de rede;<br/>dentro da biblioteca fica em ordem do host."
    note for NIC "Herança PRIVADA do Engine: dá acesso aos métodos protegidos sem expor nada<br/>fora da NIC, e permite sobrescrever handle() — que é como o Engine empurra<br/>quadros para cima. Escolha do EPOS. Cópia e atribuição são = delete.<br/>O construtor chama engine_start() se engine_valid(); o destrutor, engine_stop().<br/><br/>POOL PARTICIONADO: BUFFER_SIZE = SEND_BUFFERS + RECEIVE_BUFFERS = 16 + 16 = 32,<br/>e as duas metades são estanques — alloc() só toma de [0, 16), handle() só de [16, 32).<br/>Com um pool único, uma rajada de transmissão deixava handle() sem buffer; handle()<br/>roda em contexto de sinal e NÃO pode esperar, logo o quadro se perdia. O emissor,<br/>ao contrário, recebe 0 de alloc() e pode tentar de novo. A recepção é o lado que<br/>não pode ser faminto. Preço: nenhuma metade empresta da outra."
    note for Ethernet_Frame "CONTRATO DE unmarshal(), fechado em 9426f3b: devolve BYTES DO FRAME<br/>MENOS O CABEÇALHO — padding incluído —, e não 'bytes de payload'.<br/>A camada de enlace não tem campo de comprimento próprio: Ethernet::Header<br/>são os 14 bytes do fio, e um décimo-quinto deixaria de ser Ethernet.<br/>Quem precisa do tamanho verdadeiro lê Protocol::Header::_length (§2.4)."
    note for Raw_Socket_Engine "_instance é estático (trampolim do tratador de sinal, que é função livre<br/>e não tem 'this'). Consequência: UM Engine POR PROCESSO.<br/>O construtor NÃO arma a recepção — handle() é virtual pura e a classe<br/>derivada ainda não terminou de construir."
    note for Buffer "REGRA DE PROPRIEDADE: alloc() transfere a posse ao chamador.<br/>A posse viaja com o ponteiro; quem recebeu o buffer é quem chama free().<br/>Um free() esquecido no caminho de envio = vazamento silencioso após 16<br/>mensagens — o tamanho da metade TX, não os 32 do pool inteiro. Ver §3.4."
```

## 2.4 Contexto: Camada de Rede / Transporte

O `Protocol` tem **duas faces**, e é essa dobra que torna a recepção assíncrona
de ponta a ponta sem *rendezvous*.

```mermaid
classDiagram
    direction TB

    namespace Classes_aninhadas_de_Protocol {
        class Protocol_Address {
            -Physical_Address _paddr
            -Port _port
            +Address()
            +Address(Null)
            +Address(Physical_Address paddr, Port port)
            +const Physical_Address& paddr()
            +Port port()
            +bool operator_eq(const Address& a)
            +bool operator_neq(const Address& a)
            +bool operator_bool()
            +Address broadcast(Port p)$
        }
        class Protocol_Header {
            +Port _from_port
            +Port _to_port
            +ushort _length
            +Header()
        }
        class Protocol_Packet {
            -uchar _data[MTU]
            +Packet()
            +Header* header()
            +T* data()
        }
    }

    class Protocol~NIC_T~ {
        <<template NIC_T>>
        +ushort PROTO$
        +uint MTU$
        -NIC_T* _nic
        -Observed _observed
        +Protocol(NIC_T* nic)
        +int send(Address from, Address to, const void* data, uint size)
        +int receive(Buffer* buf, Address* from, void* data, uint size)
        +void attach(Observer* obs, const Address& address)
        +void detach(Observer* obs, const Address& address)
        -void update(const ushort& prot, Buffer* buf)
    }

    class NIC~Engine~ {
        <<template Engine>>
        +Buffer* alloc(Address dst, ushort prot, uint size)
        +int send(Buffer* buf)
        +void free(Buffer* buf)
        +int unmarshal(Buffer* buf, Address* src, Address* dst, void* data, uint size)
        +void attach(Observer* o, ushort c)
        +void detach(Observer* o, ushort c)
    }

    class Conditional_Data_Observer {
        <<template T, C — abstrata>>
        +void update(C c, T* d)*
    }
    class Concurrent_Observed {
        <<template D, C>>
        +void attach(Concurrent_Observer* o, C c)
        +bool notify(C c, D* d)
    }
    class Concurrent_Observer {
        <<template D, C>>
        +D* updated()
    }
    class Ethernet_Address {
        <<de §2.3>>
    }

    Protocol --|> Conditional_Data_Observer : «private» FACE 1 — observador da NIC
    Protocol "1" *-- "1" Concurrent_Observed : _observed — FACE 2, observado pelos Communicators
    Protocol "0..16" --> "1" NIC : _nic «associação não-proprietária»
    Protocol_Packet --|> Protocol_Header : «herança» — layout de fio
    Protocol_Address "1" *-- "1" Ethernet_Address : _paddr «por valor»
    Concurrent_Observed "1" o-- "0..16" Concurrent_Observer : por Port

    note for Protocol "AS DUAS FACES:<br/>· como OBSERVADOR da NIC herda NIC::Observer e implementa update() —<br/>  executa DENTRO do tratador de sinal, não pode bloquear;<br/>· como OBSERVADO pelos Communicators contém um Concurrent_Observed —<br/>  esse desacopla por semáforo, pois do outro lado há uma thread dormindo.<br/>O construtor faz attach na NIC; o destrutor, detach — sem isso notify()<br/>alcançaria um objeto morto. Port = unsigned short; MTU = 1500 - 6 = 1494 B."
    note for Protocol_Address "O pacote acima reúne as classes ANINHADAS de Protocol; a composição<br/>desenhada é a única real — _paddr é um Ethernet::Address por valor.<br/>Endereço LÓGICO da biblioteca = (endereço físico, porta).<br/>A NIC separa por EtherType; o Protocol separa por Port —<br/>é o que decide qual processo, dentro da VM, deve acordar."
    note for Protocol_Header "O campo _length torna o tamanho do payload confiável em<br/>QUALQUER meio. Sem ele, 'tamanho = recebido - 14' só funciona<br/>porque o virtio-net do QEMU não faz padding para 60 bytes —<br/>e deixaria de funcionar no Engine de memória compartilhada.<br/>«TODO» campos públicos, sem acessores — ver §4.4."
```

### Encapsulamento de cabeçalhos (visão de fio)

```mermaid
flowchart LR
    subgraph FRAME["Ethernet::Frame — no máximo 1514 B"]
        direction LR
        subgraph EHDR["Ethernet::Header — 14 B · ordem de rede"]
            direction LR
            D["dst<br/>6 B"]
            S["src<br/>6 B"]
            T["EtherType<br/>2 B · 0x88B5"]
        end
        subgraph PAYLOAD["payload Ethernet — até 1500 B"]
            direction LR
            subgraph PHDR["Protocol::Header — 6 B"]
                direction LR
                FP["_from_port<br/>2 B"]
                TP["_to_port<br/>2 B"]
                LEN["_length<br/>2 B"]
            end
            DATA["Protocol::Packet::_data<br/>Message::data — até 1494 B"]
        end
    end
    D --- S --- T --- FP --- TP --- LEN --- DATA
```

> **As larguras dos blocos não estão em escala** — cada caixa traz o seu tamanho
> em bytes. Os dois `static_assert` de `ethernet.h` e o `__attribute__((packed))`
> de `Ethernet::Header`, `Protocol::Header` e `Protocol::Packet` são o que garante
> que este desenho e a memória coincidam.

## 2.5 Contexto: Aplicação e Instanciação de Templates

```mermaid
classDiagram
    direction TB

    class Communicator~Channel~ {
        <<template Channel>>
        -Channel* _channel
        -Address _address
        +Communicator(Channel* channel, Address address)
        +bool send(const Message* message)
        +bool receive(Message* message)
        +const Address& address()
        -void update(const C& c, Buffer* buf)
    }

    class Concurrent_Observer {
        <<template D, C>>
        -Semaphore _semaphore
        -List _data
        +bool update(C c, D* d)
        +D* updated()
        +D* updated(uint timeout_ms)
    }

    class Message {
        +uint MAX_SIZE$
        +void* data()
        +uint size()
        +uint set(const void* data, uint size)
    }

    class Protocol~NIC_T~ {
        <<template NIC_T>>
        +int send(Address from, Address to, const void* data, uint size)
        +int receive(Buffer* buf, Address* from, void* data, uint size)
        +void attach(Observer* obs, const Address& address)
        +void detach(Observer* obs, const Address& address)
    }

    class Main_Application {
        <<artefato — app/main.cpp>>
        -Role role_of(int vm_id)$
        -int vm_id_from_env()$
        -void run_sender(Vehicle_Communicator& comm, int vm_id)$
        -void run_receiver(Vehicle_Communicator& comm, int vm_id)$
        +int main()$
    }

    Communicator --|> Concurrent_Observer : «public»
    Communicator "0..16" --> "1" Protocol : _channel «associação não-proprietária»
    Communicator ..> Message : «usa» empacota e desempacota
    Main_Application "1" *-- "1" Communicator : comm «variável local de main()»
    Main_Application ..> Message : «cria»

    note for Communicator "ÚNICO ponto de contato da aplicação com a pilha.<br/>Sensor, fusor ou ECU: todos veem apenas send(Message*) e receive(Message*).<br/>receive() BLOQUEIA — e é o único ponto da biblioteca que bloqueia.<br/>Bloqueia no SEMÁFORO herdado, não em recvfrom(): é aí que mora a assincronia.<br/>Não endereça o destino: envia sempre em broadcast.<br/>Cópia e atribuição são = delete; o destrutor faz detach no Protocol."
    note for Main_Application "Um único binário é instalado em todas as VMs;<br/>o PAPEL vem de SO2_VM_ID: 1 transmite, 2..5 recebem.<br/>«TODO» run_sender / run_receiver ainda vazios; verificação de nic.valid()<br/>e fork() dos componentes pendentes — ver §4.4."
```

### Instanciação da pilha concreta — `libvcomm.h`

O ponto **exato** onde os templates viram tipos reais. É a única linha que muda
quando a Etapa 2 trouxer o *Engine* de memória compartilhada — e se essa troca
exigir tocar em `NIC`, `Protocol` ou `Communicator`, a separação de camadas
falhou em algum lugar.

```mermaid
classDiagram
    direction LR

    class NIC~Engine~ {
        <<template Engine>>
    }
    class Protocol~NIC_T~ {
        <<template NIC_T>>
    }
    class Communicator~Channel~ {
        <<template Channel>>
    }

    class Engine_Policy {
        <<interface — conceito estrutural>>
        +Engine(const char* iface, ushort prot)
        +int engine_send(const Frame* frame, uint size)
        +bool engine_start()
        +void engine_stop()
        +const Address& engine_address()
        +bool engine_valid()
        +void handle(Frame* frame, uint size)*
    }
    class Raw_Socket_Engine {
        <<policy — Etapa 1>>
    }
    class Shared_Memory_Engine {
        <<policy — Etapa 2, planejada>>
    }

    class Vehicle_NIC {
        <<typedef>>
    }
    class Vehicle_Protocol {
        <<typedef>>
    }
    class Vehicle_Communicator {
        <<typedef>>
    }

    Vehicle_NIC ..> NIC : «bind» Engine = Raw_Socket_Engine
    Vehicle_Protocol ..> Protocol : «bind» NIC_T = Vehicle_NIC
    Vehicle_Communicator ..> Communicator : «bind» Channel = Vehicle_Protocol

    NIC ..> Engine_Policy : «requer» do parâmetro Engine
    Raw_Socket_Engine ..|> Engine_Policy : «realiza»
    Shared_Memory_Engine ..|> Engine_Policy : «realiza»

    note for Engine_Policy "Este contrato NÃO existe como tipo em C++ — não há classe base nem<br/>concept declarado. Ele é verificado pelo compilador na instanciação do<br/>template: é isso que 'policy' significa. Desenhá-lo torna visível o que<br/>a Etapa 2 precisa entregar para que NIC, Protocol e Communicator não mudem."
    note for Vehicle_NIC "typedef NIC&lt;Raw_Socket_Engine&gt; Vehicle_NIC;<br/>typedef Protocol&lt;Vehicle_NIC&gt; Vehicle_Protocol;<br/>typedef Communicator&lt;Vehicle_Protocol&gt; Vehicle_Communicator;<br/>Três linhas em libvcomm.h — e são as únicas que a Etapa 2 precisa tocar."
```

---

# 3. Diagramas Comportamentais

## 3.1 Sequência — Inicialização da pilha (bootstrap)

A ordem de construção **não é acidental**: cada camada recebe a inferior já
pronta, e o armamento da recepção acontece por último, depois que todo o estado
que `handle()` toca já existe.

```mermaid
sequenceDiagram
    autonumber
    participant Main as main
    participant NIC as Vehicle_NIC
    participant Eng as Raw_Socket_Engine
    participant K as Kernel Linux
    participant Prot as Vehicle_Protocol
    participant Comm as Vehicle_Communicator

    Main->>NIC: Vehicle_NIC nic
    activate NIC
    Note over NIC,Eng: A NIC herda o Engine em privado:<br/>o subobjeto base é construído primeiro
    NIC->>Eng: Raw_Socket_Engine(Traits::INTERFACE, Traits::PROTOCOL_NUMBER)
    activate Eng
    Eng->>K: socket(AF_PACKET, SOCK_RAW, htons(0x88B5))
    K-->>Eng: _sockfd
    Eng->>K: if_nametoindex("eth0")
    K-->>Eng: _ifindex
    Eng->>K: ioctl(SIOCGIFHWADDR)
    K-->>Eng: MAC real da interface
    Eng->>K: bind(sockaddr_ll)
    Eng->>Eng: _instance = this
    Eng->>K: sigaction(SIGIO, signal_handler)
    Eng->>K: fcntl(F_SETOWN, getpid())
    Eng->>K: fcntl(F_SETFL, O_NONBLOCK)
    Note over Eng,K: O construtor do Engine NÃO arma O_ASYNC. handle() é virtual PURA e a<br/>classe derivada ainda não terminou de construir: um quadro chegando<br/>neste ponto chamaria virtual pura em objeto meio-construído
    deactivate Eng

    NIC->>NIC: _statistics, _buffer[32] (16 TX + 16 RX) e _armed = false construídos
    alt engine_valid()
        NIC->>Eng: engine_start()
        Eng->>K: fcntl(F_SETFL, ... OR O_ASYNC) · _armed = 1
        Eng-->>NIC: true
        NIC->>NIC: _armed = true
    else socket inválido — sem CAP_NET_RAW, por exemplo
        NIC->>NIC: _armed permanece false · nic.valid() = false
    end
    Note over NIC,K: ÚLTIMO passo do construtor da NIC, e não por acaso: a partir daqui<br/>handle() pode ser chamada a qualquer instrução, e todo o estado que ela<br/>toca — pool, _statistics, lista de observadores — já existe
    deactivate NIC
    NIC-->>Main: nic pronta — nic.valid() diz se a recepção foi armada

    Main->>Prot: Vehicle_Protocol protocol(&nic)
    activate Prot
    Prot->>Prot: NIC_T::Observer(PROTO) — fixa rank = EtherType
    Prot->>NIC: attach(this, PROTO)
    NIC->>NIC: _observers.insert(protocol)
    Note over NIC,Prot: A partir daqui, notify(0x88B5, buf) alcança Protocol::update().<br/>ANTES daqui a lista está vazia: um quadro que chegue nesta janela<br/>faz notify() retornar false, a NIC libera o buffer e conta rx_dropped.<br/>Descartar é correto — a alternativa seria enfileirar sem dono
    deactivate Prot

    Main->>Comm: Vehicle_Communicator comm(&protocol, Address(nic.address(), 1000))
    activate Comm
    Comm->>Prot: attach(this, address)
    Prot->>Prot: _observed.attach(comm, address.port())
    Note over Prot,Comm: A partir daqui, notify(1000, buf)<br/>alcança Communicator::update()
    deactivate Comm

    Main->>Main: role_of(SO2_VM_ID) → run_sender ou run_receiver
    Note over Main,Comm: DESTRUIÇÃO, na ordem inversa: ~Communicator faz detach no Protocol,<br/>~Protocol faz detach na NIC, ~NIC chama engine_stop() e o ~Engine<br/>zera _instance e fecha o socket. Sem esses detach, notify() alcançaria<br/>objetos mortos
    Note over Main,Prot: «TODO» abortar com mensagem clara se nic.valid() for false<br/>(socket cru exige CAP_NET_RAW) · «TODO» fork() dos componentes
```

## 3.2 Sequência — Transmissão: `Communicator::send()` até `sendto(2)`

```mermaid
sequenceDiagram
    autonumber
    participant App as Aplicação
    participant Comm as Communicator
    participant Prot as Protocol
    participant NIC
    participant Pool as Buffer pool
    participant Eng as Raw_Socket_Engine
    participant K as Kernel

    App->>Comm: send(Message* msg)
    Comm->>Prot: send(_address, Address::broadcast(), msg->data(), msg->size())
    Prot->>NIC: alloc(to.paddr(), PROTO, sizeof(Header) + size)
    NIC->>Pool: varre a METADE DE TRANSMISSÃO — posições [0, 16) — procurando lock()

    alt Pool exaurido — nenhum lock() bem-sucedido
        Pool-->>NIC: nenhuma posição livre
        NIC-->>Prot: 0
        Prot-->>Comm: -1
        Comm-->>App: false
        Note over Prot,App: Retornar 0 é comportamento NORMAL, não erro fatal:<br/>quem chamou decide o que fazer
    else Buffer reservado
        Pool-->>NIC: buffer travado — posse transferida
        NIC->>NIC: preenche cabeçalho Ethernet:<br/>dst, src = engine_address(), prot = htons(0x88B5)
        NIC->>NIC: buf->size(14 + size)
        NIC-->>Prot: Buffer*

        Prot->>Prot: escreve o cabeçalho de Protocol no payload:<br/>_from_port, _to_port, _length = size
        Prot->>Prot: memcpy(pkt->data(), data, size)
        Prot->>NIC: send(Buffer* buf)
        NIC->>Eng: engine_send(buf->frame(), buf->size())
        Eng->>Eng: monta sockaddr_ll com sll_addr = frame->dst
        Eng->>K: sendto(_sockfd, frame, size, 0, sll, sizeof(sll))
        Note over Eng,K: ÚNICA syscall de rede de todo o caminho de envio
        K-->>Eng: n bytes entregues ao kernel
        Eng-->>NIC: n

        opt n > 0
            NIC->>NIC: _statistics.tx_packets++ · tx_bytes += n
        end

        NIC->>Pool: free(buf) → buf->unlock()
        Note over Prot,Eng: Contrato: NIC::send() SEMPRE libera o buffer,<br/>tenha êxito ou não. A alternativa — o chamador liberar —<br/>misturada a esta é vazamento garantido
        NIC-->>Prot: n
        Prot-->>Comm: n
        Comm-->>App: n > 0
        Note over App,Prot: true significa entregue ao kernel, NÃO alguém recebeu:<br/>broadcast não tem confirmação
    end
```

## 3.3 Sequência — Recepção assíncrona via `SIGIO` (fluxo mais crítico)

Este é o diagrama que resume a arquitetura. As linhas de vida estão ordenadas
**da base da pilha para o topo** — `Kernel → … → Aplicação` — de modo que todo o
caminho de recepção avança para a direita, sem cruzamentos. A **faixa
sombreada** não é um conjunto de objetos: é um **intervalo de tempo**. Tudo o
que acontece dentro dela executa no tratador de `SIGIO`, com a thread da
aplicação parada. O encontro entre os dois mundos é exatamente o par
`v()` / `p()`.

```mermaid
sequenceDiagram
    autonumber

    participant K as Kernel
    participant SH as signal_handler
    participant Eng as Raw_Socket_Engine
    participant NIC
    participant Prot as Protocol
    participant Comm as Communicator
    participant Sem as Semaphore
    participant App as Aplicação

    App->>Comm: receive(Message* msg)
    activate Comm
    Comm->>Sem: Observer::updated() → _semaphore.p()
    activate Sem
    Note over Sem,App: A thread ADORMECE aqui. É o único ponto<br/>bloqueante da biblioteca inteira

    par Thread da aplicação — bloqueada
        Note over Sem,App: aguardando em sem_wait(), sem consumir CPU
    and Contexto de interrupção — disparado pelo kernel
        rect rgba(128, 128, 128, 0.10)
        Note over K,App: FAIXA DE CONTEXTO DE TRATADOR — tudo daqui até o fim do bloco executa<br/>dentro do tratador de SIGIO e é async-signal-safe: sem printf, sem malloc,<br/>sem mutex, sem bloqueio. A thread interrompida está parada o tempo todo
        K-)SH: SIGIO — quadro disponível no socket
        activate SH
        SH->>SH: saved_errno = errno
        alt _instance == 0 ou _armed == 0
            SH->>SH: retorna sem fazer nada
        else Engine armado
            SH->>Eng: drain()
            activate Eng
            loop enquanto _armed — drena até EAGAIN
                Eng->>K: recvfrom(_sockfd, &frame, ...) «O_NONBLOCK»
                alt n == 0, ou n negativo com EAGAIN / EWOULDBLOCK
                    K-->>Eng: fila vazia
                    Eng->>Eng: retorna — drenagem concluída
                else n negativo e errno == EINTR
                    Eng->>Eng: continue — tenta de novo
                else n negativo — erro real
                    Eng->>Eng: _rx_error = errno · _rx_errors++
                    Note over Eng,NIC: drain() não pode imprimir:<br/>registra e o laço principal lê depois
                else n > 0
                    K-->>Eng: n bytes
                    alt sll_pkttype == PACKET_OUTGOING ou n menor que 14
                        Eng->>Eng: continue — descarta o eco local do próprio envio
                    else Quadro válido
                        Eng->>NIC: handle(&frame, n)
                        activate NIC
                        NIC->>NIC: revalida o tamanho — a NIC não confia no filtro do Engine
                        NIC->>NIC: varre a METADE DE RECEPÇÃO — posições [16, 32)
                        alt Pool exaurido
                            NIC->>NIC: _statistics.rx_dropped++ e RETORNA
                            Note over NIC,Prot: Descartar é resposta legítima —<br/>ESPERAR por buffer dentro de um tratador não é
                        else Buffer obtido
                            NIC->>NIC: memcpy do quadro — a memória do Engine morre no retorno
                            NIC->>NIC: buf->size(n) — total, cabeçalho incluído
                            NIC->>Prot: Observed::notify(ntohs(prot), buf) → update(prot, buf)
                            activate Prot
                            Prot->>Prot: lê _to_port do cabeçalho de Protocol
                            Prot->>Comm: _observed.notify(_to_port, buf) → update(port, buf)
                            alt Nenhum Communicator nesta porta, OU fila do Communicator cheia
                                Prot->>NIC: free(buf)
                                Note over NIC,Prot: O notify() interno devolveu false: ninguém ACEITOU.<br/>Desde 9426f3b fila cheia conta igual a 'ninguém quis' — update()<br/>devolve false e o buffer volta ao pool. Antes o buffer vazava E a<br/>mensagem se perdia. Este 'if' é a diferença entre rodar a noite<br/>toda e travar a metade RX depois de 16 quadros
                            else Communicator aceitou
                                Comm->>Comm: _data.insert(buf) — fila SPSC lock-free, devolve true
                                Comm->>Sem: _semaphore.v() — sem_post(3), só depois do insert
                                Note over Comm,App: ÚNICO ponto de cruzamento entre os dois contextos.<br/>Não bloqueia: deposita e segue
                            end
                            deactivate Prot
                            alt notify(EtherType) retornou true
                                NIC->>NIC: _statistics.rx_packets++ · rx_bytes += n
                            else Nenhum Protocol para este EtherType
                                NIC->>NIC: _statistics.rx_dropped++ · free(buf)
                            end
                        end
                        deactivate NIC
                    end
                end
            end
            deactivate Eng
        end
        SH->>SH: errno = saved_errno
        deactivate SH
        Note over K,SH: O tratador termina e a thread interrompida<br/>volta exatamente ao que fazia
        end
    end

    Sem-->>Comm: p() retorna — desperta
    deactivate Sem
    Comm->>Comm: buf = _data.remove()
    Comm->>Prot: _channel->receive(buf, &from, tmp, Message::MAX_SIZE)
    activate Prot
    Prot->>NIC: unmarshal(buf, &src_mac, 0, raw_payload, Ethernet::MTU)
    NIC-->>Prot: bytes de payload
    alt payload menor que sizeof(Header)
        Prot->>NIC: free(buf)
        Prot-->>Comm: -1
    else Pacote íntegro
        Prot->>Prot: from = Address(src_mac, hdr->_from_port)
        Prot->>Prot: copia hdr->_length bytes de dados
        Prot->>NIC: free(buf)
        Note over NIC,Prot: A POSSE DO BUFFER TERMINA AQUI —<br/>sempre, inclusive no caminho de erro
        Prot-->>Comm: bytes copiados
    end
    deactivate Prot
    Comm->>Comm: message->set(tmp, size)
    Comm-->>App: true — Message preenchida
    deactivate Comm
```

> **Uma leitura atenta deste diagrama revela um defeito real do código.** O ramo
> "Communicator registrado" depende de `_to_port` coincidir com a porta em que o
> `Communicator` fez `attach`. Hoje `Communicator::send()` endereça
> `Address::broadcast()`, cujo `Port` vale **0**, enquanto `main()` registra o
> `Communicator` na porta **1000** — logo `notify(0, buf)` não encontra
> observador e o quadro é liberado sem chegar à aplicação. O diagrama descreve o
> contrato pretendido; a divergência está registrada na
> [§4.4](#44-matriz-de-fidelidade-código-vs-diagrama).

### Por que semáforo e não *rendezvous*

O enunciado proíbe *rendezvous* na recepção — e há uma razão de projeto além da
regra. O semáforo **desacopla no tempo**: o produtor (o tratador de sinal) faz
`v()` e segue sem esperar ninguém; o consumidor faz `p()` e dorme até haver
dado. Se a mensagem chegar **antes** de a aplicação chamar `receive()`, o
contador do semáforo já vale 1 e `p()` retorna imediatamente — nada se perde. É
exatamente essa memória de um evento passado que falta a um *rendezvous*.

## 3.4 Sequência — Ciclo de vida e posse do `Buffer`

A pergunta clássica de banca: *quem é dono de um buffer recebido, e quando ele é
liberado?* Este diagrama consolida os **cinco pontos de liberação** e o único
ponto de falha de alocação.

```mermaid
sequenceDiagram
    autonumber
    participant Pool as NIC::_buffer[32] — 16 TX + 16 RX
    participant NIC
    participant Prot as Protocol
    participant Comm as Communicator

    rect rgba(128, 128, 128, 0.08)
        Note over Pool,Comm: CAMINHO DE ENVIO — posse curta, liberação na própria NIC
        Prot->>NIC: alloc(dst, prot, size)
        NIC->>Pool: lock() — test-and-set atômico
        alt Sem posição livre
            Pool-->>NIC: falha
            NIC-->>Prot: 0 «único ponto de falha de alocação»
            Note over NIC,Prot: Protocol::send() devolve -1 e<br/>Communicator::send() devolve false.<br/>Sem posse, sem free — nada a desfazer
        else Posição obtida
            Pool-->>NIC: posse transferida ao chamador
            NIC-->>Prot: Buffer*
            Prot->>NIC: send(buf)
            NIC->>Pool: free(buf) → unlock() «LIBERAÇÃO 1 — sempre, com êxito ou não»
        end
    end

    rect rgba(128, 128, 128, 0.08)
        Note over Pool,Comm: CAMINHO DE RECEPÇÃO — a posse viaja com o ponteiro
        NIC->>Pool: reserva buffer da metade RX, no tratador de sinal
        alt Sem posição livre
            Pool-->>NIC: falha
            NIC->>NIC: rx_dropped++ · descarta o quadro «sem alocação, sem posse»
        else Posição obtida
            Pool-->>NIC: posse
            NIC->>Prot: notify(EtherType, buf) — posse passa ao Protocol
            alt Nenhum Protocol para este EtherType
                NIC->>Pool: free(buf) «LIBERAÇÃO 2 — rx_dropped++»
                Note over Pool,NIC: notify() devolveu false: a posse nunca saiu da NIC
            else Protocol notificado
                Prot->>Prot: lê _to_port e notifica a porta
                alt Nenhum Communicator na porta, ou fila cheia
                    Prot->>NIC: free(buf) «LIBERAÇÃO 3 — ninguém aceitou»
                else Entregue
                    Prot->>Comm: notify(Port, buf) — posse passa ao Communicator
                    Comm->>Prot: receive(buf, ...) — devolve a posse ao Protocol
                    alt Pacote menor que o cabeçalho
                        Prot->>NIC: free(buf) «LIBERAÇÃO 4 — caminho de erro»
                    else Pacote íntegro
                        Prot->>NIC: free(buf) «LIBERAÇÃO 5 — caminho normal»
                    end
                end
            end
        end
    end

    Note over Pool,Comm: INVARIANTE: todo alloc() bem-sucedido tem exatamente um free().<br/>Um free() esquecido não falha o teste — só aparece depois de 16 mensagens<br/>(o tamanho da metade em uso), durante a demonstração.<br/>É o que o teste de exaustão do pool protege.
```

## 3.5 Diagrama de Atividade — `Raw_Socket_Engine::drain()`

O laço de drenagem é o que garante a entrega **independentemente do número de
sinais**: sinais padrão não se enfileiram — dois quadros em rajada geram um
único `SIGIO` — mas um `SIGIO` faz `drain()` esvaziar a fila inteira. É por isso
que a garantia mora no laço, e não na contagem de sinais.

```mermaid
flowchart TD
    START(["● SIGIO entregue ao processo"]) --> SAVE["signal_handler:<br/>saved_errno = errno"]
    SAVE --> CHK{"_instance != 0<br/>E _armed != 0 ?"}
    CHK -->|"não"| REST["errno = saved_errno"]
    CHK -->|"sim"| LOOP{"_armed ainda ativo ?"}

    LOOP -->|"não — engine_stop() concorrente"| REST
    LOOP -->|"sim"| RECV["recvfrom(_sockfd, &frame, sizeof(frame),<br/>0, &from, &len)<br/>«socket em O_NONBLOCK»"]

    RECV --> N0{"n == 0 ?"}
    N0 -->|"sim"| DONE["fila do kernel vazia:<br/>drenagem concluída"]
    N0 -->|"não"| NNEG{"n &lt; 0 ?"}

    NNEG -->|"sim"| ERRNO{"qual errno ?"}
    ERRNO -->|"EAGAIN / EWOULDBLOCK"| DONE
    ERRNO -->|"EINTR"| NEXT
    ERRNO -->|"erro real"| RECORD["_rx_error = errno<br/>_rx_errors++"]
    RECORD --> DONE
    DONE --> REST

    NNEG -->|"não — n &gt; 0"| FILTER{"sll_pkttype == PACKET_OUTGOING<br/>OU n &lt; 14 ?"}
    FILTER -->|"sim"| DISCARD["descarta o quadro:<br/>é o eco do próprio envio<br/>ou um quadro truncado"]
    FILTER -->|"não"| HANDLE["handle(&frame, n)<br/>«sobe toda a pilha — NIC → Protocol → Communicator»"]

    DISCARD --> NEXT(("junção"))
    HANDLE --> NEXT
    NEXT --> LOOP

    REST --> END(["◉ tratador retorna<br/>a thread interrompida volta ao que fazia"])

    NOTA1["não imprime — está dentro do tratador.<br/>O laço principal lê os contadores<br/>monotônicos depois"]
    NOTA2["ATENÇÃO: frame aponta para a pilha do Engine<br/>e só é válido durante a chamada.<br/>Quem quiser guardar, copia"]
    RECORD -.- NOTA1
    HANDLE -.- NOTA2

    classDef nota fill:#ffffff,stroke:#999999,stroke-dasharray:5 4,color:#333333
    class NOTA1,NOTA2 nota
```

> **Leitura da notação.** Os retângulos de canto arredondado são os nós inicial e
> final; os losangos, nós de decisão; o círculo **junção** é o nó de união que
> devolve o fluxo ao laço; as duas caixas tracejadas são **notas**, não ações —
> não executam nada. As três saídas que terminam em *drenagem concluída*
> correspondem, no código, aos três `return` de `drain()`.

---

# 4. Anexos

## 4.1 Diagrama de Estados — ciclo de vida do `Raw_Socket_Engine`

Notação das transições: `gatilho [guarda] / efeito`.

```mermaid
stateDiagram-v2
    direction TB
    [*] --> Construindo : Raw_Socket_Engine(iface, prot)

    Construindo --> Invalido : [falha em socket, if_nametoindex, ioctl,<br/>bind, sigaction ou fcntl] / _sockfd = -1
    Construindo --> Valido : [todas as etapas concluídas]<br/>/ _sockfd válido, _armed = 0

    note right of Construindo
        O construtor NÃO lança exceção e NÃO chama exit():
        quem chamou decide o que fazer com a falha.
        engine_valid() informa o resultado.
    end note

    Invalido --> Invalido : engine_start() / devolve false
    Invalido --> [*] : ~Raw_Socket_Engine()

    Valido --> Armado : engine_start() [fcntl O_ASYNC ok]<br/>/ _armed = 1, devolve true
    Valido --> Valido : engine_start() [fcntl O_ASYNC falha] / _armed = 0, devolve false<br/>engine_send() — o envio funciona sem armar a recepção

    Armado --> Drenando : SIGIO entregue / drain()
    Drenando --> Armado : recvfrom devolve EAGAIN, 0 ou erro real

    Armado --> Valido : engine_stop() «idempotente»<br/>/ remove O_ASYNC, _armed = 0

    note right of Valido
        Quem dispara engine_start() é o CONSTRUTOR DA NIC, no seu último passo:
        NIC() { if (engine_valid()) _armed = engine_start(); }
        Na prática, portanto, um Engine válido chega a Armado sozinho —
        e nic.valid() é o Valido AND Armado visto de fora.
    end note

    note left of Armado
        A partir daqui handle() pode ser chamada em QUALQUER instrução,
        na thread que estiver executando. Todo o estado que handle()
        toca precisa existir ANTES desta transição.
    end note

    note left of Drenando
        Estado transitório, dentro do tratador de sinal.
        A thread interrompida está parada enquanto isto dura.
    end note

    Valido --> [*] : ~Raw_Socket_Engine()<br/>/ engine_stop(), _instance = 0, close(_sockfd)
    Armado --> [*] : ~Raw_Socket_Engine()<br/>/ desarma antes de fechar
```

## 4.2 Matriz de rastreabilidade — classe → arquivo

| Classe / Artefato | Arquivo | Papel arquitetural |
|---|---|---|
| `Traits<T>` · `Traits<Ethernet>` | [../include/traits.h](../include/traits.h) | Ponto único de configuração |
| `Ethernet` + `Address` · `Header` · `Frame` · `Statistics` | [../include/ethernet.h](../include/ethernet.h) | Formatos de fio da camada de enlace |
| `Buffer<T>` | [../include/buffer.h](../include/buffer.h) | Unidade de posse — *zero-copy* pela pilha |
| `List<T,CAP>` · `Ordered_List<T,C,CAP>` · `Iterator` | [../include/list.h](../include/list.h) | Coleções *lock-free* de capacidade fixa |
| `Semaphore` | [../include/sem.h](../include/sem.h) | Desacoplamento temporal tratador ↔ thread |
| `Message` | [../include/message.h](../include/message.h) | Unidade de dados da aplicação |
| `Conditional_Data_Observer` · `Conditionally_Data_Observed` · `Concurrent_Observer` · `Concurrent_Observed` | [../include/observer.h](../include/observer.h) | Padrão Observer nas duas famílias |
| `Raw_Socket_Engine` | [../include/engine/raw_socket_engine.h](../include/engine/raw_socket_engine.h) · [../src/raw_socket_engine.cpp](../src/raw_socket_engine.cpp) | *Policy* — único ponto de syscall |
| `NIC<Engine>` | [../include/nic.h](../include/nic.h) | Enlace portável: pool, *marshalling*, notificação |
| `Protocol<NIC>` | [../include/protocol.h](../include/protocol.h) | Multiplexação por porta; a dobra do Observer |
| `Communicator<Channel>` | [../include/communicator.h](../include/communicator.h) | API unificada do agente |
| `Vehicle_NIC` · `Vehicle_Protocol` · `Vehicle_Communicator` | [../include/libvcomm.h](../include/libvcomm.h) | Instanciação da pilha concreta |
| Aplicação de teste | [../app/main.cpp](../app/main.cpp) | Papel por `SO2_VM_ID`; raiz do veículo |
| `Engine_Policy` (§2.5) | *sem arquivo* | Contrato estrutural do parâmetro `Engine`. Não existe como tipo em C++: é verificado pelo compilador na instanciação de `NIC<Engine>` |

## 4.3 Legenda de notação

### Relacionamentos usados

| Sintaxe Mermaid | UML | Semântica neste projeto |
|---|---|---|
| `A --\|> B` | Generalização (linha cheia, triângulo vazado) | Herança C++. O estereótipo indica `«public»` ou `«private»` |
| `A ..\|> B` | Realização (tracejada, triângulo vazado) | `A` cumpre um contrato de `B` (ex.: `Raw_Socket_Engine ..\|> Engine_Policy`) |
| `A ..> B : «bind»` | *Template binding* (tracejada, ponta aberta) | `A` é a instanciação do template `B` — `typedef`, ou especialização de `Traits`. **Não** é realização: a seta parte do tipo ligado e aponta para o template |
| `A *-- B` | Composição (losango cheio) | `B` é membro **por valor** de `A` e morre com ele (ex.: o pool de `Buffer` na `NIC`). Desenhada **apenas** onde existe um membro real |
| `A o-- B` | Agregação (losango vazado) | `A` guarda ponteiros para `B` mas **não** o possui (ex.: `Observed` e seus observadores) |
| `A --> B` | Associação direcional | `A` guarda um ponteiro não-proprietário para `B` (ex.: `Protocol::_nic`) |
| `A ..> B` | Dependência | `A` usa `B` sem armazená-lo (ex.: `NIC ..> Traits<Ethernet>`) |
| `namespace X { … }` | Pacote | Reúne as classes **aninhadas** de um hospedeiro (`Ethernet::Address`, `Protocol::Header`, …). Aninhamento em C++ é relação entre *tipos*, não entre objetos — por isso não há composição do hospedeiro para elas. Quando o aninhamento precisa ser explícito fora do pacote (o `Iterator` de `Ordered_List`, §2.1), usa-se dependência `«nested»` |
| `"1" … "0..16"` | Multiplicidade | Cardinalidade real, derivada das capacidades fixas do código (`Ordered_List` com `CAP = 16`, pool com 32 buffers) |

### Convenções de membros

| Marcação | Significado |
|---|---|
| `+` `-` `#` | Visibilidade pública, privada, protegida |
| membro **sublinhado** (`$` no fonte) | Membro **estático** |
| método em *itálico* (`*` no fonte) | **Virtual puro** (abstrato) |
| `«template T, C, CAP»` | Classe template, com todos os parâmetros nomeados no estereótipo; `Classe~T~` marca no nome apenas o parâmetro dominante |
| `uint` · `ushort` · `uchar` | `unsigned int` · `unsigned short` · `unsigned char` |
| `atomic_uint` · `atomic_bool` · `atomic_ptr` | `std::atomic<unsigned int>` · `std::atomic<bool>` · `std::atomic<T*>` — o `<>` colidiria com a notação de genéricos |
| `operator_eq` · `operator_neq` · `operator_deref` · `operator_arrow` · `operator_inc` | `operator==` · `operator!=` · `operator*` · `operator->` · `operator++` — renomeados para não colidir com os marcadores de sintaxe do Mermaid |
| destrutores omitidos | O caractere `~` é reservado à notação de genéricos do Mermaid e não pode abrir um nome de membro. Todas as classes com recurso a liberar (`Semaphore`, `Raw_Socket_Engine`, `NIC`, `Protocol`, `Communicator`) **têm** destrutor no código; seu efeito aparece nas notas de classe e nos diagramas de sequência (§3.1) e de estados (§4.1) |
| `«TODO»` | Declarado ou pretendido, mas **ainda não implementado** — ver §4.4 |

### Notação dos diagramas comportamentais

| Elemento | Onde | Significado |
|---|---|---|
| `alt` / `else` | §3.2 · §3.3 · §3.4 | Fragmento combinado alternativo; cada ramo traz a sua guarda entre colchetes |
| `loop` | §3.3 | Fragmento de repetição — o laço de drenagem |
| `par` / `and` | §3.3 | Fragmento paralelo: as duas raias progridem de forma independente |
| faixa sombreada (`rect`) | §3.3 · §3.4 | **Intervalo de tempo**, não conjunto de objetos: delimita o trecho que executa em contexto de tratador (§3.3) ou o caminho de posse em análise (§3.4) |
| seta aberta `-)` | §3.3 | Mensagem **assíncrona** — a entrega de `SIGIO` pelo kernel |
| seta tracejada | §3.1–§3.4 | Mensagem de **retorno** |
| `●` / `◉` | §3.5 | Nó inicial / nó final de atividade |
| losango | §3.5 | Nó de decisão |
| círculo *junção* | §3.5 | Nó de união — devolve o fluxo ao laço |
| caixa de borda tracejada | §3.5 | **Nota**: comentário, não ação executável |
| `gatilho [guarda] / efeito` | §4.1 | Rótulo de transição de máquina de estados, na forma canônica da UML |

## 4.4 Matriz de fidelidade — código vs. diagrama

Esta documentação é engenharia reversa, não especificação. A tabela abaixo é o
contrato de honestidade do documento: onde o diagrama descreve a **intenção** e
o repositório ainda não a cumpre, está registrado aqui.

> **Estado de referência:** `main` em `9426f3b` (*fix nic buffer and add timeout
> to updated()*). Esse commit **fechou** duas divergências que estavam nesta
> matriz — a política de fila cheia e o contrato de `unmarshal()` — e **abriu**
> duas novas, que hoje impedem a compilação. Os itens 1 e 2 abaixo são
> bloqueadores de build: nada nos diagramas de §3 executa enquanto existirem.

### Divergências abertas

| # | Elemento | Local | Estado real | Impacto arquitetural |
|---|---|---|---|---|
| 1 | **`Communicator::update()` não compila** | [communicator.h:109](../include/communicator.h#L109) × [observer.h:163](../include/observer.h#L163) | Continua devolvendo `void`; a base `Concurrent_Observer::update()` passou a devolver `bool` | **Bloqueia o build.** Retorno conflitante em função virtual — `make app` falha. §2.5 desenha a assinatura nova, que é a que a base exige |
| 2 | **`Semaphore::p(timeout_ms)` não existe** | [observer.h:188](../include/observer.h#L188) × [sem.h:42](../include/sem.h#L42) | `updated(timeout_ms)` foi escrito chamando um overload com prazo que nunca chegou ao `Semaphore` | **Bloqueia o build.** `make test-stack` falha. Precisa de um `bool p(unsigned ms)` sobre `sem_timedwait(3)`, com laço de `EINTR` — e `sem_timedwait` usa `CLOCK_REALTIME`, decisão a registrar |
| 3 | Porta do broadcast | [communicator.h:84](../include/communicator.h#L84) · [main.cpp:93](../app/main.cpp#L93) | `send()` endereça `Address::broadcast()`, cujo `Port` é **0**; o `Communicator` faz `attach` na porta **1000** | **Bloqueia a execução.** `notify(0, buf)` não encontra observador: o quadro é liberado e a aplicação nunca acorda. O caminho de §3.3 não fecha até emissor e receptor concordarem na porta |
| 4 | Comentário de `update()` descreve o contador errado | [observer.h:159](../include/observer.h#L159) | Afirma que, com a fila cheia, *"`NIC::handle()` counts it in `rx_dropped`"* | **Não é o que o código faz.** `Conditionally_Data_Observed::notify()` segue `void`-based e devolve `true` só por ter chamado `Protocol::update()`. Fila cheia ⇒ o `Protocol` libera o buffer e a `NIC` soma **`rx_packets`** — ver item 7 |
| 5 | `Protocol::Header` | [protocol.h:118](../include/protocol.h#L118) | Campos públicos, sem acessores | Quebra o encapsulamento do cabeçalho de fio; `Protocol::update()` e `receive()` leem os campos diretamente |
| 6 | `Protocol::send()` — retorno | [protocol.h:203](../include/protocol.h#L203) | Propaga o retorno de `NIC::send()`, que conta **cabeçalho + payload** | O contrato escrito no próprio cabeçalho fala em *bytes de payload*. Agora que `unmarshal()` teve o contrato fechado explicitamente, `send()` é o último dos dois em aberto |
| 7 | `NIC::_statistics.rx_packets` | [nic.h:224](../include/nic.h#L224) | Incrementado quando **algum `Protocol`** foi chamado, mesmo que nenhuma porta tenha aceito | Conta quadros que chegaram ao enlace, não mensagens entregues. Defensável para um contador de enlace — mas precisa ser dito assim na apresentação, e o item 4 mostra que o próprio código já se confundiu com isso |
| 8 | `run_sender()` · `run_receiver()` | [main.cpp:43](../app/main.cpp#L43) · [main.cpp:57](../app/main.cpp#L57) | Corpos vazios | Sem geração/verificação de tráfego; sem veredito `RESULT` para o script de teste. É o consumidor natural do novo `updated(timeout_ms)` |
| 9 | Verificação de `nic.valid()` | [main.cpp:84](../app/main.cpp#L84) | Ausente | Falha de `CAP_NET_RAW` passa silenciosa — o binário roda sem nunca receber nada |
| 10 | `fork()` dos componentes | [main.cpp:97](../app/main.cpp#L97) | Ausente | Um processo por veículo, não um por componente, como o enunciado pede |
| 11 | `scripts/run-fleet.sh` | [../scripts/run-fleet.sh](../scripts/run-fleet.sh) | Bloco `TODO`, `exit 1` | Frota de 5 VMs ainda não orquestrada |
| 12 | Alvos `image` · `fleet` · `capture` · `stats` | [../Makefile](../Makefile) | Esqueletos que imprimem `TODO` e falham | `make check` — o alvo de avaliação — ainda não fecha ponta a ponta |
| 13 | Referências penduradas em `design-decisions.md` | [nic.h:18](../include/nic.h#L18) · [nic.h:67](../include/nic.h#L67) · [observer.h:154](../include/observer.h#L154) | Os comentários citam `§2.6`, `§3` e a decisão `1.11` como se registrassem a partição do pool, o contrato de `unmarshal()` e o retorno `bool` — **`§2.6` e `1.11` não existem**, e `§3` é *"Honest limitations of the bench"* | A justificativa das três decisões vive só no comentário do código. `design-decisions.md` §4 ainda lista a política de fila cheia como aberta, embora já esteja fechada |

### Elementos do enunciado deliberadamente ausentes

Estes **não** são pendências: são decisões, e por isso não aparecem nos
diagramas de classes.

| Elemento | Local | Decisão |
|---|---|---|
| `NIC::receive(Address*, Protocol_Number*, void*, uint)` | [nic.h:86](../include/nic.h#L86) | Comentado. Método síncrono do PDF, redundante com o par `handle()`/`notify()` na arquitetura orientada a Observer. Sem caso de uso, fica fora da API em vez de existir devolvendo `-1` |
| `NIC::address(Address)` | [nic.h:158](../include/nic.h#L158) | Comentado. O MAC vem do kernel via `SIOCGIFHWADDR`; um *setter* só poderia mentir sobre o endereço real da interface |

Hoje a justificativa das duas vive apenas no comentário ao lado do código
(`// no use case for … yet`). A diferença entre "não implementei" e "decidi não
ter" é exatamente o que a banca pergunta: **vale promover as duas para
[design-decisions.md](design-decisions.md)** antes da entrega, ao lado das
demais decisões deliberadas.

---

# 5. Exportação para PNG

## 5.1 Via CLI — `scripts/export-uml.sh` (recomendado)

**Pré-requisitos:** `node` e `npm` (`sudo apt install nodejs npm`, ou via `nvm`).

```bash
./scripts/export-uml.sh                      # escala 4x, viewport 2400px
SCALE=6 WIDTH=3000 ./scripts/export-uml.sh   # para pôster ou slide
```

O script renderiza os 16 diagramas, **renomeia cada um pelo que ele é** e os
organiza em pastas por seção — veja o [mapa](#mapa-diagrama--arquivo) adiante.
No fim, grava também `doc/uml-png/DOCUMENTACAO_UML-imagens.md`: uma cópia deste
documento com os blocos Mermaid já substituídos pelas imagens, para colar em
editor que não renderiza Mermaid (Word, Google Docs, LaTeX via `pandoc`).

### Por que um script, e não uma linha de `npx mmdc`

O `mermaid-cli` só sabe numerar a saída na ordem em que os blocos aparecem:
`DOCUMENTACAO_UML-7.png`. Esse nome tem dois problemas — não diz o que é o
diagrama, e a numeração inteira se desloca quando você insere um diagrama no
meio do documento. O script traduz o número **uma vez**, no array `TARGETS` de
[export-uml.sh](../scripts/export-uml.sh), e o resultado sobrevive a qualquer
reexecução.

> **Ao acrescentar um diagrama**, insira a linha correspondente em `TARGETS`, na
> mesma posição em que o bloco aparece no documento. O script conta os blocos
> ```` ```mermaid ```` antes de renderizar e **aborta** se o mapa e o documento
> discordarem — em vez de exportar torto em silêncio.

### Os parâmetros por trás

O script chama o `mmdc` assim:

```bash
npx -y @mermaid-js/mermaid-cli \
    -i doc/DOCUMENTACAO_UML.md -o <staging>/DOCUMENTACAO_UML.md \
    -e png -s 4 -w 2400 -b white -t neutral -p doc/puppeteer.json
```

| Flag | Efeito | Variável de ambiente |
|---|---|---|
| `-e png` | Formato de saída (`svg` e `pdf` também são aceitos) | — |
| `-s 4` | **Fator de escala** — o que de fato produz alta resolução para impressão | `SCALE` |
| `-w 2400` | Largura do *viewport*; evita quebra dos diagramas largos (§1.1, §3.3) | `WIDTH` |
| `-b white` | Fundo branco em vez de transparente — melhor para o relatório | — |
| `-t neutral` | Tema legível em preto e branco; use `default` para colorido | `THEME` |
| `-p doc/puppeteer.json` | **Obrigatório neste bench** — veja a nota logo abaixo | `PPTR` |

### Mapa diagrama → arquivo

| Seção | Diagrama | Arquivo em `doc/uml-png/` |
|---|---|---|
| §1.1 | Componentes e camadas | [1-arquitetura/1.1-componentes-camadas.png](uml-png/1-arquitetura/1.1-componentes-camadas.png) |
| §1.2 | Pacotes e grafo de includes | [1-arquitetura/1.2-pacotes-dependencias.png](uml-png/1-arquitetura/1.2-pacotes-dependencias.png) |
| §1.3 | Implantação da frota | [1-arquitetura/1.3-implantacao-frota.png](uml-png/1-arquitetura/1.3-implantacao-frota.png) |
| §2.1 | Classes: núcleo de suporte | [2-classes/2.1-nucleo-suporte.png](uml-png/2-classes/2.1-nucleo-suporte.png) |
| §2.2 | Classes: Observer × Observed | [2-classes/2.2-observer-observed.png](uml-png/2-classes/2.2-observer-observed.png) |
| §2.3 | Classes: camada de enlace | [2-classes/2.3-camada-enlace.png](uml-png/2-classes/2.3-camada-enlace.png) |
| §2.4 | Classes: camada de rede e transporte | [2-classes/2.4-camada-rede-transporte.png](uml-png/2-classes/2.4-camada-rede-transporte.png) |
| §2.4 | Encapsulamento de cabeçalhos | [2-classes/2.4b-encapsulamento-cabecalhos.png](uml-png/2-classes/2.4b-encapsulamento-cabecalhos.png) |
| §2.5 | Classes: camada de aplicação | [2-classes/2.5-aplicacao.png](uml-png/2-classes/2.5-aplicacao.png) |
| §2.5 | Instanciação dos templates | [2-classes/2.5b-instanciacao-templates.png](uml-png/2-classes/2.5b-instanciacao-templates.png) |
| §3.1 | Sequência: bootstrap da pilha | [3-comportamento/3.1-sequencia-bootstrap.png](uml-png/3-comportamento/3.1-sequencia-bootstrap.png) |
| §3.2 | Sequência: transmissão | [3-comportamento/3.2-sequencia-transmissao.png](uml-png/3-comportamento/3.2-sequencia-transmissao.png) |
| §3.3 | Sequência: recepção via SIGIO | [3-comportamento/3.3-sequencia-recepcao-sigio.png](uml-png/3-comportamento/3.3-sequencia-recepcao-sigio.png) |
| §3.4 | Sequência: posse do `Buffer` | [3-comportamento/3.4-sequencia-posse-buffer.png](uml-png/3-comportamento/3.4-sequencia-posse-buffer.png) |
| §3.5 | Atividade: `drain()` | [3-comportamento/3.5-atividade-drain.png](uml-png/3-comportamento/3.5-atividade-drain.png) |
| §4.1 | Estados do `Raw_Socket_Engine` | [4-anexos/4.1-estados-engine.png](uml-png/4-anexos/4.1-estados-engine.png) |

### Por que o `-p doc/puppeteer.json` é obrigatório aqui

Sem ele, o `mmdc` falha antes de renderizar qualquer diagrama:

```
Error: Failed to launch the browser process
[FATAL:zygote_host_impl_linux.cc] No usable sandbox!
```

Ubuntu 23.10+ passou a **restringir user namespaces não privilegiados via
AppArmor**, e sem eles o Chromium que o Puppeteer embute não consegue montar o
próprio sandbox. O arquivo [puppeteer.json](puppeteer.json), versionado ao lado
deste documento, desliga esse sandbox:

```json
{ "args": ["--no-sandbox", "--disable-setuid-sandbox"] }
```

É seguro no contexto deste projeto: o Chromium é usado apenas para rasterizar
Markdown local, sem navegar em conteúdo remoto. A alternativa, se preferir não
desligar o sandbox, é liberar o namespace para o binário:

```bash
sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0   # até o próximo boot
```

### Exportar um diagrama isolado

Extraia o bloco desejado para um `.mmd` e gere só ele — útil para o slide da
apresentação:

```bash
npx -y @mermaid-js/mermaid-cli -i doc/recepcao.mmd -o doc/recepcao.png \
    -s 6 -w 3000 -b white -p doc/puppeteer.json
```

### Armadilha de sintaxe ao editar os diagramas

O ponto-e-vírgula **encerra uma instrução** no Mermaid. Em `flowchart` e
`classDiagram` isso não incomoda, porque os rótulos ficam entre aspas; mas em
`sequenceDiagram` o texto de `Note` e de mensagem **não é aspeado**, e um `;` no
meio da frase quebra o parser com um `Expecting 'NEWLINE' … got 'INVALID'`. Use
travessão ou vírgula nesses textos.

### Alvo de Makefile sugerido

Para integrar ao fluxo do projeto, acrescente ao [Makefile](../Makefile):

```make
uml:
	./scripts/export-uml.sh
```

### O que é fonte e o que é derivado

`doc/uml-png/` é **derivado** — o script o reconstrói inteiro a cada execução, e
apaga as quatro pastas de seção antes de gravar. A fonte de verdade é sempre
[DOCUMENTACAO_UML.md](DOCUMENTACAO_UML.md); não edite os PNGs. Pela mesma lógica
que o `.gitignore` aplica a `build/`, os PNGs poderiam ficar fora do
versionamento — a decisão depende de a entrega exigir ou não as imagens dentro
do repositório.

Já [puppeteer.json](puppeteer.json) e [export-uml.sh](../scripts/export-uml.sh)
**são fonte** e precisam ser versionados: sem eles, a exportação não roda na
máquina de mais ninguém do grupo.

## 5.2 Via VS Code — extensões

Você já está no VS Code; estas duas cobrem os dois usos:

| Extensão | ID | Para que serve |
|---|---|---|
| **Markdown Preview Mermaid Support** | `bierner.markdown-mermaid` | Renderiza os diagramas direto no *preview* nativo do Markdown (`Ctrl+Shift+V`). É a que você quer para **revisar** o documento |
| **Mermaid Chart** (oficial) | `MermaidChart.vscode-mermaid-chart` | Painel dedicado com botão **Export as PNG/SVG**, zoom e edição visual. É a que você quer para **exportar** um diagrama específico em alta resolução sem tocar no terminal |

Instalação pela linha de comando:

```bash
code --install-extension bierner.markdown-mermaid
code --install-extension MermaidChart.vscode-mermaid-chart
```

**Fluxo recomendado:** revise com `bierner.markdown-mermaid` no *preview*, e use
o `mmdc` (§5.1) para gerar o lote completo — é reprodutível, versionável e entra
no `make`.

> **Alternativa sem instalar nada:** o [mermaid.live](https://mermaid.live) cola
> o bloco e exporta PNG/SVG pelo navegador. Prático para um diagrama avulso,
> inadequado para o lote inteiro.

---

*Documento gerado por engenharia reversa do repositório `libvcomm`.
Grupo M10 — INE5424 2026/2 — UFSC.*
