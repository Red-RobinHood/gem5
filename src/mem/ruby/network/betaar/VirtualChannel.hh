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

#ifndef __MEM_RUBY_NETWORK_BETAAR_0_VIRTUALCHANNEL_HH__
#define __MEM_RUBY_NETWORK_BETAAR_0_VIRTUALCHANNEL_HH__

#include <utility>
#include <vector>

#include "mem/ruby/network/betaar/CommonTypes.hh"
#include "mem/ruby/network/betaar/flitBuffer.hh"

namespace gem5
{

namespace ruby
{

namespace betaar
{

class VirtualChannel
{
  public:
    // One outgoing branch this VC's current packet has been routed onto:
    // pairs a RouteBranch (outport + destination subset, from RoutingUnit)
    // with the per-branch output VC (allocated lazily, -1 until then) and
    // whether this specific branch has already been sent for the flit
    // currently at the head of the input buffer. The branch topology
    // (outport/dest_subset/outvc) is computed once at HEAD/HEAD_TAIL and
    // reused unchanged by BODY/TAIL flits of the same packet; only
    // granted_this_flit is reset per flit (see reset_branch_grants()).
    struct Branch
    {
        Branch(int outport_, const NetDest &dest_subset_)
            : outport(outport_),
              outvc(-1),
              dest_subset(dest_subset_),
              granted_this_flit(false),
              branch_msg_ptr(nullptr)
        {}
        int outport;
        int outvc;
        NetDest dest_subset;
        bool granted_this_flit;
        // Message carried by the copies this branch forwards. Every
        // delivered copy of a multicast message needs its own Message
        // object, since MessageBuffer stamps per-delivery bookkeeping
        // (enqueue time, delay, counter) onto it at the destination. It is
        // cloned lazily when this branch first forwards a copy, and shared
        // by all flits of the same packet travelling down this branch (the
        // destination only ever enqueues the TAIL flit's message). The
        // branch that carries the original flit keeps the original
        // message, so the number of Message objects created equals the
        // number of destinations, no more.
        MsgPtr branch_msg_ptr;
    };

    VirtualChannel();
    ~VirtualChannel() = default;

    bool need_stage(flit_stage stage, Tick time);
    void set_idle(Tick curTime);
    void set_active(Tick curTime);

    // Replaces the old scalar set_outport/set_outvc/get_outport/get_outvc:
    // a VC's packet may now be routed onto several simultaneous branches.
    inline void
    set_branches(const std::vector<RouteBranch> &route_branches)
    {
        m_branches.clear();
        m_branches.reserve(route_branches.size());
        for (auto &rb : route_branches) {
            m_branches.emplace_back(rb.outport, rb.dest_subset);
        }
        m_branches_valid = true;
    }
    inline std::vector<Branch> &
    get_branches()
    {
        return m_branches;
    }
    inline bool
    branches_valid() const
    {
        return m_branches_valid;
    }
    // Re-arms every branch as "not yet sent" for the next flit (BODY/TAIL)
    // of the same packet; the branch topology itself (outport/outvc/
    // dest_subset) is left untouched since it belongs to the whole packet.
    inline void
    reset_branch_grants()
    {
        for (auto &b : m_branches) {
            b.granted_this_flit = false;
        }
    }
    inline bool
    all_branches_sent() const
    {
        for (auto &b : m_branches) {
            if (!b.granted_this_flit) {
                return false;
            }
        }
        return true;
    }

    inline Tick
    get_enqueue_time()
    {
        return m_enqueue_time;
    }
    inline void
    set_enqueue_time(Tick time)
    {
        m_enqueue_time = time;
    }
    inline VC_state_type
    get_state()
    {
        return m_vc_state.first;
    }

    inline bool
    isReady(Tick curTime)
    {
        return inputBuffer.isReady(curTime);
    }

    inline void
    insertFlit(flit *t_flit)
    {
        inputBuffer.insert(t_flit);
    }

    inline void
    set_state(VC_state_type m_state, Tick curTime)
    {
        m_vc_state.first = m_state;
        m_vc_state.second = curTime;
    }

    inline flit *
    peekTopFlit()
    {
        return inputBuffer.peekTopFlit();
    }

    inline flit *
    getTopFlit()
    {
        return inputBuffer.getTopFlit();
    }

    bool functionalRead(Packet *pkt, WriteMask &mask);
    uint32_t functionalWrite(Packet *pkt);

  private:
    flitBuffer inputBuffer;
    std::pair<VC_state_type, Tick> m_vc_state;
    Tick m_enqueue_time;
    std::vector<Branch> m_branches;
    bool m_branches_valid;
};

} // namespace betaar
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_BETAAR_0_VIRTUALCHANNEL_HH__
