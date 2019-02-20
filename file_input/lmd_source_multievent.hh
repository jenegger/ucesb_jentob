#ifndef _LMD_SOURCE_MULTIEVENT_H_
#define _LMD_SOURCE_MULTIEVENT_H_

#include <deque>
#include <list>
#include <map>

#include <stdint.h>

struct lmd_source_multievent;
struct lmd_event_multievent;

#include "lmd_input.hh"
#include "thread_buffer.hh"

#include <math.h>

#define _ENABLE_TRACE 0
#if _ENABLE_TRACE
#define _TRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define _TRACE(...)
#endif

struct keep_buffer_wrapper : public keep_buffer_many
{
protected:
   int refcount;

public:
   keep_buffer_wrapper() : refcount(0) {};

   void release()
   {
      _TRACE("[%p] keep_buffer_wrapper::release(). refcount = %d\n", this, refcount);
      if(!refcount)
         return;

      if(--refcount == 0)
      {
         _TRACE("[%p] Release\n", this);
         keep_buffer_many::release();
      }

      assert(refcount >= 0);
   }

   void* allocate(size_t size)
   {
      refcount++;
      _TRACE("[%p] keep_buffer_wrapper::allocate(). refcount = %d\n", this, refcount);
      return keep_buffer_many::allocate(size);
   }

   bool available()
   {
      _TRACE("[%p] keep_buffer_wrapper::available() => %d\n", this, !refcount);
      return !refcount;
   }
};

struct multievent_entry
{
  keep_buffer_wrapper *data_alloc;
  lmd_subevent_10_1_host _header; 
  uint64_t timestamp;
  uint8_t channel_id;
  uint8_t module_id;
  uint8_t sfp_id;
  uint32_t proc_id;
  uint32_t *data;
  uint32_t size;	
	multievent_entry(keep_buffer_wrapper *data_alloc) : data_alloc(data_alloc), data(NULL) {}
	~multievent_entry()
	{
	       _TRACE("multievent_entry::dtor()\n");

               if(data != NULL)
               {
                  data_alloc->release();
                  data = NULL;
               }
	}

        static bool compare(const multievent_entry *e1, const multievent_entry *e2);

        void* operator new(size_t bytes, keep_buffer_wrapper &alloc);
        void operator delete(void *ptr);
};

typedef std::deque< multievent_entry* > multievent_queue;

struct lmd_source_multievent : public lmd_source
{
protected:
  enum file_status_t { ready, eof, unknown_event };
  
  std::list<keep_buffer_wrapper*> data_alloc;
  std::list<keep_buffer_wrapper*>::iterator curbuf;
  uint64_t febex_ts_current;
  uint64_t wr_ts_current;
  uint64_t febex_ts_last;
  uint64_t wr_ts_last;
  int64_t delta_febex;
  int64_t delta_wrts;
  file_status_t input_status;

  // Timestamp skew per processor
  std::map<uint32_t, int64_t> proc_ts_skew;

  lmd_event_hint event_hint;
  lmd_event_10_1_host input_event_header;
  sint32 l_count;
	
  multievent_queue events_available;
  multievent_queue events_curevent;
  multievent_queue events_read;

  file_status_t load_events();
	
  multievent_entry* next_singleevent();
  
public:
  uint64_t febex2wrts(uint64_t fbxts)
  {
    static bool warned=0;
    if (!warned++ && !wr_ts_current)
      fprintf(stderr, "febex2wrts: WRTS is zero. This should never happen.\n");
    double m=double(delta_wrts)/double(delta_febex);
    return (int64_t)wr_ts_current+(int64_t)(double((int64_t)fbxts-(int64_t)febex_ts_current)*m);
  }

  void init_ts_conv()
  {
    febex_ts_last=0;
    wr_ts_last=0;
    febex_ts_current=0;
    wr_ts_current=0;
    delta_febex=50;
    delta_wrts=3;
  }
  
  void update_ts_conv(uint64_t wrts, uint64_t fbxts)
  {
    febex_ts_last=febex_ts_current;
    wr_ts_last=wr_ts_current;
    febex_ts_current=fbxts;
    wr_ts_current=wrts;
    static uint16_t warned=0;
    if (!wrts)
      {
	wr_ts_current=50*fbxts/3;
	if (!warned++)
	  fprintf(stdout,
		  "**************************************************\n"
		  "update_ts_conv: Either epoch is currently a multiple of 2^64 ns\n"
		  " (happy anniversary)\n"
		  " or your febex data did not include WRTS data (e.g. from PEXARIA). \n"
		  "I will fake a WRTS like timestamp based on the febex ts.\n"
		  "THIS WILL BE UNMERGEABLE WITH ANYTHING ELSE.\n"
		  "*************************************************\n\n");
      }
    if (!wr_ts_last || int64_t(febex_ts_current) - int64_t(febex_ts_last) <= 0) 
      {
	// one point interpolation using the standard febex ts rate of 50/3 ns
	delta_febex =  3;
	delta_wrts  =  50;
      }
    else
      {
	delta_febex =  febex_ts_current - febex_ts_last;
	delta_wrts  =  wr_ts_current    - wr_ts_last;
	assert(delta_febex>0);
	assert(delta_wrts>0);
      }
    double m = double(delta_wrts)/double(delta_febex);
    static double num_m=0;
    static double sum_m=0;
    static double ssum_m=0;
    num_m++;
    sum_m += m;
    ssum_m += m*m;
    double mean_m=sum_m/num_m;
    //double sigma_m=sqrt(ssum_m/num_m-mean_m*mean_m);
    //printf("m=%f (mean %f, sigma %f, rel=%e)\n", m, mean_m, sigma_m, sigma_m/mean_m);
    //printf("delta WRTS: %ld    delta FebexTS: %ld\n",
    //	   delta_wrts, delta_febex);
  }
  
  lmd_source_multievent(); 

  virtual lmd_event *get_event();
};

struct wrts_header
{
  uint32_t system_id;
  uint32_t lower16;
  uint32_t midlower16;
  uint32_t midupper16;
  uint32_t upper16;
public:
  wrts_header(uint64_t ts):
    system_id(0x400),
    lower16(   0x03e10000 | (0xffff & (uint32_t)(ts    ))),
    midlower16(0x04e10000 | (0xffff & (uint32_t)(ts>>16))),
    midupper16(0x05e10000 | (0xffff & (uint32_t)(ts>>32))),
    upper16(   0x06e10000 | (0xffff & (uint32_t)(ts>>48)))
  {}
};

#endif

