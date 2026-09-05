#ifndef LIBVCOMM_ENGINE_H
#define LIBVCOMM_ENGINE_H

class Engine
{
protected:
    virtual ~Engine() = default;

    virtual int send(const void * data, unsigned int size) = 0;

    virtual int receive(void * data, unsigned size) = 0;
};

#endif // LIBVCOMM_ENGINE_H
