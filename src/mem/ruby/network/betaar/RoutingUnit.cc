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

#include "mem/ruby/network/betaar/RoutingUnit.hh"

#include "base/cast.hh"
#include "base/compiler.hh"
#include "debug/RubyNetwork.hh"
#include "mem/ruby/network/betaar/InputUnit.hh"
#include "mem/ruby/network/betaar/Router.hh"
#include "mem/ruby/slicc_interface/Message.hh"

namespace gem5
{

namespace ruby
{

namespace betaar
{

RoutingUnit::RoutingUnit(Router *router)
{
    m_router = router;
    m_routing_table.clear();
    m_weight_table.clear();
}

void
RoutingUnit::addRoute(std::vector<NetDest> &routing_table_entry)
{
    if (routing_table_entry.size() > m_routing_table.size()) {
        m_routing_table.resize(routing_table_entry.size());
    }
    for (int v = 0; v < routing_table_entry.size(); v++) {
        m_routing_table[v].push_back(routing_table_entry[v]);
    }
}

void
RoutingUnit::addWeight(int link_weight)
{
    m_weight_table.push_back(link_weight);
}

bool
RoutingUnit::supportsVnet(int vnet, std::vector<int> sVnets)
{
    // If all vnets are supported, return true
    if (sVnets.size() == 0) {
        return true;
    }

    // Find the vnet in the vector, return true
    if (std::find(sVnets.begin(), sVnets.end(), vnet) != sVnets.end()) {
        return true;
    }

    // Not supported vnet
    return false;
}

/*
 * This is the default routing algorithm in betaar.
 * The routing table is populated during topology creation.
 * Routes can be biased via weight assignments in the topology file.
 * Correct weight assignments are critical to provide deadlock avoidance.
 */
int
RoutingUnit::lookupRoutingTable(int vnet, NetDest msg_destination)
{
    // First find all possible output link candidates
    // For ordered vnet, just choose the first
    // (to make sure different packets don't choose different routes)
    // For unordered vnet, randomly choose any of the links
    // To have a strict ordering between links, they should be given
    // different weights in the topology file

    int output_link = -1;
    int min_weight = INFINITE_;
    std::vector<int> output_link_candidates;
    int num_candidates = 0;

    // Identify the minimum weight among the candidate output links
    for (int link = 0; link < m_routing_table[vnet].size(); link++) {
        if (msg_destination.intersectionIsNotEmpty(
                m_routing_table[vnet][link])) {

            if (m_weight_table[link] <= min_weight) {
                min_weight = m_weight_table[link];
            }
        }
    }

    // Collect all candidate output links with this minimum weight
    for (int link = 0; link < m_routing_table[vnet].size(); link++) {
        if (msg_destination.intersectionIsNotEmpty(
                m_routing_table[vnet][link])) {

            if (m_weight_table[link] == min_weight) {
                num_candidates++;
                output_link_candidates.push_back(link);
            }
        }
    }

    if (output_link_candidates.size() == 0) {
        fatal("Fatal Error:: No Route exists from this Router.");
        exit(0);
    }

    // Randomly select any candidate output link
    int candidate = 0;
    if (!(m_router->get_net_ptr())->isVNetOrdered(vnet)) {
        candidate = rand() % num_candidates;
    }

    output_link = output_link_candidates.at(candidate);
    return output_link;
}

void
RoutingUnit::addInDirection(PortDirection inport_dirn, int inport_idx)
{
    m_inports_dirn2idx[inport_dirn] = inport_idx;
    m_inports_idx2dirn[inport_idx] = inport_dirn;
}

void
RoutingUnit::addOutDirection(PortDirection outport_dirn, int outport_idx)
{
    m_outports_dirn2idx[outport_dirn] = outport_idx;
    m_outports_idx2dirn[outport_idx] = outport_dirn;
}

// Merges dest into an existing branch targeting the same outport, or
// appends a new branch if none exists yet. Used when bucketing a
// multi-destination NetDest, since two different destinations can resolve
// to the same outport (e.g. two local NIs behind the same "Local" port, or
// -- in principle -- two routing-table entries sharing a link).
static void
mergeOrAddBranch(std::vector<RouteBranch> &branches, int outport,
                 const NetDest &dest)
{
    for (auto &b : branches) {
        if (b.outport == outport) {
            b.dest_subset.addNetDest(dest);
            return;
        }
    }
    branches.emplace_back(outport, dest);
}

// outportCompute() is called by the InputUnit.
// Always returns at least one branch. Splits route.net_dest into
// destinations local to this router (delivered to one or more attached
// NIs) and remote destinations (dispatched to the configured routing
// algorithm). A RouteInfo with a single destination degenerates to
// exactly one branch everywhere below, so unicast behavior is unchanged.
std::vector<RouteBranch>
RoutingUnit::outportCompute(RouteInfo route, int inport,
                            PortDirection inport_dirn)
{
    std::vector<RouteBranch> branches;
    RubySystem *rs = m_router->get_net_ptr()->getRubySystem();

    NetDest remote_subset(rs);
    bool have_remote = false;

    for (NodeID n : route.net_dest.getAllDest()) {
        int dest_router =
            m_router->get_net_ptr()->get_router_id(n, route.vnet);

        NetDest single(rs);
        single.add(nodeIDToMachineID(rs, n));

        if (dest_router == m_router->get_id()) {
            // Multiple NIs may be connected to this router, all with
            // output port direction = "Local". Resolve per destination and
            // merge subsets that resolve to the same outport, since a
            // multicast message can have more than one destination local
            // to this same router.
            int outport = lookupRoutingTable(route.vnet, single);
            mergeOrAddBranch(branches, outport, single);
        } else {
            remote_subset.addNetDest(single);
            have_remote = true;
        }
    }

    if (!have_remote) {
        assert(!branches.empty());
        return branches;
    }

    RouteInfo remote_route = route;
    remote_route.net_dest = remote_subset;

    // Routing Algorithm set in BetaarNetwork.py
    // Can be over-ridden from command line using --routing-algorithm = 1
    RoutingAlgorithm routing_algorithm =
        (RoutingAlgorithm)m_router->get_net_ptr()->getRoutingAlgorithm();

    switch (routing_algorithm) {
        case XY_: {
            for (auto &b : outportComputeMulticastXY(remote_route, inport,
                                                     inport_dirn)) {
                branches.push_back(b);
            }
            break;
        }
        case TABLE_: {
            fatal_if(remote_subset.count() > 1,
                     "RoutingUnit: TABLE_ routing does not support "
                     "multicast RouteInfo (remote destination count = %d)",
                     remote_subset.count());
            branches.emplace_back(
                lookupRoutingTable(route.vnet, remote_subset), remote_subset);
            break;
        }
        // any custom algorithm
        case CUSTOM_: {
            fatal_if(remote_subset.count() > 1,
                     "RoutingUnit: CUSTOM_ routing does not support "
                     "multicast RouteInfo (remote destination count = %d)",
                     remote_subset.count());
            branches.emplace_back(
                outportComputeCustom(remote_route, inport, inport_dirn),
                remote_subset);
            break;
        }
        default: {
            fatal_if(remote_subset.count() > 1,
                     "RoutingUnit: default (table-based) routing does not "
                     "support multicast RouteInfo (remote destination "
                     "count = %d)",
                     remote_subset.count());
            branches.emplace_back(
                lookupRoutingTable(route.vnet, remote_subset), remote_subset);
            break;
        }
    }

    assert(!branches.empty());
    return branches;
}

// XY routing implemented using port directions, generalized to fan out a
// multi-destination RouteInfo. route.net_dest here is guaranteed to
// contain only REMOTE destinations (not local to this router -- those were
// already peeled off by outportCompute()). Buckets every destination by
// the direction it still needs to travel from this router -- East/West if
// its target column hasn't been reached yet, North/South once it has --
// and returns one branch per non-empty bucket (never empty; up to 4 when
// destinations disagree on every direction, which can happen for
// East+West right at/near the source before any single X direction has
// been committed to).
std::vector<RouteBranch>
RoutingUnit::outportComputeMulticastXY(RouteInfo route, int inport,
                                       PortDirection inport_dirn)
{
    [[maybe_unused]] int num_rows = m_router->get_net_ptr()->getNumRows();
    int num_cols = m_router->get_net_ptr()->getNumCols();
    assert(num_rows > 0 && num_cols > 0);

    int my_id = m_router->get_id();
    int my_x = my_id % num_cols;
    int my_y = my_id / num_cols;

    RubySystem *rs = m_router->get_net_ptr()->getRubySystem();
    NetDest bucket_E(rs), bucket_W(rs), bucket_N(rs), bucket_S(rs);

    for (NodeID n : route.net_dest.getAllDest()) {
        int dest_id = m_router->get_net_ptr()->get_router_id(n, route.vnet);
        int dest_x = dest_id % num_cols;
        int dest_y = dest_id / num_cols;

        NetDest single(rs);
        single.add(nodeIDToMachineID(rs, n));

        if (dest_x > my_x) {
            bucket_E.addNetDest(single);
        } else if (dest_x < my_x) {
            bucket_W.addNetDest(single);
        } else if (dest_y > my_y) {
            bucket_N.addNetDest(single);
        } else if (dest_y < my_y) {
            bucket_S.addNetDest(single);
        } else {
            // dest_x == my_x && dest_y == my_y: this destination is local
            // to this router and should already have been peeled off by
            // outportCompute() before this function was ever called.
            panic("outportComputeMulticastXY: destination NodeID %d "
                  "resolves to this router (%d) -- should have been routed "
                  "locally by outportCompute()",
                  n, my_id);
        }
    }

    std::vector<RouteBranch> branches;
    if (!bucket_E.isEmpty()) {
        assert(inport_dirn == "Local" || inport_dirn == "West");
        branches.emplace_back(m_outports_dirn2idx["East"], bucket_E);
    }
    if (!bucket_W.isEmpty()) {
        assert(inport_dirn == "Local" || inport_dirn == "East");
        branches.emplace_back(m_outports_dirn2idx["West"], bucket_W);
    }
    if (!bucket_N.isEmpty()) {
        assert(inport_dirn != "North");
        branches.emplace_back(m_outports_dirn2idx["North"], bucket_N);
    }
    if (!bucket_S.isEmpty()) {
        assert(inport_dirn != "South");
        branches.emplace_back(m_outports_dirn2idx["South"], bucket_S);
    }

    assert(!branches.empty());
    assert(branches.size() <= 4);
    return branches;
}

// Template for implementing custom routing algorithm
// using port directions. (Example adaptive)
int
RoutingUnit::outportComputeCustom(RouteInfo route, int inport,
                                  PortDirection inport_dirn)
{
    panic("%s placeholder executed", __FUNCTION__);
}

} // namespace betaar
} // namespace ruby
} // namespace gem5
