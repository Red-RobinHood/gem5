/*
 * Copyright (c) 2008 Princeton University
 * Copyright (c) 2016 Georgia Institute of Technology
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __MEM_RUBY_NETWORK_BETAAR_0_COMMONTYPES_HH__
#define __MEM_RUBY_NETWORK_BETAAR_0_COMMONTYPES_HH__

#include "base/logging.hh"
#include "mem/ruby/common/NetDest.hh"
#include "mem/ruby/system/RubySystem.hh"

namespace gem5
{

namespace ruby
{

namespace betaar
{

// All common enums and typedefs go here

enum flit_type
{
    HEAD_,
    BODY_,
    TAIL_,
    HEAD_TAIL_,
    CREDIT_,
    NUM_FLIT_TYPE_
};
enum VC_state_type
{
    IDLE_,
    VC_AB_,
    ACTIVE_,
    NUM_VC_STATE_TYPE_
};
enum VNET_type
{
    CTRL_VNET_,
    DATA_VNET_,
    NULL_VNET_,
    NUM_VNET_TYPE_
};
enum flit_stage
{
    I_,
    VA_,
    SA_,
    ST_,
    LT_,
    NUM_FLIT_STAGE_
};
enum link_type
{
    EXT_IN_,
    EXT_OUT_,
    INT_,
    NUM_LINK_TYPES_
};
enum RoutingAlgorithm
{
    TABLE_ = 0,
    XY_ = 1,
    CUSTOM_ = 2,
    NUM_ROUTING_ALGORITHM_
};

struct RouteInfo
{
    RouteInfo()
        : vnet(0),
          src_ni(0),
          src_router(0),
          dest_ni(0),
          dest_router(0),
          hops_traversed(0)
    {}

    // destination format for table-based routing
    // NOTE: net_dest may legitimately hold MORE THAN ONE destination when
    // routing_algorithm == XY_ -- this is what carries a multicast message's
    // full destination set through the network. RoutingUnit is responsible
    // for splitting/narrowing it hop by hop (see outportComputeMulticastXY).
    int vnet;
    NetDest net_dest;

    // src and dest format for topology-specific routing
    // NOTE: dest_ni/dest_router are only meaningful as a *representative*
    // destination (used by TABLE_/CUSTOM_ routing, which are unicast-only,
    // and by stats). For XY_ multicast routing, net_dest is authoritative.
    int src_ni;
    int src_router;
    int dest_ni;
    int dest_router;
    int hops_traversed;
};

// One outgoing branch of a (possibly multicast) routing decision: the
// subset of a flit's destinations that should leave a router via outport,
// as computed by RoutingUnit::outportCompute(). A unicast route produces
// exactly one RouteBranch; a multicast route bucketed across several
// directions (see outportComputeMulticastXY) produces one per non-empty
// bucket.
struct RouteBranch
{
    RouteBranch(int outport_, const NetDest &dest_subset_)
        : outport(outport_), dest_subset(dest_subset_)
    {}
    int outport;
    NetDest dest_subset;
};

// Converts a flat NodeID (as returned by NetDest::getAllDest()) back into
// the MachineID it was derived from. Factored out of NetworkInterface's
// old per-destination flitisize loop so RoutingUnit can reuse it when
// bucketing a multicast NetDest by destination router coordinates.
inline MachineID
nodeIDToMachineID(RubySystem *ruby_system, NodeID id)
{
    for (int m = 0; m < (int)MachineType_NUM; m++) {
        if ((id >= ruby_system->MachineType_base_number((MachineType)m)) &&
            id < ruby_system->MachineType_base_number((MachineType)(m + 1))) {
            MachineID mid;
            mid.type = (MachineType)m;
            mid.num =
                id - ruby_system->MachineType_base_number((MachineType)m);
            return mid;
        }
    }
    panic("nodeIDToMachineID: NodeID %d out of range", id);
    return MachineID();
}

#define INFINITE_ 10000

} // namespace betaar
} // namespace ruby
} // namespace gem5

#endif //__MEM_RUBY_NETWORK_BETAAR_0_COMMONTYPES_HH__
