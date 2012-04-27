
/* Copyright (c) 2005-2012, Stefan Eilemann <eile@equalizergraphics.com>
 *                    2010, Cedric Stalder<cedric.stalder@gmail.com>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *  
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "pipe.h"

#include "channel.h"
#include "client.h"
#include "config.h"
#include "exception.h"
#include "frame.h"
#include "frameData.h"
#include "global.h"
#include "log.h"
#include "node.h"
#include "nodeFactory.h"
#include "nodePackets.h"
#include "pipePackets.h"
#include "pipeStatistics.h"
#include "server.h"
#include "view.h"
#include "window.h"
#include "windowPackets.h"

#include "messagePump.h"
#include "systemPipe.h"

#include "computeContext.h"
#ifdef EQ_USE_CUDA
#  include "cudaContext.h"
#endif

#include <eq/fabric/elementVisitor.h>
#include <eq/fabric/packets.h>
#include <eq/fabric/task.h>
#include <co/command.h>
#include <co/queueSlave.h>
#include <co/worker.h>
#include <sstream>

#ifndef EQ_USE_HWLOC
#  include <hwloc.h>
#  include <hwloc/gl.h>
#endif

namespace eq
{
/** @cond IGNORE */
typedef fabric::Pipe< Node, Pipe, Window, PipeVisitor > Super;
typedef co::CommandFunc<Pipe> PipeFunc;
/** @endcond */

class Pipe::Thread : public eq::Worker
{
public:
    Thread( Pipe* pipe ) : _pipe( pipe ) {}

protected:
    virtual void run();
    virtual bool stopRunning() { return !_pipe; }

private:
    Pipe* _pipe;
    friend class Pipe;
};

namespace detail
{

/** Asynchronous, per-pipe readback thread. */
class TransferThread : public co::Worker
{
public:
    TransferThread() : co::Worker(), _running( true ){}

    virtual bool stopRunning() { return !_running; }
    void postStop() { _running = false; }

private:
    bool _running; // thread will exit if this is false
};
}

Pipe::Pipe( Node* parent )
        : Super( parent )
        , _systemPipe( 0 )
        , _state( STATE_STOPPED )
        , _currentFrame( 0 )
        , _frameTime( 0 )
        , _thread( 0 )
        , _transferThread( new detail::TransferThread( ))
        , _computeContext( 0 )
{
}

Pipe::~Pipe()
{
    LBASSERT( getWindows().empty( ));
    delete _thread;
    _thread = 0;

    delete _transferThread;
}

Config* Pipe::getConfig()
{
    Node* node = getNode();
    LBASSERT( node );
    return ( node ? node->getConfig() : 0);
}

const Config* Pipe::getConfig() const
{
    const Node* node = getNode();
    LBASSERT( node );
    return ( node ? node->getConfig() : 0);
}

ClientPtr Pipe::getClient()
{
    Node* node = getNode();
    LBASSERT( node );
    return ( node ? node->getClient() : 0);
}

ServerPtr Pipe::getServer()
{
    Node* node = getNode();
    LBASSERT( node );
    return ( node ? node->getServer() : 0);
}

void Pipe::attach( const UUID& id, const uint32_t instanceID )
{
    Super::attach( id, instanceID );
    
    co::CommandQueue* queue = getPipeThreadQueue();
    co::CommandQueue* transferQ = getTransferThreadQueue();

    registerCommand( fabric::CMD_PIPE_CONFIG_INIT, 
                     PipeFunc( this, &Pipe::_cmdConfigInit ), queue );
    registerCommand( fabric::CMD_PIPE_CONFIG_EXIT, 
                     PipeFunc( this, &Pipe::_cmdConfigExit ), queue );
    registerCommand( fabric::CMD_PIPE_CREATE_WINDOW,
                     PipeFunc( this, &Pipe::_cmdCreateWindow ), queue );
    registerCommand( fabric::CMD_PIPE_DESTROY_WINDOW, 
                     PipeFunc( this, &Pipe::_cmdDestroyWindow ), queue );
    registerCommand( fabric::CMD_PIPE_FRAME_START,
                     PipeFunc( this, &Pipe::_cmdFrameStart ), queue );
    registerCommand( fabric::CMD_PIPE_FRAME_FINISH,
                     PipeFunc( this, &Pipe::_cmdFrameFinish ), queue );
    registerCommand( fabric::CMD_PIPE_FRAME_DRAW_FINISH, 
                     PipeFunc( this, &Pipe::_cmdFrameDrawFinish ), queue );
    registerCommand( fabric::CMD_PIPE_FRAME_START_CLOCK,
                     PipeFunc( this, &Pipe::_cmdFrameStartClock ), 0 );
    registerCommand( fabric::CMD_PIPE_EXIT_THREAD,
                     PipeFunc( this, &Pipe::_cmdExitThread ), queue );
    registerCommand( fabric::CMD_PIPE_DETACH_VIEW,
                     PipeFunc( this, &Pipe::_cmdDetachView ), queue );
    registerCommand( fabric::CMD_PIPE_EXIT_TRANSFER_THREAD,
                     PipeFunc( this, &Pipe::_cmdExitTransferThread ),
                     transferQ );
}

void Pipe::setDirty( const uint64_t bits )
{
    // jump over fabric setDirty to avoid dirty'ing node pipes list
    // pipes are individually synced in frame finish for thread-safety
    Object::setDirty( bits );
}

WindowSystem Pipe::selectWindowSystem() const
{
#ifdef AGL
    return WindowSystem( "AGL" ); // prefer over GLX
#else
    return WindowSystem();
#endif
}

void Pipe::_setupCommandQueue()
{
    LBINFO << "Set up pipe message pump for " << _windowSystem << std::endl;

    Config* config = getConfig();
    config->setupMessagePump( this );

    if( !_thread ) // Non-threaded pipes have no pipe thread message pump
        return;
    
    CommandQueue* queue = _thread->getWorkerQueue();
    LBASSERT( queue );
    LBASSERT( !queue->getMessagePump( ));

    Global::enterCarbon();
    MessagePump* pump = createMessagePump();
    if( pump )
        pump->dispatchAll(); // initializes _receiverQueue

    queue->setMessagePump( pump );
    Global::leaveCarbon();
}

int Pipe::_getAutoAffinity()
{
    /* Return -1 if no port and devices were specified by the
     * configuration file*/
    int cpuIndex = -1;

    const uint32_t port = getPort();
    const uint32_t device = getDevice();
    if (port == EQ_UNDEFINED_UINT32 || device == EQ_UNDEFINED_UINT32)
    {
        EQWARN << "No valid display is provided in the configuration file"
               << std::endl;
        EQINFO << "Automatic pipe thread placement failed" << std::endl;
        return cpuIndex;
    }
    else
    {
#ifdef EQ_USE_HWLOC
        hwloc_topology_t topology;
        hwloc_topology_init( &topology );

        /* Flags used for loading the I/O devices,
         * bridges and their relevant info */
        const unsigned long loading_flags = 0x0000000000000000
                                            ^ HWLOC_TOPOLOGY_FLAG_IO_BRIDGES
                                            ^ HWLOC_TOPOLOGY_FLAG_IO_DEVICES;
        /* Set discovery flags */
        const int sucess = hwloc_topology_set_flags( topology, loading_flags );

        /* Flags not set */
        if ( sucess < 0 )
        {
              EQINFO << "hwloc_topology_set_flags() failed," <<
               " PCI devices will not be loaded in the topology" << std::endl;
              EQINFO << "Automatic pipe thread placement failed" << std::endl;

              return cpuIndex;
        }
        hwloc_topology_load( topology );

        /* Get the cpuset for the socket connected to GPU
        attached to the display defined by its port and device */
        hwloc_bitmap_t cpuSet = get_display_cpuset
            ( topology, static_cast<int> (port), static_cast<int> (device) );

        const int numCpus = hwloc_get_nbobjs_by_type
                                          ( topology, HWLOC_OBJ_SOCKET );

        for (int i = 0; i <= numCpus - 1  ; i++)
        {
            hwloc_obj_t cpuObj = hwloc_get_obj_inside_cpuset_by_type
                                 ( topology, cpuSet, HWLOC_OBJ_SOCKET, i);
            if (cpuObj != 0)
            {
                cpuIndex = cpuObj->logical_index;
                break;
            }
        }
        hwloc_topology_destroy(topology);
#else
    EQINFO << "Missing hwloc," <<
              "Automatic thread placement is not supported" << std::endl;
#endif
    }
    return cpuIndex;
}

void Pipe::_setupAffinity()
{
    const int32_t affinity = getIAttribute( IATTR_HINT_AFFINITY );
    switch( affinity )
    {
        case OFF:
            break;

        case AUTO:

        {
            const int autoAffinitySocket = _getAutoAffinity();
            if ( autoAffinitySocket == -1 )
            {
                EQINFO << "Invalid display configuration values" << std::endl;
                EQINFO << "Auto thread placement is not supported" << std::endl;
            }
            else
                Pipe::Thread::setAffinity
                           ( autoAffinitySocket + lunchbox::Thread::SOCKET );
        }

            // To be implemented later
            /*
            const int32_t cpu = getCPU();
            Pipe::Thread::setAffinity( cpu );
            */
            break;

        default:
            Pipe::Thread::setAffinity( affinity );
            break;
    }
}

void Pipe::_exitCommandQueue()
{
    // Non-threaded pipes have no pipe thread message pump
    if( !_thread )
        return;
    
    CommandQueue* queue = _thread->getWorkerQueue();
    LBASSERT( queue );

    MessagePump* pump = queue->getMessagePump();
    queue->setMessagePump( 0 );
    delete pump;
}

MessagePump* Pipe::createMessagePump()
{
    return _windowSystem.createMessagePump();
}

MessagePump* Pipe::getMessagePump()
{
    LB_TS_THREAD( _pipeThread );
    if( !_thread )
        return 0;

    CommandQueue* queue = _thread->getWorkerQueue();
    return queue->getMessagePump();
}

void Pipe::Thread::run()
{
    LB_TS_THREAD( _pipe->_pipeThread );
    LBINFO << "Entered pipe thread" << std::endl;

    Pipe* pipe = _pipe; // _pipe gets cleared on exit
    pipe->_state.waitEQ( STATE_MAPPED );
    pipe->_windowSystem = pipe->selectWindowSystem();
    pipe->_setupCommandQueue();
    pipe->_setupAffinity();

    Worker::run();

    pipe->_exitCommandQueue();
    LBINFO << "Leaving pipe thread" << std::endl;
}

co::CommandQueue* Pipe::getPipeThreadQueue()
{
    if( _thread )
        return _thread->getWorkerQueue();

    return getNode()->getMainThreadQueue();
}

co::CommandQueue* Pipe::getTransferThreadQueue()
{
    return _transferThread->getWorkerQueue();
}

co::CommandQueue* Pipe::getMainThreadQueue()
{
    return getServer()->getMainThreadQueue();
}

co::CommandQueue* Pipe::getCommandThreadQueue()
{
    return getServer()->getCommandThreadQueue();
}

Frame* Pipe::getFrame( const co::ObjectVersion& frameVersion, const Eye eye,
                       const bool isOutput )
{
    LB_TS_THREAD( _pipeThread );
    Frame* frame = _frames[ frameVersion.identifier ];

    if( !frame )
    {
        ClientPtr client = getClient();
        frame = new Frame();

        LBCHECK( client->mapObject( frame, frameVersion ));
        _frames[ frameVersion.identifier ] = frame;
    }
    else
        frame->sync( frameVersion.version );

    const co::ObjectVersion& dataVersion = frame->getDataVersion( eye );
    LBLOG( LOG_ASSEMBLY ) << "Use " << dataVersion << std::endl;

    FrameData* frameData = getNode()->getFrameData( dataVersion );
    LBASSERT( frameData );

    if( isOutput )
    {
        if( !frameData->isAttached() )
        {
            ClientPtr client = getClient();
            LBCHECK( client->mapObject( frameData, dataVersion ));
        }
        else if( frameData->getVersion() < dataVersion.version )
            frameData->sync( dataVersion.version );

        _outputFrameDatas[ dataVersion.identifier ] = frameData;
    }

    frame->setData( frameData );
    return frame;
}

void Pipe::flushFrames()
{
    LB_TS_THREAD( _pipeThread );
    ClientPtr client = getClient();
    for( FrameHash::const_iterator i = _frames.begin(); i != _frames.end(); ++i)
    {
        Frame* frame = i->second;

        frame->setData( 0 ); // 'output' datas cleared below and from node
        frame->flush();
        client->unmapObject( frame );
        delete frame;
    }
    _frames.clear();

    for( FrameDataHash::const_iterator i = _outputFrameDatas.begin();
         i != _outputFrameDatas.end(); ++i)
    {
        FrameData* data = i->second;
        data->flush();
    }
    _outputFrameDatas.clear();
}

co::QueueSlave* Pipe::getQueue( const co::ObjectVersion& queueVersion )
{
    LB_TS_THREAD( _pipeThread );
    if( queueVersion.identifier == UUID::ZERO )
        return 0;

    co::QueueSlave* queue = _queues[ queueVersion.identifier ];
    if( !queue )
    {
        queue = new co::QueueSlave;
        ClientPtr client = getClient();
        LBCHECK( client->mapObject( queue, queueVersion ));

        _queues[ queueVersion.identifier ] = queue;
    }

    return queue;
}

void Pipe::_flushQueues()
{
    LB_TS_THREAD( _pipeThread );
    ClientPtr client = getClient();

    for( QueueHash::const_iterator i = _queues.begin(); i != _queues.end(); ++i)
    {
        co::QueueSlave* queue = i->second;
        client->unmapObject( queue );
        delete queue;
    }
    _queues.clear();
}

const View* Pipe::getView( const co::ObjectVersion& viewVersion ) const
{
    // Yie-ha: we want to have a const-interface to get a view on the render
    //         clients, but view mapping is by definition non-const.
    return const_cast< Pipe* >( this )->getView( viewVersion );
}

View* Pipe::getView( const co::ObjectVersion& viewVersion )
{
    LB_TS_THREAD( _pipeThread );
    if( viewVersion.identifier == UUID::ZERO )
        return 0;

    View* view = _views[ viewVersion.identifier ];
    if( !view )
    {
        NodeFactory* nodeFactory = Global::getNodeFactory();
        view = nodeFactory->createView( 0 );
        LBASSERT( view );
        view->_pipe = this;        
        ClientPtr client = getClient();
        LBCHECK( client->mapObject( view, viewVersion ));

        _views[ viewVersion.identifier ] = view;
    }
    
    view->sync( viewVersion.version );
    return view;
}

void Pipe::_releaseViews()
{
    LB_TS_THREAD( _pipeThread );
    for( bool changed = true; changed; )
    {
        changed = false;
        for( ViewHash::iterator i = _views.begin(); i != _views.end(); ++i )
        {
            View* view = i->second;
            view->commit();
            if( view->getVersion() + 20 > view->getHeadVersion( ))
                continue;

            // release unused view to avoid memory leaks due to deltas piling up
            view->_pipe = 0;

            ClientPtr client = getClient();
            client->unmapObject( view );            
            _views.erase( i );

            NodeFactory* nodeFactory = Global::getNodeFactory();
            nodeFactory->releaseView( view );

            changed = true;
            break;
        }
    }
}

void Pipe::_flushViews()
{
    LB_TS_THREAD( _pipeThread );
    NodeFactory* nodeFactory = Global::getNodeFactory();
    ClientPtr client = getClient();

    for( ViewHash::const_iterator i = _views.begin(); i != _views.end(); ++i )
    {
        View* view = i->second;

        client->unmapObject( view );
        view->_pipe = 0;
        nodeFactory->releaseView( view );
    }
    _views.clear();
}

void Pipe::startThread()
{
    _thread = new Thread( this );
    _thread->start();
}

void Pipe::exitThread()
{
    _stopTransferThread();

    if( !_thread )
        return;

    PipeExitThreadPacket packet;
    send( getLocalNode(), packet );

    _thread->join();
    delete _thread;
    _thread = 0;
}

void Pipe::cancelThread()
{
    _stopTransferThread();

    if( !_thread )
        return;

    PipeExitThreadPacket pkg;
    co::Command& command = getLocalNode()->allocCommand( sizeof( pkg ));
    eq::PipeExitThreadPacket* packet = 
        command.getModifiable< eq::PipeExitThreadPacket >();

    memcpy( packet, &pkg, sizeof( pkg ));
    dispatchCommand( command );
}

void Pipe::waitExited() const
{
    _state.waitGE( STATE_STOPPED );
}

bool Pipe::isRunning() const
{
    return (_state == STATE_RUNNING);
}

bool Pipe::isStopped() const
{
    return (_state == STATE_STOPPED);
}

void Pipe::notifyMapped()
{
    LBASSERT( _state == STATE_STOPPED );
    _state = STATE_MAPPED;
}

namespace
{
class WaitFinishedVisitor : public PipeVisitor
{
public:
    WaitFinishedVisitor( const uint32_t frame ) : _frame( frame ) {}

    virtual VisitorResult visit( Channel* channel )
        {
            channel->waitFrameFinished( _frame );
            return TRAVERSE_CONTINUE;
        }

private:
    const uint32_t _frame;
};
}

void Pipe::waitFrameFinished( const uint32_t frameNumber ) const
{
    _finishedFrame.waitGE( frameNumber );
    WaitFinishedVisitor waiter( frameNumber );
    accept( waiter );
}

void Pipe::waitFrameLocal( const uint32_t frameNumber ) const
{
    _unlockedFrame.waitGE( frameNumber );
}

uint32_t Pipe::getCurrentFrame() const
{
    LB_TS_THREAD( _pipeThread );
    return _currentFrame;
}

uint32_t Pipe::getFinishedFrame() const
{
    return _finishedFrame.get();
}


//---------------------------------------------------------------------------
// pipe-thread methods
//---------------------------------------------------------------------------
bool Pipe::configInit( const uint128_t& initID )
{
    LB_TS_THREAD( _pipeThread );

    LBASSERT( !_systemPipe );

    if ( !configInitSystemPipe( initID ))
        return false;   

    // -------------------------------------------------------------------------
    LBASSERT(!_computeContext);

    // for now we only support CUDA
#ifdef EQ_USE_CUDA
    if( getIAttribute( IATTR_HINT_CUDA_GL_INTEROP ) == eq::ON )
    {
        LBINFO << "Initializing CUDAContext" << std::endl;
        ComputeContext* computeCtx = new CUDAContext( this );

        if( !computeCtx->configInit() )
        {
            LBASSERT( getError() != ERROR_NONE );
            LBWARN << "GPU Computing context initialization failed: "
                   << getError() << std::endl;
            delete computeCtx;
            return false;
        }
        setComputeContext( computeCtx );
    }
#endif

    return true;
}

bool Pipe::configInitSystemPipe( const uint128_t& )
{
    SystemPipe* systemPipe = _windowSystem.createPipe( this );
    LBASSERT( systemPipe );

    if( !systemPipe->configInit( ))
    {
        LBASSERT( getError() != ERROR_NONE );
        LBERROR << "System pipe context initialization failed: "
                << getError() << std::endl;
        delete systemPipe;
        return false;
    }

    setSystemPipe( systemPipe );
    return true;
}

bool Pipe::configExit()
{
    LB_TS_THREAD( _pipeThread );

    if( _computeContext )
    {
        _computeContext->configExit();
        delete _computeContext;
        _computeContext = 0;
    }

    if( _systemPipe )
    {
        _systemPipe->configExit( );
        delete _systemPipe;
        _systemPipe = 0;
    }
    return true;
}


void Pipe::frameStart( const uint128_t&, const uint32_t frameNumber ) 
{
    LB_TS_THREAD( _pipeThread );

    const Node* node = getNode();
    switch( node->getIAttribute( Node::IATTR_THREAD_MODEL ))
    {
        case ASYNC:      // No sync, release immediately
            releaseFrameLocal( frameNumber ); 
            break;

        case DRAW_SYNC:  // Sync, release in frameDrawFinish
        case LOCAL_SYNC: // Sync, release in frameFinish
            node->waitFrameStarted( frameNumber );
            break;

        default:
            LBUNIMPLEMENTED;
    }

    startFrame( frameNumber );
}

void Pipe::frameDrawFinish( const uint128_t&, const uint32_t frameNumber )
{
    const Node* node = getNode();
    switch( node->getIAttribute( Node::IATTR_THREAD_MODEL ))
    {
        case ASYNC:      // released in frameStart
            break;

        case DRAW_SYNC:  // release
            releaseFrameLocal( frameNumber );
            break;

        case LOCAL_SYNC: // release in frameFinish
            break;

        default:
            LBUNIMPLEMENTED;
    }
}

void Pipe::frameFinish( const uint128_t&, const uint32_t frameNumber )
{
    const Node* node = getNode();
    switch( node->getIAttribute( Node::IATTR_THREAD_MODEL ))
    {
        case ASYNC:      // released in frameStart
            break;

        case DRAW_SYNC:  // released in frameDrawFinish
            break;

        case LOCAL_SYNC: // release
            releaseFrameLocal( frameNumber ); 
            break;

        default:
            LBUNIMPLEMENTED;
    }

    // Global release
    releaseFrame( frameNumber );
}

void Pipe::startFrame( const uint32_t frameNumber )
{ 
    LB_TS_THREAD( _pipeThread );
    _currentFrame = frameNumber; 
    LBLOG( LOG_TASKS ) << "---- Started Frame ---- "<< frameNumber << std::endl;
}

void Pipe::releaseFrame( const uint32_t frameNumber )
{ 
    LB_TS_THREAD( _pipeThread );
    _finishedFrame = frameNumber; 
    LBLOG( LOG_TASKS ) << "---- Finished Frame --- "<< frameNumber << std::endl;
}

void Pipe::releaseFrameLocal( const uint32_t frameNumber )
{ 
    LB_TS_THREAD( _pipeThread );
    LBASSERTINFO( _unlockedFrame + 1 == frameNumber,
                  _unlockedFrame << ", " << frameNumber );

    _unlockedFrame = frameNumber;
    LBLOG( LOG_TASKS ) << "---- Unlocked Frame --- " << _unlockedFrame.get()
                       << std::endl;
}

bool Pipe::startTransferThread()
{
    if( _transferThread->isRunning( ))
        return true;

    return _transferThread->start();
}

bool Pipe::hasTransferThread() const
{
    return _transferThread->isRunning();
}

void Pipe::_stopTransferThread()
{
    if( _transferThread->isStopped( ))
        return;

    PipeExitTransferThreadPacket packet;
    send( getLocalNode(), packet );

    _transferThread->join();
}

//---------------------------------------------------------------------------
// command handlers
//---------------------------------------------------------------------------
bool Pipe::_cmdCreateWindow( co::Command& command )
{
    const PipeCreateWindowPacket* packet = 
        command.get<PipeCreateWindowPacket>();
    LBLOG( LOG_INIT ) << "Create window " << packet << std::endl;

    Window* window = Global::getNodeFactory()->createWindow( this );
    window->init(); // not in ctor, virtual method

    Config* config = getConfig();
    LBCHECK( config->mapObject( window, packet->windowID ));
    
    return true;
}

bool Pipe::_cmdDestroyWindow(  co::Command& command  )
{
    const PipeDestroyWindowPacket* packet =
        command.get<PipeDestroyWindowPacket>();
    LBLOG( LOG_INIT ) << "Destroy window " << packet << std::endl;

    Window* window = _findWindow( packet->windowID );
    LBASSERT( window );

    // re-set shared windows accordingly
    Window* newSharedWindow = 0;
    const Windows& windows = getWindows(); 
    for( Windows::const_iterator i = windows.begin(); i != windows.end(); ++i )
    {
        Window* candidate = *i;
        
        if( candidate == window )
            continue; // ignore

        if( candidate->getSharedContextWindow() == window )
        {
            if( newSharedWindow )
                candidate->setSharedContextWindow( newSharedWindow );
            else
            {
                newSharedWindow = candidate;
                newSharedWindow->setSharedContextWindow( candidate );
            }
        }

        LBASSERT( candidate->getSharedContextWindow() != window );
    }

    WindowConfigExitReplyPacket reply( packet->windowID, window->isStopped( ));

    Config* config = getConfig();
    config->unmapObject( window );
    Global::getNodeFactory()->releaseWindow( window );

    getServer()->send( reply ); // do not use Object::send()
    return true;
}

bool Pipe::_cmdConfigInit( co::Command& command )
{
    LB_TS_THREAD( _pipeThread );
    const PipeConfigInitPacket* packet = 
        command.get<PipeConfigInitPacket>();
    LBLOG( LOG_INIT ) << "Init pipe " << packet << std::endl;

    if( !isThreaded( ))
    {
        _windowSystem = selectWindowSystem();
        _setupCommandQueue();
    }

    PipeConfigInitReplyPacket reply;
    setError( ERROR_NONE );

    Node* node = getNode();
    LBASSERT( node );
    node->waitInitialized();

    if( node->isRunning( ))
    {
        _currentFrame  = packet->frameNumber;
        _finishedFrame = packet->frameNumber;
        _unlockedFrame = packet->frameNumber;
        _state = STATE_INITIALIZING;

        reply.result = configInit( packet->initID );

        if( reply.result )
            _state = STATE_RUNNING;
    }
    else
    {
        setError( ERROR_PIPE_NODE_NOTRUNNING );
        reply.result = false;
    }

    LBLOG( LOG_INIT ) << "TASK pipe config init reply " << &reply << std::endl;

    co::NodePtr netNode = command.getNode();

    commit();
    send( netNode, reply );
    return true;
}

bool Pipe::_cmdConfigExit( co::Command& command )
{
    LB_TS_THREAD( _pipeThread );
    const PipeConfigExitPacket* packet = 
        command.get<PipeConfigExitPacket>();
    LBLOG( LOG_INIT ) << "TASK pipe config exit " << packet << std::endl;

    _state = STATE_STOPPING; // needed in View::detach (from _flushViews)

    // send before node gets a chance to send its destroy packet
    NodeDestroyPipePacket destroyPacket( getID( ));
    getNode()->send( getLocalNode(), destroyPacket );

    // Flush views before exit since they are created after init
    // - application may need initialized pipe to exit
    // - configExit can't access views since all channels are gone already
    _flushViews();
    _flushQueues();
    _state = configExit() ? STATE_STOPPED : STATE_FAILED;
    return true;
}

bool Pipe::_cmdExitThread( co::Command& command )
{
    LBASSERT( _thread );
    _thread->_pipe = 0;
    return true;
}

bool Pipe::_cmdExitTransferThread( co::Command& )
{
    LBASSERT( _transferThread );
    _transferThread->postStop();
    return true;
}

bool Pipe::_cmdFrameStartClock( co::Command& )
{
    LBVERB << "start frame clock" << std::endl;
    _frameTimeMutex.set();
    _frameTimes.push_back( getConfig()->getTime( ));
    _frameTimeMutex.unset();
    return true;
}

bool Pipe::_cmdFrameStart( co::Command& command )
{
    LB_TS_THREAD( _pipeThread );
    const PipeFrameStartPacket* packet = 
        command.get<PipeFrameStartPacket>();
    LBVERB << "handle pipe frame start " << packet << std::endl;
    LBLOG( LOG_TASKS ) << "---- TASK start frame ---- " << packet << std::endl;
    sync( packet->version );
    const int64_t lastFrameTime = _frameTime;

    _frameTimeMutex.set();
    LBASSERT( !_frameTimes.empty( ));

    _frameTime = _frameTimes.front();
    _frameTimes.pop_front();
    _frameTimeMutex.unset();

    if( lastFrameTime > 0 )
    {
        PipeStatistics waitEvent( Statistic::PIPE_IDLE, this );
        waitEvent.event.data.statistic.idleTime =
            _thread ? _thread->getWorkerQueue()->resetWaitTime() : 0;
        waitEvent.event.data.statistic.totalTime = _frameTime - lastFrameTime;
    }

    const uint32_t frameNumber = packet->frameNumber;
    LBASSERTINFO( _currentFrame + 1 == frameNumber,
                  "current " << _currentFrame << " start " << frameNumber );

    frameStart( packet->frameID, frameNumber );
    return true;
}

bool Pipe::_cmdFrameFinish( co::Command& command )
{
    LB_TS_THREAD( _pipeThread );
    const PipeFrameFinishPacket* packet =
        command.get<PipeFrameFinishPacket>();
    LBLOG( LOG_TASKS ) << "---- TASK finish frame --- " << packet << std::endl;

    const uint32_t frameNumber = packet->frameNumber;
    LBASSERTINFO( _currentFrame >= frameNumber, 
                  "current " << _currentFrame << " finish " << frameNumber );

    frameFinish( packet->frameID, frameNumber );
    
    LBASSERTINFO( _finishedFrame >= frameNumber, 
                  "Pipe::frameFinish() did not release frame " << frameNumber );

    if( _unlockedFrame < frameNumber )
    {
        LBWARN << "Finished frame was not locally unlocked, enforcing unlock" 
               << std::endl << "    unlocked " << _unlockedFrame.get()
               << " done " << frameNumber << std::endl;
        releaseFrameLocal( frameNumber );
    }

    if( _finishedFrame < frameNumber )
    {
        LBWARN << "Finished frame was not released, enforcing unlock"
               << std::endl;
        releaseFrame( frameNumber );
    }

    _releaseViews();

    const uint128_t version = commit();
    if( version != co::VERSION_NONE )
    {
        fabric::ObjectSyncPacket syncPacket;
        send( command.getNode(), syncPacket );
    }
    return true;
}

bool Pipe::_cmdFrameDrawFinish( co::Command& command )
{
    LB_TS_THREAD( _pipeThread );
    const PipeFrameDrawFinishPacket* packet =
        command.get< PipeFrameDrawFinishPacket >();
    LBLOG( LOG_TASKS ) << "TASK draw finish " << getName() <<  " " << packet
                       << std::endl;

    frameDrawFinish( packet->frameID, packet->frameNumber );
    return true;
}

bool Pipe::_cmdDetachView( co::Command& command )
{
    LB_TS_THREAD( _pipeThread );
    const PipeDetachViewPacket* packet = command.get< PipeDetachViewPacket >();

    ViewHash::iterator i = _views.find( packet->viewID );
    if( i != _views.end( ))
    {
        View* view = i->second;
        _views.erase( i );

        NodeFactory* nodeFactory = Global::getNodeFactory();
        nodeFactory->releaseView( view );
    }
    return true;
}

}

#include "../fabric/pipe.ipp"
template class eq::fabric::Pipe< eq::Node, eq::Pipe, eq::Window,
                                 eq::PipeVisitor >;

/** @cond IGNORE */
template EQFABRIC_API std::ostream& eq::fabric::operator << ( std::ostream&,
                                                const eq::Super& );
/** @endcond */
