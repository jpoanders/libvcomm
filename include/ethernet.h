#ifndef LIBVCOMM_ETHERNET_H
#define LIBVCOMM_ETHERNET_H

#include <cstring>
#include "traits.h"

// =============================================================================
// class Ethernet — "all necessary definitions and formats", nas palavras do
// PDF.
//
// Esta classe NÃO fala com o kernel.  Ela só descreve o formato do que viaja no
// fio.  Quem executa syscall é a Engine.  Manter essa separação é metade da
// resposta para "por que a Engine isola o raw socket?".
// =============================================================================

class Ethernet
{
public:
    // Maior payload que cabe num frame sem fragmentação.  O enunciado garante
    // que toda mensagem do projeto é menor que isso => nunca fragmentamos.
    static const unsigned int MTU = 1500;
    static const unsigned int HEADER_SIZE = 14;

    // EtherType.  ATENÇÃO: no fio ele vai em network byte order (big-endian).
    // Dentro da biblioteca guardamos em host order e convertemos com htons()
    // apenas na fronteira da Engine.  Escolher uma convenção e não misturar as
    // duas é o que evita o bug clássico de "o receptor nunca vê nada".
    typedef unsigned short Protocol;

    // -------------------------------------------------------------------------
    // Address — um MAC address de 6 bytes.  NIC::Address *é* isto.
    // -------------------------------------------------------------------------
    class Address
    {
    public:
        enum Null
        {
            NULL_ADDRESS = 0
        };

        Address() { std::memset(_addr, 0, sizeof(_addr)); }
        Address(const Null &) { std::memset(_addr, 0, sizeof(_addr)); }

        Address(unsigned char a0, unsigned char a1, unsigned char a2,
                unsigned char a3, unsigned char a4, unsigned char a5)
        {
            _addr[0] = a0;
            _addr[1] = a1;
            _addr[2] = a2;
            _addr[3] = a3;
            _addr[4] = a4;
            _addr[5] = a5;
        }

        explicit Address(const unsigned char * raw)
        {
            std::memcpy(_addr, raw, sizeof(_addr));
        }

        unsigned char * bytes() { return _addr; }
        const unsigned char * bytes() const { return _addr; }

        bool operator==(const Address & a) const
        {
            return !std::memcmp(_addr, a._addr, sizeof(_addr));
        }
        bool operator!=(const Address & a) const { return !(*this == a); }

        // Verdadeiro se o endereço não for todo zero.  Usado por
        // Protocol::Address.
        operator bool() const
        {
            for (unsigned int i = 0; i < sizeof(_addr); i++)
                if (_addr[i])
                    return true;
            return false;
        }

        // Escreve "aa:bb:cc:dd:ee:ff" em buf (>= 18 bytes).  Devolve buf.
        char * to_string(char * buf) const;

    private:
        unsigned char _addr[6];
    } __attribute__((packed));

    // Destino OBRIGATÓRIO de todo frame do projeto: o meio modela uma célula de
    // rádio, não um cabo ponto a ponto.  (full_assignment.pdf,
    // "Identificadores")
    static const Address BROADCAST;

    // -------------------------------------------------------------------------
    // Header / Frame — o layout literal do fio.
    //
    //   +--------------+--------------+-----------+------------------+
    //   | destination  | source       | EtherType | payload          |
    //   |   6 bytes    |   6 bytes    |  2 bytes  |  <= MTU          |
    //   +--------------+--------------+-----------+------------------+
    // -------------------------------------------------------------------------
    struct Header
    {
        Address dst;
        Address src;
        Protocol prot; // network order quando no fio
    } __attribute__((packed));

    struct Frame : public Header
    {
        unsigned char data[MTU];
    } __attribute__((packed));

    // -------------------------------------------------------------------------
    // Statistics — o PDF pede NIC::statistics().  Contadores vivem aqui porque
    // são conceito de enlace, não de uma Engine específica.
    // -------------------------------------------------------------------------
    struct Statistics
    {
        Statistics()
            : tx_packets(0), tx_bytes(0), rx_packets(0), rx_bytes(0),
              rx_dropped(0)
        {}

        unsigned int tx_packets;
        unsigned int tx_bytes;
        unsigned int rx_packets;
        unsigned int rx_bytes;
        unsigned int
            rx_dropped; // frame chegou mas não havia buffer / observador
    };
};

// O header de enlace tem 14 bytes.  Se esta linha falhar, algum campo ganhou
// padding e o frame no fio está errado — o compilador avisa antes da VM.
static_assert(sizeof(Ethernet::Header) == 14,
              "Ethernet::Header precisa ter 14 bytes");
static_assert(sizeof(Ethernet::Address) == 6,
              "Ethernet::Address precisa ter 6 bytes");

inline const Ethernet::Address Ethernet::BROADCAST(0xff, 0xff, 0xff, 0xff, 0xff,
                                                   0xff);

inline char * Ethernet::Address::to_string(char * buf) const
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < 6; i++) {
        buf[i * 3 + 0] = hex[_addr[i] >> 4];
        buf[i * 3 + 1] = hex[_addr[i] & 0x0f];
        buf[i * 3 + 2] = ':';
    }
    buf[17] = '\0';
    return buf;
}

#endif // LIBVCOMM_ETHERNET_H
