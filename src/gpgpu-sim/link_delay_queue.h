#ifndef LINK_DELAY_QUEUE_H
#define LINK_DELAY_QUEUE_H

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <queue>
#include <set>
#include "../abstract_hardware_model.h"
#include "../cuda-sim/memory.h"
#include "gpu-sim.h"
#include "comp.h"

class link_delay_queue {
public:
  link_delay_queue(const char* nm, unsigned int size, unsigned int latency);
  ~link_delay_queue();

  // methods
  void push(bool is_head, bool is_tail, mem_fetch* mf);
  mem_fetch *pop();

  void print() const;

  // getters
  const char* get_name();

protected:
    const char* m_name;

    unsigned int m_latency;
    unsigned int m_size;
    unsigned int m_arr_size;

    unsigned int m_wr_ptr;
    unsigned int m_rd_ptr;

    mem_fetch **m_data_array;
    bool *m_is_head_array;
    bool *m_is_tail_array;
};


class compressed_link_delay_queue {
public:
    compressed_link_delay_queue(const char* nm, unsigned int size, unsigned int latency);
    ~compressed_link_delay_queue();

    // methods
    void push(mem_fetch* mf, unsigned size);
    std::pair<mem_fetch *, unsigned> top();
    void pop();

    void print() const;

    // getters
    const char* get_name();

protected:
    const char* m_name;

    unsigned int m_latency;
    unsigned int m_size;
    unsigned int m_arr_size;

    unsigned int m_wr_ptr;
    unsigned int m_rd_ptr;

    mem_fetch **m_data_array;
    unsigned *m_size_array;
    unsigned long long *m_time_array;
};

#endif
