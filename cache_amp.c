/*
Advanced Computing Center for Research and Education Proprietary License
Version 1.0 (April 2006)

Copyright (c) 2006, Advanced Computing Center for Research and Education,
 Vanderbilt University, All rights reserved.

This Work is the sole and exclusive property of the Advanced Computing Center
for Research and Education department at Vanderbilt University.  No right to
disclose or otherwise disseminate any of the information contained herein is
granted by virtue of your possession of this software except in accordance with
the terms and conditions of a separate License Agreement entered into with
Vanderbilt University.

THE AUTHOR OR COPYRIGHT HOLDERS PROVIDES THE "WORK" ON AN "AS IS" BASIS,
WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT
LIMITED TO THE WARRANTIES OF MERCHANTABILITY, TITLE, FITNESS FOR A PARTICULAR
PURPOSE, AND NON-INFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Vanderbilt University
Advanced Computing Center for Research and Education
230 Appleton Place
Nashville, TN 37203
http://www.accre.vanderbilt.edu
*/

//*************************************************************************
//*************************************************************************

#define _log_module_index 181

#include "cache.h"
#include "cache_amp_priv.h"
#include "type_malloc.h"
#include "log.h"
#include "ex3_compare.h"
#include "thread_pool.h"

atomic_int_t amp_dummy = -1000;

typedef struct {
  segment_t *seg;
  ex_off_t lo;
  ex_off_t hi;
  int start_prefetch;
  int start_trigger;
op_generic_t *gop;
} amp_prefetch_op_t;

int _amp_logging = 15;  //** Kludge to flip the low level loggin statements on/off

//*************************************************************************
// _amp_max_bytes - REturns the max amount of space to use
//*************************************************************************

ex_off_t _amp_max_bytes(cache_t *c)
{
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;

  return(cp->max_bytes);
}

//*************************************************************************
//  _amp_stream_get - returns the *nearest* stream ot the offset if nbytes<=0.
//      Otherwise it will create a blank new page stream with the offset and return it.
//      NOTE: Assumes cache is locked!
//*************************************************************************

amp_page_stream_t *NEW_amp_stream_get(cache_t *c, segment_t *seg, ex_off_t offset, ex_off_t nbytes, amp_page_stream_t **pse_call)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  amp_stream_table_t *as = (amp_stream_table_t *)s->cache_priv;
  amp_page_stream_t *ps, *pse, *pss;
  list_iter_t it;
  ex_off_t *poff, dn, pos, lo_offset;
  int i, j;
  int n = 10;
  ex_off_t dropping_offsets[n];

  //** Find the last in the chain
  it = list_iter_search(as->streams, &offset, 0);
  list_next(&it, (list_key_t **)&poff, (list_data_t **)&ps);
  pse = ps;  pss = ps;
  i=0;
  if (ps != NULL) { lo_offset = ps->last_offset - ps->nbytes; }
  while ((ps != NULL) && (i<n)) {
    dn = ps->last_offset - pos;
    if (dn > ps->nbytes) {
log_printf(_amp_logging, "offset=" XOT " last_offset=" XOT " prefetch=%d trigger=%d\n", offset, pse->last_offset, pse->prefetch_size, pse->trigger_distance);
       break;
    }

    dn = ps->last_offset - ps->nbytes;
    if (dn < lo_offset) lo_offset = dn;
    dropping_offsets[i] = ps->last_offset;  i++;
    pse = ps;
    pos = ps->last_offset + s->page_size;
    list_next(&it, (list_key_t **)&poff, (list_data_t **)&ps);
  }

  if (pse != NULL) {
     i--;  //** Last index holds actual pse

     for (j=0; j<i; j++) {  //** Delete the interior chain
log_printf(_amp_logging, " dropping last=" XOT "\n", dropping_offsets[j]);
        list_remove(as->streams, &(dropping_offsets[j]), NULL);
     }

//if (pse != pss) {
//dn = pse->last_offset - pss->last_offset + 1 + pss->nbytes;
dn = pse->last_offset - lo_offset + 1;
log_printf(_amp_logging, " pse->last=" XOT " pse->nbytes=" XOT " pss->last=" XOT " pss->nbytes=" XOT " nbytes=" XOT "\n",
  pse->last_offset, pse->nbytes, pss->last_offset, pss->nbytes, dn);
//}
  pse->nbytes = pse->last_offset - lo_offset;  //** Adjust the range to cover everything just removed
//     if (pse != pss) pse->nbytes = pse->last_offset - pss->last_offset + 1 + pss->nbytes;  //** Adjust the range to cover everything just removed
  }

  if ((pse == NULL) && (nbytes > 0)) {
     //** Unlink the old one and remove it
     pse = &(as->stream_table[as->index]);
     as->index = (as->index + 1) % as->max_streams;
log_printf(_amp_logging, "seg=" XIDT " offset=" XOT " dropping ps=%p  ps->last_offset=" XOT "\n", segment_id(seg), offset, pse, pse->last_offset);
     list_remove(as->streams, &(pse->last_offset), pse);

     //** Store the new info in it
     pse->last_offset = offset;
     pse->nbytes = nbytes;
     pse->prefetch_size = 0;
     pse->trigger_distance = 0;

log_printf(_amp_logging, "seg=" XIDT " offset=" XOT " moving to MRU ps=%p ps->last_offset=" XOT "\n", segment_id(seg), offset, pse, pse->last_offset);

     //** Add the entry back into the stream table
     list_insert(as->streams, &(pse->last_offset), pse);
  } if (pse != NULL) {
log_printf(_amp_logging, "offset=" XOT " last_offset=" XOT " prefetch=%d trigger=%d\n", offset, pse->last_offset, pse->prefetch_size, pse->trigger_distance);
} else {
log_printf(_amp_logging, "offset=" XOT " NO MATCH FOUND\n", offset);
}

  if (pse_call != NULL) *pse_call = pse;

  return(pse);
}

//*************************************************************************
//  _amp_stream_get - returns the *nearest* stream ot the offset if nbytes<=0.
//      Otherwise it will create a blank new page stream with the offset and return it.
//      NOTE: Assumes cache is locked!
//*************************************************************************

amp_page_stream_t *_amp_stream_get(cache_t *c, segment_t *seg, ex_off_t offset, ex_off_t nbytes, amp_page_stream_t **pse)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  amp_stream_table_t *as = (amp_stream_table_t *)s->cache_priv;
//  Stack_ele_t *ele;
  amp_page_stream_t *ps, *ps2;
  list_iter_t it;
  ex_off_t *poff, dn, pos;

  if (nbytes > 0) {
     ps = list_search(as->streams, &offset);

log_printf(_amp_logging, "seg=" XIDT " offset=" XOT " nbytes=" XOT "\n", segment_id(seg), offset, nbytes);
     if ((ps == NULL) && (nbytes > 0)) { //** Got a miss and they want a new one
        //** Unlink the old one and remove it
        ps = &(as->stream_table[as->index]);
        if (pse != NULL) *pse = ps;
        as->index = (as->index + 1) % as->max_streams;
log_printf(_amp_logging, "seg=" XIDT " offset=" XOT " dropping ps=%p  ps->last_offset=" XOT "\n", segment_id(seg), offset, ps, ps->last_offset);
        list_remove(as->streams, &(ps->last_offset), ps);

        //** Store the new info in it
        ps->last_offset = offset;
        ps->nbytes = nbytes;
        ps->prefetch_size = 0;
        ps->trigger_distance = 0;

log_printf(_amp_logging, "seg=" XIDT " offset=" XOT " moving to MRU ps=%p ps->last_offset=" XOT "\n", segment_id(seg), offset, ps, ps->last_offset);

        //** Add the entry back into the stream table
        list_insert(as->streams, &(ps->last_offset), ps);
     } else if (ps != NULL) {   //** Move it to the MRU slot
log_printf(_amp_logging, "seg=" XIDT " offset=" XOT " moving to MRU ps=%p ps->last_offset=" XOT " prefetch=%d trigger=%d\n", segment_id(seg), offset, ps, ps->last_offset, ps->prefetch_size, ps->trigger_distance);
     }
  } else {
     it = list_iter_search(as->streams, &offset, 0);
     list_next(&it, (list_key_t **)&poff, (list_data_t **)&ps);
     if (ps != NULL) {
        dn = ps->last_offset - offset;
        if (dn > ps->nbytes) ps = NULL;
     }
     ps2 = ps;
if (ps != NULL) {
log_printf(_amp_logging, "seg=" XIDT " offset=" XOT " moving to MRU ps=%p ps->last_offset=" XOT " prefetch=%d trigger=%d\n", segment_id(seg), offset, ps, ps->last_offset, ps->prefetch_size, ps->trigger_distance);
}
     if (pse != NULL) {
        pos = offset;
        *pse = ps2;
        while (ps2 != NULL) {
          dn = ps2->last_offset - pos;
          if (dn > ps->nbytes) {
log_printf(_amp_logging, "PSE offset=" XOT " last_offset=" XOT " prefetch=%d trigger=%d\n", offset, (*pse)->last_offset, (*pse)->prefetch_size, (*pse)->trigger_distance);
             return(ps);
          }

          *pse = ps2;
          pos = ps2->last_offset + s->page_size;
          list_next(&it, (list_key_t **)&poff, (list_data_t **)&ps2);
        }
     }
  }

  return(ps);
}

//*************************************************************************
//  _amp_stream_chain_last - returns the *last* link in the stream chain based on the offset.
//      NOTE: Assumes cache is locked!
//*************************************************************************

amp_page_stream_t *MERGED_amp_stream_chain_last(cache_t *c, segment_t *seg, ex_off_t offset)
{
//return(_amp_stream_get(c, seg, offset, -1));

  cache_segment_t *s = (cache_segment_t *)seg->priv;
  amp_stream_table_t *as = (amp_stream_table_t *)s->cache_priv;
  amp_page_stream_t *ps, *pse;
  list_iter_t it;
  ex_off_t *poff, dn, pos;

  it = list_iter_search(as->streams, &offset, 0);
  list_next(&it, (list_key_t **)&poff, (list_data_t **)&ps);
  pse = ps;
  while (ps != NULL) {
    dn = ps->last_offset - pos;
    if (dn > ps->nbytes) {
log_printf(_amp_logging, "offset=" XOT " last_offset=" XOT " prefetch=%d trigger=%d\n", offset, pse->last_offset, pse->prefetch_size, pse->trigger_distance);
       return(pse);
    }

    pse = ps;
    pos = ps->last_offset + s->page_size;
    list_next(&it, (list_key_t **)&poff, (list_data_t **)&ps);
  }

if (pse != NULL) {
log_printf(_amp_logging, "offset=" XOT " last_offset=" XOT " prefetch=%d trigger=%d\n", offset, pse->last_offset, pse->prefetch_size, pse->trigger_distance);
} else {
log_printf(_amp_logging, "offset=" XOT " NO MATCH FOUND\n", offset);
}
  return(pse);
}


//*************************************************************************
// amp_dirty_thread - Thread to handle flushing due to dirty ratio
//*************************************************************************

void *amp_dirty_thread(apr_thread_t *th, void *data)
{
  cache_t *c = (cache_t *)data;
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;
  double df;
  int n, i;
  ex_id_t *id;
  segment_t *seg;
  opque_t *q;
  op_generic_t *gop;
  cache_segment_t *s;
  skiplist_iter_t it;
  segment_t **flush_list;

  cache_lock(c);

log_printf(15, "Dirty thread launched\n");
  while (c->shutdown_request == 0) {
     apr_thread_cond_timedwait(cp->dirty_trigger, c->lock, cp->dirty_max_wait);

     df = cp->max_bytes;
     df = c->stats.dirty_bytes / df;

log_printf(15, "Dirty thread running.  dirty fraction=%lf dirty bytes=" XOT " inprogress=%d  cached segments=%d\n", df, c->stats.dirty_bytes, cp->flush_in_progress, list_key_count(c->segments));

     cp->flush_in_progress = 1;
     q = new_opque();

     n = list_key_count(c->segments);
     type_malloc(flush_list, segment_t *, n);

     it = list_iter_search(c->segments, NULL, 0);
     list_next(&it, (list_key_t **)&id, (list_data_t **)&seg);
     i = 0;
     while (seg != NULL) {
log_printf(15, "Flushing seg=" XIDT " i=%d\n", *id, i);
flush_log();
        flush_list[i] = seg;
        s = (cache_segment_t *)seg->priv;
        atomic_set(s->cache_check_in_progress, 1);  //** Flag it as being checked
        gop = cache_flush_range(seg, s->c->da, 0, -1, s->c->timeout);
        gop_set_myid(gop, i);
        opque_add(q, gop);
        i++;

        list_next(&it, (list_key_t **)&id, (list_data_t **)&seg);
     }
     cache_unlock(c);

     //** Flag the tasks as they complete
     opque_start_execution(q);
     while ((gop = opque_waitany(q)) != NULL) {
         i = gop_get_myid(gop);
         segment_lock(flush_list[i]);
         s = (cache_segment_t *)flush_list[i]->priv;

log_printf(15, "Flush completed seg=" XIDT " i=%d\n", segment_id(flush_list[i]), i);
flush_log();
         atomic_set(s->cache_check_in_progress, 0);  //** Flag it as being finished
         segment_unlock(flush_list[i]);

         gop_free(gop, OP_DESTROY);
     }
     opque_free(q, OP_DESTROY);

     cache_lock(c);

     cp->flush_in_progress = 0;
     free(flush_list);

df = cp->max_bytes;
df = c->stats.dirty_bytes / df;
log_printf(15, "Dirty thread sleeping.  dirty fraction=%lf dirty bytes=" XOT " inprogress=%d\n", df, c->stats.dirty_bytes, cp->flush_in_progress);
//     apr_thread_cond_timedwait(cp->dirty_trigger, c->lock, cp->dirty_max_wait);
  }

log_printf(15, "Dirty thread Exiting\n");

  cache_unlock(c);

  return(NULL);

}


//*************************************************************************
// amp_adjust_dirty - Adjusts the dirty ratio and if needed trigger a flush
//*************************************************************************

void amp_adjust_dirty(cache_t *c, ex_off_t tweak)
{
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;

  cache_lock(c);
  c->stats.dirty_bytes += tweak;
  if (c->stats.dirty_bytes > cp->dirty_bytes_trigger) {
     if (cp->flush_in_progress == 0) {
        cp->flush_in_progress = 1;
        apr_thread_cond_signal(cp->dirty_trigger);
     }
  }
  cache_unlock(c);
}

//*************************************************************************
//  _amp_new_page - Creates the physical page
//*************************************************************************

cache_page_t *_amp_new_page(cache_t *c, segment_t *seg)
{
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  page_amp_t *lp;
  cache_page_t *p;

  type_malloc_clear(lp, page_amp_t, 1);
  p = &(lp->page);
  p->curr_data = &(p->data[0]);
  p->current_index = 0;
  type_malloc_clear(p->curr_data->ptr, char, s->page_size);

  cp->bytes_used += s->page_size;

  p->priv = (void *)lp;
  p->seg = seg;
  p->offset = atomic_dec(amp_dummy);
  atomic_set(p->bit_fields, C_EMPTY);  //** This way it's not accidentally deleted
  lp->stream_offset = -1;

  //** Store my position
  push(cp->stack, p);
  lp->ele = get_ptr(cp->stack);

log_printf(_amp_logging, " seg=" XIDT " MRU page created initial->offset=" XOT " page_size=" XOT " bytes_used=" XOT " stack_size=%d\n", segment_id(seg), p->offset, s->page_size, cp->bytes_used, stack_size(cp->stack));
  return(p);
}

//*************************************************************************
// _amp_process_waiters - Checks if watiers can be handled
//*************************************************************************

void _amp_process_waiters(cache_t *c)
{
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;
  amp_page_wait_t *pw = NULL;
  cache_cond_t *cache_cond;
  ex_off_t bytes_free, bytes_needed;

  if (stack_size(cp->pending_free_tasks) > 0) {  //**Check on pending free tasks 1st
     while ((cache_cond = (cache_cond_t *)pop(cp->pending_free_tasks)) != NULL) {
log_printf(15, "waking up pending task cache_cond=%p stack_size left=%d\n", cache_cond, stack_size(cp->pending_free_tasks));
        apr_thread_cond_signal(cache_cond->cond);    //** Wake up the paused thread
     }
//     return;
  }

  if (stack_size(cp->waiting_stack) > 0) {  //** Also handle the tasks waiting for flushes to complete
     bytes_free = _amp_max_bytes(c) - cp->bytes_used;

     move_to_top(cp->waiting_stack);
     pw = get_ele_data(cp->waiting_stack);
     bytes_needed = pw->bytes_needed;
     while ((bytes_needed <= bytes_free) && (pw != NULL)) {
        bytes_free -= bytes_needed;
        delete_current(cp->waiting_stack, 1, 0);
log_printf(15, "waking up waiting stack pw=%d\n", pw);

        apr_thread_cond_signal(pw->cond);    //** Wake up the paused thread

        //** Get the next one if available
        pw = get_ele_data(cp->waiting_stack);
        bytes_needed = (pw == NULL) ? bytes_free + 1 : pw->bytes_needed;
     }
  }

}

//*******************************************************************************
// amp_pretech_fn - Does the actual prefetching
//*******************************************************************************

op_status_t amp_prefetch_fn(void *arg, int id)
{
  amp_prefetch_op_t *ap = (amp_prefetch_op_t *)arg;
  segment_t *seg = ap->seg;
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  cache_amp_t *cp = (cache_amp_t *)s->c->fn.priv;
  page_handle_t page[CACHE_MAX_PAGES_RETURNED];
  cache_page_t *p;
  page_amp_t *lp;
  amp_page_stream_t *ps;
  ex_off_t offset, *poff, trigger_offset, nbytes;
  skiplist_iter_t it;
  int n_pages, i, nloaded, pending_read;

  nbytes = ap->hi + s->page_size - ap->lo;
//  if (ap->start_trigger == 0) {
//     ap->start_trigger = nbytes / (2*s->page_size);
//  }
  trigger_offset = ap->hi - ap->start_trigger*s->page_size;

log_printf(_amp_logging, "seg=" XIDT " initial lo=" XOT " hi=" XOT " trigger=" XOT " start_trigger=%d start_prefetch=%d\n", segment_id(ap->seg), ap->lo, ap->hi, trigger_offset, ap->start_trigger, ap->start_prefetch);

  pending_read = 0;
  nloaded = 0;
  offset = ap->lo;
  while (offset <= ap->hi) {
     n_pages = CACHE_MAX_PAGES_RETURNED;
     cache_advise(ap->seg, CACHE_READ, offset, ap->hi, page, &n_pages, 1);
log_printf(_amp_logging, "seg=" XIDT " lo=" XOT " hi=" XOT " n_pages=%d\n", segment_id(ap->seg), offset, ap->hi, n_pages);
     if (n_pages == 0) { //** Hit an existing page
        segment_lock(seg);
        cache_lock(s->c);
        it = iter_search_skiplist(s->pages, &offset, 0);
        next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);
log_printf(15, "seg=" XIDT " before while offset=" XOT " p=%p\n", segment_id(ap->seg), offset, p);
        while (p != NULL) {
log_printf(_amp_logging, "seg=" XIDT " p->offset=" XOT " offset=" XOT "\n", segment_id(ap->seg), p->offset, offset);
           if (p->offset != offset) {  //** got a hole
              p = NULL;
           } else {
             if (offset == ap->hi) { //** Kick out we hit the end
                offset += s->page_size;
                p = NULL;
             } else {
                if (offset == trigger_offset) {  //** Set the trigger page
                   lp = (page_amp_t *)p->priv;
                   lp->bit_fields |= CAMP_TAG;
                   lp->stream_offset = ap->hi;
log_printf(_amp_logging, "seg=" XIDT " SET_TAG offset=" XOT "\n", segment_id(ap->seg), offset);
                }

                //** Attempt to get the next page
                next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);
                offset += s->page_size;
                if (p != NULL) {
                   if (p->offset != offset) p = NULL;  //** Hit a hole so kick out
                }
             }
           }
        }
        cache_unlock(s->c);
        segment_unlock(seg);
     } else {  //** Process the pages just loaded
       cache_lock(s->c);
       nloaded += n_pages;
       for (i=0; i<n_pages; i++) {
          if (atomic_get(page[i].p->access_pending[CACHE_READ]) > 0) pending_read++;

          if (page[i].p->offset == trigger_offset) {
             lp = (page_amp_t *)page[i].p->priv;
             lp->bit_fields |= CAMP_TAG;
             lp->stream_offset = ap->hi;
log_printf(_amp_logging, "seg=" XIDT " SET_TAG offset=" XOT " last=" XOT "\n", segment_id(ap->seg), offset, lp->stream_offset);
          }
       }
       offset = page[n_pages-1].p->offset;
       offset += s->page_size;

       cache_unlock(s->c);

       cache_release_pages(n_pages, page, CACHE_READ);
     }
  }

  //** Update the stream info
  cache_lock(s->c);
  ps = _amp_stream_get(s->c, seg, ap->hi, nbytes, NULL);
  if (ps != NULL) {
     ps->prefetch_size = (ap->start_prefetch >  (ps->trigger_distance+1)) ? ap->start_prefetch : ps->trigger_distance + 1;
     ps->trigger_distance = ap->start_trigger;
     if (pending_read > 0) {
        ps->trigger_distance += (ap->hi + s->page_size - ap->lo) / s->page_size;
log_printf(_amp_logging, "seg=" XIDT " LAST read waiting=%d for offset=" XOT " increasing trigger_distance=%d prefetch_pages=%d\n", segment_id(ap->seg), pending_read, ap->hi, ps->trigger_distance, ps->prefetch_size);
     }
  }

  cp->prefetch_in_process -= nbytes;  //** Adjust the prefetch bytes
  cache_unlock(s->c);


  //** Update the stats
  offset = nloaded * s->page_size;
log_printf(15, "seg=" XIDT " additional system read bytes=" XOT "\n", segment_id(ap->seg), offset);
  segment_lock(seg);
  s->stats.system.read_count++;
  s->stats.system.read_bytes += offset;
  segment_unlock(seg);

  return(op_success_status);
}

//*******************************************************************************
// _amp_prefetch - Prefetch the given range
//   NOTE : ASsumes the cache is locked!
//*******************************************************************************

void _amp_prefetch(segment_t *seg, ex_off_t lo, ex_off_t hi, int start_prefetch, int start_trigger)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  cache_amp_t *cp = (cache_amp_t *)s->c->fn.priv;
  ex_off_t lo_row, hi_row, child_size, nbytes, dn;
  amp_prefetch_op_t *ca;
  op_generic_t *gop;
  int tid;

  child_size = segment_size(s->child_seg);

tid = atomic_thread_id;
log_printf(_amp_logging, "tid=%d START seg=" XIDT " lo=" XOT " hi=" XOT " child_size=" XOT "\n", tid, segment_id(seg), lo, hi, child_size);

//  if (child_size < lo) return;
//  if (child_size < hi) hi = child_size;

  if (child_size < lo) {
     log_printf(15, "OOPS read beyond EOF\n");
     return;
  }
  if (child_size <= hi) {
     hi = child_size-1;
     log_printf(15, "OOPS read beyond EOF  truncating hi=child\n");
  }

  if (s->c->max_fetch_size <= cp->prefetch_in_process) return;  //** To much prefetching

  nbytes = hi + s->page_size - lo;
  if (nbytes < cp->min_prefetch_size) {
log_printf(_amp_logging, " SMALL prefetch!  nbytes=" XOT "\n", nbytes);
     hi = lo + cp->min_prefetch_size;
  }

  //** To much fetching going on so truncate the fetch
  dn = s->c->max_fetch_size - cp->prefetch_in_process;
  nbytes = hi + s->page_size - lo;
  if (dn < nbytes) {
     hi = lo + dn;
  }


  //** Map the rage to the page boundaries
  lo_row = lo / s->page_size; lo_row = lo_row * s->page_size;
  hi_row = hi / s->page_size; hi_row = hi_row * s->page_size;
  nbytes = hi_row + s->page_size - lo_row;

log_printf(_amp_logging, "seg=" XIDT " max_fetch=" XOT " prefetch_in_process=" XOT " nbytes=" XOT "\n", segment_id(seg), s->c->max_fetch_size, cp->prefetch_in_process, nbytes);

  cp->prefetch_in_process += nbytes;  //** Adjust the prefetch size

  type_malloc(ca, amp_prefetch_op_t, 1);
  ca->seg = seg;
  ca->lo = lo_row;
  ca->hi = hi_row;
  ca->start_prefetch = start_prefetch;
  ca->start_trigger = start_trigger;
  gop = new_thread_pool_op(s->tpc_unlimited, NULL, amp_prefetch_fn, (void *)ca, free, 1);
ca->gop = gop;
//log_printf(15, "tid=%d seg=" XIDT " lo=" XOT " hi=" XOT " rw_mode=%d ca=%p gid=%d\n", tid, segment_id(seg), lo, hi, rw_mode, ca, gop_id(gop));
//flush_log();

  gop_set_auto_destroy(gop, 1);

  gop_start_execution(gop);
}

//*************************************************************************
//  amp_pages_release - Releases the page using the amp algorithm.
//    Returns 0 if the page still exits and 1 if it was removed.
//*************************************************************************

int amp_pages_release(cache_t *c, cache_page_t **page, int n_pages)
{
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;
  cache_segment_t *s;
  page_amp_t *lp;
  cache_page_t *p;
  int bits, i;

  cache_lock(c);

  for (i=0; i<n_pages; i++) {
     p = page[i];
     bits = atomic_get(p->bit_fields);
log_printf(15, "seg=" XIDT " p->offset=" XOT " bits=%d bytes_used=" XOT "\n", segment_id(p->seg), p->offset, bits, cp->bytes_used);
     if ((bits & C_TORELEASE) > 0) {
log_printf(15, "DESTROYING seg=" XIDT " p->offset=" XOT " bits=%d bytes_used=" XOT "cache_pages=%d\n", segment_id(p->seg), p->offset, bits, cp->bytes_used, stack_size(cp->stack));
        s = (cache_segment_t *)p->seg->priv;
        lp = (page_amp_t *)p->priv;

        cp->bytes_used -= s->page_size;
        if (lp->ele != NULL) {
           move_to_ptr(cp->stack, lp->ele);
           delete_current(cp->stack, 0, 0);
        } else {
           cp->limbo_pages--;
log_printf(15, "seg=" XIDT " limbo page p->offset=" XOT " limbo=%d\n", segment_id(p->seg), p->offset, cp->limbo_pages);
        }

        if (p->offset > -1) {
           list_remove(s->pages, &(p->offset), p);  //** Have to do this here cause p->offset is the key var
        }
        if (p->data[0].ptr) free(p->data[0].ptr);
        if (p->data[1].ptr) free(p->data[1].ptr);
        free(lp);
     }
  }

  //** Now check if we can handle some waiters
  _amp_process_waiters(c);

  cache_unlock(c);

  return(0);
}

//*************************************************************************
//  amp_pages_destroy - Destroys the page list.  Since this is called from a
//    forced cache page requests it's possible that another empty page request
//    created the page already.  If so we just need to drop this page cause
//    it wasnn't added to the segment (remove_from_segment=0)
//*************************************************************************

void amp_pages_destroy(cache_t *c, cache_page_t **page, int n_pages, int remove_from_segment)
{
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;
  cache_segment_t *s;
  page_amp_t *lp;
  cache_page_t *p;
//  cache_cond_t *cache_cond;
  int i;
  int cr, cw, cf, count;
  cache_lock(c);

log_printf(15, " START cp->bytes_used=" XOT "\n", cp->bytes_used);

  for (i=0; i<n_pages; i++) {
     p = page[i];
     s = (cache_segment_t *)p->seg->priv;

     cr = atomic_get(p->access_pending[CACHE_READ]);
     cw = atomic_get(p->access_pending[CACHE_WRITE]);
     cf = atomic_get(p->access_pending[CACHE_FLUSH]);
     count = cr +cw + cf;

//     cache_cond = (cache_cond_t *)pigeon_coop_hole_data(&(p->cond_pch));
//     if (cache_cond == NULL) {  //** No one listening so free normally
     if (count == 0) {  //** No one is listening
log_printf(15, "amp_pages_destroy i=%d p->offset=" XOT " seg=" XIDT " remove_from_segment=%d limbo=%d\n", i, p->offset, segment_id(p->seg), remove_from_segment, cp->limbo_pages);
        cp->bytes_used -= s->page_size;
        lp = (page_amp_t *)p->priv;

        if (lp->ele != NULL) {
           move_to_ptr(cp->stack, lp->ele);
           delete_current(cp->stack, 0, 0);
        }

        if (remove_from_segment == 1) {
           s = (cache_segment_t *)p->seg->priv;
           list_remove(s->pages, &(p->offset), p);  //** Have to do this here cause p->offset is the key var
        }

        if (p->data[0].ptr) free(p->data[0].ptr);
        if (p->data[1].ptr) free(p->data[1].ptr);
        free(lp);
     } else {  //** Someone is listening so trigger them and also clear the bits so it will be released
       atomic_set(p->bit_fields, C_TORELEASE);
log_printf(15, "amp_pages_destroy i=%d p->offset=" XOT " seg=" XIDT " remove_from_segment=%d cr=%d cw=%d cf=%d limbo=%d\n", i, p->offset, segment_id(p->seg), remove_from_segment, cr, cw, cf, cp->limbo_pages);
     }
  }

log_printf(15, " AFTER LOOP cp->bytes_used=" XOT "\n", cp->bytes_used);

log_printf(15, " END cp->bytes_used=" XOT "\n", cp->bytes_used);

  cache_unlock(c);
}

//*************************************************************************
//  amp_page_access - Updates the access time for the cache block
//*************************************************************************

int amp_page_access(cache_t *c, cache_page_t *p, int rw_mode, ex_off_t request_len)
{
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;
  cache_segment_t *s = (cache_segment_t *)p->seg->priv;
  page_amp_t *lp = (page_amp_t *)p->priv;
  amp_page_stream_t *ps, *pse;
  ex_off_t lo, hi, psize, last_offset;
  int prefetch_pages, trigger_distance, tag;

  if (rw_mode == CACHE_FLUSH) return(0);  //** Nothing to do for a flush

  //** Only update the position if the page is linked.
  //** Otherwise the page is destined to be dropped
  if (lp->ele != NULL) {
     cache_lock(c);
     if (lp->ele != NULL) {  //** Recehck with the lock on
        //** Move to the MRU position
        if ((lp->bit_fields & CAMP_ACCESSED) > 0) {
log_printf(_amp_logging, "seg=" XIDT " MRU offset=" XOT "\n", segment_id(p->seg), p->offset);
           move_to_ptr(cp->stack, lp->ele);
           stack_unlink_current(cp->stack, 1);
           move_to_top(cp->stack);
           insert_link_above(cp->stack, lp->ele);
        }
else {
log_printf(_amp_logging, "seg=" XIDT " SKIPPED offset=" XOT "\n", segment_id(p->seg), p->offset);
}
     }

     if (rw_mode == CACHE_WRITE) {  //** Write update so return
        lp->bit_fields |= CAMP_ACCESSED;
        cache_unlock(c);
        return(0);
     }

     //** IF made it to here we are doing a READ access update
     psize = s->page_size;
     ps = NULL;
     //** Check if we need to do a prefetch
     tag = lp->bit_fields & CAMP_TAG;
     if (tag > 0) {
        lp->bit_fields ^= CAMP_TAG;
        ps = _amp_stream_get(c, p->seg, lp->stream_offset, -1, &pse);
        if (ps != NULL) {
           last_offset = ps->last_offset;
           prefetch_pages = ps->prefetch_size;
           trigger_distance = ps->trigger_distance;
        } else {
           last_offset = lp->stream_offset;
           prefetch_pages = request_len / s->page_size;
           if (prefetch_pages < 2) prefetch_pages = 2;
           trigger_distance = prefetch_pages / 2;
        }

        hi = last_offset + (prefetch_pages + 2) * psize - 1;

        if ((hi - last_offset - psize + 1) > s->c->max_fetch_size) hi = last_offset + psize + s->c->max_fetch_size;
lo = last_offset + psize;
log_printf(_amp_logging, "seg=" XIDT " HIT_TAG offset=" XOT " last_offset=" XOT " lo=" XOT " hi=" XOT " prefetch_pages=%d\n", segment_id(p->seg), p->offset, lp->stream_offset, lo, hi, prefetch_pages);
        _amp_prefetch(p->seg, last_offset + psize, hi, prefetch_pages, trigger_distance);
     } else {
        ps = _amp_stream_get(c, p->seg, p->offset, -1, &pse);
     }

     if (pse != NULL) {
        if ((p->offset == pse->last_offset) && ((lp->bit_fields & CAMP_OLD) == 0)) { //** Last in chain so increase the readahead size
           pse->prefetch_size += request_len / psize;
log_printf(_amp_logging, "seg=" XIDT " LAST offset=" XOT " prefetch_size=%d trigger=%d\n", segment_id(p->seg), p->offset, pse->prefetch_size, pse->trigger_distance);
        }
     }

     lp->bit_fields |= CAMP_ACCESSED;

     cache_unlock(c);
  }

  return(0);
}

//*************************************************************************
//  _amp_free_mem - Frees page memory OPPORTUNISTICALLY
//   Returns the pending bytes to free.  Aborts as soon as it encounters
//   a page it has to flush or can't access
//*************************************************************************

int _amp_free_mem(cache_t *c, segment_t *pseg, ex_off_t bytes_to_free)
{
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;
  cache_segment_t *s;
  cache_page_t *p;
  page_amp_t *lp;
  Stack_ele_t *ele;
  apr_thread_mutex_t *plock;
  ex_off_t total_bytes, pending_bytes;
  int gotlock, count, bits, err;

  total_bytes = 0;
  err = 0;

log_printf(15, "START seg=" XIDT " bytes_to_free=" XOT " bytes_used=" XOT " stack_size=%d\n", segment_id(pseg), bytes_to_free, cp->bytes_used, stack_size(cp->stack));

  move_to_bottom(cp->stack);
  ele = get_ptr(cp->stack);
  while ((total_bytes < bytes_to_free) && (ele != NULL) && (err == 0)) {
    p = (cache_page_t *)get_stack_ele_data(ele);
    lp = (page_amp_t *)p->priv;
    plock = p->seg->lock;
    gotlock = apr_thread_mutex_trylock(plock);
    if ((gotlock == APR_SUCCESS) || (p->seg == pseg)) {
       bits = atomic_get(p->bit_fields);
       if ((bits & C_TORELEASE) == 0) { //** Skip it if already flagged for removal
          count = atomic_get(p->access_pending[CACHE_READ]) + atomic_get(p->access_pending[CACHE_WRITE]) + atomic_get(p->access_pending[CACHE_FLUSH]);
          if (count == 0) { //** No one is using it
             s = (cache_segment_t *)p->seg->priv;
             if (((bits & C_ISDIRTY) == 0) && ((lp->bit_fields & (CAMP_OLD|CAMP_ACCESSED)) > 0)) {  //** Don't have to flush it
//             if ((bits & C_ISDIRTY) == 0) {  //** Don't have to flush it
                total_bytes += s->page_size;
log_printf(_amp_logging, "amp_free_mem: freeing page seg=" XIDT " p->offset=" XOT " bits=%d\n", segment_id(p->seg), p->offset, bits);
                list_remove(s->pages, &(p->offset), p);  //** Have to do this here cause p->offset is the key var
                delete_current(cp->stack, 1, 0);
                if (p->data[0].ptr) free(p->data[0].ptr);
                if (p->data[1].ptr) free(p->data[1].ptr);
                free(lp);
             } else {         //** Got to flush the page first
                err = 1;
             }
          } else {
            err = 1;
          }
       }
       if (gotlock == APR_SUCCESS) apr_thread_mutex_unlock(plock);
    } else {
      err = 1;
    }

    if ((total_bytes < bytes_to_free) && (err == 0)) ele = get_ptr(cp->stack);
  }

  cp->bytes_used -= total_bytes;
  pending_bytes = bytes_to_free - total_bytes;
log_printf(15, "END seg=" XIDT " bytes_to_free=" XOT " pending_bytes=" XOT " bytes_used=" XOT "\n", segment_id(pseg), bytes_to_free, pending_bytes, cp->bytes_used);

  return(pending_bytes);
}

//*************************************************************************
// amp_attempt_free_mem - Attempts to forcefully Free page memory
//   Returns the total number of bytes freed
//*************************************************************************

ex_off_t _amp_attempt_free_mem(cache_t *c, segment_t *page_seg, ex_off_t bytes_to_free)
{
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;
  cache_segment_t *s;
  segment_t *pseg;
  cache_page_t *p;
  page_amp_t *lp;
  Stack_ele_t *ele, *curr_ele;
  op_generic_t *gop;
  opque_t *q;
  amp_page_stream_t *ps;
  ex_off_t total_bytes, freed_bytes, pending_bytes, *poff;
  ex_off_t *segid;
  ex_off_t min_off, max_off;
  list_iter_t sit;
  int count, bits, cw, flush_count;
  list_t *table;
  page_table_t *ptable;
  pigeon_coop_hole_t pch, pt_pch;

log_printf(15, "START seg=" XIDT " bytes_to_free=" XOT " bytes_used=" XOT " stack_size=%d\n", segment_id(page_seg), bytes_to_free, cp->bytes_used, stack_size(cp->stack));

  freed_bytes = 0;
  pending_bytes = 0;
  total_bytes = 0;

  //** cache_lock(c) is already acquired
  pch = reserve_pigeon_coop_hole(cp->free_pending_tables);
  table = *(list_t **)pigeon_coop_hole_data(&pch);

  //** Get the list of pages to free
  move_to_bottom(cp->stack);
  ele = get_ptr(cp->stack);
  while ((total_bytes < bytes_to_free) && (ele != NULL)) {
    p = (cache_page_t *)get_stack_ele_data(ele);
    lp = (page_amp_t *)p->priv;

    bits = atomic_get(p->bit_fields);
log_printf(15, "checking page for release seg=" XIDT " p->offset=" XOT " bits=%d\n", segment_id(p->seg), p->offset, bits);
flush_log();

    if ((bits & C_TORELEASE) == 0) { //** Skip it if already flagged for removal
       if ((lp->bit_fields & (CAMP_OLD|CAMP_ACCESSED)) > 0) {  //** Already used once or cycled so ok to evict
          ptable = (page_table_t *)list_search(table, (list_key_t *)&(segment_id(p->seg)));
          if (ptable == NULL) {  //** Have to make a new segment entry
             pt_pch = reserve_pigeon_coop_hole(cp->free_page_tables);
             ptable = (page_table_t *)pigeon_coop_hole_data(&pt_pch);
             ptable->seg = p->seg;
             ptable->id = segment_id(p->seg);
             ptable->pch = pt_pch;
             list_insert(table, &(ptable->id), ptable);
          }

          stack_unlink_current(cp->stack, 1);  //** Unlink it.  This is ele

          s = (cache_segment_t *)p->seg->priv;
          if ((lp->bit_fields & CAMP_ACCESSED) == 0) c->stats.unused_bytes += s->page_size;

          cp->limbo_pages++;
log_printf(_amp_logging, "UNLINKING seg=" XIDT " p->offset=" XOT " bits=%d limbo=%d\n", segment_id(p->seg), p->offset, bits, cp->limbo_pages);

          atomic_inc(p->access_pending[CACHE_READ]);  //** Do this so it's not accidentally deleted
          push(ptable->stack, p);
          total_bytes += s->page_size;
          free(lp->ele); lp->ele = NULL;  //** Mark it as removed from the list so a page_release doesn't free also
       } else {
          lp->bit_fields |= CAMP_OLD;  //** Flag it as old

log_printf(_amp_logging, "seg=" XIDT " MRU retry offset=" XOT "\n", segment_id(p->seg), p->offset);

          stack_unlink_current(cp->stack, 1);  //** and move it to the MRU slot.  This is ele
          curr_ele = get_ptr(cp->stack);
          move_to_top(cp->stack);
          insert_link_above(cp->stack, lp->ele);
          move_to_ptr(cp->stack, curr_ele);

          //** Tweak the stream info
          _amp_stream_get(c, p->seg, p->offset, -1, &ps);  //** Don't care aboutthe initial element in the chaing.  Just the last
          if (ps != NULL) {
             if (ps->prefetch_size > 0) ps->prefetch_size--;
             if (ps->trigger_distance > 0) ps->trigger_distance--;
             if ((ps->prefetch_size-1) < ps->trigger_distance) ps->trigger_distance = ps->prefetch_size - 1;
          }
       }
    }

    if (total_bytes < bytes_to_free) ele = get_ptr(cp->stack);
  }


  if (total_bytes == 0) {  //** Nothing to do so exit
     log_printf(15, "Nothing to do so exiting\n");
     release_pigeon_coop_hole(cp->free_pending_tables, &pch);
     return(0);
  }

  cache_unlock(c);  //** Don't need the cache lock for the next part

  q = new_opque();
  opque_start_execution(q);

  //** Now cycle through the segments to be freed
  pending_bytes = 0;
  sit = list_iter_search(table, list_first_key(table), 0);
  list_next(&sit, (list_key_t **)&segid, (list_data_t **)&ptable);
  while (ptable != NULL) {
    //** Verify the segment is still valid.  If not then just delete everything
    pseg = list_search(c->segments, segid);
    if (pseg != NULL) {   //** VAlid segment if pseg is non-null
       segment_lock(ptable->seg);
       min_off = s->total_size;
       max_off = -1;

       s = (cache_segment_t *)ptable->seg->priv;
       while ((p = pop(ptable->stack)) != NULL) {
          atomic_dec(p->access_pending[CACHE_READ]); //** Removed my access control from earlier
          flush_count = atomic_get(p->access_pending[CACHE_FLUSH]);
          cw = atomic_get(p->access_pending[CACHE_WRITE]);
          count = atomic_get(p->access_pending[CACHE_READ]) + cw + flush_count;
          bits = atomic_get(p->bit_fields);
          if (count != 0) { //** Currently in use so wait for it to be released
             if (cw > 0) {  //** Got writes so need to wait until they complete otherwise the page may not get released
                bits = bits | C_TORELEASE;  //** Mark it for release
                atomic_set(p->bit_fields, bits);
                _cache_drain_writes(p->seg, p);  //** Drain the write ops
                bits = atomic_get(p->bit_fields);  //** Get the bit fields to see if it's dirty
             }

             if (flush_count == 0) {  //** Make sure it's not already being flushed
                if ((bits & C_ISDIRTY) != 0) {  //** Have to flush it don't have to track it cause the flush will do the release 
                   if (min_off > p->offset) min_off = p->offset;
                   if (max_off < p->offset) max_off = p->offset;
                }
             }
             bits = bits | C_TORELEASE;

log_printf(_amp_logging, "in use marking for release seg=" XIDT " p->offset=" XOT " bits=%d\n", segment_id(p->seg), p->offset, bits);
             atomic_set(p->bit_fields, bits);

             pending_bytes += s->page_size;
          } else {  //** Not in use
             if ((bits & (C_ISDIRTY|C_EMPTY)) == 0) {  //** Don't have to flush it just drop the page
                cp->limbo_pages--;
log_printf(_amp_logging, "FREEING page seg=" XIDT " p->offset=" XOT " bits=%d limbo=%d\n", segment_id(p->seg), p->offset, bits, cp->limbo_pages);
                list_remove(s->pages, &(p->offset), p);  //** Have to do this here cause p->offset is the key var
                if (p->data[0].ptr) free(p->data[0].ptr);
                if (p->data[1].ptr) free(p->data[1].ptr);
                lp = (page_amp_t *)p->priv;
                free(lp);
                freed_bytes += s->page_size;
             } else {         //** Got to flush the page first but don't have to track it cause the flush will do the release
                if (p->offset > -1) { //** Skip blank pages
                   if (min_off > p->offset) min_off = p->offset;
                   if (max_off < p->offset) max_off = p->offset;
                }

                bits = bits | C_TORELEASE;
                atomic_set(p->bit_fields, bits);

                pending_bytes += s->page_size;
if (p->offset > -1) {
  log_printf(15, "FLUSHING page seg=" XIDT " p->offset=" XOT " bits=%d\n", segment_id(p->seg), p->offset, bits);
} else {
  log_printf(15, "RELEASE trigger for empty page seg=" XIDT " p->offset=" XOT " bits=%d\n", segment_id(p->seg), p->offset, bits);
}
             }
          }

         list_next(&sit, (list_key_t **)&poff, (list_data_t **)&p);
       }

       segment_unlock(ptable->seg);

       if (max_off>-1) {
          if ((max_off - min_off) < 10*s->page_size) max_off = min_off + 10*s->page_size;
          gop = cache_flush_range(ptable->seg, s->c->da, min_off, max_off + s->page_size - 1, s->c->timeout);
          opque_add(q, gop);
       }
    } else {  //** Segment has been deleted so drop everything but undo the pending read so the cache_destroy will complete
log_printf(_amp_logging, "seg=" XIDT " looks to be removed so undoing the pending CACHE_READ for marked pages\n", segment_id(p->seg));
       segment_lock(ptable->seg);
       while ((p = pop(ptable->stack)) != NULL) {
          atomic_dec(p->access_pending[CACHE_READ]); //** Removed my access control from earlier
       }
       segment_unlock(ptable->seg);
    }

    cache_lock(c);
    release_pigeon_coop_hole(cp->free_page_tables, &(ptable->pch));
    cache_unlock(c);

    list_next(&sit, (skiplist_key_t **)&pseg, (skiplist_data_t **)&ptable);
  }

cache_lock(c);
log_printf(15, "BEFORE waitall seg=" XIDT " bytes_to_free=" XOT " bytes_used=" XOT " freed_bytes=" XOT " pending_bytes=" XOT "\n", 
    segment_id(page_seg), bytes_to_free, cp->bytes_used, freed_bytes, pending_bytes);
cache_unlock(c);


  //** Wait for any tasks to complete
  opque_waitall(q);
  opque_free(q, OP_DESTROY);

  //** Had this when we came in
  cache_lock(c);

log_printf(15, "AFTER waitall seg=" XIDT " bytes_used=" XOT "\n", segment_id(page_seg), cp->bytes_used);

  cp->bytes_used -= freed_bytes;  //** Update how much I directly freed

log_printf(15, "AFTER used update seg=" XIDT " bytes_used=" XOT "\n", segment_id(page_seg), cp->bytes_used);

  //** Clean up
  empty_skiplist(table);
  release_pigeon_coop_hole(cp->free_pending_tables, &pch);

log_printf(15, "total_bytes marked for removal =" XOT "\n", total_bytes);

  return(total_bytes);
}

//*************************************************************************
// _amp_force_free_mem - Frees page memory
//   Returns the number of bytes freed
//*************************************************************************

ex_off_t _amp_force_free_mem(cache_t *c, segment_t *page_seg, ex_off_t bytes_to_free, int check_waiters)
{
  cache_segment_t *s = (cache_segment_t *)page_seg->priv;
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;
  ex_off_t freed_bytes, bytes_left;
  int top, finished;
  pigeon_coop_hole_t pch;
  cache_cond_t *cache_cond;

  //** I'm holding this coming in but don't need it cause I can touch all segs
  segment_unlock(page_seg);

  top = 0;
  bytes_left = bytes_to_free;
  freed_bytes = _amp_attempt_free_mem(c, page_seg, bytes_left);
  finished = 0;

  while ((freed_bytes < bytes_to_free) && (finished == 0)) {  //** Keep trying to mark space as free until I get enough
     if (top == 0) {
        top = 1;
        pch = reserve_pigeon_coop_hole(s->c->cond_coop);
        cache_cond = (cache_cond_t *)pigeon_coop_hole_data(&pch);
        cache_cond->count = 0;

        move_to_bottom(cp->pending_free_tasks);
        insert_below(cp->pending_free_tasks, cache_cond);  //** Add myself to the bottom
     } else {
        push(cp->pending_free_tasks, cache_cond);  //** I go on the top
     }

log_printf(15, "not enough space so waiting cache_cond=%p freed_bytes=" XOT " bytes_to_free=" XOT " dirty=" XOT "\n", cache_cond, freed_bytes, bytes_to_free, c->stats.dirty_bytes);
     //** Now wait until it's my turn
     apr_thread_cond_wait(cache_cond->cond, c->lock);

     bytes_left -= freed_bytes;
     freed_bytes = _amp_attempt_free_mem(c, page_seg, bytes_left);
     finished = 1;
  }

  //** Now check if we can handle some waiters
  if (check_waiters == 1) _amp_process_waiters(c);

  cache_unlock(c);  //** Reacquire the lock in the proper order
  segment_lock(page_seg);  //** Reacquire the lock cause I had it coming in
  cache_lock(c);

  if (top == 1) release_pigeon_coop_hole(s->c->cond_coop, &pch);

  freed_bytes = bytes_to_free - bytes_left;
//NEW  freed_bytes = bytes_left - freed_bytes;

  return(freed_bytes);
}

//*************************************************************************
// _amp_wait_for_page - Waits for space to free up
//*************************************************************************

void _amp_wait_for_page(cache_t *c, segment_t *seg, int ontop)
{
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  amp_page_wait_t pw;
  pigeon_coop_hole_t pch;
  cache_cond_t *cc;
  ex_off_t bytes_free, bytes_needed, n;
  int check_waiters_first;

  check_waiters_first = (ontop == 0) ? 1 : 0;
  pch = reserve_pigeon_coop_hole(c->cond_coop);
  cc = (cache_cond_t *)pigeon_coop_hole_data(&pch);
  pw.cond = cc->cond;
  pw.bytes_needed = s->page_size;

  bytes_free = _amp_max_bytes(c) - cp->bytes_used;
  while (s->page_size > bytes_free) {
     //** Attempt to free pages
     bytes_needed = s->page_size - bytes_free;
     n = _amp_force_free_mem(c, seg, bytes_needed, check_waiters_first);

     if (n > 0) { //** Didn't make it so wait
        if (ontop == 0) {
           move_to_bottom(cp->waiting_stack);
           insert_below(cp->waiting_stack, &pw);
        } else {
           push(cp->waiting_stack, &pw);
        }

        segment_unlock(seg);  //** Unlock the segment to prevent deadlocks

        apr_thread_cond_wait(pw.cond, c->lock);  //** Wait for the space to become available

        //** Have to reaquire both locks in the correct order
        cache_unlock(c);
        segment_lock(seg);
        cache_lock(c);

        ontop = 1;  //** 2nd time we are always placed on the top of the stack
        check_waiters_first = 0;  //** And don't check on waiters
     }

     bytes_free = _amp_max_bytes(c) - cp->bytes_used;
  }

  release_pigeon_coop_hole(c->cond_coop, &pch);

  return;
}

//*************************************************************************
// amp_create_empty_page - Creates an empty page for use
//*************************************************************************

cache_page_t *amp_create_empty_page(cache_t *c, segment_t *seg, int doblock)
{
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  ex_off_t max_bytes, bytes_to_free;
  cache_page_t *p = NULL;
  int qend;

  cache_lock(c);

  qend = 0;
  do {
     max_bytes = _amp_max_bytes(c);
     bytes_to_free = s->page_size + cp->bytes_used - max_bytes;
log_printf(15, "amp_create_empty_page: max_bytes=" XOT " used=" XOT " bytes_to_free=" XOT " doblock=%d\n", max_bytes, cp->bytes_used, bytes_to_free, doblock);
     if (bytes_to_free > 0) {
        bytes_to_free = _amp_free_mem(c, seg, bytes_to_free);
        if ((doblock==1) && (bytes_to_free>0)) _amp_wait_for_page(c, seg, qend);
        qend = 1;
     }
  } while ((doblock==1) && (bytes_to_free>0));

  if (bytes_to_free <= 0) p = _amp_new_page(c, seg);

  cache_unlock(c);

  return(p);
}

//*************************************************************************
//  amp_update - Updates the cache prefetch informaion upon task completion
//*************************************************************************

void amp_update(cache_t *c, segment_t *seg, int rw_mode, ex_off_t lo, ex_off_t hi, void *miss_info)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  amp_stream_table_t *as = (amp_stream_table_t *)s->cache_priv;
  int prevp, npages;
  ex_off_t offset, *poff, nbytes;
  cache_page_t *p2;
  page_amp_t *lp2;
  amp_page_stream_t *pps, *ps;
  skiplist_iter_t it;

  if ((miss_info == NULL) || (rw_mode != CACHE_READ)) return;  //** Only used on a missed READ

log_printf(_amp_logging, "seg=" XIDT " initial lo=" XOT " hi=" XOT "\n", segment_id(seg), lo, hi);

  lo = lo / s->page_size; npages = lo; lo = lo * s->page_size;
  hi = hi / s->page_size; npages = hi - npages + 1; hi = hi * s->page_size;
  nbytes = npages * s->page_size;

  //** Get the missed offset and free the pointer
//  miss_off = *(ex_off_t *)miss_info;
  free(miss_info);

  //** Adjust the read range prefetch params
  segment_lock(seg);
  cache_lock(s->c);

  offset = lo - s->page_size;
  pps = _amp_stream_get(c, seg, offset, -1, NULL);
  prevp = (pps == NULL) ? 0 : pps->prefetch_size;
log_printf(_amp_logging, "seg=" XIDT " hi=" XOT " pps=%p prevp=%d npages=%d lo=" XOT " hi=" XOT "\n", segment_id(seg), hi, pps, prevp, npages, lo, hi);
  ps = _amp_stream_get(c, seg, hi, nbytes, NULL);
  ps->prefetch_size = prevp + npages;
  if (ps->prefetch_size > as->start_apt_pages) {
     ps->trigger_distance = as->start_apt_pages / 2;

     offset = hi - ps->trigger_distance * s->page_size;
     it = iter_search_skiplist(s->pages, &offset, 0);
     next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p2);
     if (p2) {
        if (*poff < hi) {
          lp2 = (page_amp_t *)p2->priv;
          lp2->bit_fields |= CAMP_TAG;
          lp2->stream_offset = ps->last_offset;
log_printf(_amp_logging, "seg=" XIDT " SET_TAG offset=" XOT " last=" XOT "\n", segment_id(seg), p2->offset, lp2->stream_offset);
        }
     }

  }

log_printf(_amp_logging, "seg=" XIDT " MODIFY ps=%p last_offset=" XOT " prefetch=%d trigger=%d\n", segment_id(seg), ps, ps->last_offset, ps->prefetch_size, ps->trigger_distance);

  //** and load the extra pages

  if (prevp > 0) {
     lo = hi + s->page_size;
     hi = lo + prevp * s->page_size - 1;
     _amp_prefetch(seg, lo, hi, pps->prefetch_size, pps->trigger_distance);
  }

  cache_unlock(s->c);
  segment_unlock(seg);


  return;
}

//*************************************************************************
//  amp_miss_tag - Dummy routine
//*************************************************************************

void amp_miss_tag(cache_t *c, segment_t *seg, int mode, ex_off_t lo, ex_off_t hi, ex_off_t missing_offset, void **miss)
{
  ex_off_t *off;
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;

  if (mode == CACHE_READ) {
    if (*miss != NULL) return;
log_printf(_amp_logging, "seg=" XIDT "  miss set offset=" XOT "\n", segment_id(seg), missing_offset);

    type_malloc(off, ex_off_t, 1);
    *off = missing_offset;
    *miss = off;
  } else {  //** For a write just trigger a dirty flush
     cache_lock(c);
     if (cp->flush_in_progress == 0) {
        cp->flush_in_progress = 1;
        apr_thread_cond_signal(cp->dirty_trigger);
     }
     cache_unlock(c);
  }

  return;
}

//*************************************************************************
//  amp_adding_segment - Called each time a segment is being added
//     NOTE: seg is locked but the cache is!
//*************************************************************************

void amp_adding_segment(cache_t *c, segment_t *seg)
{
  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  amp_stream_table_t *stable;
  int i;

  type_malloc(stable, amp_stream_table_t, 1);
  type_malloc_clear(stable->stream_table, amp_page_stream_t, cp->max_streams);

  stable->streams = list_create(0, &skiplist_compare_ex_off, NULL, NULL, NULL);
  stable->max_streams = cp->max_streams;
  stable->index = 0;
  stable->start_apt_pages = cp->async_prefetch_threshold / s->page_size;
  if (stable->start_apt_pages < 2) stable->start_apt_pages = 2;

log_printf(_amp_logging, "cp->min_prefetch_size=" XOT " start_apt_pages=%d\n", cp->min_prefetch_size, stable->start_apt_pages);
  for (i=0; i < stable->max_streams; i++) {
     stable->stream_table[i].last_offset = -i;
  }

  s->cache_priv = stable;

  return;
}

//*************************************************************************
//  amp_removing_segment - Called each time a segment is being removed
//     NOTE: seg is locked but the cache is!
//*************************************************************************

void amp_removing_segment(cache_t *c, segment_t *seg)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  amp_stream_table_t *stable = (amp_stream_table_t *)s->cache_priv;

  list_destroy(stable->streams);
  free(stable->stream_table);

  free(stable);

  return;
}

//*************************************************************************
// amp_cache_destroy - Destroys the cache structure.
//     NOTE: Data is not flushed!
//*************************************************************************

int amp_cache_destroy(cache_t *c)
{
  apr_status_t value;

  cache_amp_t *cp = (cache_amp_t *)c->fn.priv;

  //** Shutdown the dirty thread
  cache_lock(c);
  c->shutdown_request = 1;
  apr_thread_cond_signal(cp->dirty_trigger);
  cache_unlock(c);

  apr_thread_join(&value, cp->dirty_thread);  //** Wait for it to complete

  cache_base_destroy(c);

  free_stack(cp->stack, 0);
  free_stack(cp->waiting_stack, 0);
  free_stack(cp->pending_free_tasks, 0);

  destroy_pigeon_coop(cp->free_pending_tables);
  destroy_pigeon_coop(cp->free_page_tables);

  free(cp);
  free(c);

  return(0);
}


//*************************************************************************
// amp_cache_create - Creates an empty amp cache structure
//*************************************************************************

cache_t *amp_cache_create(void *arg, data_attr_t *da, int timeout)
{
  cache_t *cache;
  cache_amp_t *c;

  type_malloc_clear(cache, cache_t, 1);
  type_malloc_clear(c, cache_amp_t, 1);
  cache->fn.priv = c;

  cache_base_create(cache, da, timeout);

  cache->shutdown_request = 0;
  c->stack = new_stack();
  c->waiting_stack = new_stack();
  c->pending_free_tasks = new_stack();
  c->max_bytes = 100*1024*1024;
  c->max_streams = 500;
  c->bytes_used = 0;
  c->prefetch_in_process = 0;
  c->dirty_fraction = 0.1;
  c->async_prefetch_threshold = 256*1024*1024;
  c->min_prefetch_size = 1024*1024;
  cache->max_fetch_fraction = 0.1;
  cache->max_fetch_size = cache->max_fetch_fraction * c->max_bytes;
  cache->write_temp_overflow_used = 0;
  cache->write_temp_overflow_fraction = 0.01;
  cache->write_temp_overflow_size = cache->write_temp_overflow_fraction * c->max_bytes;

  c->dirty_bytes_trigger = c->dirty_fraction * c->max_bytes;
  c->dirty_max_wait = apr_time_make(1, 0);
  c->flush_in_progress = 0;
  c->limbo_pages = 0;
  c->free_pending_tables = new_pigeon_coop("free_pending_tables", 50, sizeof(list_t *), cache->mpool, free_pending_table_new, free_pending_table_free);
  c->free_page_tables = new_pigeon_coop("free_page_tables", 50, sizeof(page_table_t), cache->mpool, free_page_tables_new, free_page_tables_free);

  cache->fn.create_empty_page = amp_create_empty_page;
  cache->fn.adjust_dirty = amp_adjust_dirty;
  cache->fn.destroy_pages = amp_pages_destroy;
  cache->fn.cache_update = amp_update;
  cache->fn.cache_miss_tag = amp_miss_tag;
  cache->fn.s_page_access = amp_page_access;
  cache->fn.s_pages_release = amp_pages_release;
  cache->fn.destroy = amp_cache_destroy;
  cache->fn.adding_segment = amp_adding_segment;
  cache->fn.removing_segment = amp_removing_segment;

  apr_thread_cond_create(&(c->dirty_trigger), cache->mpool);
  apr_thread_create(&(c->dirty_thread), NULL, amp_dirty_thread, (void *)cache, cache->mpool);

  return(cache);
}


//*************************************************************************
// amp_cache_load -Creates and configures an amp cache structure
//*************************************************************************

cache_t *amp_cache_load(void *arg, data_attr_t *da, int timeout, char *fname, char *grp)
{
  cache_t *c;
  cache_amp_t *cp;
  inip_file_t *fd;
  int dt;

  if (grp == NULL) grp = "cache-amp";

  //** Create the default structure
  c = amp_cache_create(arg, da, timeout);
  cp = (cache_amp_t *)c->fn.priv;

  //** Parse the ini text
  fd = inip_read(fname);

  cache_lock(c);
  cp->max_bytes = inip_get_integer(fd, grp, "max_bytes", cp->max_bytes);
  cp->max_streams = inip_get_integer(fd, grp, "max_streams", cp->max_streams);
  cp->dirty_fraction = inip_get_double(fd, grp, "dirty_fraction", cp->dirty_fraction);
  cp->dirty_bytes_trigger = cp->dirty_fraction * cp->max_bytes;
  c->default_page_size = inip_get_integer(fd, grp, "default_page_size", c->default_page_size);
  cp->async_prefetch_threshold = inip_get_integer(fd, grp, "async_prefetch_threshold", cp->async_prefetch_threshold);
  cp->min_prefetch_size = inip_get_integer(fd, grp, "min_prefetch_bytes", cp->min_prefetch_size);
  dt = inip_get_integer(fd, grp, "dirty_max_wait", apr_time_sec(cp->dirty_max_wait));
  cp->dirty_max_wait = apr_time_make(dt, 0);
  c->max_fetch_fraction = inip_get_double(fd, grp, "max_fetch_fraction", c->max_fetch_fraction);
  c->max_fetch_size = c->max_fetch_fraction * cp->max_bytes;
  c->write_temp_overflow_fraction = inip_get_double(fd, grp, "write_temp_overflow_fraction", c->write_temp_overflow_fraction);
  c->write_temp_overflow_size = c->write_temp_overflow_fraction * cp->max_bytes;
  cache_unlock(c);

  inip_destroy(fd);

  return(c);
}