
/* Copyright (c) 2005-2012, Stefan Eilemann <eile@equalizergraphics.com>
 *                    2010, Cedric Stalder <cedric.stalder@gmail.com>
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

#include "objectStore.h"

#include "barrier.h"
#include "command.h"
#include "connection.h"
#include "connectionDescription.h"
#include "global.h"
#include "instanceCache.h"
#include "log.h"
#include "nodePackets.h"
#include "objectCM.h"
#include "objectDataIStream.h"
#include "objectPackets.h"

#include <lunchbox/scopedMutex.h>

#include <limits>

//#define DEBUG_DISPATCH
#ifdef DEBUG_DISPATCH
#  include <set>
#endif

namespace co
{
typedef CommandFunc<ObjectStore> CmdFunc;

ObjectStore::ObjectStore( LocalNode* localNode )
        : _localNode( localNode )
        , _instanceIDs( -0x7FFFFFFF )
        , _instanceCache( new InstanceCache( Global::getIAttribute( 
                              Global::IATTR_INSTANCE_CACHE_SIZE ) * LB_1MB ) )
{
    LBASSERT( localNode );
    CommandQueue* queue = localNode->getCommandThreadQueue();

    localNode->_registerCommand( CMD_NODE_FIND_MASTER_NODE_ID,
        CmdFunc( this, &ObjectStore::_cmdFindMasterNodeID ), queue );
    localNode->_registerCommand( CMD_NODE_FIND_MASTER_NODE_ID_REPLY,
        CmdFunc( this, &ObjectStore::_cmdFindMasterNodeIDReply ), 0 );
    localNode->_registerCommand( CMD_NODE_ATTACH_OBJECT,
        CmdFunc( this, &ObjectStore::_cmdAttachObject ), 0 );
    localNode->_registerCommand( CMD_NODE_DETACH_OBJECT,
        CmdFunc( this, &ObjectStore::_cmdDetachObject ), 0 );
    localNode->_registerCommand( CMD_NODE_REGISTER_OBJECT,
        CmdFunc( this, &ObjectStore::_cmdRegisterObject ), queue );
    localNode->_registerCommand( CMD_NODE_DEREGISTER_OBJECT,
        CmdFunc( this, &ObjectStore::_cmdDeregisterObject ), queue );
    localNode->_registerCommand( CMD_NODE_MAP_OBJECT,
        CmdFunc( this, &ObjectStore::_cmdMapObject ), queue );
    localNode->_registerCommand( CMD_NODE_MAP_OBJECT_SUCCESS,
        CmdFunc( this, &ObjectStore::_cmdMapObjectSuccess ), 0 );
    localNode->_registerCommand( CMD_NODE_MAP_OBJECT_REPLY,
        CmdFunc( this, &ObjectStore::_cmdMapObjectReply ), 0 );
    localNode->_registerCommand( CMD_NODE_UNMAP_OBJECT,
        CmdFunc( this, &ObjectStore::_cmdUnmapObject ), 0 );
    localNode->_registerCommand( CMD_NODE_UNSUBSCRIBE_OBJECT,
        CmdFunc( this, &ObjectStore::_cmdUnsubscribeObject ), queue );
    localNode->_registerCommand( CMD_NODE_OBJECT_INSTANCE,
        CmdFunc( this, &ObjectStore::_cmdInstance ), 0 );
    localNode->_registerCommand( CMD_NODE_OBJECT_INSTANCE_MAP,
        CmdFunc( this, &ObjectStore::_cmdInstance ), 0 );
    localNode->_registerCommand( CMD_NODE_OBJECT_INSTANCE_COMMIT,
        CmdFunc( this, &ObjectStore::_cmdInstance ), 0 );
    localNode->_registerCommand( CMD_NODE_OBJECT_INSTANCE_PUSH,
        CmdFunc( this, &ObjectStore::_cmdInstance ), 0 );
    localNode->_registerCommand( CMD_NODE_DISABLE_SEND_ON_REGISTER,
        CmdFunc( this, &ObjectStore::_cmdDisableSendOnRegister ), queue );
    localNode->_registerCommand( CMD_NODE_REMOVE_NODE,
        CmdFunc( this, &ObjectStore::_cmdRemoveNode ), queue );
    localNode->_registerCommand( CMD_NODE_OBJECT_PUSH,
        CmdFunc( this, &ObjectStore::_cmdObjectPush ), queue );
}

ObjectStore::~ObjectStore()
{
    LBVERB << "Delete ObjectStore @" << (void*)this << std::endl;
    
#ifndef NDEBUG
    if( !_objects->empty( ))
    {
        LBWARN << _objects->size() << " attached objects in destructor"
               << std::endl;
        
        for( ObjectsHash::const_iterator i = _objects->begin();
             i != _objects->end(); ++i )
        {
            const Objects& objects = i->second;
            LBWARN << "  " << objects.size() << " objects with id " 
                   << i->first << std::endl;
            
            for( Objects::const_iterator j = objects.begin();
                 j != objects.end(); ++j )
            {
                const Object* object = *j;
                LBINFO << "    object type " << lunchbox::className( object )
                       << std::endl;
            }
        }
    }
    //LBASSERT( _objects->empty( ))
#endif
   clear();
   delete _instanceCache;
   _instanceCache = 0;
}

void ObjectStore::clear( )
{
    LBASSERT( _objects->empty( ));
    expireInstanceData( 0 );
    LBASSERT( !_instanceCache || _instanceCache->isEmpty( ));

    _objects->clear();
    _sendQueue.clear();
}

void ObjectStore::disableInstanceCache()
{
    LBASSERT( _localNode->isClosed( ));
    delete _instanceCache;
    _instanceCache = 0;
}

void ObjectStore::expireInstanceData( const int64_t age )
{
    if( _instanceCache )
        _instanceCache->expire( age ); 
}

void ObjectStore::removeInstanceData( const NodeID& nodeID )
{
    if( _instanceCache )
        _instanceCache->remove( nodeID ); 
}

void ObjectStore::enableSendOnRegister()
{
    ++_sendOnRegister;
}

void ObjectStore::disableSendOnRegister()
{
    if( Global::getIAttribute( Global::IATTR_NODE_SEND_QUEUE_SIZE ) > 0 )
    {
        NodeDisableSendOnRegisterPacket packet;
        packet.requestID = _localNode->registerRequest();
        _localNode->send( packet );
        _localNode->waitRequest( packet.requestID );
    }
    else // OPT
        --_sendOnRegister;
}

//---------------------------------------------------------------------------
// identifier master node mapping
//---------------------------------------------------------------------------
NodeID ObjectStore::_findMasterNodeID( const UUID& identifier )
{
    LB_TS_NOT_THREAD( _commandThread );

    // OPT: look up locally first?
    Nodes nodes;
    _localNode->getNodes( nodes );
    
    // OPT: send to multiple nodes at once?
    for( NodesIter i = nodes.begin(); i != nodes.end(); ++i )
    {
        NodePtr node = *i;
        NodeFindMasterNodeIDPacket packet;
        packet.requestID = _localNode->registerRequest();
        packet.identifier = identifier;

        LBLOG( LOG_OBJECTS ) << "Finding " << identifier << " on " << node
                             << " req " << packet.requestID << std::endl;
        node->send( packet );

        NodeID masterNodeID = UUID::ZERO;
        _localNode->waitRequest( packet.requestID, masterNodeID );

        if( masterNodeID != UUID::ZERO )
        {
            LBLOG( LOG_OBJECTS ) << "Found " << identifier << " on "
                                 << masterNodeID << std::endl;
            return masterNodeID;
        }
    }

    return UUID::ZERO;
}

//---------------------------------------------------------------------------
// object mapping
//---------------------------------------------------------------------------
void ObjectStore::attachObject( Object* object, const UUID& id, 
                                const uint32_t instanceID )
{
    LBASSERT( object );
    LB_TS_NOT_THREAD( _receiverThread );

    NodeAttachObjectPacket packet;
    packet.requestID = _localNode->registerRequest( object );
    packet.objectID = id;
    packet.objectInstanceID = instanceID;

    _localNode->send( packet );
    _localNode->waitRequest( packet.requestID );
}

namespace
{
uint32_t _genNextID( lunchbox::a_int32_t& val )
{
    uint32_t result;
    do
    {
        const long id = ++val;
        result = static_cast< uint32_t >(
            static_cast< int64_t >( id ) + 0x7FFFFFFFu );
    }
    while( result > EQ_INSTANCE_MAX );

    return result;
}
}

void ObjectStore::_attachObject( Object* object, const UUID& id, 
                                 const uint32_t inInstanceID )
{
    LBASSERT( object );
    LB_TS_THREAD( _receiverThread );

    uint32_t instanceID = inInstanceID;
    if( inInstanceID == EQ_INSTANCE_INVALID )
        instanceID = _genNextID( _instanceIDs );

    object->attach( id, instanceID );

    {
        lunchbox::ScopedFastWrite mutex( _objects );
        Objects& objects = _objects.data[ id ];
        LBASSERTINFO( !object->isMaster() || objects.empty(),
            "Attaching master " << *object << ", " << objects.size() <<
            " attached objects with same ID, first is: " << *objects[0] );
        objects.push_back( object );
    }

    _localNode->flushCommands(); // redispatch pending commands

    LBLOG( LOG_OBJECTS ) << "attached " << *object << " @" 
                         << static_cast< void* >( object ) << std::endl;
}

void ObjectStore::detachObject( Object* object )
{
    LBASSERT( object );
    LB_TS_NOT_THREAD( _receiverThread );

    NodeDetachObjectPacket packet;
    packet.requestID = _localNode->registerRequest();
    packet.objectID  = object->getID();
    packet.objectInstanceID  = object->getInstanceID();

    _localNode->send( packet );
    _localNode->waitRequest( packet.requestID );
}

void ObjectStore::swapObject( Object* oldObject, Object* newObject )
{
    LBASSERT( newObject );
    LBASSERT( oldObject );
    LBASSERT( oldObject->isMaster() );
    LB_TS_THREAD( _receiverThread );

    if( !oldObject->isAttached() )
        return;

    LBLOG( LOG_OBJECTS ) << "Swap " << lunchbox::className( oldObject ) <<std::endl;
    const UUID& id = oldObject->getID();

    lunchbox::ScopedFastWrite mutex( _objects );
    ObjectsHash::iterator i = _objects->find( id );
    LBASSERT( i != _objects->end( ));
    if( i == _objects->end( ))
        return;

    Objects& objects = i->second;
    Objects::iterator j = find( objects.begin(), objects.end(), oldObject );
    LBASSERT( j != objects.end( ));
    if( j == objects.end( ))
        return;

    newObject->transfer( oldObject );
    *j = newObject;
}

void ObjectStore::_detachObject( Object* object )
{
    // check also _cmdUnmapObject when modifying!
    LBASSERT( object );
    LB_TS_THREAD( _receiverThread );

    if( !object->isAttached() )
        return;

    const UUID& id = object->getID();

    LBASSERT( _objects->find( id ) != _objects->end( ));
    LBLOG( LOG_OBJECTS ) << "Detach " << *object << std::endl;

    Objects& objects = _objects.data[ id ];
    Objects::iterator i = find( objects.begin(),objects.end(), object );
    LBASSERT( i != objects.end( ));

    {
        lunchbox::ScopedFastWrite mutex( _objects );
        objects.erase( i );
        if( objects.empty( ))
            _objects->erase( id );
    }

    LBASSERT( object->getInstanceID() != EQ_INSTANCE_INVALID );
    object->detach();
    return;
}

uint32_t ObjectStore::mapObjectNB( Object* object, const UUID& id,
                                   const uint128_t& version )
{
    LBASSERTINFO( id.isGenerated(), id );
    if( !id.isGenerated( ))
        return LB_UNDEFINED_UINT32;

    NodePtr master = _connectMaster( id );
    LBASSERT( master );
    if( master )
        return mapObjectNB( object, id, version, master );
    return LB_UNDEFINED_UINT32;
}

uint32_t ObjectStore::mapObjectNB( Object* object, const UUID& id, 
                                   const uint128_t& version, NodePtr master )
{
    if( !master )
        return mapObjectNB( object, id, version ); // will call us again

    LB_TS_NOT_THREAD( _commandThread );
    LB_TS_NOT_THREAD( _receiverThread );
    LBLOG( LOG_OBJECTS )
        << "Mapping " << lunchbox::className( object ) << " to id " << id
        << " version " << version << std::endl;
    LBASSERT( object );
    LBASSERTINFO( id.isGenerated(), id );

    if( !object || !id.isGenerated( ))
    {
        LBWARN << "Invalid object " << object << " or id " << id << std::endl;
        return LB_UNDEFINED_UINT32;
    }

    const bool isAttached = object->isAttached();
    const bool isMaster = object->isMaster();
    LBASSERT( !isAttached );
    LBASSERT( !isMaster ) ;
    if( isAttached || isMaster )
    {
        LBWARN << "Invalid object state: attached " << isAttached << " master "
               << isMaster << std::endl;
        return LB_UNDEFINED_UINT32;
    }

    if( !master || !master->isConnected( ))
    {
        LBWARN << "Mapping of object " << id << " failed, invalid master node"
               << std::endl;
        return LB_UNDEFINED_UINT32;
    }

    NodeMapObjectPacket packet;
    packet.requestID        = _localNode->registerRequest( object );
    packet.objectID         = id;
    packet.requestedVersion = version;
    packet.maxVersion       = object->getMaxVersions();
    packet.instanceID       = _genNextID( _instanceIDs );

    if( _instanceCache )
    {
        const InstanceCache::Data& cached = (*_instanceCache)[ id ];
        if( cached != InstanceCache::Data::NONE )
        {
            const ObjectDataIStreamDeque& versions = cached.versions;
            LBASSERT( !cached.versions.empty( ));
            packet.useCache = true;
            packet.masterInstanceID = cached.masterInstanceID;
            packet.minCachedVersion = versions.front()->getVersion();
            packet.maxCachedVersion = versions.back()->getVersion();
            LBLOG( LOG_OBJECTS ) << "Object " << id << " have v"
                                 << packet.minCachedVersion << ".."
                                 << packet.maxCachedVersion << std::endl;
        }
    }

    object->notifyAttach();
    master->send( packet );
    return packet.requestID;
}

bool ObjectStore::mapObjectSync( const uint32_t requestID )
{
    if( requestID == LB_UNDEFINED_UINT32 )
        return false;

    void* data = _localNode->getRequestData( requestID );    
    if( data == 0 )
        return false;

    Object* object = LBSAFECAST( Object*, data );
    uint128_t version = VERSION_NONE;
    _localNode->waitRequest( requestID, version );

    const bool mapped = object->isAttached();
    if( mapped )
        object->applyMapData( version ); // apply initial instance data

    object->notifyAttached();
    LBLOG( LOG_OBJECTS ) << "Mapped " << lunchbox::className( object ) << std::endl;
    return mapped;
}

void ObjectStore::unmapObject( Object* object )
{
    LBASSERT( object );
    if( !object->isAttached( )) // not registered
        return;

    const UUID& id = object->getID();
    
    LBLOG( LOG_OBJECTS ) << "Unmap " << object << std::endl;

    object->notifyDetach();

    // send unsubscribe to master, master will send detach packet.
    LBASSERT( !object->isMaster( ));
    LB_TS_NOT_THREAD( _commandThread );
    
    const uint32_t masterInstanceID = object->getMasterInstanceID();
    if( masterInstanceID != EQ_INSTANCE_INVALID )
    {
        NodePtr master = object->getMasterNode();
        LBASSERT( master )

        if( master && master->isConnected( ))
        {
            NodeUnsubscribeObjectPacket packet;
            packet.requestID = _localNode->registerRequest();
            packet.objectID  = id;
            packet.masterInstanceID = masterInstanceID;
            packet.slaveInstanceID  = object->getInstanceID();
            master->send( packet );

            _localNode->waitRequest( packet.requestID );
            object->notifyDetached();
            return;
        }
        LBERROR << "Master node for object id " << id << " not connected"
                << std::endl;
    }

    // no unsubscribe sent: Detach directly
    detachObject( object );
    object->setupChangeManager( Object::NONE, false, 0, EQ_INSTANCE_INVALID );
    object->notifyDetached();
}

bool ObjectStore::registerObject( Object* object )
{
    LBASSERT( object );
    LBASSERT( !object->isAttached() );

    const UUID& id = object->getID( );
    LBASSERTINFO( id.isGenerated(), id );

    object->notifyAttach();
    object->setupChangeManager( object->getChangeType(), true, _localNode,
                                EQ_INSTANCE_INVALID );
    attachObject( object, id, EQ_INSTANCE_INVALID );

    if( Global::getIAttribute( Global::IATTR_NODE_SEND_QUEUE_SIZE ) > 0 )
    {
        NodeRegisterObjectPacket packet;
        packet.object = object;
        _localNode->send( packet );
    }

    object->notifyAttached();

    LBLOG( LOG_OBJECTS ) << "Registered " << object << std::endl;
    return true;
}

void ObjectStore::deregisterObject( Object* object )
{
    LBASSERT( object );
    if( !object->isAttached( )) // not registered
        return;

    LBLOG( LOG_OBJECTS ) << "Deregister " << *object << std::endl;
    LBASSERT( object->isMaster( ));

    object->notifyDetach();

    if( Global::getIAttribute( Global::IATTR_NODE_SEND_QUEUE_SIZE ) > 0  )
    {
        // remove from send queue
        NodeDeregisterObjectPacket packet;
        packet.requestID = _localNode->registerRequest( object );
        _localNode->send( packet );
        _localNode->waitRequest( packet.requestID );
    }

    const UUID id = object->getID();
    detachObject( object );
    object->setupChangeManager( Object::NONE, true, 0, EQ_INSTANCE_INVALID );
    if( _instanceCache )
        _instanceCache->erase( id );
    object->notifyDetached();
}

NodePtr ObjectStore::_connectMaster( const UUID& id )
{
    const NodeID masterNodeID = _findMasterNodeID( id );
    if( masterNodeID == UUID::ZERO )
    {
        LBWARN << "Can't find master node for object id " << id <<std::endl;
        return 0;
    }

    NodePtr master = _localNode->connect( masterNodeID );
    if( master.isValid() && !master->isClosed( ))
        return master;

    LBWARN << "Can't connect master node with id " << masterNodeID
           << " for object id " << id << std::endl;
    return 0;
}

bool ObjectStore::notifyCommandThreadIdle()
{
    LB_TS_THREAD( _commandThread );
    if( _sendQueue.empty( ))
        return false;

    LBASSERT( _sendOnRegister > 0 );
    SendQueueItem& item = _sendQueue.front();

    if( item.age > _localNode->getTime64( ))
    {
        Nodes nodes;
        _localNode->getNodes( nodes, false );
        if( nodes.empty( ))
        {
            lunchbox::Thread::yield();
            return !_sendQueue.empty();
        }

        item.object->sendInstanceData( nodes );
    }
    _sendQueue.pop_front();
    return !_sendQueue.empty();
}

void ObjectStore::removeNode( NodePtr node )
{
    NodeRemoveNodePacket packet;
    packet.node = node.get();
    packet.requestID = _localNode->registerRequest( );
    _localNode->send( packet );
    _localNode->waitRequest( packet.requestID );
}

//===========================================================================
// Packet handling
//===========================================================================
bool ObjectStore::dispatchObjectCommand( Command& command )
{
    LB_TS_THREAD( _receiverThread );
    const ObjectPacket* packet = command.get< ObjectPacket >();
    const UUID& id = packet->objectID;
    const uint32_t instanceID = packet->instanceID;

    ObjectsHash::const_iterator i = _objects->find( id );

    if( i == _objects->end( ))
        // When the instance ID is set to none, we only care about the packet
        // when we have an object of the given ID (multicast)
        return ( instanceID == EQ_INSTANCE_NONE );

    const Objects& objects = i->second;
    LBASSERTINFO( !objects.empty(), packet );

    if( instanceID <= EQ_INSTANCE_MAX )
    {
        for( Objects::const_iterator j = objects.begin(); j!=objects.end(); ++j)
        {
            Object* object = *j;
            if( instanceID == object->getInstanceID( ))
            {
                LBCHECK( object->dispatchCommand( command ));
                return true;
            }
        }
        LBUNREACHABLE;
        return false;
    }

    Objects::const_iterator j = objects.begin();
    Object* object = *j;
    LBCHECK( object->dispatchCommand( command ));

    for( ++j; j != objects.end(); ++j )
    {
        object = *j;
        Command& clone = _localNode->cloneCommand( command );
        LBCHECK( object->dispatchCommand( clone ));
    }
    return true;
}

bool ObjectStore::_cmdFindMasterNodeID( Command& command )
{
    LB_TS_THREAD( _commandThread );

    const NodeFindMasterNodeIDPacket* packet = 
          command.get<NodeFindMasterNodeIDPacket>();

    const UUID& id = packet->identifier;
    LBASSERT( id.isGenerated() );

    NodeFindMasterNodeIDReplyPacket reply( packet );
    {
        lunchbox::ScopedFastRead mutex( _objects );
        ObjectsHashCIter i = _objects->find( id );

        if( i != _objects->end( ))
        {
            const Objects& objects = i->second;
            LBASSERTINFO( !objects.empty(), packet );

            for( ObjectsCIter j = objects.begin(); j != objects.end(); ++j )
            {
                Object* object = *j;
                if( object->isMaster( ))
                    reply.masterNodeID = _localNode->getNodeID();
                else
                {
                    NodePtr master = object->getMasterNode();
                    if( master.isValid( ))
                        reply.masterNodeID = master->getNodeID();
                }
                if( reply.masterNodeID != UUID::ZERO )
                    break;
            }
        }
    }

    LBLOG( LOG_OBJECTS ) << "Object " << id << " master " << reply.masterNodeID
                         << " req " << reply.requestID << std::endl;
    command.getNode()->send( reply );
    return true;
}

bool ObjectStore::_cmdFindMasterNodeIDReply( Command& command )
{
    const NodeFindMasterNodeIDReplyPacket* packet =
          command.get< NodeFindMasterNodeIDReplyPacket >();
    _localNode->serveRequest( packet->requestID, packet->masterNodeID );
    return true;
}

bool ObjectStore::_cmdAttachObject( Command& command )
{
    LB_TS_THREAD( _receiverThread );
    const NodeAttachObjectPacket* packet =
        command.get< NodeAttachObjectPacket >();
    LBLOG( LOG_OBJECTS ) << "Cmd attach object " << packet << std::endl;

    Object* object = static_cast< Object* >( _localNode->getRequestData( 
                                                 packet->requestID ));
    _attachObject( object, packet->objectID, packet->objectInstanceID );
    _localNode->serveRequest( packet->requestID );
    return true;
}

bool ObjectStore::_cmdDetachObject( Command& command )
{
    LB_TS_THREAD( _receiverThread );
    const NodeDetachObjectPacket* packet =
        command.get< NodeDetachObjectPacket >();
    LBLOG( LOG_OBJECTS ) << "Cmd detach object " << packet << std::endl;

    const UUID& id = packet->objectID;
    ObjectsHash::const_iterator i = _objects->find( id );
    if( i != _objects->end( ))
    {
        const Objects& objects = i->second;

        for( Objects::const_iterator j = objects.begin();
             j != objects.end(); ++j )
        {
            Object* object = *j;
            if( object->getInstanceID() == packet->objectInstanceID )
            {
                _detachObject( object );
                break;
            }
        }
    }

    LBASSERT( packet->requestID != LB_UNDEFINED_UINT32 );
    _localNode->serveRequest( packet->requestID );
    return true;
}

bool ObjectStore::_cmdRegisterObject( Command& command )
{
    LB_TS_THREAD( _commandThread );
    if( _sendOnRegister <= 0 )
        return true;

    const NodeRegisterObjectPacket* packet = 
        command.get< NodeRegisterObjectPacket >();
    LBLOG( LOG_OBJECTS ) << "Cmd register object " << packet << std::endl;

    const int32_t age = Global::getIAttribute(
                            Global::IATTR_NODE_SEND_QUEUE_AGE );
    SendQueueItem item;
    item.age = age ? age + _localNode->getTime64() :
                     std::numeric_limits< int64_t >::max();
    item.object = packet->object;
    _sendQueue.push_back( item );

    const uint32_t size = Global::getIAttribute( 
                             Global::IATTR_NODE_SEND_QUEUE_SIZE );
    while( _sendQueue.size() > size )
        _sendQueue.pop_front();

    return true;
}

bool ObjectStore::_cmdDeregisterObject( Command& command )
{
    LB_TS_THREAD( _commandThread );
    const NodeDeregisterObjectPacket* packet = 
        command.get< NodeDeregisterObjectPacket >();
    LBLOG( LOG_OBJECTS ) << "Cmd deregister object " << packet << std::endl;

    const void* object = _localNode->getRequestData( packet->requestID ); 

    for( SendQueue::iterator i = _sendQueue.begin(); i < _sendQueue.end(); ++i )
    {
        if( i->object == object )
        {
            _sendQueue.erase( i );
            break;
        }
    }

    _localNode->serveRequest( packet->requestID );
    return true;
}

bool ObjectStore::_cmdMapObject( Command& command )
{
    LB_TS_THREAD( _commandThread );
    const NodeMapObjectPacket* packet = command.get< NodeMapObjectPacket >();
    LBLOG( LOG_OBJECTS ) << "Cmd map object " << packet << std::endl;

    NodePtr node = command.getNode();
    const UUID& id = packet->objectID;
    Object* master = 0;
    {
        lunchbox::ScopedFastRead mutex( _objects );
        ObjectsHash::const_iterator i = _objects->find( id );
        if( i != _objects->end( ))
        {
            const Objects& objects = i->second;

            for( ObjectsCIter j = objects.begin(); j != objects.end(); ++j )
            {
                Object* object = *j;
                if( object->isMaster( ))
                {
                    master = object;
                    break;
                }
            }
        }
    }

    if( master )
        master->addSlave( command );
    else
    {
        LBWARN << "Can't find master object to map " << id << std::endl;

        NodeMapObjectReplyPacket reply( packet );
        reply.nodeID = node->getNodeID();
        node->send( reply );
    }
    return true;
}

bool ObjectStore::_cmdMapObjectSuccess( Command& command )
{
    LB_TS_THREAD( _receiverThread );
    const NodeMapObjectSuccessPacket* packet = 
        command.get<NodeMapObjectSuccessPacket>();

    // Map success packets are potentially multicasted (see above)
    // verify that we are the intended receiver
    if( packet->nodeID != _localNode->getNodeID( ))
        return true;

    LBLOG( LOG_OBJECTS ) << "Cmd map object success " << packet << std::endl;

    // set up change manager and attach object to dispatch table
    Object* object = static_cast<Object*>( _localNode->getRequestData( 
                                               packet->requestID ));    
    LBASSERT( object );
    LBASSERT( !object->isMaster( ));

    object->setupChangeManager( Object::ChangeType( packet->changeType ), false,
                                _localNode, packet->masterInstanceID );
    _attachObject( object, packet->objectID, packet->instanceID );
    return true;
}

bool ObjectStore::_cmdMapObjectReply( Command& command )
{
    LB_TS_THREAD( _receiverThread );
    const NodeMapObjectReplyPacket* packet = 
        command.get< NodeMapObjectReplyPacket >();
    LBLOG( LOG_OBJECTS ) << "Cmd map object reply " << packet << std::endl;

    // Map reply packets are potentially multicasted (see above)
    // verify that we are the intended receiver
    if( packet->nodeID != _localNode->getNodeID( ))
        return true;

    LBASSERT( _localNode->getRequestData( packet->requestID ));

    if( packet->result )
    {
        Object* object = static_cast<Object*>( 
            _localNode->getRequestData( packet->requestID ));    
        LBASSERT( object );
        LBASSERT( !object->isMaster( ));

        object->setMasterNode( command.getNode( ));

        if( packet->useCache )
        {
            LBASSERT( packet->releaseCache );
            LBASSERT( _instanceCache );

            const UUID& id = packet->objectID;
            const InstanceCache::Data& cached = (*_instanceCache)[ id ];
            LBASSERT( cached != InstanceCache::Data::NONE );
            LBASSERT( !cached.versions.empty( ));

            object->addInstanceDatas( cached.versions, packet->version );
            LBCHECK( _instanceCache->release( id, 2 ));
        }
        else if( packet->releaseCache )
        {
            LBCHECK( _instanceCache->release( packet->objectID, 1 ));
        }
    }
    else
    {
        if( packet->releaseCache )
            _instanceCache->release( packet->objectID, 1 );

        LBWARN << "Could not map object " << packet->objectID << std::endl;
    }

    _localNode->serveRequest( packet->requestID, packet->version );
    return true;
}

bool ObjectStore::_cmdUnsubscribeObject( Command& command )
{
    LB_TS_THREAD( _commandThread );
    const NodeUnsubscribeObjectPacket* packet =
        command.get< NodeUnsubscribeObjectPacket >();
    LBLOG( LOG_OBJECTS ) << "Cmd unsubscribe object  " << packet << std::endl;

    NodePtr node = command.getNode();
    const UUID& id = packet->objectID;

    {
        lunchbox::ScopedFastWrite mutex( _objects );
        ObjectsHash::const_iterator i = _objects->find( id );
        if( i != _objects->end( ))
        {
            const Objects& objects = i->second;
            for( ObjectsCIter j = objects.begin(); j != objects.end(); ++j )
            {
                Object* object = *j;
                if( object->isMaster() && 
                    object->getInstanceID() == packet->masterInstanceID )
                {
                    object->removeSlave( node, packet->slaveInstanceID );
                    break;
                }
            }   
        }
    }

    NodeDetachObjectPacket detachPacket( packet );
    node->send( detachPacket );
    return true;
}

bool ObjectStore::_cmdUnmapObject( Command& command )
{
    LB_TS_THREAD( _receiverThread );
    const NodeUnmapObjectPacket* packet = 
        command.get< NodeUnmapObjectPacket >();

    LBLOG( LOG_OBJECTS ) << "Cmd unmap object " << packet << std::endl;
    if( _instanceCache )
        _instanceCache->erase( packet->objectID );

    ObjectsHash::iterator i = _objects->find( packet->objectID );
    if( i == _objects->end( )) // nothing to do
        return true;

    const Objects objects = i->second;
    {
        lunchbox::ScopedFastWrite mutex( _objects );
        _objects->erase( i );
    }

    for( Objects::const_iterator j = objects.begin(); j != objects.end(); ++j )
    {
        Object* object = *j;
        object->detach();
    }

    return true;
}

bool ObjectStore::_cmdInstance( Command& command )
{
    LB_TS_THREAD( _receiverThread );
    LBASSERT( _localNode );

    ObjectInstancePacket* packet =
        command.getModifiable< ObjectInstancePacket >();
    LBLOG( LOG_OBJECTS ) << "Cmd instance " << packet << std::endl;

    const uint32_t type = packet->command;

    packet->type = PACKETTYPE_CO_OBJECT;
    packet->command = CMD_OBJECT_INSTANCE;
    const ObjectVersion rev( packet->objectID, packet->version ); 

    if( _instanceCache )
    {
#ifndef CO_AGGRESSIVE_CACHING // Issue #82: 
        if( type != CMD_NODE_OBJECT_INSTANCE_PUSH )
#endif
            _instanceCache->add( rev, packet->masterInstanceID, command, 0 );
    }

    switch( type )
    {
      case CMD_NODE_OBJECT_INSTANCE:
        LBASSERT( packet->nodeID == NodeID::ZERO );
        LBASSERT( packet->instanceID == EQ_INSTANCE_NONE );
        return true;

      case CMD_NODE_OBJECT_INSTANCE_MAP:
        if( packet->nodeID != _localNode->getNodeID( )) // not for me
            return true;

        LBASSERT( packet->instanceID <= EQ_INSTANCE_MAX );
        return dispatchObjectCommand( command );

      case CMD_NODE_OBJECT_INSTANCE_COMMIT:
        LBASSERT( packet->nodeID == NodeID::ZERO );
        LBASSERT( packet->instanceID == EQ_INSTANCE_NONE );
        return dispatchObjectCommand( command );

      case CMD_NODE_OBJECT_INSTANCE_PUSH:
        LBASSERT( packet->nodeID == NodeID::ZERO );
        LBASSERT( packet->instanceID == EQ_INSTANCE_NONE );
        _pushData.addDataPacket( packet->objectID, command );
        return true;

      default:
        LBUNREACHABLE;
        return false;
    }
}

bool ObjectStore::_cmdDisableSendOnRegister( Command& command )
{
    LB_TS_THREAD( _commandThread );
    LBASSERTINFO( _sendOnRegister > 0, _sendOnRegister );

    if( --_sendOnRegister == 0 )
    {
        _sendQueue.clear();

        Nodes nodes;
        _localNode->getNodes( nodes, false );
        for( NodesCIter i = nodes.begin(); i != nodes.end(); ++i )
        {
            NodePtr node = *i;
            ConnectionPtr connection = node->useMulticast();
            if( connection )
                connection->finish();
        }
    }

    const NodeDisableSendOnRegisterPacket* packet =
        command.get< NodeDisableSendOnRegisterPacket >();
    _localNode->serveRequest( packet->requestID );
    return true;
}

bool ObjectStore::_cmdRemoveNode( Command& command )
{
    LB_TS_THREAD( _commandThread );
    const NodeRemoveNodePacket* packet = command.get< NodeRemoveNodePacket >();

    LBLOG( LOG_OBJECTS ) << "Cmd object  " << packet << std::endl;

    lunchbox::ScopedFastWrite mutex( _objects );
    for( ObjectsHashCIter i = _objects->begin(); i != _objects->end(); ++i )
    {
        const Objects& objects = i->second;
        for( ObjectsCIter j = objects.begin(); j != objects.end(); ++j )
            (*j)->removeSlaves( packet->node );
    }

    if( packet->requestID != LB_UNDEFINED_UINT32 )
        _localNode->serveRequest( packet->requestID );

    return true;
}

bool ObjectStore::_cmdObjectPush( Command& command )
{
    LB_TS_THREAD( _commandThread );
    const NodeObjectPushPacket* packet = command.get< NodeObjectPushPacket >();
    ObjectDataIStream* is = _pushData.pull( packet->objectID );
    
    _localNode->objectPush( packet->groupID, packet->typeID, packet->objectID,
                            *is );
    _pushData.recycle( is );
    return true;
}

std::ostream& operator << ( std::ostream& os, ObjectStore* objectStore )
{
    if( !objectStore )
    {
        os << "NULL objectStore";
        return os;
    }
    
    os << "objectStore (" << (void*)objectStore << ")";

    return os;
}
}
