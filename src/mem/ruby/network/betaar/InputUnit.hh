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

#ifndef __MEM_RUBY_NETWORK_BETAAR_0_INPUTUNIT_HH__
#define __MEM_RUBY_NETWORK_BETAAR_0_INPUTUNIT_HH__

#include <iostream>
#include <vector>

#include "mem/ruby/common/Consumer.hh"
#include "mem/ruby/network/betaar/BetaarCreditLink.hh"
#include "mem/ruby/network/betaar/BetaarNetworkLink.hh"
#include "mem/ruby/network/betaar/CommonTypes.hh"
#include "mem/ruby/network/betaar/Router.hh"
#include "mem/ruby/network/betaar/VirtualChannel.hh"
#include "mem/ruby/network/betaar/flitBuffer.hh"

namespace gem5
{

namespace ruby
{

namespace betaar
{

class InputUnit : public Consumer
{
  public:
    InputUnit(int id, PortDirection direction, Router *router);
    ~InputUnit() = default;

    void wakeup();
    void print(std::ostream &out) const {};

    inline PortDirection
    get_direction()
    {
        return m_direction;
    }

    inline void
    set_vc_idle(int vc, Tick curTime)
    {
        virtualChannels[vc].set_idle(curTime);
    }

    inline void
    set_vc_active(int vc, Tick curTime)
    {
        virtualChannels[vc].set_active(curTime);
    }

    // Replaces the old scalar grant_outport/grant_outvc/get_outport/
    // get_outvc: a VC's packet may be routed onto several simultaneous
    // branches (see VirtualChannel::Branch), so these are branch-list
    // aware. Computed once at HEAD/HEAD_TAIL by InputUnit::wakeup(),
    // reused by BODY/TAIL flits of the same packet.
    inline void
    set_branches(int vc, const std::vector<RouteBranch> &branches)
    {
        virtualChannels[vc].set_branches(branches);
    }

    inline std::vector<VirtualChannel::Branch> &
    get_branches(int vc)
    {
        return virtualChannels[vc].get_branches();
    }

    inline void
    reset_branch_grants(int vc)
    {
        virtualChannels[vc].reset_branch_grants();
    }

    inline bool
    all_branches_sent(int vc)
    {
        return virtualChannels[vc].all_branches_sent();
    }

    inline Tick
    get_enqueue_time(int invc)
    {
        return virtualChannels[invc].get_enqueue_time();
    }

    void increment_credit(int in_vc, bool free_signal, Tick curTime);

    inline flit *
    peekTopFlit(int vc)
    {
        return virtualChannels[vc].peekTopFlit();
    }

    inline flit *
    getTopFlit(int vc)
    {
        return virtualChannels[vc].getTopFlit();
    }

    inline bool
    need_stage(int vc, flit_stage stage, Tick time)
    {
        return virtualChannels[vc].need_stage(stage, time);
    }

    inline bool
    isReady(int invc, Tick curTime)
    {
        return virtualChannels[invc].isReady(curTime);
    }

    flitBuffer *
    getCreditQueue()
    {
        return &creditQueue;
    }

    inline void
    set_in_link(BetaarNetworkLink *link)
    {
        m_in_link = link;
    }

    inline int
    get_inlink_id()
    {
        return m_in_link->get_id();
    }

    inline void
    set_credit_link(BetaarCreditLink *credit_link)
    {
        m_credit_link = credit_link;
    }

    double
    get_buf_read_activity(unsigned int vnet) const
    {
        return m_num_buffer_reads[vnet];
    }
    double
    get_buf_write_activity(unsigned int vnet) const
    {
        return m_num_buffer_writes[vnet];
    }

    bool functionalRead(Packet *pkt, WriteMask &mask);
    uint32_t functionalWrite(Packet *pkt);

    void resetStats();

  private:
    Router *m_router;
    int m_id;
    PortDirection m_direction;
    int m_vc_per_vnet;
    BetaarNetworkLink *m_in_link;
    BetaarCreditLink *m_credit_link;
    flitBuffer creditQueue;

    // Input Virtual channels
    std::vector<VirtualChannel> virtualChannels;

    // Statistical variables
    std::vector<double> m_num_buffer_writes;
    std::vector<double> m_num_buffer_reads;
};

} // namespace betaar
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_BETAAR_0_INPUTUNIT_HH__
