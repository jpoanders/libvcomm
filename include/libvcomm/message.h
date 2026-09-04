#ifndef LIBVCOMM_MESSAGE_H
#define LIBVCOMM_MESSAGE_H

#include <cstring>


class Message
{
public:

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

    // Returns how many bytes actually made it in (truncates at MAX_SIZE).
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
