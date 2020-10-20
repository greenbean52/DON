#pragma once

#include <thread>

/// On OSX threads other than the main thread are created with a reduced stack
/// size of 512KB by default, this is too low for deep searches, which require
/// somewhat more than 1MB stack, so adjust it to TH_STACK_SIZE.
/// The implementation calls pthread_create() with the stack size parameter
/// equal to the linux 8MB default, on platforms that support it.
#if defined(__APPLE__) || defined(__MINGW32__) || defined(__MINGW64__) || defined(USE_PTHREADS)

#include <pthread.h>

template<class T, class P = std::pair<T*, void(T::*)()>>
void* threadRoutine(void *arg) {
    P *p{ reinterpret_cast<P*>(arg) };
    (p->first->*(p->second))(); // Call member function pointer
    delete p;
    return nullptr;
}

class NativeThread {

public:

    template<class T, class P = std::pair<T*, void (T::*)()>>
    explicit NativeThread(void(T::*fun)(), T *obj) {
        static constexpr size_t TH_STACK_SIZE{ 8 * 1024 * 1024 };

        pthread_attr_t threadAttr;
        pthread_attr_init(&threadAttr);
        pthread_attr_setstacksize(&threadAttr, TH_STACK_SIZE);
        pthread_create(&thread, &threadAttr, threadRoutine<T>, new P(obj, fun));
        pthread_attr_destroy(&threadAttr);
    }

    void join() {
        pthread_join(thread, nullptr);
    }

private:

    pthread_t thread;
};

#else // Default case: use STL classes

using NativeThread = std::thread;

#endif
