#ifndef LIBVCOMM_MESSAGE_H
#define LIBVCOMM_MESSAGE_H

#include <cstring>

// =============================================================================
// Message — apoio.  JÁ IMPLEMENTADA (o suficiente para a Etapa 1).
//
// O enunciado é explícito: nesta etapa a mensagem é M = {.*}, um array de bytes
// e nada mais.  Não invente campo agora.
//
// Como ela cresce nas próximas etapas — deixe o espaço, não o código:
//   Etapa 2: M = {origin, payload}
//   Etapa 3: M = {origin, timestamp, payload}
//   Etapa 5: I = {origin, timestamp, type, period} / R = {..., value}
//   Etapa 6: + MAC
// =============================================================================

class Message
{
public:
    // Teto deliberadamente baixo: o enunciado garante mensagem < MTU, e um
    // buffer menor deixa o erro aparecer no teste em vez de no fio.
    static const unsigned int MAX_SIZE = 1024;

    Message() : _size(0) { std::memset(_data, 0, sizeof(_data)); }

    Message(const void * data, unsigned int size) : _size(0)
    {
        std::memset(_data, 0, sizeof(_data));
        if (data && size)
            set(data, size);
    }

    void * data() { return _data; }
    const void * data() const { return _data; }

    unsigned int size() const { return _size; }
    void size(unsigned int s) { _size = (s > MAX_SIZE) ? MAX_SIZE : s; }

    // Devolve quantos bytes realmente entraram (trunca em MAX_SIZE).
    unsigned int set(const void * data, unsigned int size)
    {
        _size = (size > MAX_SIZE) ? MAX_SIZE : size;
        std::memcpy(_data, data, _size);
        return _size;
    }

private:
    unsigned char _data[MAX_SIZE];
    unsigned int _size;
};

#endif // LIBVCOMM_MESSAGE_H
