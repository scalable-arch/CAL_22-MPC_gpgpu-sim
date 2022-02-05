#include "gpu-sim.h"
#include "oneway_link.h"

extern gpgpu_sim* g_the_gpu;

// -------------------------------------------------------------------------
// Base oneway link interface
// -------------------------------------------------------------------------
oneway_link::oneway_link(const char* nm, unsigned latency, unsigned src_cnt, unsigned dst_cnt)
  : m_src_cnt(src_cnt), m_dst_cnt(dst_cnt)
{
  strcpy(m_name, nm);
  queue = new link_delay_queue(nm, 4000, latency);   // Queue of FLITs
  m_ready_list = new std::queue<mem_fetch *>[m_src_cnt];
  m_complete_list = new std::queue<mem_fetch *>[m_dst_cnt];

  m_cur_src_id = 0;
  m_cur_flit_cnt = 0;
  m_total_flit_cnt = 0ull;
  m_transfer_flit_cnt = 0ull;
  m_transfer_single_flit_cnt = 0ull;
  m_transfer_multi_flit_cnt = 0ull;
}
oneway_link::~oneway_link()
{
  delete queue;
  delete [] m_ready_list;
  delete [] m_complete_list;
}

bool oneway_link::full(unsigned src_id)
{
  return (m_ready_list[src_id].size()>=4);
}
void oneway_link::push(unsigned src_id, mem_fetch *mf)
{
  assert(!full(src_id));
  m_ready_list[src_id].push(mf);
}
unsigned oneway_link::get_dst_id(mem_fetch *mf)
{
  return mf->get_sub_partition_id();
}
bool oneway_link::empty(unsigned dst_id)
{
  return (m_complete_list[dst_id].size()==0);
}
mem_fetch* oneway_link::top(unsigned dst_id)
{
  if (!empty(dst_id)) {
    mem_fetch *mf = m_complete_list[dst_id].front();
    return mf;
  }
  return NULL;
}
void oneway_link::pop(unsigned dst_id)
{
  assert (!empty(dst_id));
  mem_fetch *mf = m_complete_list[dst_id].front();
  assert(mf!=NULL);
  m_complete_list[dst_id].pop();
}
void oneway_link::step_link_pop(unsigned n_flit)
{
  // pop old entries
  for (unsigned i=0; i<n_flit; i++) {
    mem_fetch *mf = queue->pop();
    if (mf!=NULL) {
      //printf("QQ:pop  %p %8u\n", mf, mf->get_request_uid());
      unsigned dst_id = get_dst_id(mf);
      //if (m_complete_list[dst_id].size()>=1000) {
      //    assert(0);
      //}
      m_complete_list[dst_id].push(mf);
    }
  }
}
void oneway_link::step_link_push(unsigned n_flit)
{
  // push new entries
  unsigned n_sent_flit_cnt = 0;
  for (unsigned i=0; (i<m_src_cnt) && (n_sent_flit_cnt<n_flit); i++) {
    unsigned src_id = (m_cur_src_id+i) % m_src_cnt;
    if (m_ready_list[src_id].size()>0) {
      mem_fetch *mf = m_ready_list[src_id].front();
      if (m_cur_flit_cnt==0) {    // first FLIT of a request
        if ((mf->get_type()==READ_REQUEST)||(mf->get_type()==WRITE_ACK)) {
          m_packet_bit_size = HT_OVERHEAD;
        } else if ((mf->get_type()==WRITE_REQUEST)||(mf->get_type()==READ_REPLY)) {
          m_packet_bit_size = HT_OVERHEAD + mf->get_data_size()*8;
        } else {
          assert(0);
        }
      }

      for (unsigned i=m_cur_flit_cnt*FLIT_SIZE; (i<m_packet_bit_size) && (n_sent_flit_cnt<n_flit); i+=FLIT_SIZE) {
        bool is_first = (i==0);
        bool is_last = (i>=(m_packet_bit_size-FLIT_SIZE));
        queue->push(is_first, is_last, mf);
        n_sent_flit_cnt++;

        if (is_last) {
          m_ready_list[src_id].pop();
          m_cur_src_id = (src_id+1) % m_src_cnt;
          m_cur_flit_cnt = 0;
        } else {
          m_cur_flit_cnt++;
        }
        if (m_packet_bit_size==HT_OVERHEAD) {
          m_transfer_single_flit_cnt++;
        } else {
          m_transfer_multi_flit_cnt++;
        }
        m_transfer_flit_cnt++;
      }
    }
  }

  for (; n_sent_flit_cnt < n_flit; n_sent_flit_cnt++) {
    queue->push(false, false, NULL);
  }
}
void oneway_link::step(unsigned n_flit)
{
  m_total_flit_cnt += n_flit;

  step_link_pop(n_flit);

  step_link_push(n_flit);
}
void oneway_link::print() const
{
  queue->print();
}
void oneway_link::print_stat() const
{
  printf("%s TOT %f (%lld/%lld)\n", m_name, m_transfer_flit_cnt*1./m_total_flit_cnt, m_transfer_flit_cnt, m_total_flit_cnt);
  printf("%s SIN %f (%lld/%lld)\n", m_name, m_transfer_single_flit_cnt*1./m_total_flit_cnt, m_transfer_single_flit_cnt, m_total_flit_cnt);
  printf("%s MUL %f (%lld/%lld)\n", m_name, m_transfer_multi_flit_cnt*1./m_total_flit_cnt, m_transfer_multi_flit_cnt, m_total_flit_cnt);
}

// -------------------------------------------------------------------------
// Compressed oneway link interface
// -------------------------------------------------------------------------
compressed_oneway_link::compressed_oneway_link(const char* nm, unsigned latency, unsigned src_cnt, unsigned dst_cnt)
  : oneway_link(nm, latency, src_cnt, dst_cnt)
{
  m_ready_long_list = new std::queue<mem_fetch *>[src_cnt];
  m_ready_short_list = new std::queue<mem_fetch *>[src_cnt];
  m_ready_decompressed = new compressed_link_delay_queue(nm, 4000, 10);    // 10 cycle latency

  is_current_long = false;
  m_cur_comp_id = 0;
  m_leftover = 0;
}

void compressed_oneway_link::push(unsigned mem_id, mem_fetch *mf)
{
  assert(!full(mem_id));
  if ((mf->get_type()==WRITE_REQUEST)||(mf->get_type()==READ_REPLY)) {
    m_ready_long_list[mem_id].push(mf);
  } else {
    m_ready_short_list[mem_id].push(mf);
  }
}

bool compressed_oneway_link::push(mem_fetch *mf,
    unsigned packet_bit_size, unsigned& n_sent_flit_cnt, unsigned n_flit, bool update)
{
  for (unsigned i=m_cur_flit_cnt*FLIT_SIZE; i<packet_bit_size; i+=FLIT_SIZE) {
    if (n_sent_flit_cnt==n_flit) {
      return false;
    }
    bool is_first = (i==0);
    bool is_last = (i>=(packet_bit_size-FLIT_SIZE));
    queue->push(is_first, is_last, mf);
    n_sent_flit_cnt++;
    if (update) {
      if (is_last) {
        m_cur_flit_cnt = 0;
      } else {
        m_cur_flit_cnt++;
      }
    }
    if (packet_bit_size==HT_OVERHEAD) {
      m_transfer_single_flit_cnt++;
    } else {
      m_transfer_multi_flit_cnt++;
    }
    m_transfer_flit_cnt++;
  }
  return true;
}


compressed_dn_link::compressed_dn_link(const char* nm, unsigned latency, unsigned src_cnt, unsigned dst_cnt)
  : compressed_oneway_link(nm, latency, src_cnt, dst_cnt)
{
  m_ready_compressed = new compressed_link_delay_queue(nm, 4000, 1);
}

void compressed_dn_link::step_link_push(unsigned n_flit)
{
  unsigned n_sent_flit_cnt = 0;

  // Priorities
  // 1. Write request if there is an on-going write request
  // 2. Read request if no left-over space
  // 3. Write request if no read request

  // 1. Write request if there is an on-going write request
  while ((n_sent_flit_cnt<n_flit) && (m_cur_flit_cnt!=0)) {
    auto it = m_ready_compressed->top();
    if (it.first!=NULL) {
      assert(it.first->get_type()==WRITE_REQUEST);
      if (m_cur_flit_cnt==0) {    // this is the first FLIT of a packet
        unsigned packet_bit_size = HT_OVERHEAD+it.second-m_leftover;
        m_packet_bit_size = ((packet_bit_size+FLIT_SIZE-1)/FLIT_SIZE)*FLIT_SIZE;
        m_leftover = ((packet_bit_size%FLIT_SIZE)==0) ? 0 : FLIT_SIZE - (packet_bit_size%FLIT_SIZE);
      }

      bool is_complete = push(it.first, m_packet_bit_size, n_sent_flit_cnt, n_flit);
      if (is_complete) {
        m_ready_compressed->pop();
      }
    } else {
      break;
    }
  }

  // 2. Read request if no left-over space
  for (unsigned i=0; (i<m_src_cnt) && (n_sent_flit_cnt<n_flit); i++) {
    unsigned src_id = (m_cur_src_id+i) % m_src_cnt;
    if (m_ready_short_list[src_id].size()>0) {
      mem_fetch *mf = m_ready_short_list[src_id].front();
      assert(mf->get_type()==READ_REQUEST);
      m_packet_bit_size = HT_OVERHEAD;
      bool is_complete = push(mf, m_packet_bit_size, n_sent_flit_cnt, n_flit, false);
      assert(is_complete);
      m_ready_short_list[src_id].pop();
    }
  }

  // 3. Write request if no read request
  while (n_sent_flit_cnt<n_flit) {
    auto it = m_ready_compressed->top();
    if (it.first!=NULL) {
      assert(it.first->get_type()==WRITE_REQUEST);
      if (m_cur_flit_cnt==0) {
        unsigned packet_bit_size = HT_OVERHEAD+it.second-m_leftover;
        m_packet_bit_size = ((packet_bit_size+FLIT_SIZE-1)/FLIT_SIZE)*FLIT_SIZE;
        m_leftover = ((packet_bit_size%FLIT_SIZE)==0) ? 0 : FLIT_SIZE - (packet_bit_size%FLIT_SIZE);
      }

      bool is_complete = push(it.first, m_packet_bit_size, n_sent_flit_cnt, n_flit);
      if (is_complete) {
        m_ready_compressed->pop();
      }
    } else {
      break;
    }
  }

  for (; n_sent_flit_cnt < n_flit; n_sent_flit_cnt++) {
    queue->push(false, false, NULL);
    m_leftover = 0;     // left-over space is discarded
  }
  assert(n_sent_flit_cnt==n_flit);

  // Compress write requests
  for (unsigned i=0; i<m_src_cnt; i++) {
    unsigned src_id = (m_cur_comp_id+i) % m_src_cnt;
    assert(m_ready_long_list[src_id].size()<=1);
    if (m_ready_long_list[src_id].size()>0) {
      unsigned char buffer[128];
      unsigned comp_bit_size;

      // compress
      mem_fetch *mf = m_ready_long_list[src_id].front();
      if (mf->get_data_size() == 128) {   // for now, compress only 128B blocks only
        static int cnt = 0;
        g_the_gpu->get_global_memory()->read(mf->get_addr(), mf->get_data_size(), buffer);
        comp_bit_size = g_comp->compress(mf->get_vstream_id(), buffer, mf->get_addr(), mf->get_data_size());
        cnt += comp_bit_size;
        if (cnt > 1024) {   // spread over two packets
          cnt -= 1024;
        } else {            // compacted packet --> TAG overhead
          comp_bit_size += 11;
        }
      } else {
        comp_bit_size = mf->get_data_size() * 8;
      }

      m_ready_compressed->push(mf, comp_bit_size);
      m_ready_long_list[src_id].pop();
    }
  }
}

void compressed_dn_link::step_link_pop(unsigned n_flit)
{
  // pop old entries
  for (unsigned i=0; i<n_flit; i++) {
    mem_fetch *mf = queue->pop();
    if (mf!=NULL) {
      //printf("QQ:pop  %p %8u\n", mf, mf->get_request_uid());
      if (mf->get_type()==READ_REQUEST) { // no decompression
        unsigned dst_id = get_dst_id(mf);
        if (m_complete_list[dst_id].size()>=1000) {
          assert(0);
        }
        m_complete_list[dst_id].push(mf);
      } else {
        assert(mf->get_type()==WRITE_REQUEST);  // decompression
        unsigned dst_id = get_dst_id(mf);
        if (m_complete_list[dst_id].size()>=1000) {
          assert(0);
        }
        m_complete_list[dst_id].push(mf);
      }
    }
  }
}


compressed_up_link::compressed_up_link(const char* nm, unsigned latency, unsigned src_cnt, unsigned dst_cnt)
  : compressed_oneway_link(nm, latency, src_cnt, dst_cnt)
{
  m_ready_compressed = new compressed_link_delay_queue(nm, 4000, 1);
}

void compressed_up_link::step_link_push(unsigned n_flit)
{
  unsigned n_sent_flit_cnt = 0;

  // Priorities
  // 1. Read data
  // 2. Write acknowledge if no read data

  // 1. Read data
  while (n_sent_flit_cnt<n_flit) {
    pair<mem_fetch *, unsigned> it = m_ready_compressed->top();
    if (it.first!=NULL) {
      //printf("TOP1 @%08d %p %d\n", gpu_sim_cycle, it.first, it.first->get_request_uid());
      //it.first->print(stdout, false);
      assert(it.first->get_type()==READ_REPLY);
      if (m_cur_flit_cnt==0) {
        unsigned packet_bit_size = HT_OVERHEAD+it.second-m_leftover;
        m_packet_bit_size = ((packet_bit_size+FLIT_SIZE-1)/FLIT_SIZE)*FLIT_SIZE;
        m_leftover = ((packet_bit_size%FLIT_SIZE)==0) ? 0 : FLIT_SIZE - (packet_bit_size%FLIT_SIZE);
      }

      bool is_complete = push(it.first, m_packet_bit_size, n_sent_flit_cnt, n_flit);
      if (is_complete) {
        //printf("POP1 @%08d %p %d\n", gpu_sim_cycle, it.first, it.first->get_request_uid());
        m_ready_compressed->pop();
      }
    } else {
      break;
    }
  }

  // 2. Write acknowledge if no read data
  for (unsigned i=0; (i<m_src_cnt) && (n_sent_flit_cnt<n_flit); i++) {
    unsigned src_id = (m_cur_src_id+i) % m_src_cnt;
    if (m_ready_short_list[src_id].size()>0) {
      mem_fetch *mf = m_ready_short_list[src_id].front();
      assert(mf->get_type()==WRITE_ACK);
      m_packet_bit_size = HT_OVERHEAD;
      bool is_complete = push(mf, m_packet_bit_size, n_sent_flit_cnt, n_flit);
      assert(is_complete);
      m_ready_short_list[src_id].pop();
    }
  }

  for (; n_sent_flit_cnt < n_flit; n_sent_flit_cnt++) {
    queue->push(false, false, NULL);
    m_leftover = 0;
  }
  assert(n_sent_flit_cnt==n_flit);

  // Compress read data
  for (unsigned i=0; i<m_src_cnt; i++) {
    unsigned src_id = (m_cur_comp_id+i) % m_src_cnt;
    assert(m_ready_long_list[src_id].size()<=1);
    if (m_ready_long_list[src_id].size()>0) {
      unsigned char buffer[128];
      unsigned comp_bit_size;

      // compress
      mem_fetch *mf = m_ready_long_list[src_id].front();
      if (mf->get_data_size() == 128) {
        static int cnt = 0;

        g_the_gpu->get_global_memory()->read(mf->get_addr(), mf->get_data_size(), buffer);
        comp_bit_size = g_comp->compress(mf->get_vstream_id(), buffer, mf->get_addr(), mf->get_data_size());

        cnt += comp_bit_size;
        if (cnt > 1024) {   // spread over two packets
          cnt -= 1024;
        } else {            // compacted packet --> TAG overhead
          comp_bit_size += 11;
        }
      } else {
        comp_bit_size = mf->get_data_size() * 8;
      }

      //printf("PUSH @%08d %p %d\n", gpu_sim_cycle, mf, mf->get_request_uid());
      m_ready_compressed->push(mf, comp_bit_size);
      m_ready_long_list[src_id].pop();
    }
  }
}
