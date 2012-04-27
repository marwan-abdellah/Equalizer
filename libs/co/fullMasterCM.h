
/* Copyright (c) 2007-2012, Stefan Eilemann <eile@equalizergraphics.com> 
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

#ifndef CO_FULLMASTERCM_H
#define CO_FULLMASTERCM_H

#include "versionedMasterCM.h"        // base class
#include "objectInstanceDataOStream.h" // member

#include <deque>

namespace co
{
    class ObjectDataIStream;

    /** 
     * An object change manager handling only full versions for the master
     * instance.
     * @internal
     */
    class FullMasterCM : public VersionedMasterCM
    {
    public:
        FullMasterCM( Object* object );
        virtual ~FullMasterCM();

        virtual void init();
        virtual uint128_t commit( const uint32_t incarnation );
        virtual void push( const uint128_t& groupID, const uint128_t& typeID,
                           const Nodes& nodes );

        /** @name Versioning */
        //@{
        virtual void setAutoObsolete( const uint32_t count );
        virtual uint32_t getAutoObsolete() const { return _nVersions; }
        //@}

        /** Speculatively send instance data to all nodes. */
        virtual void sendInstanceData( Nodes& nodes );
    
    protected:
        struct InstanceData
        {
            InstanceData( const VersionedMasterCM* cm ) 
                    : os( cm ), commitCount( 0 ) {}

            ObjectInstanceDataOStream os;
            uint32_t commitCount;
        };
        
        virtual void _initSlave( NodePtr node, const uint128_t& version,
                                 const NodeMapObjectPacket* packet,
                                 NodeMapObjectSuccessPacket& success,
                                 NodeMapObjectReplyPacket& reply );

        InstanceData* _newInstanceData();
        void _addInstanceData( InstanceData* data );
        void _releaseInstanceData( InstanceData* data );

        void _updateCommitCount( const uint32_t incarnation );
        void _obsolete();
        void _checkConsistency() const;

        virtual bool isBuffered() const{ return true; }
        virtual void _commit();

    private:
        /** The number of commits, needed for auto-obsoletion. */
        uint32_t _commitCount;

        /** The number of old versions to retain. */
        uint32_t _nVersions;

        typedef std::deque< InstanceData* > InstanceDataDeque;
        typedef std::vector< InstanceData* > InstanceDatas;

        /** The list of full instance datas, head version last. */
        InstanceDataDeque _instanceDatas;
        InstanceDatas _instanceDataCache;

        /* The command handlers. */
        bool _cmdCommit( Command& command );
        bool _cmdObsolete( Command& command );
        bool _cmdPush( Command& command );
    };
}

#endif // CO_FULLMASTERCM_H
