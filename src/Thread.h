#ifdef _MSC_VER
#   pragma once
#endif

#ifndef _THREAD_H_INC_
#define _THREAD_H_INC_

#include <bitset>
#include <vector>

#include "Position.h"
#include "Pawns.h"
#include "Material.h"
#include "MovePicker.h"
#include "Searcher.h"

// Windows or MinGW
#if defined(_WIN32) || defined(_MSC_VER) || defined(__CYGWIN__) || defined(__MINGW32__) || defined(__MINGW64__) || defined(__BORLANDC__)

#   ifndef  NOMINMAX
#       define NOMINMAX // disable macros min() and max()
#   endif
#   ifndef  WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif

#   include <windows.h>

#   undef WIN32_LEAN_AND_MEAN
#   undef NOMINMAX


// We use critical sections on Windows to support Windows XP and older versions,
// unfortunatly cond_wait() is racy between lock_release() and WaitForSingleObject()
// but apart from this they have the same speed performance of SRW locks.
typedef CRITICAL_SECTION   Lock;
typedef HANDLE              WaitCondition;
typedef HANDLE              NativeHandle;

// On Windows 95 and 98 parameter lpThreadId my not be null
inline DWORD* dwWin9xKludge () { static DWORD dw; return &dw; }

#   define lock_create(x)        InitializeCriticalSection (&(x))
#   define lock_grab(x)          EnterCriticalSection (&(x))
#   define lock_release(x)       LeaveCriticalSection (&(x))
#   define lock_destroy(x)       DeleteCriticalSection (&(x))
#   define cond_create(x)        x = CreateEvent (0, FALSE, FALSE, 0);
#   define cond_destroy(x)       CloseHandle (x)
#   define cond_signal(x)        SetEvent (x)
#   define cond_wait(x,y)        { lock_release (y); WaitForSingleObject (x, INFINITE); lock_grab (y); }
#   define cond_timedwait(x,y,z) { lock_release (y); WaitForSingleObject (x, z); lock_grab (y); }
#   define thread_create(x,f,t)  x = CreateThread (NULL, 0, LPTHREAD_START_ROUTINE (f), t, 0, dwWin9xKludge ())
#   define thread_join(x)        { WaitForSingleObject (x, INFINITE); CloseHandle (x); }

#else    // Linux - Unix

#   include <pthread.h>
#   include <unistd.h>  // for sysconf()

typedef pthread_mutex_t     Lock;
typedef pthread_cond_t      WaitCondition;
typedef pthread_t           NativeHandle;
typedef void* (*FnStart) (void*);

#   define lock_create(x)   pthread_mutex_init (&(x), NULL)
#   define lock_grab(x)     pthread_mutex_lock (&(x))
#   define lock_release(x)  pthread_mutex_unlock (&(x))
#   define lock_destroy(x)  pthread_mutex_destroy (&(x))
#   define cond_create(x)   pthread_cond_init (&(x), NULL)
#   define cond_destroy(x)  pthread_cond_destroy (&(x))
#   define cond_signal(x)   pthread_cond_signal (&(x))
#   define cond_wait(x,y)   pthread_cond_wait (&(x), &(y))
#   define cond_timedwait(x,y,z)    pthread_cond_timedwait (&(x), &(y), z)
#   define thread_create(x,f,t)     pthread_create (&(x), NULL, FnStart (f), t)
#   define thread_join(x)   pthread_join (x, NULL)

#endif

namespace Threads {

    using namespace Searcher;

    const u08   MAX_THREADS            = 128; // Maximum threads
    const u08   MAX_SPLITPOINT_THREADS =   8; // Maximum threads per splitpoint
    const u08   MAX_SPLIT_DEPTH        =  15; // Maximum split depth

    extern void timed_wait (WaitCondition &sleep_cond, Lock &sleep_lock, i32 msec);

    typedef struct Mutex
    {
    private:
        Lock _lock;
        
        friend struct Condition;

    public:
        Mutex () { lock_create (_lock); }
       ~Mutex () { lock_destroy (_lock); }

        void   lock () { lock_grab (_lock); }
        void unlock () { lock_release (_lock); }
    } Mutex;

    typedef struct Condition
    {
    private:
        WaitCondition condition;

    public:
        Condition () { cond_create  (condition); }
       ~Condition () { cond_destroy (condition); }

        void wait (Mutex &m) { cond_wait (condition, m._lock); }

        void wait_for (Mutex &m, i32 ms) { timed_wait (condition, m._lock, ms); }

        void notify_one () { cond_signal (condition); }
    } Condition;

    struct Thread;

    typedef struct SplitPoint
    {

    public:
        // Const data after splitpoint has been setup
        const Stack    *ss;
        const Position *pos;

        Thread *master;
        Value   beta;
        Depth   depth;
        NodeT   node_type;
        bool    cut_node;
        Mutex   mutex;

        // Const pointers to shared data
        MovePicker  *movepicker;
        SplitPoint  *parent_splitpoint;

        // Shared data
        std::bitset<MAX_THREADS> slaves_mask;
        volatile u08   moves_count;
        volatile Value alpha;
        volatile Value best_value;
        volatile Move  best_move;
        volatile u64   nodes;
        volatile bool  cut_off;
    } SplitPoint;

    // ThreadBase struct is the base of the hierarchy from where
    // we derive all the specialized thread classes.
    typedef struct ThreadBase
    {

    public:
        Mutex         mutex;
        NativeHandle  handle;
        Condition     sleep_condition;
        volatile bool exit;

        ThreadBase ()
            : exit (false)
        {}

        virtual ~ThreadBase () {}

        virtual void idle_loop () = 0;

        void notify_one ();

        void wait_for (const volatile bool &condition);
    } ThreadBase;

    // TimerThread is derived from ThreadBase
    // used for special purpose: the recurring timer.
    typedef struct TimerThread
        : public ThreadBase
    {

    public:
        // This is the minimum interval in msec between two check_time() calls
        static const i32 Resolution = 5;

        bool run;

        TimerThread ()
            : run (false)
        {}

        virtual void idle_loop ();

    } TimerThread;

    // Thread is derived from ThreadBase
    // Thread struct keeps together all the thread related stuff like locks, state
    // and especially splitpoints. We also use per-thread pawn-hash and material-hash tables
    // so that once get a pointer to a thread entry its life time is unlimited
    // and we don't have to care about someone changing the entry under our feet.
    typedef struct Thread
        : public ThreadBase
    {

    public:
        SplitPoint splitpoints[MAX_SPLITPOINT_THREADS];
        
        Material::Table   material_table;
        Pawns   ::Table   pawns_table;
        EndGame::Endgames endgames;

        Position *active_pos;
        u08   idx
            , max_ply;

        SplitPoint* volatile active_splitpoint;
        volatile u08  splitpoint_threads;
        volatile bool searching;

        Thread ();

        virtual void idle_loop ();

        bool cutoff_occurred () const;

        bool available_to (const Thread *master) const;

        template <bool FAKE>
        void split (Position &pos, const Stack *ss, Value alpha, Value beta, Value &best_value, Move &best_move,
            Depth depth, u08 moves_count, MovePicker &movepicker, NodeT node_type, bool cut_node);

    } Thread;

    // MainThread is derived from Thread
    // used for special purpose: the main thread.
    typedef struct MainThread
        : public Thread
    {
        volatile bool thinking;

        MainThread ()
            : thinking (true)
        {} // Avoid a race with start_thinking ()

        virtual void idle_loop ();

    } MainThread;

    // ThreadPool struct handles all the threads related stuff like initializing,
    // starting, parking and, the most important, launching a slave thread
    // at a splitpoint.
    // All the access to shared thread data is done through this class.
    typedef struct ThreadPool
        : public std::vector<Thread*>
    {
        bool    idle_sleep;
        Depth   split_depth;
        Mutex   mutex;

        Condition   sleep_condition;
        
        TimerThread *timer;

        MainThread* main () { return static_cast<MainThread*> ((*this)[0]); }

        // No c'tor and d'tor, threads rely on globals that should
        // be initialized and valid during the whole thread lifetime.
        void   initialize ();
        void deinitialize ();

        void configure ();

        Thread* available_slave (const Thread *master) const;

        void start_thinking (const Position &pos, const LimitsT &limit, StateInfoStackPtr &states);

        void wait_for_think_finished ();

    } ThreadPool;

    // timed_wait() waits for msec milliseconds. It is mainly an helper to wrap
    // conversion from milliseconds to struct timespec, as used by pthreads.
    inline void timed_wait (WaitCondition &sleep_cond, Lock &sleep_lock, i32 msec)
    {

#if defined(_WIN32) || defined(_MSC_VER) || defined(__CYGWIN__) || defined(__MINGW32__) || defined(__MINGW64__) || defined(__BORLANDC__)

        i32 tm = msec;

#else    // Linux - Unix

        timespec ts
            ,   *tm = &ts;
        u64 ms = Time::now() + msec;

        ts.tv_sec = ms / Time::M_SEC;
        ts.tv_nsec = (ms % Time::M_SEC) * 1000000LL;

#endif

        cond_timedwait (sleep_cond, sleep_lock, tm);

    }

}

//#if __cplusplus > 199711L
//#   include <thread>
//#endif

inline u32 cpu_count ()
{

//#if __cplusplus > 199711L
//    // May return 0 when not able to detect
//    return std::thread::hardware_concurrency ();
//
//#else    

#   ifdef WIN32

    SYSTEM_INFO sys_info;
    GetSystemInfo (&sys_info);
    return sys_info.dwNumberOfProcessors;

#   elif MACOS

    u32 count;
    u32 len = sizeof (count);

    i32 nm[2];
    nm[0] = CTL_HW;
    nm[1] = HW_AVAILCPU;
    sysctl (nm, 2, &count, &len, NULL, 0);
    if (count < 1)
    {
        nm[1] = HW_NCPU;
        sysctl (nm, 2, &count, &len, NULL, 0);
        if (count < 1) count = 1;
    }
    return count;

#   elif _SC_NPROCESSORS_ONLN // LINUX, SOLARIS, & AIX and Mac OS X (for all OS releases >= 10.4)

    return sysconf (_SC_NPROCESSORS_ONLN);

#   elif __IRIX

    return sysconf (_SC_NPROC_ONLN);

#   elif __HPUX

    pst_dynamic psd;
    return (pstat_getdynamic (&psd, sizeof (psd), 1, 0) == -1)
        ? 1 : psd.psd_proc_cnt;

    //return mpctl (MPC_GETNUMSPUS, NULL, NULL);

#   else

    return 1;

#   endif

//#endif

}

typedef enum SyncT { IO_LOCK, IO_UNLOCK } SyncT;

#define sync_cout std::cout << IO_LOCK
#define sync_endl std::endl << IO_UNLOCK

// Used to serialize access to std::cout to avoid multiple threads writing at the same time.
inline std::ostream& operator<< (std::ostream &os, const SyncT &sync)
{
    static Threads::Mutex m;

    if      (sync == IO_LOCK)
    {
        m.lock ();
    }
    else if (sync == IO_UNLOCK)
    {
        m.unlock ();
    }
    return os;
}


extern Threads::ThreadPool  Threadpool;


#endif // _THREAD_H_INC_
