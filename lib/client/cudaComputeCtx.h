
/* Copyright (c) 2009, Philippe Robert <philippe.robert@gmail.com> 
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

#ifndef EQ_CUDA_COMPUTE_CONTEXT_H
#define EQ_CUDA_COMPUTE_CONTEXT_H

#include <eq/client/computeCtx.h> // base class

namespace eq
{
    class EQ_EXPORT CUDAComputeCtx : public ComputeCtx
    {
    public:
        /** Create a new CUDAComputeCtx.*/
        CUDAComputeCtx( Pipe* parent );

        /** Destroy the ComputeCtx. */
        virtual ~CUDAComputeCtx( );

        /** @name Methods forwarded from eq::Pipe */
        //@{
        /** Initialize the ComputeCtx. */
        virtual bool configInit( );

        /** De-initialize the ComputeCtx. */
        virtual void configExit( );
        //@}

    private:

        int _getMaxGflopsDeviceId();

        union // placeholder for binary-compatible changes
        {
            char dummy[64];
        };

    };		
}

#endif // EQ_CUDA_COMPUTE_CONTEXT_H
