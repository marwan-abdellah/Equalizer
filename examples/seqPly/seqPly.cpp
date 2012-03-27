
/* Copyright (c) 2011, Stefan Eilemann <eile@eyescale.ch> 
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Eyescale Software GmbH nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "seqPly.h"

#include "renderer.h"

#ifndef MIN
#  define MIN EQ_MIN
#endif
#include <tclap/CmdLine.h>

namespace seqPly
{

bool Application::init( const int argc, char** argv )
{
    if( !seq::Application::init( argc, argv, 0 ))
        return false;

    _loadModel( argc, argv );
    return true;
}

bool Application::run()
{
    return seq::Application::run( &_frameData );
}

bool Application::exit()
{
    _unloadModel();
    return seq::Application::exit();
}

co::Object* Application::createObject( const uint32_t type )
{
//    switch( type )
    {
//      default:
          EQUNIMPLEMENTED;
          return 0;
    }
}

seq::Renderer* Application::createRenderer()
{
    return new Renderer( *this );
}

namespace
{
static bool _isPlyfile( const std::string& filename )
{
    const size_t size = filename.length();
    if( size < 5 )
        return false;

    if( filename[size-4] != '.' || filename[size-3] != 'p' ||
        filename[size-2] != 'l' || filename[size-1] != 'y' )
    {
        return false;
    }
    return true;
}
}

void Application::_loadModel( const int argc, char** argv )
{
    TCLAP::CmdLine command( "seqPly - Sequel polygonal rendering example" );
    TCLAP::ValueArg<std::string> modelArg( "m", "model", "ply model file name",
                                           false, "", "string", command );
    TCLAP::VariableSwitchArg ignoreEqArgs( "eq", "Ignored Equalizer options",
                                           command );
    TCLAP::UnlabeledMultiArg< std::string > 
        ignoreArgs( "ignore", "Ignored unlabeled arguments", false, "any",
                    command );
    command.parse( argc, argv );

    eq::Strings filenames;
#ifdef EQ_RELEASE
#  ifdef _WIN32 // final INSTALL_DIR is not known at compile time
    filenames.push_back( "../share/Equalizer/data" );
#  else
    filenames.push_back( std::string( EQ_INSTALL_DIR ) +
                         std::string( "share/Equalizer/data" ));
#  endif
#else
    filenames.push_back( std::string( EQ_SOURCE_DIR ) + 
                         std::string( "examples/eqPly" ));
#endif

    if( modelArg.isSet( ))
        filenames.push_back( modelArg.getValue( ));

    while( !filenames.empty( ))
    {
        const std::string filename = filenames.back();
        filenames.pop_back();
     
        if( _isPlyfile( filename ))
        {
            _model = new Model;
            if( _model->readFromFile( filename.c_str( )))
            {
                _modelDist = new ModelDist( _model );
                _modelDist->registerTree( this );
                _frameData.setModelID( _modelDist->getID( ));
                return;
            }
            delete _model;
            _model = 0;
        }
        else
        {
            const std::string basename = co::base::getFilename( filename );
            if( basename == "." || basename == ".." )
                continue;

            // recursively search directories
            const eq::Strings subFiles = co::base::searchDirectory( filename,
                                                                    "*" );

            for(eq::StringsCIter i = subFiles.begin(); i != subFiles.end(); ++i)
                filenames.push_back( filename + '/' + *i );
        }
    }
}

void Application::_unloadModel()
{
    if( !_modelDist )
        return;

    _modelDist->deregisterTree();
    delete _modelDist;
    _modelDist = 0;

    delete _model;
    _model = 0;
}

const Model* Application::getModel( const eq::uint128_t& modelID )
{
    if( modelID == eq::UUID::ZERO )
        return 0;
    if( _model )
        return _model;
    co::base::memoryBarrier();

    // Accessed concurrently from render threads
    co::base::ScopedMutex<> mutex( _modelLock );

    EQASSERT( !_modelDist );
    _modelDist = new ModelDist;
    Model* model = _modelDist->loadModel( getMasterNode(), this, modelID );
    EQASSERT( model );
    _model = model;

    return model;
}


}

