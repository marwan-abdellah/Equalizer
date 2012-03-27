
/* Copyright (c) 2011, Stefan Eilemann <eile@eyescale.ch> 
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

#include "application.h"

#include "error.h"
#include "objectType.h"
#include "renderer.h"
#include "viewData.h"
#include "detail/application.h"
#include "detail/config.h"

#include <eq/client/config.h>
#include <eq/client/configParams.h>
#include <eq/client/init.h>
#include <eq/client/server.h>

namespace seq
{

Application::Application()
        : _impl( 0 )
{
}

Application::~Application()
{
    EQASSERT( !_impl );
}

co::NodePtr Application::getMasterNode()
{
    eq::Config* config = getConfig();
    EQASSERT( config );
    if( !config )
        return 0;
    return config->getApplicationNode();
}

eq::Config* Application::getConfig()
{
    EQASSERT( _impl );
    if( !_impl )
        return 0;
    return _impl->getConfig();
}

void Application::destroyRenderer( Renderer* renderer )
{
    delete renderer;
}

ViewData* Application::createViewData()
{
    return new ViewData;
}

void Application::destroyViewData( ViewData* viewData )
{
    delete viewData;
}

bool Application::init( const int argc, char** argv, co::Object* initData )
{
    EQASSERT( !_impl );
    if( _impl )
    {
        EQERROR << "Already initialized" << std::endl;
        return false;
    }

    _impl = new detail::Application( this, initData );
    initErrors();
    if( !eq::init( argc, argv, _impl ))
    {
        EQERROR << "Equalizer initialization failed" << std::endl;
        return false;
    }

    if( !initLocal( argc, argv ))
    {
        EQERROR << "Can't initialization client node" << std::endl;
        exit();
        return false;
    }

    if( !_impl->init( ))
    {
        exit();
        return false;
    }
        
    return true;
}

bool Application::run( co::Object* frameData )
{
    return _impl->run( frameData );
}

bool Application::exit()
{
    bool retVal = true;
    if( _impl )
        retVal = _impl->exit();

    if( !exitLocal( ))
        retVal = false;

    if( !eq::exit( ))
        retVal = false;

    exitErrors();
    delete _impl;
    _impl = 0;

    EQASSERTINFO( getRefCount() == 1, this->getRefCount( ));
    return retVal;
}

void Application::stopRunning()
{
    getConfig()->stopRunning();
}

}
