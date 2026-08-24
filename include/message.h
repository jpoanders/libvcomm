#ifndef LIBVCOMM_MESSAGE_H
#define LIBVCOMM_MESSAGE_H

#include <cstring>

// =============================================================================
// Message — support class.  ALREADY IMPLEMENTED (enough for Stage 1).
//
// The assignment is explicit: at this stage the message is M = {.*}, an array
// of bytes and nothing more.  Do not invent fields now.
//
// How it grows in the coming stages — leave the room, not the code:
//   Stage 2: M = {origin, payload}
//   Stage 3: M = {origin, timestamp, payload}
//   Stage 5: I = {origin, timestamp, type, period} / R = {..., value}
//   Stage 6: + MAC
// =============================================================================

class Message
{
public:
    // Deliberately low ceiling: the assignment guarantees messages < MTU, and a
    // smaller buffer makes the error show up in the test instead of on the
    // wire.
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
