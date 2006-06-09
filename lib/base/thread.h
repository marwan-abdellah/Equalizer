
/* Copyright (c) 2005, Stefan Eilemann <eile@equalizergraphics.com> 
   All rights reserved. */

#ifndef EQBASE_THREAD_H
#define EQBASE_THREAD_H

#include "base.h"

namespace eqBase
{
    class Lock;

    /**
     * An abstraction to create a new execution thread.
     * 
     * Depending on the type, a different implementation is used to create the
     * thread. Please note that certain implementations, e.g., fork, create the
     * working unit in another address space. 
     */
    class Thread {

    public:
        /** The thread types. */
        enum Type
        {
            PTHREAD,
            FORK
        };

        /** 
         * Constructs a new thread.
         * 
         * @param type the execution model to use for the thread.
         */
        Thread( const Type type = PTHREAD );

        /** Destructs the thread. */
        virtual ~Thread();

        /** 
         * Starts the thread using the specified entry function.
         * 
         * @return <code>true</code> if the thread was launched,
         *         <code>false</code> otherwise.
         */
        bool start();

        /** 
         * The init function for the child thread.
         *
         * The parent thread will not be unlocked before this function has been
         * executed. If the thread initialisation fails, that is, this method
         * did return false, the thread will be stopped and the start() method
         * will return false.
         * 
         * @return the success value of the thread initialisation.
         */
        virtual bool init(){ return true; }

        /** 
         * The entry function for the child thread.
         * 
         * @return the return value of the child thread.
         */
        virtual ssize_t run() = 0;

        /** 
         * Exits the child thread immediately.
         * 
         * This function does not return. It is only to be called from the child
         * thread.
         *
         * @param retVal the return value of the thread.
         */
        virtual void exit( ssize_t retVal = NULL );

        /** 
         * Cancels (stops) the child thread.
         *
         * This function is not to be called from the child thread.
         */
        void cancel();

        /** 
         * Waits for the exit of the child thread.
         * 
         * The actual size of the return value is thread-type dependent and may
         * be as low as 8 bits.
         *
         * @param retVal output value for the return value of the child, can be
         *               <code>NULL</code>.
         * @return <code>true</code> if the thread was joined,
         *         <code>false</code> otherwise.
         */
        bool join( ssize_t* retVal=NULL );

        /** 
         * Returns the execution type for this thread.
         * 
         * @return the execution type for this thread.
         */
        Type getType() { return _type; }

        /** 
         * Returns if the thread is stopped.
         * 
         * Note that the thread may be neither running nor stopped if it is
         * currently starting or stopping.
         *
         * @return <code>true</code> if the thread is stopped,
         * <code>false</code> if not.
         */
        bool isStopped() const { return ( _threadState == STATE_STOPPED ); }

        /** 
         * Returns if the thread is running.
         * 
         * Note that the thread may be neither running nor stopped if it is
         * currently starting or stopping.
         *
         * @return <code>true</code> if the thread is running,
         * <code>false</code> if not.
         */
        bool isRunning() const { return ( _threadState == STATE_RUNNING ); }

        /** 
         * Returns if this thread is the current (calling) thread.
         * 
         * @return <code>true</code> if the current thread has is the same
         *         thread as this thread, <code>false</code> if not.
         */
        bool isCurrent() const;

    private:
        /** The current state of this thread. */
        enum State
        {
            STATE_STOPPED,
            STATE_STARTING, // start() in progress
            STATE_RUNNING,
            STATE_STOPPING  // child no longer active, join() not yet called
        };

        Type  _type;
        State _threadState;
        Lock* _lock;

        union ThreadID
        {
            pthread_t pthread;
            pid_t     fork;
        };

        ThreadID             _threadID;

        static void* runChild( void* arg );
        void        _runChild();

        ThreadID    _getLocalThreadID();
    };
}

#endif //EQBASE_THREAD_H
