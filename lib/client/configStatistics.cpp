
/* Copyright (c) 2008, Stefan Eilemann <eile@equalizergraphics.com> 
   All rights reserved. */

#include "configStatistics.h"

#include "config.h"
#include "global.h"

namespace eq
{

ConfigStatistics::ConfigStatistics( const Statistic::Type type, 
                                    Config* config )
        : _config( config )
{
    event.data.type                  = Event::STATISTIC;
    event.data.originator            = config->getID();
    event.data.statistic.type        = type;
    event.data.statistic.frameNumber = config->getCurrentFrame();

    event.data.statistic.startTime  = config->getTime();
}


ConfigStatistics::~ConfigStatistics()
{
    event.data.statistic.endTime     = _config->getTime();
    _config->sendEvent( event );
}

}
