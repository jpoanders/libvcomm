# libvcomm

Biblioteca de comunicação para sistemas autônomos críticos.
INE5424 — Sistemas Operacionais II — UFSC — 2026/2 — **Grupo M10** — Etapa 1.

Cada veículo é uma VM QEMU; cada componente do veículo é um processo POSIX. As
VMs conversam por **frames Ethernet crus**, em broadcast, sem IP.

---

## O caminho de uma mensagem

Saber recitar isto é metade da apresentação:

```
  aplicação                     Communicator::send(Message*)
      |                                   |
      v                         Protocol::send()  -> monta o Header do protocolo
  Communicator                            |
      |                         NIC::alloc() -> pega buffer do pool, monta o
      v                                        cabeçalho Ethernet (dst=broadcast)
   Protocol                                |
      |                         NIC::send(buf)
      v                                   |
     NIC                       Engine::engine_send() -> sendto()  <<< ÚNICA syscall
      |                                   |
      v                          [ barramento multicast do QEMU ]
   Engine                                 |
      |                      [SIGIO] Engine::drain() -> recvfrom()
      v                                   |
  raw socket                     NIC::handle()  -> copia para um buffer
                                          |
                              Observed::notify(EtherType, buf)
                                          |
                                Protocol::update()
                                          |
                              Observed::notify(Port, buf)
                                          |
                              Communicator::update() -> semáforo.v()
                                          |
                    ~~~ aqui o SIGNAL HANDLER termina e a thread interrompida ~~~
                    ~~~ volta a fazer o que fazia                            ~~~
                                          |
                    thread da aplicação acorda em Communicator::receive()
```

Nenhuma camada acima da `Engine` conhece socket. É essa promessa que faz a
Etapa 2 (memória compartilhada) trocar só a `Engine`.

**Tudo entre `drain()` e `semáforo.v()` roda dentro de um signal handler** — o
enunciado exige propagação por sinais POSIX, e no EPOS esse mesmo trecho roda no
handler de interrupção da NIC. Consequência prática que morde na primeira hora:
nada de `printf`, `new` ou `std::mutex` nesse caminho. Ver `doc/decisoes.md` §2.1
e `man 7 signal-safety`.

---

## Mapa dos arquivos

| Arquivo | Estado | O que é |
|---|---|---|
| `include/traits.h` | pronto | EtherType, interface, tamanho do pool — configuração |
| `include/ethernet.h` | pronto | `Address`, `Header`, `Frame`, MTU, `Statistics` |
| `include/list.h` | pronto | `List` (FIFO) e `Ordered_List` (observadores) |
| `include/sem.h` | pronto | `Semaphore` sobre `sem_t` |
| `include/buffer.h` | pronto | `Buffer<T>` e a regra de posse |
| `include/message.h` | pronto | `Message` da Etapa 1 (array de bytes) |
| `include/observer.h` | **misto** | `Concurrent_*` transcritos do PDF; `Conditionally_Data_Observed` é seu |
| `include/engine/raw_socket_engine.h` + `src/raw_socket_engine.cpp` | **seu** | as syscalls. O coração da Etapa 1 |
| `include/nic.h` | **seu** | pool de buffers, marshalling, notificação |
| `include/protocol.h` | **seu** | portas, cabeçalho do protocolo, a dobra do Observer |
| `include/communicator.h` | **seu** | a API que a aplicação enxerga |
| `app/main.cpp` | **seu** | papel por `SO2_VM_ID`, fork dos componentes |
| `scripts/*.sh` | **seu** | frota, captura, estatística |
| `doc/decisoes.md` | pronto | desvios em relação ao PDF, com justificativa — material de banca |

`src/channel.cpp` é seu rascunho de raw socket. Fica fora do build de propósito.

---

## Ordem de implementação

Está em **[`ROADMAP.md`](ROADMAP.md)** — 8 fases, cada uma com o que implementar, como
verificar que acabou, quanto tempo estimar e qual armadilha ela esconde. Lá também estão
o plano de fim de semana e o corte de emergência, caso a apresentação seja antecipada.

Resumo de uma linha: **comece pelo `Conditionally_Data_Observed::notify`** em
`include/observer.h`. É a fase mais barata e destrava `NIC` e `Protocol` de uma vez.

---

## Comandos

```bash
make app            # binário estático x86-64 para dentro da VM
make test-support   # classes de apoio — tem que passar hoje
make test-stack     # a pilha — falha até você implementar
make help           # o resto
```

Variáveis que valem conhecer:

```bash
STARTER=~/work/so2/pratical-class-1/INE5424-x86_64-starter-6.15.5   # material do professor (READ-ONLY)
SO2_MCAST=239.10.10.10:15424                                        # o barramento do grupo
VM_TIMEOUT=20                                                       # teto por VM no teste
```

---

## Antes de apresentar

O checklist de aceitação está na seção 9 de `practical_class_1_guide.md`. Os
itens que mais dão trabalho e menos aparecem no código:

- `make` na raiz tem que **falhar** quando um receptor perde frame, quando uma
  VM estoura o timeout ou quando a captura sai vazia. Teste que só imprime aviso
  não é avaliável.
- a latência precisa sair **automaticamente** ao final do `make`, com o rótulo
  certo (round-trip ou via única — não troque um pelo outro).
- diagramas e slides em `doc/`.
- o commit avaliado tem que estar na `main`.

Ressalvas honestas da bancada (sem `/dev/kvm`, `virtio-net` sem padding, captura
não prova recepção) estão em `doc/decisoes.md` §3. Dizer isso na apresentação é
mais forte do que esconder.
