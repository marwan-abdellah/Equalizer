
/* Copyright (c) 2012, Daniel Nachbaur <danielnachbaur@googlemail.com>
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

#ifndef CO_DATAISTREAMARCHIVE_H
#define CO_DATAISTREAMARCHIVE_H

#include <co/base/defines.h>
#ifdef CO_USE_BOOST_SERIALIZATION

#include <co/api.h>
#include <co/dataIStream.h>

#include <boost/archive/detail/common_iarchive.hpp>
#include <boost/archive/detail/register_archive.hpp>
#include <boost/archive/shared_ptr_helper.hpp>

namespace co
{
    /**
     * An implemenation of a boost.serialization archive for loading data from a
     * co::DataIStream.
     */
    class DataIStreamArchive
            : public boost::archive::detail::common_iarchive<DataIStreamArchive>
            , public boost::archive::detail::shared_ptr_helper
    {
    public:
        CO_API DataIStreamArchive( DataIStream& stream );

        // archives are expected to support this function
        CO_API void load_binary( void* data, std::size_t size );

    private:
        friend class boost::archive::load_access;

        template<class T>
        void load( T& t )
        {
            _stream >> t;
        }

        DataIStream& _stream;
    };
}

BOOST_SERIALIZATION_REGISTER_ARCHIVE(co::DataIStreamArchive)

#endif //CO_USE_BOOST_SERIALIZATION
#endif //CO_DATAISTREAMARCHIVE_H