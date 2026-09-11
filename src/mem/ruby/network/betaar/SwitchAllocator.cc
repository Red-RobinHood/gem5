/*
 * Copyright (c) 2020 Inria
 * Copyright (c) 2016 Georgia Institute of Technology
 * Copyright (c) 2008 Princeton University
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

#include "mem/ruby/network/betaar/SwitchAllocator.hh"

#include <algorithm>

#include "debug/RubyNetwork.hh"
#include "mem/ruby/network/betaar/BetaarNetwork.hh"
#include "mem/ruby/network/betaar/InputUnit.hh"
#include "mem/ruby/network/betaar/OutputUnit.hh"
#include "mem/ruby/network/betaar/Router.hh"

namespace gem5
{

namespace ruby
{

namespace betaar
{

SwitchAllocator::SwitchAllocator(Router *router) : Consumer(router)
{
    m_router = router;
    m_num_vcs = m_router->get_num_vcs();
    m_vc_per_vnet = m_router->get_vc_per_vnet();

    m_input_arbiter_activity = 0;
    m_output_arbiter_activity = 0;
}

void
SwitchAllocator::init()
{
    m_num_inports = m_router->get_num_inports();
    m_num_outports = m_router->get_num_outports();
    m_round_robin_inport.resize(m_num_outports);
    m_round_robin_invc.resize(m_num_inports);
    m_port_requests.resize(m_num_inports);
    m_vc_winners.resize(m_num_inports);

    for (int i = 0; i < m_num_inports; i++) {
        m_round_robin_invc[i] = 0;
        m_port_requests[i].clear();
        m_vc_winners[i] = -1;
    }

    for (int i = 0; i < m_num_outports; i++) {
        m_round_robin_inport[i] = 0;
    }
}

/*
 * The wakeup function of the SwitchAllocator performs a 2-stage
 * seperable switch allocation. At the end of the 2nd stage, a free
 * output VC is assigned to the winning flits of each output port.
 * There is no separate VCAllocator stage like the one in betaar1.0.
 * At the end of this function, the router is rescheduled to wakeup
 * next cycle for peforming SA for any flits ready next cycle.
 */

void
SwitchAllocator::wakeup()
{
    arbitrate_inports();  // First stage of allocation
    arbitrate_outports(); // Second stage of allocation

    clear_request_vector();
    check_for_wakeup();
}

/*
 * SA-I (or SA-i) loops through all input VCs at every input port,
 * and selects one in a round robin manner.
 *    - For HEAD/HEAD_TAIL flits only selects an input VC whose output port
 *     has at least one free output VC.
 *    - For BODY/TAIL flits, only selects an input VC that has credits
 *      in its output VC.
 * Places a request for every output port this input VC still needs to send
 * the flit at the head of its buffer to. A unicast flit has exactly one
 * such branch; a multicast flit that forks here has one per direction its
 * remaining destinations require, minus any branch already sent in an
 * earlier cycle (independent per-branch retirement).
 */

void
SwitchAllocator::arbitrate_inports()
{
    // Select a VC from each input in a round robin manner
    // Independent arbiter at each input port
    for (int inport = 0; inport < m_num_inports; inport++) {
        int invc = m_round_robin_invc[inport];

        for (int invc_iter = 0; invc_iter < m_num_vcs; invc_iter++) {
            auto input_unit = m_router->getInputUnit(inport);

            if (input_unit->need_stage(invc, SA_, curTick())) {
                // This flit is in SA stage

                std::vector<int> requested_outports;
                for (auto &br : input_unit->get_branches(invc)) {
                    if (br.granted_this_flit) {
                        // this branch already left in an earlier cycle
                        continue;
                    }

                    // check if the flit in this InputVC is allowed to be
                    // sent out this branch's outport.
                    // send_allowed conditions described in that function.
                    if (send_allowed(inport, invc, br.outport, br.outvc)) {
                        requested_outports.push_back(br.outport);
                    }
                }

                if (!requested_outports.empty()) {
                    m_input_arbiter_activity++;
                    m_port_requests[inport] = requested_outports;
                    m_vc_winners[inport] = invc;

                    break; // got one vc winner for this port
                }
            }

            invc++;
            if (invc >= m_num_vcs) {
                invc = 0;
            }
        }
    }
}

/*
 * SA-II (or SA-o) loops through all output ports,
 * and selects one input VC (that placed a request during SA-I)
 * as the winner for this output port in a round robin manner.
 *      - For HEAD/HEAD_TAIL flits, performs simplified outvc allocation.
 *        (i.e., select a free VC from the output port).
 *      - For BODY/TAIL flits, decrement a credit in the output vc.
 * The winning flit is read out from the input VC and sent to the
 * CrossbarSwitch.
 * An increment_credit signal is sent from the InputUnit
 * to the upstream router. For HEAD_TAIL/TAIL flits, is_free_signal in the
 * credit is set to true.
 */

void
SwitchAllocator::arbitrate_outports()
{
    // Now there are a set of input vc requests for output vcs.
    // Again do round robin arbitration on these requests
    // Independent arbiter at each output port
    for (int outport = 0; outport < m_num_outports; outport++) {
        int inport = m_round_robin_inport[outport];

        for (int inport_iter = 0; inport_iter < m_num_inports; inport_iter++) {

            // inport has a request this cycle for outport
            auto req = std::find(m_port_requests[inport].begin(),
                                 m_port_requests[inport].end(), outport);
            if (req != m_port_requests[inport].end()) {
                auto output_unit = m_router->getOutputUnit(outport);
                auto input_unit = m_router->getInputUnit(inport);

                // grant this outport to this inport
                int invc = m_vc_winners[inport];

                auto &branches = input_unit->get_branches(invc);
                auto br_it =
                    std::find_if(branches.begin(), branches.end(),
                                 [outport](const VirtualChannel::Branch &b) {
                                     return b.outport == outport;
                                 });
                assert(br_it != branches.end());
                int br_idx = std::distance(branches.begin(), br_it);
                auto &br = *br_it;
                assert(!br.granted_this_flit);

                if (br.outvc == -1) {
                    // VC Allocation - select any free VC from outport.
                    // Each branch of a multicast packet gets its own
                    // output VC at its own outport.
                    br.outvc = vc_allocate(outport, inport, invc);
                }
                int outvc = br.outvc;

                // Is this the last branch of this flit still waiting to be
                // sent? Only then does the flit actually leave the input
                // buffer; every earlier branch forwards an independent
                // copy while the original stays put for the branches that
                // have not been granted yet.
                bool is_last_branch = true;
                for (auto &b : branches) {
                    if (!b.granted_this_flit && b.outport != outport) {
                        is_last_branch = false;
                        break;
                    }
                }

                // Peek (don't pop): the shared flit is only removed from
                // the Input VC once every branch has been sent.
                flit *src_flit = input_unit->peekTopFlit(invc);

                // Forward an independent copy down every branch but the
                // last one, leaving the original flit in the buffer for
                // the branches that have not been granted yet.
                flit *t_flit = is_last_branch ? src_flit : new flit(*src_flit);

                // Every delivered copy of a multicast message needs its
                // own Message object, because the destination
                // MessageBuffer stamps per-delivery bookkeeping onto
                // whatever Message it is handed. The first branch carries
                // the packet's original Message and each other branch
                // carries a clone made once for the whole packet -- keyed
                // on the branch rather than on which branch happens to win
                // arbitration last, so every flit of a packet travelling
                // down a given branch carries the same Message.
                if (br_idx != 0) {
                    if (br.branch_msg_ptr == nullptr) {
                        br.branch_msg_ptr = src_flit->get_msg_ptr()->clone();
                    }
                    t_flit->set_msg_ptr(br.branch_msg_ptr);
                }

                DPRINTF(RubyNetwork,
                        "SwitchAllocator at Router %d "
                        "granted outvc %d at outport %d "
                        "to invc %d at inport %d to flit %s at "
                        "cycle: %lld\n",
                        m_router->get_id(), outvc,
                        m_router->getPortDirectionName(
                            output_unit->get_direction()),
                        invc,
                        m_router->getPortDirectionName(
                            input_unit->get_direction()),
                        *t_flit, m_router->curCycle());

                // Narrow this copy's destination set to the subset that
                // travels down this branch. Downstream routers re-bucket
                // whatever is left, so the multicast tree keeps pruning
                // itself hop by hop.
                RouteInfo branch_route = t_flit->get_route();
                branch_route.net_dest = br.dest_subset;
                t_flit->set_route(branch_route);

                // Update outport field in the flit since this is
                // used by CrossbarSwitch code to send it out of
                // correct outport.
                // Note: post route compute in InputUnit,
                // outport is updated in VC, but not in flit
                t_flit->set_outport(outport);

                // set outvc (i.e., invc for next hop) in flit
                // (This was updated in the branch by vc_allocate,
                // but not in flit)
                t_flit->set_vc(outvc);

                // decrement credit in outvc
                output_unit->decrement_credit(outvc);

                // flit ready for Switch Traversal
                t_flit->advance_stage(ST_, curTick());
                m_router->grant_switch(inport, t_flit);
                m_output_arbiter_activity++;

                br.granted_this_flit = true;

                if (is_last_branch) {
                    // Every branch has now been sent: the flit really
                    // leaves this Input VC, and exactly one credit goes
                    // back upstream for it (the input buffer only ever
                    // held one flit, however many branches it fanned into)
                    input_unit->getTopFlit(invc);

                    if ((t_flit->get_type() == TAIL_) ||
                        t_flit->get_type() == HEAD_TAIL_) {

                        // This Input VC should now be empty
                        assert(!(input_unit->isReady(invc, curTick())));

                        // Free this VC
                        input_unit->set_vc_idle(invc, curTick());

                        // Send a credit back
                        // along with the information that this VC is now
                        // idle
                        input_unit->increment_credit(invc, true, curTick());
                    } else {
                        // Re-arm every branch for the next flit of this
                        // packet (same branch topology and output VCs,
                        // none of them sent yet for that flit).
                        input_unit->reset_branch_grants(invc);

                        // Send a credit back
                        // but do not indicate that the VC is idle
                        input_unit->increment_credit(invc, false, curTick());
                    }
                }

                // remove this request
                m_port_requests[inport].erase(req);

                // Update Round Robin pointer
                m_round_robin_inport[outport] = inport + 1;
                if (m_round_robin_inport[outport] >= m_num_inports) {
                    m_round_robin_inport[outport] = 0;
                }

                // Update Round Robin pointer to the next VC
                // We do it here to keep it fair.
                // Only the VC which got switch traversal is updated, and
                // only once it is completely done with this flit -- a
                // partially forwarded multicast flit keeps its priority so
                // its remaining branches can drain.
                if (is_last_branch) {
                    m_round_robin_invc[inport] = invc + 1;
                    if (m_round_robin_invc[inport] >= m_num_vcs) {
                        m_round_robin_invc[inport] = 0;
                    }
                }

                break; // got a input winner for this outport
            }

            inport++;
            if (inport >= m_num_inports) {
                inport = 0;
            }
        }
    }
}

/*
 * A flit can be sent only if
 * (1) there is at least one free output VC at the
 *     output port (for HEAD/HEAD_TAIL),
 *  or
 * (2) if there is at least one credit (i.e., buffer slot)
 *     within the VC for BODY/TAIL flits of multi-flit packets.
 * and
 * (3) pt-to-pt ordering is not violated in ordered vnets, i.e.,
 *     there should be no other flit in this input port
 *     within an ordered vnet
 *     that arrived before this flit and is requesting the same output port.
 */

bool
SwitchAllocator::send_allowed(int inport, int invc, int outport, int outvc)
{
    // Check if outvc needed
    // Check if credit needed (for multi-flit packet)
    // Check if ordering violated (in ordered vnet)

    int vnet = get_vnet(invc);
    bool has_outvc = (outvc != -1);
    bool has_credit = false;

    auto output_unit = m_router->getOutputUnit(outport);
    if (!has_outvc) {

        // needs outvc
        // this is only true for HEAD and HEAD_TAIL flits.

        if (output_unit->has_free_vc(vnet)) {

            has_outvc = true;

            // each VC has at least one buffer,
            // so no need for additional credit check
            has_credit = true;
        }
    } else {
        has_credit = output_unit->has_credit(outvc);
    }

    // cannot send if no outvc or no credit.
    if (!has_outvc || !has_credit) {
        return false;
    }

    // protocol ordering check
    if ((m_router->get_net_ptr())->isVNetOrdered(vnet)) {
        auto input_unit = m_router->getInputUnit(inport);

        // enqueue time of this flit
        Tick t_enqueue_time = input_unit->get_enqueue_time(invc);

        // check if any other flit is ready for SA and for same output port
        // and was enqueued before this flit
        int vc_base = vnet * m_vc_per_vnet;
        for (int vc_offset = 0; vc_offset < m_vc_per_vnet; vc_offset++) {
            int temp_vc = vc_base + vc_offset;
            if (!input_unit->need_stage(temp_vc, SA_, curTick()) ||
                (input_unit->get_enqueue_time(temp_vc) >= t_enqueue_time)) {
                continue;
            }

            // An older flit at this input port blocks us only if it is
            // itself still waiting to be sent out this same outport.
            for (auto &br : input_unit->get_branches(temp_vc)) {
                if (!br.granted_this_flit && br.outport == outport) {
                    return false;
                }
            }
        }
    }

    return true;
}

// Assign a free VC to the winner of the output port.
// The caller stores it in the branch it was allocated for, since a
// multicast packet holds one output VC per branch.
int
SwitchAllocator::vc_allocate(int outport, int inport, int invc)
{
    // Select a free VC from the output port
    int outvc =
        m_router->getOutputUnit(outport)->select_free_vc(get_vnet(invc));

    // has to get a valid VC since it checked before performing SA
    assert(outvc != -1);
    return outvc;
}

// Wakeup the router next cycle to perform SA again
// if there are flits ready.
void
SwitchAllocator::check_for_wakeup()
{
    Tick nextCycle = m_router->clockEdge(Cycles(1));

    if (m_router->alreadyScheduled(nextCycle)) {
        return;
    }

    for (int i = 0; i < m_num_inports; i++) {
        for (int j = 0; j < m_num_vcs; j++) {
            if (m_router->getInputUnit(i)->need_stage(j, SA_, nextCycle)) {
                m_router->schedule_wakeup(Cycles(1));
                return;
            }
        }
    }
}

int
SwitchAllocator::get_vnet(int invc)
{
    int vnet = invc / m_vc_per_vnet;
    assert(vnet < m_router->get_num_vnets());
    return vnet;
}

// Clear the request vector within the allocator at end of SA-II.
// Was populated by SA-I.
void
SwitchAllocator::clear_request_vector()
{
    for (auto &requests : m_port_requests) {
        requests.clear();
    }
}

void
SwitchAllocator::resetStats()
{
    m_input_arbiter_activity = 0;
    m_output_arbiter_activity = 0;
}

} // namespace betaar
} // namespace ruby
} // namespace gem5
