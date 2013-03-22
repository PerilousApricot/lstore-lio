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

#define _log_module_index 161

#include <apr_thread_mutex.h>
#include <apr_thread_cond.h>
#include "cache.h"
#include "type_malloc.h"
#include "log.h"
#include "append_printf.h"
#include "ex3_abstract.h"
#include "segment_cache.h"
#include "string_token.h"
#include "ex3_system.h"
#include "ex3_compare.h"

typedef struct {
  segment_t *seg;
  data_attr_t *da;
  ex_off_t new_size;
  int timeout;
} cache_truncate_op_t;

typedef struct {
  segment_t *seg;
  tbuffer_t *buf;
  data_attr_t *da;
  ex_off_t   boff;
  ex_iovec_t *iov;
  ex_iovec_t iov_single;
  int        rw_mode;
  int        n_iov;
  int skip_ppages;
  int timeout;
} cache_rw_op_t;

typedef struct {
  segment_t *seg;
  ex_off_t lo;
  ex_off_t hi;
  int rw_mode;
  int force_wait;
  page_handle_t *page;
  int *n_pages;
} cache_advise_op_t;

typedef struct {
  op_generic_t *gop;
  iovec_t *iov;
  page_handle_t *page;
  ex_iovec_t ex_iov;
  ex_off_t nbytes;
  tbuffer_t buf;
  int n_iov;
  int myid;
} cache_rw_iovec_t;

typedef struct {
  segment_t *sseg;
  segment_t *dseg;
  op_generic_t *gop;
} cache_clone_t;

atomic_int_t _cache_count = 0;
atomic_int_t _flush_count = 0;

op_status_t cache_rw_func(void *arg, int id);

//*************************************************************
// cache_cond_new - Creates a new shelf of cond variables
//*************************************************************

void *cache_cond_new(void *arg, int size)
{
  apr_pool_t *mpool = (apr_pool_t *)arg;
  cache_cond_t *shelf;
  int i;

  type_malloc_clear(shelf, cache_cond_t, size);

log_printf(15, "cache_cond_new: making new shelf of size %d\n", size);
  for (i=0; i<size; i++) {
    apr_thread_cond_create(&(shelf[i].cond), mpool);
  }

  return((void *)shelf);
}

//*************************************************************
// cache_cond_free - Destroys a new shelf of cond variables
//*************************************************************

void cache_cond_free(void *arg, int size, void *data)
{
//  apr_pool_t *mpool = (apr_pool_t *)arg;
  cache_cond_t *shelf = (cache_cond_t *)data;
  int i;

log_printf(15, "cache_cond_free: destroying shelf of size %d\n", size);

  for (i=0; i<size; i++) {
    apr_thread_cond_destroy(shelf[i].cond);
  }

  free(shelf);
  return;
}

//*******************************************************************************
//  cache_new_range - Makes a new cache range object
//*******************************************************************************

cache_range_t *cache_new_range(ex_off_t lo, ex_off_t hi, ex_off_t boff, int iov_index)
{
  cache_range_t *r;

  type_malloc(r, cache_range_t, 1);

  r->lo = lo;
  r->hi = hi;
  r->boff = boff;
  r->iov_index = iov_index;

  return(r);
}

//*******************************************************************************
//  flush_wait - Waits for pending flushes to complete
//*******************************************************************************

void flush_wait(segment_t *seg, ex_off_t *my_flush)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  int finished;
  ex_off_t *check;

  segment_lock(seg);

  do {
    finished = 1;
    move_to_bottom(s->flush_stack);
    while ((check = (ex_off_t *)get_ele_data(s->flush_stack)) != NULL) {
//log_printf(0, "check[2]=" XOT " me[2]=" XOT "\n", check[2], my_flush[2]);

      if (check[2] < my_flush[2]) {
         if ((check[0] <= my_flush[0]) && (check[1] >= my_flush[0])) {
            finished = 0;
            break;
         } else if ((check[0] > my_flush[0]) && (check[0] <= my_flush[1])) {
            finished = 0;
            break;
         }
      }
      move_up(s->flush_stack);
    }
    if (finished == 0) apr_thread_cond_wait(s->flush_cond, seg->lock);
  } while (finished == 0);

  segment_unlock(seg);

  return;
}

//*******************************************************************************
// full_page_overlap - Returns 1 if the range fully overlaps with the given page
//     and 0 otherwise
//*******************************************************************************

int full_page_overlap(ex_off_t poff, ex_off_t psize, ex_off_t lo, ex_off_t hi)
{
  ex_off_t phi;

  phi = poff + psize - 1;
  if ((lo <= poff) && (phi <= hi)) {
     log_printf(15, "FULL_PAGE prange=(" XOT ", " XOT ") (lo,hi)=(" XOT ", " XOT ")\n", poff, phi, lo, hi);
     return(1);
  }

  log_printf(15, "PARTIAL_PAGE prange=(" XOT ", " XOT ") (lo,hi)=(" XOT ", " XOT ")\n", poff, phi, lo, hi);

  return(0);
}

//*******************************************************************************
//  _cache_drain_writes - Waits for all writes to complete.
//
//  NOTE: Assumes segment is locked and the C_TORELEASE flag is set
//*******************************************************************************

void _cache_drain_writes(segment_t *seg, cache_page_t *p)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  cache_cond_t *cache_cond;
  int count, bits;

int cr, cw, cf;
cr = atomic_get(p->access_pending[CACHE_READ]);
cw = atomic_get(p->access_pending[CACHE_WRITE]);
cf =  atomic_get(p->access_pending[CACHE_FLUSH]);
bits =  atomic_get(p->bit_fields);
log_printf(15, "seg=" XIDT " START p->offset=" XOT " cr=%d cw=%d cf=%d bit_fields=%d\n", segment_id(seg),p->offset, cr, cw, cf, bits);

  cache_cond = (cache_cond_t *)pigeon_coop_hole_data(&(p->cond_pch));
  if (cache_cond == NULL) {
     p->cond_pch = reserve_pigeon_coop_hole(s->c->cond_coop);
     cache_cond = (cache_cond_t *)pigeon_coop_hole_data(&(p->cond_pch));
     cache_cond->count = 0;
  }

  cache_cond->count++;
  count = atomic_get(p->access_pending[CACHE_WRITE]);
  bits = atomic_get(p->bit_fields);
  while ((count > 0) || ((bits & C_EMPTY) > 0)) {
     apr_thread_cond_wait(cache_cond->cond, seg->lock);
     count = atomic_get(p->access_pending[CACHE_WRITE]);
     bits = atomic_get(p->bit_fields);
  }

cr = atomic_get(p->access_pending[CACHE_READ]);
cw = atomic_get(p->access_pending[CACHE_WRITE]);
cf =  atomic_get(p->access_pending[CACHE_FLUSH]);
bits =  atomic_get(p->bit_fields);
log_printf(15, "seg=" XIDT " END p->offset=" XOT " cr=%d cw=%d cf=%d bit_fields=%d\n", segment_id(seg),p->offset, cr, cw, cf, bits);

  cache_cond->count--;
  if (cache_cond->count <= 0) release_pigeon_coop_hole(s->c->cond_coop, &(p->cond_pch));

}

//*******************************************************************************
//  _cache_wait_for_page - Waits for a page to become accessible.
//
//  NOTE: Assumes segment is locked and the appropriate access_pending is set
//*******************************************************************************

void _cache_wait_for_page(segment_t *seg, int rw_mode, cache_page_t *p)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  cache_cond_t *cache_cond;
  int count, bits;

  cache_cond = (cache_cond_t *)pigeon_coop_hole_data(&(p->cond_pch));
  if (cache_cond == NULL) {
     p->cond_pch = reserve_pigeon_coop_hole(s->c->cond_coop);
     cache_cond = (cache_cond_t *)pigeon_coop_hole_data(&(p->cond_pch));
     cache_cond->count = 0;
  }

int cr, cw, cf;
cr = atomic_get(p->access_pending[CACHE_READ]);
cw = atomic_get(p->access_pending[CACHE_WRITE]);
cf =  atomic_get(p->access_pending[CACHE_FLUSH]);
bits =  atomic_get(p->bit_fields);
log_printf(15, "seg=" XIDT " START rw_mode=%d p->offset=" XOT " cr=%d cw=%d cf=%d bit_fields=%d\n", segment_id(seg), rw_mode, p->offset, cr, cw, cf, bits);

  cache_cond->count++;
  if (rw_mode == CACHE_WRITE) {
     count = atomic_get(p->access_pending[CACHE_FLUSH]);
     bits = atomic_get(p->bit_fields);
     while ((count > 0) || ((bits & C_EMPTY) > 0)) {
         apr_thread_cond_wait(cache_cond->cond, seg->lock);
         count = atomic_get(p->access_pending[CACHE_FLUSH]);
         bits = atomic_get(p->bit_fields);
     }
  } else {
     bits = atomic_get(p->bit_fields);
     while ((bits & C_EMPTY) > 0) {
         apr_thread_cond_wait(cache_cond->cond, seg->lock);
         bits = atomic_get(p->bit_fields);
     }
  }

cr = atomic_get(p->access_pending[CACHE_READ]);
cw = atomic_get(p->access_pending[CACHE_WRITE]);
cf =  atomic_get(p->access_pending[CACHE_FLUSH]);
bits =  atomic_get(p->bit_fields);
log_printf(15, "seg=" XIDT " END rw_mode=%d p->offset=" XOT " cr=%d cw=%d cf=%d bit_fields=%d\n", segment_id(seg), rw_mode, p->offset, cr, cw, cf, bits);

  cache_cond->count--;
  if (cache_cond->count <= 0) release_pigeon_coop_hole(s->c->cond_coop, &(p->cond_pch));
}


//*******************************************************************************
// s_cache_page_init - Initializes a cache page for use and addes it to the segment page list
//*******************************************************************************

void s_cache_page_init(segment_t *seg, cache_page_t *p, ex_off_t poff)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  int bits;

log_printf(15, "s_cache_page_init: seg=" XIDT " p->offset=" XOT " start->offset=" XOT "\n", segment_id(seg), poff, p->offset);
  p->seg = seg;
  p->offset = poff;
//  memset(&(p->cond_pch), 0, sizeof(pigeon_coop_hole_t));
  atomic_set(p->used_count, 0);

  bits = C_EMPTY;
  atomic_set(p->bit_fields, bits);

  list_insert(s->pages, &(p->offset), p);

int cr, cw, cf;
cr = atomic_get(p->access_pending[CACHE_READ]);
cw = atomic_get(p->access_pending[CACHE_WRITE]);
cf =  atomic_get(p->access_pending[CACHE_FLUSH]);
bits =  atomic_get(p->bit_fields);
log_printf(15, "seg=" XIDT " init p->offset=" XOT " cr=%d cw=%d cf=%d bit_fields=%d\n", segment_id(seg),p->offset, cr, cw, cf, bits);

}

//*******************************************************************************
//  cache_rw_pages - Reads or Writes pages on the given segment.  Optionally releases the pages
//*******************************************************************************

int cache_rw_pages(segment_t *seg, page_handle_t *plist, int pl_size, int rw_mode, int do_release)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  page_handle_t *ph;
  cache_rw_iovec_t *cio;
  opque_t *q;
  op_generic_t *gop;
  cache_cond_t *cache_cond;
  iovec_t iovec[pl_size];
  page_handle_t blank_pages[pl_size];
  cache_counters_t cc;
  int error_count, blank_count;
  int myid, n, i, j, pli, contig_start, bits;
  ex_off_t off, last_page, contig_last;

log_printf(15, "START pl_size=%d\n", pl_size);

  if (pl_size == 0) return(0);

  memset(&cc, 0, sizeof(cc));  //** Reset the counters

  error_count = 0;
  blank_count = 0;
  last_page = -1;

  //** Figure out the contiguous blocks
  q = new_opque();
  opque_start_execution(q);  //** Go ahead and start executing tasks as the are created
  myid = -1;
//  for (pli=0; pli<pl_size; pli++) {
  pli = 0;
  while (pli<pl_size) {
    if (plist[pli].data->ptr != NULL) { break; } //** Kick out if not NULL
    log_printf(15, "skipping NULL page p->offset=" XOT "\n", plist[pli].p->offset); //** Skip error pages
    blank_pages[blank_count] = plist[pli]; blank_count++;
    pli++;
  }
  contig_start = pli;
  if (pli < pl_size) off = plist[pli].p->offset;
  while (pli<pl_size) {
     ph = &(plist[pli]);
     if ((ph->p->offset != off) || (ph->data == NULL)) {  //** Continuity break so bundle up the ops into a single command
        myid++;
        n = pli - contig_start;
        type_malloc(cio, cache_rw_iovec_t, 1);
        cio->n_iov = n;
        cio->myid = myid;
        cio->nbytes = s->page_size * n;
        cio->page = &(plist[contig_start]);
        cio->iov = &(iovec[contig_start]);

log_printf(15, "cache_rw_pages: rw_mode=%d pli=%d contig_start=%d n=%d start_offset=" XOT "\n", rw_mode, pli, contig_start, n, plist[contig_start].p->offset);

        for (i=0; i<n; i++) {
           cio->iov[i].iov_base = plist[contig_start+i].data->ptr;
           cio->iov[i].iov_len = s->page_size;
log_printf(15, "cache_rw_pages: rw_mode=%d i=%d offset=" XOT "\n", rw_mode, i, plist[contig_start+i].p->offset);
        }

        tbuffer_vec(&(cio->buf), cio->nbytes, cio->n_iov, cio->iov);
        ex_iovec_single(&(cio->ex_iov), plist[contig_start].p->offset, cio->nbytes);
        if (rw_mode == CACHE_READ) {
           cc.read_count++;
           cc.read_bytes += cio->nbytes;
           cio->gop = segment_read(s->child_seg, s->c->da, 1, &(cio->ex_iov), &(cio->buf), 0, s->c->timeout);
        } else {
           cc.write_count++;
           cc.write_bytes += cio->nbytes;
           cio->gop = segment_write(s->child_seg, s->c->da, 1, &(cio->ex_iov), &(cio->buf), 0, s->c->timeout);
        }
log_printf(2, "rw_mode=%d gid=%d offset=" XOT " len=" XOT "\n", rw_mode, gop_id(cio->gop), plist[contig_start].p->offset, cio->nbytes);
flush_log();

        gop_set_myid(cio->gop, myid);
        gop_set_private(cio->gop, (void *)cio);
        opque_add(q, cio->gop);

        //** Skip error pages
        while (pli<pl_size) {
          if (plist[pli].data != NULL) { break; } //** Kick out if not NULL
          log_printf(15, "skipping NULL page p->offset=" XOT "\n", plist[pli].p->offset); //** Skip error pages
          blank_pages[blank_count] = plist[pli]; blank_count++;
          pli++;
        }
        contig_start = pli;
        if (pli <pl_size)  {
           ph = &(plist[pli]);
           pli++; //** Start checking with the next page
        }
     } else {
       pli++;
     }

     off = ph->p->offset + s->page_size;
  }


  //** Handle the last chunk if needed

  n = pl_size - contig_start;
  if (n > 0) {
     myid++;
     type_malloc(cio, cache_rw_iovec_t, 1);
     cio->n_iov = n;
     cio->myid = myid;
     cio->nbytes = s->page_size * n;
     cio->iov = &(iovec[contig_start]);
     cio->page = &(plist[contig_start]);
log_printf(15, "cache_rw_pages: end rw_mode=%d pli=%d contig_start=%d n=%d start_offset=" XOT " iov[0]=%p\n", rw_mode, pli, contig_start, n, plist[contig_start].p->offset, cio->iov);

     for (i=0; i<n; i++) {
        cio->iov[i].iov_base = plist[contig_start+i].data->ptr;
        cio->iov[i].iov_len = s->page_size;
log_printf(15, "cache_rw_pages: end rw_mode=%d i=%d offset=" XOT "\n", rw_mode, i, plist[contig_start+i].p->offset);
     }

     tbuffer_vec(&(cio->buf), cio->nbytes, cio->n_iov, cio->iov);
     ex_iovec_single(&(cio->ex_iov), plist[contig_start].p->offset, cio->nbytes);  //** Last page is the starting point
     if (rw_mode == CACHE_READ) {
        cc.read_count++;
        cc.read_bytes += cio->nbytes;
        cio->gop = segment_read(s->child_seg, s->c->da, 1, &(cio->ex_iov), &(cio->buf), 0, s->c->timeout);
     } else {
        cc.write_count++;
        cc.write_bytes += cio->nbytes;
        cio->gop = segment_write(s->child_seg, s->c->da, 1, &(cio->ex_iov), &(cio->buf), 0, s->c->timeout);
     }
log_printf(2, "end rw_mode=%d gid=%d offset=" XOT " len=" XOT "\n", rw_mode, gop_id(cio->gop), plist[contig_start].p->offset, cio->nbytes);
log_printf(15, "end rw_mode=%d myid=%d gid=%d\n", rw_mode, myid, gop_id(cio->gop));
flush_log();

     gop_set_myid(cio->gop, myid);
     gop_set_private(cio->gop, (void *)cio);

     opque_add(q, cio->gop);
  }

  //** Dump the blank pages
  if (blank_count > 0) {
log_printf(15, "Dumping blank pages blank_count=%d\n", blank_count);
     segment_lock(seg);
     for (i=0; i<blank_count; i++) {
        ph = &(blank_pages[i]);
        bits = atomic_get(ph->p->bit_fields);
        if ((bits & C_EMPTY) > 0) {
           bits = bits ^ C_EMPTY;
           atomic_set(ph->p->bit_fields, bits);
        }
        cache_cond = (cache_cond_t *)pigeon_coop_hole_data(&(ph->p->cond_pch));
        if (cache_cond != NULL) {  //** Someone is listening so wake them up
           apr_thread_cond_broadcast(cache_cond->cond);
        }
     }
     segment_unlock(seg);

     if (do_release == 1) cache_release_pages(blank_count, blank_pages, rw_mode);
  }

  //** Process tasks as they complete
  n = opque_task_count(q);
log_printf(15, "cache_rw_pages: total tasks=%d\n", n); flush_log();

  for (i=0; i<n; i++) {
     gop = opque_waitany(q);
     myid= gop_get_myid(gop);
log_printf(15, "cache_rw_pages: myid=%d gid=%d completed\n", myid, gop_id(gop)); flush_log();

     cio = gop_get_private(gop);
     if (gop_completed_successfully(gop) != OP_STATE_SUCCESS) {
log_printf(15, "cache_rw_pages: myid=%d gid=%d completed with errors!\n", myid, gop_id(gop)); flush_log();

        if (rw_mode == CACHE_READ) {
           for (j=0; j<cio->n_iov; j++) {
log_printf(15, "error with read nullifying data p->offset=" XOT "\n", cio->page[j].p->offset);
              free(cio->page[j].data->ptr);  //** Errors are signified by data=NULL;
              error_count++;
              cio->page[j].data->ptr = NULL;
           }
        }
     }

     contig_last = cio->page[cio->n_iov-1].p->offset;
     if (last_page < contig_last) last_page = contig_last;  //** Keep track of the largest page

     segment_lock(seg);
     if ((rw_mode != CACHE_READ) && (last_page > s->child_last_page)) s->child_last_page = last_page;
//     if (rw_mode != CACHE_READ) s->child_last_page = (segment_size(s->child_seg) / s->page_size) * s->page_size;

     for (j=0; j<cio->n_iov; j++) {
        bits = atomic_get(cio->page[j].p->bit_fields);
        if ((bits & C_EMPTY) > 0) {
           bits = bits ^ C_EMPTY;
           atomic_set(cio->page[j].p->bit_fields, bits);
        }
        ph = &(cio->page[j]);
        cache_cond = (cache_cond_t *)pigeon_coop_hole_data(&(ph->p->cond_pch));
        if (cache_cond != NULL) {  //** Someone is listening so wake them up
           apr_thread_cond_broadcast(cache_cond->cond);
        }
     }
     segment_unlock(seg);

     if (do_release == 1) cache_release_pages(cio->n_iov, cio->page, rw_mode);

     gop_free(gop, OP_DESTROY);
     free(cio);
  }

  //** And final clean up
  opque_free(q, OP_DESTROY);

log_printf(15, "END error_count=%d blank_count=%d rw_mode=%d\n", error_count, blank_count, rw_mode);
  return(error_count);
}

//*******************************************************************************
// cache_page_force_get - Waits until the requested page is loaded
//*******************************************************************************

cache_page_t  *cache_page_force_get(segment_t *seg, int rw_mode, ex_off_t poff, ex_off_t lo, ex_off_t hi)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  cache_page_t *p, *p2;
  page_handle_t ph;
  ex_off_t off_row;
  int bits;
  int cr, cw, cf;

  off_row = poff / s->page_size; off_row = off_row * s->page_size;

  segment_lock(seg); //** Now get the lock

  p = list_search(s->pages, (skiplist_key_t *)(&off_row));
log_printf(15, "cache_page_force_get: seg=" XIDT " offset=" XOT " p=%p count=%d\n", segment_id(seg), poff, p, skiplist_key_count(s->pages));
flush_log();
  if (p == NULL) {  //** New page so may need to load it
log_printf(15, "seg=" XIDT " offset=" XOT ". Not there so create it. count=%d\n", segment_id(seg), poff, skiplist_key_count(s->pages));
flush_log();
     p = s->c->fn.create_empty_page(s->c, seg, 1);  //** Get the empty page
     p->seg = seg;

     //** During the page creation we may have released and reacquired the lock letting another thread insert the page
     p2 = list_search(s->pages, (skiplist_key_t *)(&off_row));
     if (p2 == NULL) {    //** Not inserted so I do it
        s_cache_page_init(seg, p, off_row);  //** Add the page
        atomic_inc(p->access_pending[rw_mode]);  //** and mark it for my access mode

log_printf(15, "seg=" XIDT " rw_mode=%d offset=" XOT ". child_last_page=" XOT "\n", segment_id(seg), rw_mode, p->offset, s->child_last_page);

        if (rw_mode == CACHE_READ) {
           if (s->child_last_page >= p->offset) {  //** Data exists on disk so get it
log_printf(15, "seg=" XIDT " CACHE_RW_PAGES rw_mode=%d offset=" XOT ". child_last_page=" XOT "\n", segment_id(seg), rw_mode, p->offset, s->child_last_page);
              ph.p = p;
              ph.data = p->curr_data;
              p->curr_data->usage_count++;
              segment_unlock(seg);  //** Now prep it
              cache_rw_pages(seg, &ph, 1, CACHE_READ, 0);
              segment_lock(seg);
              ph.data->usage_count--;
           } else {   //** No data on disk yet and if not in memory then it's all zero's so flag it as such
             bits = 0;
             atomic_set(p->bit_fields, bits);
           }
        } else if (rw_mode == CACHE_WRITE) {
          if (full_page_overlap(p->offset, s->page_size, lo, hi) == 0) { //** Determine if I need to load the page
             if (s->child_last_page >= p->offset) {
log_printf(15, "seg=" XIDT " CACHE_RW_PAGES rw_mode=%d offset=" XOT ". child_last_page=" XOT "\n", segment_id(seg), rw_mode, p->offset, s->child_last_page);
                ph.p = p;
                ph.data = p->curr_data;
                p->curr_data->usage_count++;
                segment_unlock(seg);  //** Now prep it
                cache_rw_pages(seg, &ph, 1, CACHE_READ, 0);
                segment_lock(seg);
                ph.data->usage_count--;
             }
          }
        }
     } else {   //** Somebody else beat me to it so wait until the data is available
       s->c->fn.destroy_pages(s->c, &p, 1, 0);  //** Destroy my page
       p = p2;
log_printf(15, "cache_page_force_get: seg=" XIDT " offset=" XOT " rw_mode=%d. Already exists so wait for it to become accessible\n", segment_id(seg), poff, rw_mode);

       atomic_inc(p->access_pending[CACHE_READ]);  //** Use a read to hold the page
       _cache_wait_for_page(seg, rw_mode, p);
       atomic_inc(p->access_pending[rw_mode]);
       atomic_dec(p->access_pending[CACHE_READ]);
     }

     segment_unlock(seg); //** Now release  the lock

  } else {  //** Page already exists so wait for it to be filled if needed
log_printf(15, "cache_page_force_get: seg=" XIDT " offset=" XOT " rw_mode=%d. Already exists so wait for it to become free\n", segment_id(seg), poff, rw_mode);
     atomic_inc(p->access_pending[CACHE_READ]);  //** Use a read to hold the page
     _cache_wait_for_page(seg, rw_mode, p);
     atomic_inc(p->access_pending[rw_mode]);
     atomic_dec(p->access_pending[CACHE_READ]);

    segment_unlock(seg);
  }


if (p!= NULL) {
  cr = atomic_get(p->access_pending[CACHE_READ]);
  cw = atomic_get(p->access_pending[CACHE_WRITE]);
  cf =  atomic_get(p->access_pending[CACHE_FLUSH]);
  bits =  atomic_get(p->bit_fields);
  log_printf(15, "PAGE_GET seg=" XIDT " get p->offset=" XOT " cr=%d cw=%d cf=%d bit_fields=%d usage=%d index=%d\n", segment_id(seg), p->offset, cr, cw, cf, bits, p->curr_data->usage_count, p->current_index);

}

  return(p);
}


//*******************************************************************************
// cache_advise_fn - Performs the cache_advise function
//*******************************************************************************

op_status_t cache_advise_fn(void *arg, int id)
{
  cache_advise_op_t *ca = (cache_advise_op_t *)arg;
  segment_t *seg = ca->seg;
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  ex_off_t lo_row, hi_row, nbytes, *poff, coff, poff2;
  cache_page_t *p, *p2, *np;
  skiplist_iter_t it;
  int err, max_pages;

  //** Only works for READ ops
  if (ca->rw_mode != CACHE_READ) { *ca->n_pages = 0; return(op_success_status); }

  //** Check if we have a huge range.  If so truncate it to the max allowed
//  nbytes = ca->hi - ca->lo + 1;
//  coff = (nbytes > s->c->max_advise_size) ? ca->lo + s->c->max_advise_size - 1 : ca->hi;

//log_printf(15, "seg=" XIDT " max_advise_size=" XOT " frac=%lf\n", segment_id(seg), s->c->max_advise_size, s->c->max_advise_fraction);

  //** Map the range to the page boundaries
  lo_row = ca->lo / s->page_size; nbytes = lo_row; lo_row = lo_row * s->page_size;
  hi_row = ca->hi / s->page_size; nbytes = hi_row - nbytes + 1; hi_row = hi_row * s->page_size;
  nbytes = nbytes * s->page_size;

  //** Figure out if any pages need to be loaded

log_printf(15, "START seg=" XIDT " lo=" XOT " hi=" XOT "\n", segment_id(seg), ca->lo, ca->hi);

  max_pages = *ca->n_pages;
  *ca->n_pages = 0;

  segment_lock(seg);

  //** Generate the page list to load
  coff = lo_row;
  err = 0;
  it = iter_search_skiplist(s->pages, &lo_row, 0);
  for (coff = lo_row; coff <= hi_row; coff += s->page_size) {
     //** Make sure the next page matches coff
     next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);
     err = 0;
     if (p == NULL) {  //** End of range and no pages
log_printf(15, "seg=" XIDT " coff=" XOT "p->offset=NULL err=1\n", segment_id(seg), coff); flush_log();
        err = 1;
     } else if (p->offset != coff) {  //** Missing page
        err = 1;
log_printf(15, "seg=" XIDT " coff=" XOT "p->offset=" XOT " err=1\n", segment_id(seg), coff, p->offset); flush_log();
}else {
log_printf(15, "seg=" XIDT " coff=" XOT "p->offset=" XOT " err=0\n", segment_id(seg), coff, p->offset); flush_log();
     }

     //** If needed add the empty page
     if (err == 1) {
log_printf(15, "seg=" XIDT " attempting to create page coff=" XOT "\n", segment_id(seg), coff); flush_log();
        np = s->c->fn.create_empty_page(s->c, seg, 0);  //** Get the empty page
        if ((np == NULL) && (ca->force_wait == 1) && (*ca->n_pages == 0)) {  //**may need to force a page to be created
              np = s->c->fn.create_empty_page(s->c, seg, ca->force_wait);  //** Get the empty page
        }
log_printf(15, "seg=" XIDT " after attempt to create page coff=" XOT " new_page=%p\n", segment_id(seg), coff, np); flush_log();
        if (np != NULL) { //** This was an opportunistic request so it could be denied
           //** During the page creation we may have released and reacquired the lock letting another thread insert the page
           p2 = list_search(s->pages, (skiplist_key_t *)(&coff));
           if (p2 == NULL) {    //** Not inserted so I do it
              s_cache_page_init(seg, np, coff);
              atomic_inc(np->access_pending[ca->rw_mode]);
              ca->page[*ca->n_pages].p = np;
              ca->page[*ca->n_pages].data = np->curr_data;
              np->curr_data->usage_count++;

              (*ca->n_pages)++;
              if (*ca->n_pages >= max_pages) break;
           } else {   //** Somebody else beat me to it so skip it
log_printf(15, "seg=" XIDT " duplicate page for coff=" XOT "\n", segment_id(seg), coff); flush_log();
              s->c->fn.destroy_pages(s->c, &np, 1, 0);  //** Destroy my page
           }
        } else {
log_printf(15, "seg=" XIDT " cant find the space for coff=" XOT " so stopping scan\n", segment_id(seg), coff); flush_log();
          break;
        }

        //** Tried to add the page and lost/reacuired the lock so reposition the iterator
        poff2 = coff + s->page_size;
        it = iter_search_skiplist(s->pages, &poff2, 0);
     } else {
        break;  //** Hit a valid page.
     }
  }

  segment_unlock(seg);

  if (*ca->n_pages > 0) cache_rw_pages(seg, ca->page, *(ca->n_pages), ca->rw_mode, 0);

//  cache_release_pages(n, parray, ca->rw_mode);

log_printf(15, "END seg=" XIDT " lo=" XOT " hi=" XOT " n_pages=%d\n", segment_id(seg), ca->lo, ca->hi, *ca->n_pages);

  return(op_success_status);
}

//*******************************************************************************
// cache_advise - Inform the cache system about the immediate R/W intent
//*******************************************************************************

void cache_advise(segment_t *seg, int rw_mode, ex_off_t lo, ex_off_t hi, page_handle_t *page, int *n_pages, int force_wait)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  ex_off_t lo_row, hi_row, nbytes, *poff;
  cache_page_t *p;
  skiplist_iter_t it;
  cache_advise_op_t ca;
  int err, tid;

  //** Map the rage to the page boundaries
  lo_row = lo / s->page_size; nbytes = lo_row; lo_row = lo_row * s->page_size;
  hi_row = hi / s->page_size; nbytes = hi_row - nbytes + 1; hi_row = hi_row * s->page_size;
  nbytes = nbytes * s->page_size;
//nbytes = hi - lo + 1;

ex_off_t len = hi - lo + 1;
tid = atomic_thread_id;
log_printf(15, "cache_advise: tid=%d START seg=" XIDT " lo=" XOT " hi=" XOT " lo_row=" XOT " hi_row=" XOT " nbytes=" XOT " hi-lo-1=" XOT "\n", tid, segment_id(seg), lo, hi, lo_row, hi_row, nbytes, len);

  //** Figure out if any pages need to be loaded
  segment_lock(seg);
  it = iter_search_skiplist(s->pages, &lo_row, 0);
  err = next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);
  while ((p != NULL) && (err == 0)) {
log_printf(15, "cache_advise: tid=%d CHECKING seg=" XIDT " p->offset=" XOT " nleft=" XOT "\n", tid, segment_id(seg), p->offset, nbytes);
     if (p->offset <= hi_row) {
        nbytes -= s->page_size;
log_printf(15, "cache_advise: tid=%d IN loop seg=" XIDT " p->offset=" XOT " nleft=" XOT "\n", tid, segment_id(seg), p->offset, nbytes);

        err = next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);
     } else {
        err = 1;
     }
  }
  segment_unlock(seg);

log_printf(15, "cache_advise: tid=%d AFTER loop seg=" XIDT " nleft=" XOT "\n", tid, segment_id(seg), nbytes);

  //** If none just return.  Otherwise trigger page fetches (and/or flushes)
  if (nbytes > 0) {
     ca.seg = seg;
     ca.lo = lo;
     ca.hi = hi;
     ca.force_wait = force_wait;
     ca.rw_mode = rw_mode;
     ca.page = page;
     ca.n_pages = n_pages;
     cache_advise_fn((void *)&ca, atomic_thread_id);
//     gop = new_thread_pool_op(s->tpc_unlimited, NULL, cache_advise_fn, (void *)ca, free, 1);
//log_printf(15, "cache_advise: tid=%d seg=" XIDT " lo=" XOT " hi=" XOT " rw_mode=%d ca=%p gid=%d\n", tid, segment_id(seg), lo, hi, rw_mode, ca, gop_id(gop));
//flush_log();
//     gop_waitall(gop);
//     gop_free(gop, OP_DESTROY);
  } else {
     *n_pages = 0;
  }
}

//*******************************************************************************
//  cache_page_drop - Permanately removes pages from cache within the given range
//     Pages are not flushed before removal!  This is mainly used for a truncate
//     or semenget close operation
//*******************************************************************************

int cache_page_drop(segment_t *seg, ex_off_t lo, ex_off_t hi)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  ex_off_t lo_row, hi_row, *poff, coff;
//  ex_off_t my_flush[3];
  skiplist_iter_t it;
  cache_page_t *p;
  cache_page_t *page[CACHE_MAX_PAGES_RETURNED];
  int do_again, count, n, tid;
int cr, cw, cf, bits;

  //** Map the rage to the page boundaries
  lo_row = lo / s->page_size; lo_row = lo_row * s->page_size;
  hi_row = hi / s->page_size; hi_row = hi_row * s->page_size;

  //** Make the initial flush checker
//  my_flush[0] = lo; my_flush[1] = hi;

tid = atomic_thread_id;
log_printf(5, "cache_page_drop: tid=%d START seg=" XIDT " lo=" XOT " hi=" XOT "\n", tid, segment_id(seg), lo, hi);

  do {
    do_again = 0;
    n = 0;

    segment_lock(seg);
    it = iter_search_skiplist(s->pages, &lo_row, 0);
    next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);
    coff = (p == NULL) ? hi+1 : p->offset;
log_printf(15, "cache_page_drop: tid=%d seg=" XIDT " loop start coff=" XOT "\n", tid, segment_id(seg), coff);

    while (coff < hi) {
      count = atomic_get(p->access_pending[CACHE_READ])
            + atomic_get(p->access_pending[CACHE_WRITE])
            + atomic_get(p->access_pending[CACHE_FLUSH]);

cr = atomic_get(p->access_pending[CACHE_READ]);
cw = atomic_get(p->access_pending[CACHE_WRITE]);
cf =  atomic_get(p->access_pending[CACHE_FLUSH]);
bits =  atomic_get(p->bit_fields);
log_printf(15, "PAGE_GET seg=" XIDT " get p->offset=" XOT " cr=%d cw=%d cf=%d bit_fields=%d usage=%d index=%d\n", segment_id(seg), p->offset, cr, cw, cf, bits, p->curr_data->usage_count, p->current_index);

//log_printf(15, "cache_page_drop: tid=%d seg=" XIDT " p->offset=" XOT " count=%d\n", tid, segment_id(seg), p->offset, count);
      if (count > 0) {
         do_again = 1;
      } else {
         page[n] = p;
log_printf(15, "cache_page_drop: tid=%d seg=" XIDT " adding p[%d]->offset=" XOT " n=%d\n", tid, segment_id(seg), n, page[n]->offset, n);
         n++;
         if (n == CACHE_MAX_PAGES_RETURNED) {
log_printf(15, "cache_page_drop: tid=%d 1. seg=" XIDT " p[0]->offset=" XOT " n=%d\n", tid, segment_id(seg), page[0]->offset, n);
            s->c->fn.destroy_pages(s->c, page, n, 1);
            it = iter_search_skiplist(s->pages, &lo_row, 0);
            n=0;
         }
      }

      next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);
      coff = (p == NULL) ? hi+1 : p->offset;
log_printf(15, "cache_page_drop: tid=%d seg=" XIDT " loop bottom coff=" XOT " p=%p\n", tid, segment_id(seg), coff, p);
    }

log_printf(15, "cache_page_drop: outer loop tid=%d seg=" XIDT " n=%d\n", tid, segment_id(seg), n);
    if (n>0) {
//log_printf(15, "cache_page_drop: outer loop tid=%d seg=" XIDT " p[0]->offset=" XOT " n=%d\n", tid, segment_id(seg), page[0]->offset, n);
       s->c->fn.destroy_pages(s->c, page, n, 1);
    }
    segment_unlock(seg);

    if (do_again != 0) {
       usleep(10000);  //** Do a simple sleep
    }
  } while (do_again != 0);

//segment_lock(seg);
//err = list_key_count(s->pages);
//log_printf(0, "cache_page_drop: seg=" XIDT " n_pages=%d\n", segment_id(seg), err);
//segment_unlock(seg);

log_printf(5, "cache_page_drop: tid=%d END seg=" XIDT " lo=" XOT " hi=" XOT "\n", tid, segment_id(seg), lo, hi);

  return(0);
}


//*******************************************************************************
//  cache_drop_pages - Permanately removes pages from cache within the given range
//     Pages are not flushed before removal!  This is mainly used for a truncate
//     or semenget close operation
//
//  NOTE:  This is designed to be called by other apps whereas the "cache_drop_page"
//     rotuine is deisgned to be used by segment_cache routines only
//*******************************************************************************

int cache_drop_pages(segment_t *seg, ex_off_t lo, ex_off_t hi)
{
  if (strcmp(seg->header.type, SEGMENT_TYPE_CACHE) != 0) return(0);

  return(cache_page_drop(seg, lo, hi));
}

//*******************************************************************************
//  cache_dirty_pages_get - Retrieves dirty pages from cache over the given range
//*******************************************************************************

int cache_dirty_pages_get(segment_t *seg, int mode, ex_off_t lo, ex_off_t hi, ex_off_t *hi_got, page_handle_t *page, int *n_pages)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  ex_off_t lo_row, hi_row, *poff, n, old_hi;
  skiplist_iter_t it;
  cache_page_t *p;
  int err, skip_mode, bits, count, can_get;
  cache_cond_t *cache_cond;

  //** Map the rage to the page boundaries
  lo_row = lo / s->page_size; lo_row = lo_row * s->page_size;
  hi_row = hi / s->page_size; hi_row = hi_row * s->page_size;

count = -1;

log_printf(15, "START: seg=" XIDT " mode=%d lo=" XOT " hi=" XOT " lo_row=" XOT "\n", segment_id(seg), mode, lo, hi, lo_row);

  segment_lock(seg);

  //** Get the 1st point and figure out the if we are skipping or getting pages
  //** If I can acquire a lock on the 1st block we retreive pages otherwise
  //** we are in skipping mode
  skip_mode = 0;
  it = iter_search_skiplist(s->pages, &lo_row, 0);
  err = next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);
  if (p != NULL) {
     bits = atomic_get(p->bit_fields);  //** Skip over the non-dirty pages or pages already being flushed
     count = atomic_get(p->access_pending[CACHE_FLUSH]);
     if (p->offset > hi) err = 1;
log_printf(15, "seg=" XIDT " p->offset=" XOT " bits=%d count=%d\n", segment_id(seg), p->offset, bits, count);
     while ((((bits & C_ISDIRTY) == 0) || (count > 0)) && (err == 0)) {
        err = next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);
        if (p!= NULL) {
           if (p->offset > hi) err = 1;
           bits = atomic_get(p->bit_fields);
           count = atomic_get(p->access_pending[CACHE_FLUSH]);
log_printf(15, "seg=" XIDT " checking p->offset=" XOT " bits=%d count=%d\n", segment_id(seg), p->offset, bits, count);
        }
     }

log_printf(15, "seg=" XIDT " after initial loop p=%p err=%d\n", segment_id(seg), p, err);

     if (p != NULL) {
        count = atomic_get(p->access_pending[CACHE_WRITE]);
        bits = atomic_get(p->bit_fields);
log_printf(15, "seg=" XIDT " checking mode=%d p->offset=" XOT " write_count=%d bits=%d\n", segment_id(seg), mode, p->offset, count, bits);

        if ((mode == CACHE_DOBLOCK) && ((count > 0) || ((bits & C_EMPTY) > 0))) {  //** Wait until I can acquire a lock
           cache_cond = (cache_cond_t *)pigeon_coop_hole_data(&(p->cond_pch));
           if (cache_cond == NULL) {
              p->cond_pch = reserve_pigeon_coop_hole(s->c->cond_coop);
              cache_cond = (cache_cond_t *)pigeon_coop_hole_data(&(p->cond_pch));
              cache_cond->count = 0;
           }
           atomic_inc(p->access_pending[CACHE_FLUSH]);
           cache_cond->count++;
           count = atomic_get(p->access_pending[CACHE_WRITE]);
           bits = atomic_get(p->bit_fields);
           while ((count > 0) || ((bits & C_EMPTY) > 0)) {
              apr_thread_cond_wait(cache_cond->cond, seg->lock);
              count = atomic_get(p->access_pending[CACHE_WRITE]);
              bits = atomic_get(p->bit_fields);
           }
           atomic_dec(p->access_pending[CACHE_FLUSH]);
           cache_cond->count--;
           if (cache_cond->count <= 0) release_pigeon_coop_hole(s->c->cond_coop, &(p->cond_pch));

           //** Need to reset iterator due to potential changes while waiting
           it = iter_search_skiplist(s->pages, &(p->offset), 0);
           err = next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);
        } else {
           skip_mode = (count == 0) ? 0 : 1;
        }
     }
  }

log_printf(15, "seg=" XIDT " mode=%d lo=" XOT " hi=" XOT " skip_mode=%d write_count=%d\n", segment_id(seg), mode, lo, hi, skip_mode, count);

  *hi_got = lo;
  n = 0;
  err = 0;
  while ((err == 0) && (p != NULL)) {
    can_get = 1;
    count = atomic_get(p->access_pending[CACHE_WRITE]);
    bits = atomic_get(p->bit_fields);
    if ((count > 0) || ((bits & C_EMPTY) > 0)) can_get = 0;

    err = 0;
    if (skip_mode == 0) {
       if (can_get == 0) err = 1;
    } else {  //** Skipping mode so looking for a block I *could* get
       if (can_get == 1) err = 1;
    }

log_printf(15, "1. seg=" XIDT " mode=%d lo=" XOT " hi=" XOT " hi_got=" XOT " skip_mode=%d err=%d p->offset=" XOT " can_get=%d wcount=%d\n", segment_id(seg), mode, lo, hi, *hi_got, skip_mode, err, p->offset, can_get, count);

    if (err == 0) {
       old_hi = *hi_got;
       *hi_got = p->offset + s->page_size - 1;
       if (p->offset > hi) { err = 1; *hi_got = old_hi; }

log_printf(15, "2. seg=" XIDT " mode=%d lo=" XOT " hi=" XOT " hi_got= " XOT " skip_mode=%d err=%d p->offset=" XOT "\n", segment_id(seg), mode, lo, hi, *hi_got, skip_mode, err, p->offset);

       if ((n < *n_pages) && (p->offset < *hi_got)) {
          if (skip_mode == 0) {
             s->c->fn.s_page_access(s->c, p, CACHE_FLUSH, 0);  //** Update page access information
             atomic_inc(p->access_pending[CACHE_FLUSH]);
log_printf(15, "PAGE_GET seg=" XIDT " p->offset=" XOT " usage=%d index=%d\n", segment_id(seg), p->offset, p->curr_data->usage_count, p->current_index);

             page[n].p = p;
             page[n].data = p->curr_data;
             p->curr_data->usage_count++;
             n++;
             if (n >= *n_pages) err = 1;
          }

          next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);
          if ((p != NULL) && (err == 0)) {
             bits = atomic_get(p->bit_fields);  //** Skip over the non-dirty pages or pages already being flushed
             count = atomic_get(p->access_pending[CACHE_FLUSH]);
             old_hi = p->offset;
log_printf(15, "2a. seg=" XIDT " p->offset=" XOT " old_hi=" XOT " bits=%d fcount=%d\n", segment_id(seg), p->offset, old_hi, bits, count);

             while ((((bits & C_ISDIRTY) == 0) || (count > 0)) && (err == 0)) {
                err = next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);
                if (p != NULL) {
                   bits = atomic_get(p->bit_fields);
                   count = atomic_get(p->access_pending[CACHE_FLUSH]);
                   old_hi = p->offset;
log_printf(15, "3. seg=" XIDT " lo=" XOT " hi=" XOT " err=%d p->offset=" XOT " bits=%d count=%d err=%d\n", segment_id(seg), lo, hi, err, p->offset, bits, count, err);
                   if (p->offset > hi) { err = 1; }
                } else {
                   err = 1;
                   old_hi = old_hi + s->page_size;
                }
             }

             *hi_got = old_hi - 1;
          }
       } else {
         err = 1;
       }
    }
  }

  segment_unlock(seg);

  *n_pages = n;  //** Store the number of pages found

  if (n == 0) *hi_got = hi;

  if (*hi_got > hi) *hi_got = hi;

log_printf(15, "END: seg=" XIDT " mode=%d lo=" XOT " hi=" XOT " hi_got=" XOT " skip_mode=%d\n", segment_id(seg), mode, lo, hi, *hi_got, skip_mode);

  return(skip_mode);
}

//*******************************************************************************
//  cache_read_pages_get - Retrieves pages from cache for READING over the given range
//*******************************************************************************

int cache_read_pages_get(segment_t *seg, int mode, ex_off_t lo, ex_off_t hi, ex_off_t *hi_got, page_handle_t *page, iovec_t *iov, int *n_pages, void **cache_missed, ex_off_t master_size)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  ex_off_t lo_row, hi_row, *poff, n, old_hi, last_off;
  skiplist_iter_t it;
  cache_page_t *p;
  int err, i, skip_mode, bits, count, can_get, max_pages;

int cr, cw, cf;
  //** Map the rage to the page boundaries
  lo_row = lo / s->page_size; lo_row = lo_row * s->page_size;
  hi_row = hi / s->page_size; hi_row = hi_row * s->page_size;

  max_pages = *n_pages;
log_printf(15, "START seg=" XIDT " mode=%d lo=" XOT " hi=" XOT " lo_row=" XOT " hi_row=" XOT "\n", segment_id(seg), mode, lo, hi, lo_row, hi_row);
  segment_lock(seg);

  //** Get the 1st point and figure out the if we are skipping or getting pages
  //** If I can acquire a lock on the 1st block we retreive pages otherwise
  //** we are in skipping mode
  skip_mode = 0;
  it = iter_search_skiplist(s->pages, &lo_row, 0);
  err = next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);

  if (p != NULL) {
log_printf(15, "seg=" XIDT " mode=%d lo=" XOT " hi=" XOT " p->offset=" XOT "\n", segment_id(seg), mode, lo, hi, p->offset);

     if (*poff != lo_row) {  //** Should find an exact match otherwise it's a hole
        s->c->fn.cache_miss_tag(s->c, seg, CACHE_READ, lo_row, hi_row, lo_row, cache_missed);
        if ((*poff <= hi_row) && (mode != CACHE_DOBLOCK)) {
           *hi_got = *poff - 1;
           *n_pages = 0;
           segment_unlock(seg);
           return(1);
        } else {
           p = NULL;
        }
     } else {
        count = atomic_get(p->access_pending[CACHE_FLUSH]);
        bits = atomic_get(p->bit_fields);

log_printf(15, "seg=" XIDT " mode=%d lo=" XOT " hi=" XOT " p->offset=" XOT " cf=%d bits=%d\n", segment_id(seg), mode, lo, hi, p->offset, count, bits);

        if ((bits & (C_EMPTY|C_TORELEASE)) > 0) {  //** Always skip if empty or being released
           skip_mode = 1;
        }

        if ((mode == CACHE_DOBLOCK) && (skip_mode == 1)) { //** Got to wait until I can acquire a lock
           atomic_inc(p->access_pending[CACHE_READ]);
           _cache_wait_for_page(seg, CACHE_READ, p);
           atomic_dec(p->access_pending[CACHE_READ]);
           skip_mode = 0;

           //** Need to reset iterator due to potential changes while waiting
           it = iter_search_skiplist(s->pages, &(p->offset), 0);
           err = next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);
        }
     }
  }

log_printf(15, "seg=" XIDT " mode=%d lo=" XOT " hi=" XOT " skip_mode=%d\n", segment_id(seg), mode, lo, hi, skip_mode);

  if (p == NULL) err = 2;  //** Nothing valid so trigger it to be loaded

  *hi_got = lo;
  n = 0;
  while ((err == 0) && (p != NULL)) {
    can_get = 1;
    bits = atomic_get(p->bit_fields);
    if ((bits & (C_EMPTY|C_TORELEASE)) > 0) {  //** If empty can't access it
       can_get = 0;
    }

    err = 0;
    if (skip_mode == 0) {
       if (can_get == 0) err = 1;
    } else {  //** Skipping mode so looking for a block I *could* get
       if (can_get == 1) err = 1;
    }

    if (err == 0) {
       old_hi = *hi_got;
       *hi_got = p->offset + s->page_size - 1;
       if (p->offset > hi) { err = 1; *hi_got = old_hi; }

       if ((n < *n_pages) && (err == 0)) {
          if (skip_mode == 0) {
             atomic_inc(p->access_pending[CACHE_READ]);
             atomic_inc(p->used_count);
             s->c->fn.s_page_access(s->c, p, CACHE_READ, master_size);  //** Update page access information
             page[n].p = p;
             page[n].data = p->curr_data;
             p->curr_data->usage_count++;
             iov[n].iov_base = p->curr_data->ptr;
             iov[n].iov_len = s->page_size;

log_printf(15, "seg=" XIDT " adding page[" XOT "]->offset=" XOT "\n", segment_id(seg), n, p->offset);
cr = atomic_get(p->access_pending[CACHE_READ]);
cw = atomic_get(p->access_pending[CACHE_WRITE]);
cf =  atomic_get(p->access_pending[CACHE_FLUSH]);
bits =  atomic_get(p->bit_fields);
log_printf(15, "PAGE_GET seg=" XIDT " get p->offset=" XOT " cr=%d cw=%d cf=%d bit_fields=%d usage=%d index=%d\n", segment_id(seg), p->offset, cr, cw, cf, bits, p->curr_data->usage_count, p->current_index);

             n++;
             if (n >= *n_pages) err = 1;
          }

          err = next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);

          if ((p != NULL) && (n < *n_pages)) {
             if (*hi_got != (p->offset - 1)) {  //** Check for a hole
                err = 2;
                s->c->fn.cache_miss_tag(s->c, seg, CACHE_READ, lo_row, hi_row, *hi_got + 1, cache_missed);
             }
log_printf(15, "seg=" XIDT " checking next page->offset=" XOT " hi_got=" XOT " hole_check=%d\n", segment_id(seg), p->offset, *hi_got, err);
          } else {
log_printf(15, "seg=" XIDT " No more pages\n", segment_id(seg));
            err = 1;
          }
       } else {
         err = 1;
       }
    }
  }

  //**Used to determine if cache_advise needs to be called
  //** This is done here cause p may be removed after the unlock call
  last_off = (p == NULL) ? hi : p->offset - 1;

  segment_unlock(seg);

  *n_pages = n;  //** Store the number of pages found

  //** Check if there are missing pages, if so load them
  if ((err == 2) || (n == 0)) {
     if (mode != CACHE_DOBLOCK) {
        if (*hi_got < hi) {
           old_hi = (*hi_got == lo) ? lo : *hi_got + 1;
           count = (last_off - old_hi) / s->page_size;
//count= -1;
//log_printf(15, "seg=" XIDT " mode=%d checking cache_advise for lo(old_hi)=" XOT " hi(last_off)=" XOT " missing pages=%d\n", segment_id(seg), mode, old_hi, last_off, count);

//           if (count > 4) {  //** Only do a cache_advise if enough missing pages justify the overhead
//log_printf(15, "seg=" XIDT " mode=%d WILL call cache_advise for lo=" XOT " hi=" XOT " missing pages=%d\n", segment_id(seg), mode, old_hi, last_off, count);
//              cache_advise(seg, CACHE_READ, old_hi, last_off);
//           }
        }
     } else if (n == 0) {     //** Force the first page to be loaded
        *n_pages = max_pages;
log_printf(15, "seg=" XIDT " calling cache_advise lo=" XOT " hi=" XOT "\n", segment_id(seg), lo_row, hi_row);
        s->c->fn.cache_miss_tag(s->c, seg, CACHE_READ, lo_row, hi_row, lo_row, cache_missed);

        cache_advise(seg, CACHE_READ, lo_row, hi_row, page, n_pages, 0);
n = atomic_thread_id;
log_printf(15, "seg=" XIDT " cache_advise lo=" XOT " hi=" XOT " n_pages=%d tid=%d\n", segment_id(seg), lo_row, hi_row, *n_pages, n);
        if (*n_pages > 0) {
           for (i=0; i < *n_pages; i++) {
              iov[i].iov_base = page[i].data->ptr;
              iov[i].iov_len = s->page_size;
           }
           *hi_got = page[*n_pages-1].p->offset + s->page_size - 1;
        } else {
           p = cache_page_force_get(seg, CACHE_READ, lo_row, lo, hi);  //** This routine does it's own seg locking
           if (p != NULL) {
              *n_pages = 1;
log_printf(15, "PAGE_GET seg=" XIDT " forcing page load lo_row=" XOT "\n", segment_id(seg), lo_row);
              segment_lock(seg);
              page[0].p = p;
              page[0].data = p->curr_data;
              p->curr_data->usage_count++;
              segment_unlock(seg);
              iov[0].iov_base = page[0].data->ptr;
              iov[0].iov_len = s->page_size;
              *hi_got = lo_row + s->page_size - 1;
              skip_mode = 0;
           }
        }
     }
  }

  if (*n_pages == 0) skip_mode = 1;
log_printf(15, "END seg=" XIDT " mode=%d lo=" XOT " hi=" XOT " hi_got=" XOT " skip_mode=%d n_pages=%d\n", segment_id(seg), mode, lo, hi, *hi_got, skip_mode, *n_pages);
flush_log();

  return(skip_mode);
}

//*******************************************************************************
//  cache_write_pages_get - Retrieves pages from cache over the given range for WRITING
//*******************************************************************************

int cache_write_pages_get(segment_t *seg, int mode, ex_off_t lo, ex_off_t hi, ex_off_t *hi_got, page_handle_t *page, iovec_t *iov, int *n_pages, void **cache_missed, ex_off_t master_size)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  ex_off_t lo_row, hi_row, *poff, n, old_hi, coff, pstart, page_off;
  skiplist_iter_t it;
  page_handle_t pload[2], pcheck;
  cache_page_t *p, *np;
  int pload_iov_index[2], i;
  int err, skip_mode, bits, count, can_get, pload_count;

int flush_skip = 0;

int cr, cw, cf;
  //** Map the rage to the page boundaries
  lo_row = lo / s->page_size; lo_row = lo_row * s->page_size;
  hi_row = hi / s->page_size; hi_row = hi_row * s->page_size;

  *hi_got = lo;

log_printf(15, "START seg=" XIDT " mode=%d lo=" XOT " hi=" XOT " lo_row=" XOT " hi_row=" XOT "\n", segment_id(seg), mode, lo, hi, lo_row, hi_row);
  segment_lock(seg);

  //** Get the 1st point and figure out the if we are skipping or getting pages
  //** If I can acquire a lock on the 1st block we retreive pages otherwise
  //** we are in skipping mode
  skip_mode = 0;
  it = iter_search_skiplist(s->pages, &lo_row, 0);
  err = next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);

  n = 0;
  pload_count = 0;
  page_off = -1;
  pcheck.p = NULL;

  pstart = hi;
  err = 0;
  if (p == NULL) {
     err = 1;
  } else if (*poff != lo_row) { //** Should find an exact match otherwise it's a hole
log_printf(15, "seg=" XIDT " initial page p->offset=" XOT "\n", segment_id(seg), *poff); flush_log();

     pcheck.p = p;
     pcheck.data = p->curr_data;

     pstart = *poff;
     page_off = *poff;
     err = 1;
  }


  if (err == 1) { //** Missing the starting point so see if we can make some blank pages
     err = 0;
     if (pstart > hi) pstart = hi;
     coff = lo_row;

     if (pcheck.p != NULL) atomic_inc(pcheck.p->access_pending[CACHE_READ]);  //** Tag it so it doesn't get removed

     np = s->c->fn.create_empty_page(s->c, seg, 0);  //** Get the empty page  if possible
     while ((np != NULL) && (coff < pstart) && (n < *n_pages)) {
       if (np != NULL) {
          s_cache_page_init(seg, np, coff);
          if (full_page_overlap(coff, s->page_size, lo, hi) == 0) {
             if (s->child_last_page >= coff) {  //** Only load the page if not a write beyond the current EOF
log_printf(15, "seg=" XIDT " adding page for reading p->offset=" XOT " current child_last_page=" XOT "\n", segment_id(seg), np->offset, s->child_last_page);
               pload[pload_count].p = np;
               pload[pload_count].data = np->curr_data;
               pload_iov_index[pload_count] = n;
               pload_count++;
             }
          }
          *hi_got = coff + s->page_size - 1;

          atomic_inc(np->access_pending[CACHE_WRITE]);
          atomic_inc(np->used_count);
          s->c->fn.s_page_access(s->c, np, CACHE_WRITE, master_size);  //** Update page access information
          page[n].p = np;
          page[n].data = np->curr_data;
          np->curr_data->usage_count++;
          iov[n].iov_base = np->curr_data->ptr;
          iov[n].iov_len = s->page_size;
log_printf(15, "seg=" XIDT " adding page[" XOT "]->offset=" XOT "\n", segment_id(seg), n, np->offset);
cr = atomic_get(np->access_pending[CACHE_READ]);
cw = atomic_get(np->access_pending[CACHE_WRITE]);
cf =  atomic_get(np->access_pending[CACHE_FLUSH]);
bits =  atomic_get(np->bit_fields);
log_printf(15, "PAGE_GET seg=" XIDT " get np->offset=" XOT " n=%d cr=%d cw=%d cf=%d bit_fields=%d np=%p usage=%d index=%d\n", segment_id(seg), np->offset, n, cr, cw, cf, bits, np, np->curr_data->usage_count, np->current_index);

          n++;
          if (n >= *n_pages) err = 1;
          coff += s->page_size;
          if ((coff < pstart) && (n < *n_pages)) np = s->c->fn.create_empty_page(s->c, seg, 0);  //** Get the empty page  if possible
       }

log_printf(15, " pstart=" XOT " coff=" XOT "\n", pstart, coff);
     }

     ///** Done if acquired if (pcheck != NULL) atomic_dec(pcheck->access_pending[CACHE_READ]); //** Release the tag but may need to do an official release later

     if (coff < pstart) { //** Didn't make it up to the 1st loaded page
        err = 1;
        if (n < *n_pages) s->c->fn.cache_miss_tag(s->c, seg, CACHE_WRITE, lo_row, hi_row, coff, cache_missed);
     }
  } else {  //** The 1st page exists so see if I can get it
     count = atomic_get(p->access_pending[CACHE_FLUSH]);
     bits = atomic_get(p->bit_fields);

log_printf(15, "seg=" XIDT "mode=%d lo=" XOT " hi=" XOT " p->offset=" XOT " cf=%d bits=%d\n", segment_id(seg), mode, lo, hi, p->offset, count, bits);

     if ((bits & (C_EMPTY|C_TORELEASE)) > 0) {  //** Always skip if empty
        skip_mode = 1;
     } else {
        if (count > 0) {  //Got a flush op in progress so see if we can do a copy-on-write
            skip_mode = 1;
flush_skip = 1;
            i = atomic_get(p->access_pending[CACHE_WRITE]);
            if (i == 0) {
               cache_lock(s->c);
               if ((s->c->write_temp_overflow_used+s->page_size) < s->c->write_temp_overflow_size) {
                  i = (p->current_index+1) % 2;
                  if (p->data[i].ptr == NULL) {  //** We can use the COW space
                     skip_mode = 0;
//log_printf(0, "seg=" XIDT " p->offset=" XOT " COP triggered used=" XOT " usage=%d\n", segment_id(seg), p->offset, s->c->write_temp_overflow_used, p->curr_data->usage_count);
flush_skip = 0;
                  }
else {
flush_skip = 2;
}
               }
            }
            cache_unlock(s->c);
        }

        if ((mode == CACHE_DOBLOCK) && (skip_mode == 1)) { //** Got to wait until I can acquire a lock
log_printf(15, "seg=" XIDT " waiting for p->offset=" XOT " lo=" XOT " hi=" XOT " skip_mode=%d\n", segment_id(seg), p->offset, lo, hi, skip_mode);
           atomic_inc(p->access_pending[CACHE_READ]);  //** Use a read to hold the page
           _cache_wait_for_page(seg, CACHE_WRITE, p);
           atomic_dec(p->access_pending[CACHE_READ]);

           skip_mode = 0;

           //** Need to reset iterator due to potential changes while waiting
           it = iter_search_skiplist(s->pages, &(p->offset), 0);
           err = next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);
        }
     }
  }

log_printf(15, "seg=" XIDT " mode=%d lo=" XOT " hi=" XOT " skip_mode=%d\n", segment_id(seg), mode, lo, hi, skip_mode);

  while ((err == 0) && (p != NULL)) {
    can_get = 1;
    count = atomic_get(p->access_pending[CACHE_FLUSH]);
    bits = atomic_get(p->bit_fields);
    if ((bits & (C_EMPTY|C_TORELEASE)) > 0) {  //** If empty can't access it yet
       can_get = 0;
    } else if (count > 0) {  //** Doing a flush and a write so block
       can_get = 0;
flush_skip = 3;
       if (skip_mode == 0) {  //** See if we can do a COW to access it
          i = atomic_get(p->access_pending[CACHE_WRITE]);
          if (i== 0) {
             if ((s->c->write_temp_overflow_used+s->page_size) < s->c->write_temp_overflow_size) {
                i = (p->current_index+1) % 2;
                if (p->data[i].ptr == NULL) {  //** We can use the COW space
                   s->c->write_temp_overflow_used += s->page_size;
                   type_malloc(p->data[i].ptr, char, s->page_size);
                   memcpy(p->data[i].ptr, p->data[p->current_index].ptr, s->page_size);
                   p->current_index = i;
                   p->curr_data = &(p->data[i]);
                   can_get = 1;
//log_printf(0, "seg=" XIDT " p->offset=" XOT " COP triggered used=" XOT " usage=%d\n", segment_id(seg), p->offset, s->c->write_temp_overflow_used, p->curr_data->usage_count);
//flush_skip = 0;
                }
else {
flush_skip = 4;
}
             }
          }
       }
    }

    err = 0;
    if (skip_mode == 0) {
       if (can_get == 0) err = 1;
    } else {  //** Skipping mode so looking for a block I *could* get
       if (can_get == 1) err = 1;
    }

    if (err == 0) {
       old_hi = *hi_got;
       *hi_got = p->offset + s->page_size - 1;
       if (p->offset > hi) { err = 1; *hi_got = old_hi; }

       if ((n < *n_pages) && (err == 0)) {
          if (skip_mode == 0) {
             if (p == pcheck.p) { //** Actually using the page so no need to do the release
                atomic_dec(pcheck.p->access_pending[CACHE_READ]);
                pcheck.p = NULL;
             }

             atomic_inc(p->access_pending[CACHE_WRITE]);
             atomic_inc(p->used_count);
             s->c->fn.s_page_access(s->c, p, CACHE_WRITE, master_size);  //** Update page access information
             page[n].p = p;
             page[n].data = p->curr_data;
             p->curr_data->usage_count++;
             iov[n].iov_base = p->curr_data->ptr;
             iov[n].iov_len = s->page_size;
log_printf(15, "seg=" XIDT " adding page[" XOT "]->offset=" XOT "\n", segment_id(seg), n, p->offset);
cr = atomic_get(p->access_pending[CACHE_READ]);
cw = atomic_get(p->access_pending[CACHE_WRITE]);
cf =  atomic_get(p->access_pending[CACHE_FLUSH]);
bits =  atomic_get(p->bit_fields);
log_printf(15, "PAGE_GET seg=" XIDT " get p->offset=" XOT " n=%d cr=%d cw=%d cf=%d bit_fields=%d usage=%d index=%d\n", segment_id(seg), p->offset, n, cr, cw, cf, bits, p->curr_data->usage_count, p->current_index);

             n++;
             if (n >= *n_pages) err = 1;
          }

          err = next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);

          if ((p != NULL) && (n<*n_pages)) {
             if ((*hi_got != (p->offset - 1)) && (skip_mode == 0)) { //** Got a hole so see if we can fill it with blank pages
                page_off = p->offset;
                coff = *hi_got + 1;
                pstart = p->offset;
                if (pstart > hi) pstart = hi;
                pcheck.p = p;  //** TRack it
                pcheck.data = p->curr_data;
                atomic_inc(pcheck.p->access_pending[CACHE_READ]);  //** Tag it so it doesn't disappear
                np = NULL;

log_printf(15, "seg=" XIDT " before blank loop coff=" XOT " pstart=" XOT "\n", segment_id(seg), coff, pstart);
                if (coff < pstart) np = s->c->fn.create_empty_page(s->c, seg, 0);  //** Get the empty page  if possible
                while ((np != NULL) && (coff < pstart) && (n < *n_pages)) {
                   if (np != NULL) {
                      s_cache_page_init(seg, np, coff);
                      if (full_page_overlap(coff, s->page_size, lo, hi) == 0) {
                         if (s->child_last_page >= coff) {  //** Only load the page if not a write beyond the current EOF
log_printf(15, "seg=" XIDT " adding page for reading p->offset=" XOT " current child_last_page=" XOT "\n", segment_id(seg), np->offset, s->child_last_page);
                            pload[pload_count].p = np;
                            pload[pload_count].data = np->curr_data;
                            pload_iov_index[pload_count] = n;
                            pload_count++;
//                            s->total_size = coff + s->page_size;
                         }
                      }
                      *hi_got = coff + s->page_size - 1;

                      atomic_inc(np->access_pending[CACHE_WRITE]);
                      atomic_inc(np->used_count);
                      s->c->fn.s_page_access(s->c, np, CACHE_WRITE, master_size);  //** Update page access information
                      page[n].p = np;
                      page[n].data = np->curr_data;
                      np->curr_data->usage_count++;
                      iov[n].iov_base = np->curr_data->ptr;
                      iov[n].iov_len = s->page_size;
log_printf(15, "seg=" XIDT " adding page[" XOT "]->offset=" XOT "\n", segment_id(seg), n, np->offset);
cr = atomic_get(np->access_pending[CACHE_READ]);
cw = atomic_get(np->access_pending[CACHE_WRITE]);
cf =  atomic_get(np->access_pending[CACHE_FLUSH]);
bits =  atomic_get(np->bit_fields);
log_printf(15, "PAGE_GET seg=" XIDT " get p->offset=" XOT " cr=%d cw=%d cf=%d bit_fields=%d usage=%d index=%d\n", segment_id(seg), np->offset, cr, cw, cf, bits, np->curr_data->usage_count, np->current_index);

                      n++;
                      coff += s->page_size;

                      if (n >= *n_pages) err = 1;
                      if ((coff < pstart) && (n < *n_pages)) np = s->c->fn.create_empty_page(s->c, seg, 0);  //** Get the empty page  if possible
                   }
log_printf(15, "pstart=" XOT " coff=" XOT "\n", pstart, coff);
                }

                //** Reset the iterator cause the page could have been removed in the interim
                it = iter_search_skiplist(s->pages, &page_off, 0);
                err = next_skiplist(&it, (skiplist_key_t **)&poff, (skiplist_data_t **)&p);

                if (coff < pstart) { //** Didn't make it up to the 1st loaded page
                   err = 1;
                   if (n < *n_pages) s->c->fn.cache_miss_tag(s->c, seg, CACHE_WRITE, lo_row, hi_row, coff, cache_missed);
                }
             }
if (p != NULL) log_printf(15, "cache_page_get: seg=" XIDT " checking next page->offset=" XOT " hi_got=" XOT " hole_check=%d\n", segment_id(seg), p->offset, *hi_got, err);
          } else {
log_printf(15, "seg=" XIDT " No more pages\n", segment_id(seg));
          }
       } else {
         err = 1;
       }
    }
  }

  segment_unlock(seg);

  *n_pages = n;  //** Store the number of pages found

  //** See if we need to do a formal release on the check page
  if (pcheck.p != NULL) {
     pcheck.data->usage_count++;
     cache_release_pages(1, &pcheck, CACHE_READ);
  }

  //** Check if there are missing pages, if so force the loding if needed
  if ((n == 0) && (mode == CACHE_DOBLOCK)) {
log_printf(15, "PAGE_GET seg=" XIDT " forcing page load lo_row=" XOT "\n", segment_id(seg), lo_row);
     p = cache_page_force_get(seg, CACHE_WRITE, lo_row, lo, hi);  //** This routine does it's own seg locking
     if (p != NULL) {
        *n_pages = 1;
        segment_lock(seg);
        page[0].p = p;
        page[0].data = p->curr_data;
        p->curr_data->usage_count++;
        segment_unlock(seg);

        iov[0].iov_base = page[0].data->ptr;
        iov[0].iov_len = s->page_size;
        *hi_got = lo_row + s->page_size - 1;
        skip_mode = 0;
     }
  }

  //** If needed load some pages before returning
  if (pload_count > 0) {
     err = cache_rw_pages(seg, pload, pload_count, CACHE_READ, 0);
     if (err > 0) { //** Handle any errors that may have occurred
        for (i=0; i<pload_count; i++) {
           if (pload[i].data->ptr == NULL)  { iov[pload_iov_index[i]].iov_base = NULL; log_printf(15, "blanking p->offset=" XOT " i=%d iov_index=%d\n", pload[i].p->offset, i, pload_iov_index[i]); }
        }
     }
  }


  if (*n_pages == 0) skip_mode = 1;

log_printf(15, "END seg=" XIDT " mode=%d lo=" XOT " hi=" XOT " hi_got=" XOT " skip_mode=%d n_pages=%d\n", segment_id(seg), mode, lo, hi, *hi_got, skip_mode, *n_pages);
if (flush_skip == 1) { log_printf(5, "END seg=" XIDT " mode=%d lo=" XOT " hi=" XOT " hi_got=" XOT " skip_mode=%d n_pages=%d flush_skip=%d\n", segment_id(seg), mode, lo, hi, *hi_got, skip_mode, *n_pages, flush_skip); flush_log(); }
//flush_log();

  return(skip_mode);
}


//*******************************************************************************
//  cache_release_pages - Releases a collection of cache pages
//    NOTE:  ALL PAGES MUST BE FROM THE SAME SEGMENT
//*******************************************************************************

int cache_release_pages(int n_pages, page_handle_t *page_list, int rw_mode)
{
  segment_t *seg = page_list[0].p->seg;
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  cache_page_t *page;
  cache_cond_t *cache_cond;
  int bits, changed, count, i, cow_hit;
  ex_off_t min_off, max_off;
  op_generic_t *gop;

int cr, cw, cf;

  apr_thread_mutex_lock(seg->lock);

  min_off = s->total_size;
  max_off = -1;

  for (i=0; i<n_pages; i++) {
    page = page_list[i].p;

    atomic_dec(page->access_pending[rw_mode]);
    page_list[i].data->usage_count--;

cr = atomic_get(page->access_pending[CACHE_READ]);
cw = atomic_get(page->access_pending[CACHE_WRITE]);
cf =  atomic_get(page->access_pending[CACHE_FLUSH]);
bits =  atomic_get(page->bit_fields);
log_printf(15, "seg=" XIDT " initial rw_mode=%d p->offset=" XOT " cr=%d cw=%d cf=%d bit_fields=%d usage=%d index=%d\n", segment_id(seg), rw_mode, page->offset, cr, cw, cf, bits, page_list[i].data->usage_count, page->current_index);

    cow_hit = 0;
    if (page_list[i].data != page->curr_data) {
       cow_hit = 1;
       if (page_list[i].data->usage_count <= 0) {  //** Clean up a COW
          cache_lock(s->c);
          free(page_list[i].data->ptr);
          page_list[i].data->ptr = NULL;
          s->c->write_temp_overflow_used -= s->page_size;
log_printf(15, "seg=" XIDT " p->offset=" XOT " COP cleanup used=" XOT " rw_mode=%d usage=%d\n", segment_id(seg), page->offset, s->c->write_temp_overflow_used, rw_mode, page_list[i].data->usage_count);
flush_log();
          cache_unlock(s->c);
       }
    }

    if (rw_mode == CACHE_WRITE) {  //** Write release
       bits = atomic_get(page->bit_fields);
       changed = bits;
       if (bits & C_EMPTY) bits = bits ^ C_EMPTY;
       if ((bits & C_ISDIRTY) == 0) {
          s->c->fn.adjust_dirty(s->c, s->page_size);
          bits = bits | C_ISDIRTY;
       }
       if (changed != bits) atomic_set(page->bit_fields, bits);
    } else if (rw_mode == CACHE_FLUSH) {  //** Flush release so tweak dirty page info
       if (cow_hit == 0) {
          bits = atomic_get(page->bit_fields);
          s->c->fn.adjust_dirty(s->c, -s->page_size);
          bits = bits ^ C_ISDIRTY;
          atomic_set(page->bit_fields, bits);
       }
    }

cr = atomic_get(page->access_pending[CACHE_READ]);
cw = atomic_get(page->access_pending[CACHE_WRITE]);
cf =  atomic_get(page->access_pending[CACHE_FLUSH]);
bits =  atomic_get(page->bit_fields);
log_printf(15, "seg=" XIDT " released rw_mode=%d p->offset=" XOT " cr=%d cw=%d cf=%d bit_fields=%d\n", segment_id(seg), rw_mode, page->offset, cr, cw, cf, bits);

    cache_cond = (cache_cond_t *)pigeon_coop_hole_data(&(page->cond_pch));
    if (cache_cond != NULL) {  //** Someone is listening so wake them up
       apr_thread_cond_broadcast(cache_cond->cond);
    } else {
       bits = atomic_get(page->bit_fields);
       if ((bits & C_TORELEASE) > 0) {
          count = atomic_get(page->access_pending[CACHE_READ]) + atomic_get(page->access_pending[CACHE_WRITE]) + atomic_get(page->access_pending[CACHE_FLUSH]);
          if (count == 0) {
             if (((bits & C_ISDIRTY) == 0) || (page->data == NULL)) {  //** Not dirty so release it
               s->c->fn.s_pages_release(s->c, &page, 1); //** No one else is listening so release the page
             } else {  //** Should be manually flushed so force one
               if (min_off > page->offset) min_off = page->offset;
               if (max_off < page->offset) max_off = page->offset;
             }
          }
       }
    }
  }

  apr_thread_mutex_unlock(seg->lock);

  if (max_off > -1) {  //** Got to flush some pages
log_printf(5, "Looks like we need to do a manual flush.  min_off=" XOT " max_off=" XOT "\n", min_off, max_off);
     gop = cache_flush_range(seg, s->c->da, min_off, max_off+s->page_size-1, s->c->timeout);
     gop_set_auto_destroy(gop, 1);
     gop_start_execution(gop);
  }

  return(0);
}

//*******************************************************************************
// _cache_ppages_flush - Fluses the partial pages
//     NOTE:  Segment should be locked on entry
//*******************************************************************************

int _cache_ppages_flush(segment_t *seg, data_attr_t *da)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  cache_partial_page_t *pp;
  cache_rw_op_t cop;
  ex_iovec_t *ex_iov;
  iovec_t *iov;
  tbuffer_t tbuf;
  ex_off_t *rng, *rng0, r[2];
  int i, j, n_ranges, n, again, slot;
  ex_off_t nbytes, len;
  op_status_t status;

  if (s->ppages_used == 0) return(0);

log_printf(5, "Flushing ppages seg=" XIDT " ppages_used=%d\n", segment_id(seg), s->ppages_used);

  //** Cycle through the pages makng the write map for each page
  n_ranges = 0;
  for (i=0; i<s->ppages_used; i++) {
     pp = &(s->ppage[i]);
log_printf(5, "START RSTACK[%d]=%p size=%d flags=%d\n", i, pp->range_stack, stack_size(pp->range_stack), pp->flags); flush_log();
     if ( pp->flags == 1) { //** SEe if we get lucky and it's a full page
        empty_stack(pp->range_stack, 1);
        type_malloc(rng, ex_off_t, 2);
        rng[0] = 0; rng[1] = s->page_size;
        push(pp->range_stack, rng);
        n_ranges++;
        continue;
     }

     //** Start the iterative process to make the irreducible set for storing
//k=0;
     do {
        again = 0;
        rng0 = (ex_off_t *)pop(pp->range_stack);
        r[0] = rng0[0] - 1; r[1] = rng0[1] + 1;
        n = stack_size(pp->range_stack);
//if (k==0) {
//log_printf(0, "p[%d].off=" XOT " rlo=" XOT " rhi= "XOT " stack_size=%d\n", i, pp->page_start, rng0[0], rng0[1], n);
//}
        for (j=0; j<n; j++) {
           rng = (ex_off_t *)pop(pp->range_stack);
//if (k==0) {
//log_printf(0, "p[%d].off=" XOT " j=%d lo=" XOT " hi= "XOT "\n", i, pp->page_start, j, rng[0], rng[1]);
//}
           if ((rng[0] <= r[0]) && (rng[1] >= r[0])) {
              rng0[0] = rng[0]; r[0] = rng[0] - 1;
              if (rng[1] >= r[1]) { rng0[1] = rng[1]; r[1] = rng[1] + 1; }
              free(rng);
              again = 1;
           } else if ((rng[0] > r[0]) && (rng[0] <= r[1])) {
              if (rng[1] >= r[1]) { rng0[1] = rng[1]; r[1] = rng[1] + 1; }
              free(rng);
              again = 1;
           } else {   //** No overlap so just copy it
              move_to_bottom(pp->range_stack);
              insert_below(pp->range_stack, rng);
           }
        }
        push(pp->range_stack, rng0);
//k=1;
     } while (again == 1);

     n_ranges += stack_size(pp->range_stack);
log_printf(5, "END RSTACK[%d]=%p size=%d n_ranges=%d\n", i, pp->range_stack, stack_size(pp->range_stack), n_ranges); flush_log();
  }


  //** Fill in the RW op struct
  type_malloc_clear(ex_iov, ex_iovec_t, n_ranges);
  type_malloc_clear(iov, iovec_t, n_ranges);
  cop.seg = seg;
  cop.da = da;
  cop.n_iov = n_ranges;
  cop.iov = ex_iov;
  cop.rw_mode = CACHE_WRITE;
  cop.boff = 0;
  cop.buf = &tbuf;
  cop.skip_ppages = 1;

  nbytes = 0;
  slot = 0;
  for (i=0; i<s->ppages_used; i++) {
     pp = &(s->ppage[i]);
     while ((rng = (ex_off_t *)pop(pp->range_stack)) != NULL) {
        len = rng[1] - rng[0] + 1;
        iov[slot].iov_base = &(pp->data[rng[0]]);
        iov[slot].iov_len = len;
        ex_iov[slot].offset = pp->page_start + rng[0];
        ex_iov[slot].len = len;
        nbytes += len;
r[1] = ex_iov[slot].offset + len - 1;
log_printf(5, "seg=" XIDT " ppage=%d slot=%d off=" XOT " end=" XOT " len=" XOT "\n", segment_id(seg), i, slot, ex_iov[slot].offset, r[1], ex_iov[slot].len);
        slot++;
        free(rng);
     }
     pp->flags = 0;
  }

  //** finish the tbuf setup
  tbuffer_vec(&tbuf, nbytes, n_ranges, iov);

  //** Do the flush
  s->ppages_flushing = 1;
log_printf(5, "Performing flush now\n");

  segment_unlock(seg);
  status = cache_rw_func(&cop, 0);

  //** Notify everyone it's done
  segment_lock(seg);
  s->ppages_flushing = 0;
  s->ppages_used = 0;
  s->ppage_max = -1;
log_printf(5, "Flush completed\n");

  apr_thread_cond_broadcast(s->ppages_cond);

  free(ex_iov);
  free(iov);
  return((status.op_status == OP_STATE_SUCCESS) ? 0 : 1);
}

//*******************************************************************************
// cache_ppages_read - Checks if a read request overlaps a partail page and if so
//      flushes the ppages.
//*******************************************************************************

int cache_ppages_read(segment_t *seg, data_attr_t *da, ex_off_t lo, ex_off_t hi)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  cache_partial_page_t *pp;
  ex_off_t lo_page, hi_page, n_pages;
  int i, hit, err;

  err = 0;
  segment_lock(seg);

  if (s->ppages_flushing != 0) {  //** Flushing ppages so wait until finished
log_printf(5, "Waiting for flush to complete\n");
     do {
       apr_thread_cond_wait(s->ppages_cond, seg->lock);
     } while (s->ppages_flushing != 0);
log_printf(5, "Flush completed\n");
  }

  //** Kick out if nothing to check
  if (s->ppages_used == 0) { segment_unlock(seg); return(0); }

  lo_page = lo / s->page_size; n_pages = lo_page;               lo_page = lo_page * s->page_size;
  hi_page = hi / s->page_size; n_pages = hi_page - n_pages + 1; hi_page = hi_page * s->page_size;

  hit = 0;
  for (i=0; i<s->ppages_used; i++) {
     pp = &(s->ppage[i]);
     if (((lo_page < pp->page_start) && (pp->page_start < hi_page)) ||
         (lo_page == pp->page_start) || (hi_page == pp->page_start)) {
         hit = 1;
         break;
     }
  }

  if (hit == 1) {  //** Got a hit so flush everything
     err = _cache_ppages_flush(seg, da);
  }
  segment_unlock(seg);

  return(err);
}

//*******************************************************************************
// cache_ppages_handle - Process partail page requests storing them in interim
//     staging area
//*******************************************************************************

int cache_ppages_handle(segment_t *seg, data_attr_t *da, int rw_mode, ex_off_t *lo, ex_off_t *hi, ex_off_t *len, ex_off_t *bpos, tbuffer_t *tbuf)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  cache_partial_page_t *pp;
  ex_off_t lo_page, hi_page, n_pages, poff, boff, nbytes, pend, nhandled;
  ex_off_t lo_new, hi_new, bpos_new;
  ex_off_t *rng;
  cache_page_t *p;
  tbuffer_t pptbuf;
  int do_flush, i, err, lo_mapped, hi_mapped;
  err = 0;

log_printf(5, "START lo=" XOT " hi=" XOT " bpos=" XOT "\n", *lo, *hi, *bpos); flush_log();

  if (s->n_ppages == 0) return(0);

  //** Check for a page overlap and if so flush everything
  if (rw_mode == CACHE_READ) {
     cache_ppages_read(seg, da, *lo, *hi);
     return(0);
  }

  segment_lock(seg);

  //** Wait for any flushed to complete
  if (s->ppages_flushing != 0) {
log_printf(5, "Waiting for flush to complete\n");
     do {
       apr_thread_cond_wait(s->ppages_cond, seg->lock);
     } while (s->ppages_flushing != 0);
log_printf(5, "Flush completed\n");
  }

  //** Only writes from here on
  lo_page = *lo / s->page_size; n_pages = lo_page;               lo_page = lo_page * s->page_size;
  hi_page = *hi / s->page_size; n_pages = hi_page - n_pages + 1; hi_page = hi_page * s->page_size;

log_printf(5, "lo=" XOT " hi=" XOT " lo_page=" XOT " hi_page=" XOT " n_pages=%d \n", *lo, *hi, lo_page, hi_page, n_pages);
  //** Check if we already have the lo/hi pages loaded
  p = list_search(s->pages, (skiplist_key_t *)(&lo_page));
  if (p != NULL) {
     if (n_pages > 1) p = list_search(s->pages, (skiplist_key_t *)(&hi_page));
     if (p != NULL) {  //** End pages already loaded
log_printf(5, "Pages already loaded\n");
        segment_unlock(seg);
        return(0);
     }
  }

  //** If we made it here the end pages at least don't exist
  //** See if we map to a existing pages
  do_flush = 0;
  nhandled = 0;
  lo_mapped = 0; hi_mapped = 0;
  lo_new = *lo;  hi_new = *hi; bpos_new = *bpos;
  for (i=0; i<s->ppages_used; i++) {
     pp = &(s->ppage[i]);

     //** Interior whole page check  (always copy the data to make sure we have a full page before flushing)
     if ((n_pages > 2) && (lo_page < pp->page_start) && (pp->page_start < hi_page)) {
         poff = 0;
         boff = *bpos + pp->page_start - *lo;
         nbytes = s->page_size;
         tbuffer_single(&pptbuf, s->page_size, pp->data);
         tbuffer_copy(tbuf, boff, &pptbuf, poff, nbytes);
         do_flush = 1;
         pp->flags = 1; //** Full page
         nhandled++;
log_printf(5, "INTERIOR FULL seg=" XIDT " using ppage=%d pstart=" XOT " pend=" XOT "\n", segment_id(seg), i, pp->page_start, pp->page_end);
     }

     //** Move the hi end down
     if ((hi_page == pp->page_start) && (lo_page != hi_page)) {
         poff = 0;
         boff = *bpos + pp->page_start - *lo;
         nbytes = *hi - pp->page_start + 1;
         tbuffer_single(&pptbuf, s->page_size, pp->data);
         tbuffer_copy(tbuf, boff, &pptbuf, poff, nbytes);
         hi_new = pp->page_start - 1;
         type_malloc(rng, ex_off_t, 2);
         rng[0] = 0;
         rng[1] = nbytes - 1;
         push(pp->range_stack, rng);
         pend = pp->page_start + nbytes - 1;
         if (pend > s->ppage_max) s->ppage_max = pend;
         nhandled++;
         hi_mapped = 1;
log_printf(5, "HI_MAPPED INSERT seg=" XIDT " using ppage=%d pstart=" XOT " pend=" XOT " rlo=" XOT " rhi=" XOT "\n", segment_id(seg), i, pp->page_start, pp->page_end, rng[0], rng[1]);
     }

     //** Move the lo end partial page (also handles if lo_page=hi_page)
     if (lo_page == pp->page_start) {
         poff = *lo - pp->page_start;
         boff = *bpos;
         if (*hi > pp->page_end) {
            pend = s->page_size - 1;
         } else {
            hi_mapped = 1;
            pend = *hi - pp->page_start;
         }
         nbytes = pend - poff + 1;
         tbuffer_single(&pptbuf, s->page_size, pp->data);
         tbuffer_copy(tbuf, boff, &pptbuf, poff, nbytes);
         lo_new = *lo + nbytes;
         bpos_new = *bpos + nbytes;
         type_malloc(rng, ex_off_t, 2);
         rng[0] = poff;
         rng[1] = pend;
         pend = pp->page_start + pend;
         if (pend > s->ppage_max) s->ppage_max = pend;
         push(pp->range_stack, rng);
         nhandled++;
         lo_mapped = 1;
log_printf(5, "LO_MAPPED INSERT seg=" XIDT " using ppage=%d pstart=" XOT " pend=" XOT " rlo=" XOT " rhi=" XOT "\n", segment_id(seg), i, pp->page_start, pp->page_end, rng[0], rng[1]);
//log_printf(0, "RSTACK[%d]=%p size=%d\n", i, pp->range_stack, stack_size(pp->range_stack));
     }
  }

  //** Check if we have full coverage.  If so kick out
  if (nhandled == n_pages) {
     segment_unlock(seg);
     *lo = lo_new;  *hi = hi_new; *bpos = bpos_new;
log_printf(5, "END lo=" XOT " hi=" XOT " bpos=" XOT "\n", *lo, *hi, *bpos); flush_log();
     return(1);
  }

  //** See if we have enough free ppages to store the ends. If not flush
  if ((s->n_ppages - s->ppages_used) < (2 - lo_mapped - hi_mapped)) {
log_printf(5, "Triggering a flush\n");

     do_flush = 0;
     err = _cache_ppages_flush(seg, da);
     if (err != 0) {
        segment_unlock(seg);
        return(err);
     }

     //** During the flush we lost the lock and so the pages could have been loaded
     //** in either cache or ppages.  So we're just going to call ourself again
     *lo = lo_new;  *hi = hi_new; *bpos = bpos_new;
log_printf(5, "RECURSE lo=" XOT " hi=" XOT " bpos=" XOT "\n", *lo, *hi, *bpos); flush_log();
     segment_unlock(seg);
     return(cache_ppages_handle(seg, da, rw_mode, lo, hi, len, bpos, tbuf));
  }

  //** NOTE if we have whole pages don't store
  if (lo_mapped == 0) { // ** Map the lo end
     i = s->ppages_used; s->ppages_used++;
     pp = &(s->ppage[i]);
     pp->page_start = lo_page;
     pp->page_end = lo_page + s->page_size -1;

     poff = *lo - pp->page_start;
     boff = *bpos;
     if (*hi > pp->page_end) {
        pend = s->page_size - 1;
     } else {
        hi_mapped = 1;
        pend = *hi - pp->page_start;
     }
     nbytes = pend - poff + 1;
     tbuffer_single(&pptbuf, s->page_size, pp->data);
     tbuffer_copy(tbuf, boff, &pptbuf, poff, nbytes);
     lo_new = *lo + nbytes;
     bpos_new = *bpos + nbytes;
     type_malloc(rng, ex_off_t, 2);
     rng[0] = poff;
     rng[1] = pend;
     push(pp->range_stack, rng);
     pend = pp->page_start + pend;
     if (pend > s->ppage_max) s->ppage_max = pend;
     nhandled++;
     lo_mapped = 1;
log_printf(5, "LO_MAPPED ADDED seg=" XIDT " using ppage=%d pstart=" XOT " pend=" XOT " rlo=" XOT " rhi=" XOT "\n", segment_id(seg), i, pp->page_start, pp->page_end, rng[0], rng[1]);

  }

  if (hi_mapped == 0) { // ** Do the same for the hi end
     i = s->ppages_used; s->ppages_used++;
     pp = &(s->ppage[i]);
     pp->page_start = hi_page;
     pp->page_end = hi_page + s->page_size -1;

     poff = 0;
     boff = *bpos + pp->page_start - *lo;
     nbytes = *hi - pp->page_start + 1;
     tbuffer_single(&pptbuf, s->page_size, pp->data);
     tbuffer_copy(tbuf, boff, &pptbuf, poff, nbytes);
     hi_new = pp->page_start - 1;
     type_malloc(rng, ex_off_t, 2);
     rng[0] = 0;
     rng[1] = nbytes - 1;
     push(pp->range_stack, rng);
     pend = pp->page_start + nbytes - 1;
     if (pend > s->ppage_max) s->ppage_max = pend;
     nhandled++;
     hi_mapped = 1;
log_printf(5, "HI_MAPPED ADDED seg=" XIDT " using ppage=%d pstart=" XOT " pend=" XOT " rlo=" XOT " rhi=" XOT "\n", segment_id(seg), i, pp->page_start, pp->page_end, rng[0], rng[1]);
  }


  if ((do_flush == 1) && (nhandled != n_pages)) {  //** Do a flush if not completely covered
log_printf(1, "Triggering a flush do_flush=%d nhandled=%d n_pages=%d\n", do_flush, nhandled, n_pages);

     err = _cache_ppages_flush(seg, da);
  }

  segment_unlock(seg);
  *lo = lo_new;  *hi = hi_new; *bpos = bpos_new;
log_printf(5, "END lo=" XOT " hi=" XOT " bpos=" XOT "\n", *lo, *hi, *bpos); flush_log();
  return((n_pages == nhandled) ? 1 : 0);
}

//*******************************************************************************
// cache_rw_func - Function for reading/writing to cache
//*******************************************************************************

op_status_t cache_rw_func(void *arg, int id)
{
   cache_rw_op_t *cop = (cache_rw_op_t *)arg;
   segment_t *seg = cop->seg;
   cache_segment_t *s = (cache_segment_t *)seg->priv;
   page_handle_t page[CACHE_MAX_PAGES_RETURNED];
   iovec_t iov[CACHE_MAX_PAGES_RETURNED];
   int status, n_pages;
   Stack_t stack;
   cache_range_t *curr, *r;
   int progress, tb_err, rerr, first_time;
   int mode, i, j, top_cnt, bottom_cnt;
   op_status_t err;
   ex_off_t bpos2, bpos, poff, len, mylen, lo, hi, ngot, pstart, plen;
   ex_off_t hi_got, new_size, blen;
   ex_off_t total_bytes, hit_bytes;
   tbuffer_t tb;
   void *cache_missed_table[100];
   void **cache_missed;
   int tid = atomic_thread_id;
   apr_time_t hit_time, miss_time;

   tb_err = 0;
   err = op_success_status;

   init_stack(&stack);

   //** Push the initial ranges onto the work queue
   bpos = cop->boff;
   bpos2 = bpos;
   new_size = 0;
   rerr = 0;
   mylen = 0;
   total_bytes = 0;
   for (i=0; i< cop->n_iov; i++) {
      if (cop->iov[i].len <= 0) rerr = 1;
      lo = cop->iov[i].offset;
      len = cop->iov[i].len;
      hi = lo + len - 1;
      total_bytes += len;
      bpos2 = bpos;
      bpos += len;
      j = (cop->skip_ppages == 0) ? cache_ppages_handle(seg, cop->da, cop->rw_mode, &lo, &hi, &len, &bpos2, cop->buf) : 0;
      if (j == 0) { //** Check if the ppages slurped it up
         if (new_size < hi) new_size = hi;
         r = cache_new_range(lo, hi, bpos2, i);
         mylen += len;
         push(&stack, r);
      } else if (j < 0) {
         rerr = -1;
      }
log_printf(15, "cache_rw_func: tid=%d gid=%d START i=%d lo=" XOT " hi=" XOT " new_size=" XOT " rw_mode=%d rerr=%d\n", tid, id, i, lo, hi, new_size, cop->rw_mode, rerr);
   }

   if (stack_size(&stack) == 0) { //** Handled via ppages
      log_printf(15, "seg=" XIDT " Nothing to do. Handled by the ppage code.  rerr=%d\n", segment_id(cop->seg), rerr);
      return((rerr == 0) ? op_success_status : op_failure_status);
   }

//   hit_bytes = bpos - cop->boff;
//   hit_bytes = total_bytes - hit_bytes;
   if (new_size > 0) new_size++;

   ngot = bpos - cop->boff;

log_printf(15, "seg=" XIDT " new_size=" XOT " child_size=" XOT "\n", segment_id(cop->seg),new_size, segment_size(cop->seg));
   //** Check for some input range errors
   if (((new_size > segment_size(cop->seg)) && (cop->rw_mode == CACHE_READ)) || (rerr != 0)) {
      log_printf(1, "ERROR  Read beyond EOF, bad range, or ppage_flush error!  rw_mode=%d rerr=%d new_size=" XOT " ssize=" XOT "\n", cop->rw_mode, rerr, new_size, segment_size(cop->seg));
      while ((r = pop(&stack)) != NULL) {
         free(r);
      }
      return(op_failure_status);
   }

   //** Make space the cache miss info
   if (cop->n_iov > 100) {
      type_malloc_clear(cache_missed, void *, cop->n_iov);
   } else {
      memset(cache_missed_table, 0, sizeof(cache_missed_table));
      cache_missed = cache_missed_table;
   }

   miss_time = 0;
   hit_bytes = 0;
   first_time = 1;
   mode = CACHE_NONBLOCK;
   top_cnt = cop->n_iov;
   bottom_cnt = 0;
   progress = 0;
   hit_time = apr_time_now();
   while ((curr=(cache_range_t *)pop(&stack)) != NULL) {
      n_pages = CACHE_MAX_PAGES_RETURNED;

log_printf(15, "cache_rw_func: tid=%d processing range: lo=" XOT " hi=" XOT " progress=%d mode=%d\n", tid, curr->lo, curr->hi, progress, mode);

      if (cop->rw_mode == CACHE_READ) {
         status = cache_read_pages_get(seg, mode, curr->lo, curr->hi, &hi_got, page, iov, &n_pages, &(cache_missed[curr->iov_index]), cop->iov[curr->iov_index].len);
      } else if (cop->rw_mode == CACHE_WRITE) {
         status = cache_write_pages_get(seg, mode, curr->lo, curr->hi, &hi_got, page, iov, &n_pages, &(cache_missed[curr->iov_index]), cop->iov[curr->iov_index].len);
      } else {
log_printf(0, "ERROR invalid rw_mode!!!!!! rw_mode=%d\n", cop->rw_mode);
        err = op_error_status;
      }

      if (hi_got > curr->hi) hi_got = curr->hi;  //** Returned value is based on page size so may need to truncate

log_printf(15, "cache_rw_func: tid=%d processing range: lo=" XOT " hi=" XOT " hi_got=" XOT " rw_mode=%d mode=%d skip_mode=%d n_pages=%d\n", tid, curr->lo, curr->hi, hi_got, cop->rw_mode, mode, status, n_pages);

      if (status == 0) {  //** Got some data to process
         progress = 1;  //** Flag that progress was made

//log_printf(15, "Printing page table n_pages=%d\n", n_pages);
//for (i=0; i<n_pages; i++) {
//  log_printf(15, "   p[%d]->offset=" XOT "\n", i, page[i]->offset);
//}

         pstart = page[0].p->offset;  //** Get the current starting offset

         //** Set the page transfer buffer size
         plen = hi_got - pstart + 1;
         tbuffer_vec(&tb, plen, n_pages, iov);

         //** Determine the buffer / to page offset
         if (curr->lo >= pstart) {
            poff = curr->lo - pstart;
            bpos = curr->boff;
         } else {
            poff = 0;
            bpos = curr->boff + pstart - curr->lo;
         }

         //** and how much data to move
         len = plen - poff;
         blen = curr->hi - curr->lo + 1;
         if (blen > len) {
            blen = len;
         }

//log_printf(0, "first=%d hit=" XOT "\n", first_time, hit_bytes);
         if (first_time == 1) hit_bytes += blen;

log_printf(15, "cache_rw_func: tid=%d lo=" XOT " hi=" XOT " rw_mode=%d pstart=" XOT " poff=" XOT " bpos=" XOT " len=" XOT "\n",
  tid, curr->lo, curr->hi, cop->rw_mode, pstart, poff, bpos, blen);

         if (cop->rw_mode == CACHE_WRITE) {
            tb_err += tbuffer_copy(cop->buf, bpos, &tb, poff, blen);
            segment_lock(seg);  //** Tweak the size if needed
//            if (curr->hi > s->total_size) s->total_size = curr->hi + 1;
if (curr->hi > s->total_size) {
  log_printf(0, "seg=" XIDT " total_size=" XOT " curr->hi=" XOT "\n", segment_id(cop->seg), s->total_size, curr->hi);
  s->total_size = curr->hi + 1;
}
            segment_unlock(seg);
         } else {
            tb_err += tbuffer_copy(&tb, poff, cop->buf, bpos, blen);
         }

log_printf(15, " tb_err=%d\n", tb_err);

         //** Release the pages
         len = s->page_size;
         cache_release_pages(n_pages, page, cop->rw_mode);

         //** Add the top 1/2 of the old range back on the top of the stack if needed
         if (hi_got < curr->hi) {
            top_cnt++;
            push(&stack, cache_new_range(hi_got+1, curr->hi, curr->boff + hi_got+1 - curr->lo, curr->iov_index));
         }
      } else {  //** Empty range so push it and the extra range on the bottom of the stack to retry later
        if (hi_got == curr->lo) { //** Got nothing
           bottom_cnt++;
           move_to_bottom(&stack); //** Skipped range goes on the bottom of the stack
log_printf(15, "cache_rw_func: tid=%d got nothing inserting on bottom range lo=" XOT " hi=" XOT "\n", tid, curr->lo, curr->hi);
           insert_below(&stack, cache_new_range(curr->lo, curr->hi, curr->boff, curr->iov_index));
        } else {
           bottom_cnt++;
           move_to_bottom(&stack); //** Skipped range goes on the bottom of the stack
log_printf(15, "cache_rw_func: tid=%d inserting on bottom range lo=" XOT " hi=" XOT "\n", tid, curr->lo, hi_got);
           insert_below(&stack, cache_new_range(curr->lo, hi_got, curr->boff, curr->iov_index));

           if (hi_got < curr->hi) {  //** The upper 1/2 has data so handle it 1st
log_printf(15, "cache_rw_func: tid=%d inserting on top range lo=" XOT " hi=" XOT "\n", tid, curr->lo, hi_got);
              top_cnt++;
              push(&stack, cache_new_range(hi_got+1, curr->hi, curr->boff + hi_got+1 - curr->lo, curr->iov_index));  //** and the rest of the range on the top
           }
        }

      }

      //** If getting ready to cycle through again check if we need to switch modes
      top_cnt--;
      if (top_cnt <= 0) {
log_printf(15, "cache_rw_func: tid=%d completed cycle through list top=%d bottom=%d progress=%d\n", tid, top_cnt, bottom_cnt, progress);
         if (first_time == 1) miss_time = apr_time_now();
         first_time = 0;
         top_cnt = stack_size(&stack);
         bottom_cnt = 0;
         if (progress == 0) mode = CACHE_DOBLOCK;
         progress = 0;
      }

log_printf(15, "cache_rw_func: tid=%d bottom lo=" XOT " hi=" XOT " progress=%d mode=%d top=%d bottom=%d\n", tid, curr->lo, curr->hi, progress, mode, top_cnt, bottom_cnt);
flush_log();
      free(curr);
   }

//log_printf(0, "hit_start=" XOT " miss_start=" XOT "\n", hit_time, miss_time);
   hit_time = miss_time - hit_time;
   miss_time = apr_time_now() - miss_time;
//log_printf(0, "hit_time=" XOT " miss_time=" XOT "\n", hit_time, miss_time);

   //** Let the caching aglorithm now of the 1st missed pages
   for (i=0; i < cop->n_iov; i++) {
      if (cache_missed[i] != NULL) {
         hi = cop->iov[i].offset + cop->iov[i].len - 1;
         s->c->fn.cache_update(s->c, seg, cop->rw_mode, cop->iov[i].offset, hi, cache_missed[i]);
      }
   }
   if (cache_missed != cache_missed_table) free(cache_missed);

    //** Update the counters
  segment_lock(seg);
  if (cop->rw_mode == CACHE_READ) {
     s->stats.user.read_count++;
     s->stats.user.read_bytes += ngot;
  } else {
     s->stats.user.write_count++;
     s->stats.user.write_bytes += ngot;

     //** Update the size if needed
     if ((s->total_size < new_size) && (cop->rw_mode == CACHE_WRITE)) s->total_size = new_size;

if ((s->total_size < new_size) && (cop->rw_mode == CACHE_WRITE)) {
  log_printf(0, "seg=" XIDT " total_size=" XOT " new_size=" XOT "\n", segment_id(cop->seg), s->total_size, new_size);
  s->total_size = new_size;
}
  }
  s->stats.hit_bytes += hit_bytes;
  s->stats.miss_bytes += total_bytes - hit_bytes;
  s->stats.hit_time += hit_time;
  s->stats.miss_time += miss_time;

log_printf(15, "cache_rw_func: tid=%d END size=" XOT "\n", tid, s->total_size);

  segment_unlock(seg);


  if (tb_err != 0) err = op_failure_status;

  return(err);
}


//***********************************************************************
// cache_read - Read from cache
//***********************************************************************

op_generic_t *cache_read(segment_t *seg, data_attr_t *da, int n_iov, ex_iovec_t *iov, tbuffer_t *buffer, ex_off_t boff, int timeout)
{
  cache_rw_op_t *cop;
  cache_segment_t *s = (cache_segment_t *)seg->priv;

  type_malloc_clear(cop, cache_rw_op_t, 1);
  cop->seg = seg;
  cop->da = da;
  cop->n_iov = n_iov;
  cop->iov = iov;
  cop->rw_mode = CACHE_READ;
  cop->boff = boff;
  cop->buf = buffer;

  return(new_thread_pool_op(s->tpc_cpu, s->qname, cache_rw_func, (void *)cop, free, 1));
}


//***********************************************************************
// cache_write - Write to cache
//***********************************************************************

op_generic_t *cache_write(segment_t *seg, data_attr_t *da, int n_iov, ex_iovec_t *iov, tbuffer_t *buffer, ex_off_t boff, int timeout)
{
  cache_rw_op_t *cop;
  cache_segment_t *s = (cache_segment_t *)seg->priv;

  type_malloc_clear(cop, cache_rw_op_t, 1);
  cop->seg = seg;
  cop->da = da;
  cop->n_iov = n_iov;
  cop->iov = iov;
  cop->rw_mode = CACHE_WRITE;
  cop->boff = boff;
  cop->buf = buffer;

  return(new_thread_pool_op(s->tpc_cpu, s->qname, cache_rw_func, (void *)cop, free, 1));
}


//*******************************************************************************
// cache_flush_range - Flushes the given segment's byte range to disk
//*******************************************************************************

op_status_t cache_flush_range_func(void *arg, int id)
{
   cache_rw_op_t *cop = (cache_rw_op_t *)arg;
  cache_segment_t *s = (cache_segment_t *)cop->seg->priv;
   page_handle_t page[CACHE_MAX_PAGES_RETURNED];
   int status, n_pages, max_pages, total_pages;
   ex_off_t flush_id[3];
   Stack_t stack;
   cache_range_t *curr, *r;
   int progress;
   int mode, err;
   ex_off_t lo, hi, hi_got;
   double dt;
   apr_time_t now;


   err = OP_STATE_SUCCESS;

   init_stack(&stack);

now = apr_time_now();
log_printf(1, "COP seg=" XIDT " offset=" XOT " len=" XOT " size=" XOT "\n", segment_id(cop->seg), cop->iov_single.offset, cop->iov_single.len, segment_size(cop->seg));
flush_log();

   total_pages = 0;
   lo = cop->iov_single.offset;
   if (cop->iov_single.len == -1) cop->iov_single.len = segment_size(cop->seg);  //** if len == -1 flush the whole file
   hi = lo + cop->iov_single.len - 1;

   if (hi == -1) {  //** segment_size == 0 so nothing to do
      return(op_success_status);
   }

   //** Push myself on the flush stack
   segment_lock(cop->seg);
   flush_id[0] = lo;  flush_id[1] = hi;  flush_id[2] = atomic_inc(_flush_count);
   push(s->flush_stack, flush_id);
   segment_unlock(cop->seg);

   max_pages = CACHE_MAX_PAGES_RETURNED;
//   max_pages = 128*1024*1024 / s->page_size;
//   if (max_pages > CACHE_MAX_PAGES_RETURNED) max_pages = CACHE_MAX_PAGES_RETURNED;

log_printf(5, "START seg=" XIDT " lo=" XOT " hi=" XOT " flush_id=" XOT "\n", segment_id(cop->seg), lo, hi, flush_id[2]);
   r = cache_new_range(lo, hi, 0, 0);
   push(&stack, r);

   mode = CACHE_NONBLOCK;
   progress = 0;
   while ((curr=(cache_range_t *)pop(&stack)) != NULL) {
log_printf(5, "cache_flush_range_func: processing range: lo=" XOT " hi=" XOT " mode=%d\n", curr->lo, curr->hi, mode);
      n_pages = max_pages;
      status = cache_dirty_pages_get(cop->seg, mode, curr->lo, curr->hi, &hi_got, page, &n_pages);
log_printf(1, "seg=" XIDT " processing range: lo=" XOT " hi=" XOT " hi_got=" XOT " mode=%d skip_mode=%d n_pages=%d\n", segment_id(cop->seg), curr->lo, curr->hi, hi_got, mode, status, n_pages);
flush_log();

      if (status == 0) {  //** Got some data to process
        progress = 1;  //** Flag that progress was made
        total_pages += n_pages;
        err = (n_pages > 0) ? cache_rw_pages(cop->seg, page, n_pages, CACHE_FLUSH, 1) : 0;
        err = (err == 0) ? OP_STATE_SUCCESS : OP_STATE_FAILURE;
        if (curr->hi > hi_got) push(&stack, cache_new_range(hi_got+1, curr->hi, 0, 0));  //** and the rest of the range on the top
      } else if ( status == 1) {  //** Empty range so push it and the extra range on the stackon the stack to retry later
        if (hi_got == curr->lo) { //** Got nothing
           move_to_bottom(&stack); //** Skipped range goes on the bottom of the stack
log_printf(5, "got nothing inserting on bottom range lo=" XOT " hi=" XOT "\n", curr->lo, curr->hi);
           insert_below(&stack, cache_new_range(curr->lo, curr->hi, 0, 0));
        } else {
           move_to_bottom(&stack); //** Skipped range goes on the bottom of the stack
log_printf(5, "inserting on bottom range lo=" XOT " hi=" XOT "\n", curr->lo, hi_got);
           insert_below(&stack, cache_new_range(curr->lo, hi_got, 0, 0));

           if (hi_got < curr->hi) {  //** The upper 1/2 has data so handle it 1st
log_printf(5, "inserting on top range lo=" XOT " hi=" XOT "\n", curr->lo, hi_got);
              push(&stack, cache_new_range(hi_got+1, curr->hi, 0, 0));  //** and the rest of the range on the top
           }
        }
      } else {
        err = OP_STATE_FAILURE;
      }

      //** If getting ready to cycle through again check if we need to switch modes
      if (hi_got == hi) {
         if (progress == 0) mode = CACHE_DOBLOCK;
         progress = 0;
      }

      free(curr);
   }

   //** Wait for underlying flushed to complete
   flush_wait(cop->seg, flush_id);

   segment_lock(cop->seg);
   //** Remove myself from the stack
   move_to_top(s->flush_stack);
   while ((ex_off_t *)get_ele_data(s->flush_stack) != flush_id) {
      move_down(s->flush_stack);
   }
   delete_current(s->flush_stack, 0, 0);
log_printf(5, "END seg=" XIDT " lo=" XOT " hi=" XOT " flush_id=" XOT " AFTER WAIT\n", segment_id(cop->seg), lo, hi, flush_id[2]);

   //** Notify anyone else
   apr_thread_cond_broadcast(s->flush_cond);

   //** Now wait for any overlapping flushes that chould have started during my run to complete as well
   progress = flush_id[2];
   flush_id[2] = atomic_get(_flush_count) + 1;
   segment_unlock(cop->seg);

   flush_wait(cop->seg, flush_id);
   flush_id[2] = progress;

dt = apr_time_now() - now;
dt /= APR_USEC_PER_SEC;
log_printf(1, "END seg=" XIDT " lo=" XOT " hi=" XOT " flush_id=" XOT " total_pages=%d status=%d dt=%lf\n", segment_id(cop->seg), lo, hi, flush_id[2], total_pages, err, dt);
  return((err == OP_STATE_SUCCESS) ? op_success_status : op_failure_status);
}

//***********************************************************************
// cache_flush_range - Flush dirty pages to disk
//***********************************************************************

op_generic_t *cache_flush_range(segment_t *seg, data_attr_t *da, ex_off_t lo, ex_off_t hi, int timeout)
{
  cache_rw_op_t *cop;
  cache_segment_t *s = (cache_segment_t *)seg->priv;

  type_malloc(cop, cache_rw_op_t, 1);
  cop->seg = seg;
  cop->da = da;
  cop->iov = &(cop->iov_single);
  cop->iov_single.offset = lo;
  cop->iov_single.len = (hi == -1) ? -1 : hi - lo + 1;
  cop->rw_mode = CACHE_READ;
  cop->boff = 0;
  cop->buf = NULL;
  cop->timeout = timeout;

  return(new_thread_pool_op(s->tpc_unlimited, s->qname, cache_flush_range_func, (void *)cop, free, 1));
}


//***********************************************************************
// segment_cache_stats - Returns the cache stats for the segment
//***********************************************************************

cache_stats_t segment_cache_stats(segment_t *seg)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  cache_stats_t cs;

  segment_lock(seg);
  cs = s->stats;
  segment_unlock(seg);

  return(cs);
}

//***********************************************************************
// cache_stats - Returns the overal cache stats
//   Returns the number of skipped segments due to locking
//***********************************************************************

int cache_stats(cache_t *c, cache_stats_t *cs)
{
  cache_segment_t *s;
  list_iter_t it;
  segment_t *seg2;
  ex_id_t *sid2;
  int i, n;

  n = 0;
  cache_lock(c);

  *cs = c->stats;
//log_printf(0, "core hit=" XOT "\n", cs->hit_bytes);
//log_printf(0, "core miss=" XOT "\n", cs->miss_bytes);

  n = list_key_count(c->segments);
  it = list_iter_search(c->segments, NULL, 0);
  for (i=0; i<n; i++) {
     list_next(&it, (list_key_t **)&sid2, (list_data_t **)&seg2);

     if (seg2 != NULL) {
       if (apr_thread_mutex_trylock(seg2->lock) == APR_SUCCESS) {
          s = (cache_segment_t *)seg2->priv;
          cs->system.read_count += s->stats.system.read_count;
          cs->system.write_count += s->stats.system.write_count;
          cs->system.read_bytes += s->stats.system.read_bytes;
          cs->system.write_bytes += s->stats.system.write_bytes;

          cs->user.read_count += s->stats.user.read_count;
          cs->user.write_count += s->stats.user.write_count;
          cs->user.read_bytes += s->stats.user.read_bytes;
          cs->user.write_bytes += s->stats.user.write_bytes;

          cs->hit_time += s->stats.hit_time;
          cs->miss_time += s->stats.miss_time;
          cs->hit_bytes += s->stats.hit_bytes;
          cs->miss_bytes += s->stats.miss_bytes;
          cs->unused_bytes += s->stats.unused_bytes;
          segment_unlock(seg2);
        } else {
          n++;
        }
     }
//log_printf(0, "after cycle hit=" XOT "\n", cs->hit_bytes);
//log_printf(0, "after cycle miss=" XOT "\n", cs->miss_bytes);

  }

  cache_unlock(c);

  return(n);
}

//***********************************************************************
// cache_stats_print - Prints the cache stats to a string
//***********************************************************************

int cache_stats_print(cache_stats_t *cs, char *buffer, int *used, int nmax)
{
   int n = 0;
   ex_off_t tsum1, tsum2, sum1, sum2;
   double d1, d2, d3, dt, drate;

   d1 = cs->system.read_bytes * 1.0 / (1024.0*1024.0*1024.0);
   n += append_printf(buffer, used, nmax, "System:: Read " XOT " bytes (%lf GiB) in " XOT " ops\n", cs->system.read_bytes, d1, cs->system.read_count);
   d1 = cs->system.write_bytes * 1.0 / (1024.0*1024.0*1024.0);
   n += append_printf(buffer, used, nmax, "System:: Write " XOT " bytes (%lf GiB) in " XOT " ops\n", cs->system.write_bytes, d1, cs->system.write_count);
   d1 = cs->user.read_bytes * 1.0 / (1024.0*1024.0*1024.0);
   n += append_printf(buffer, used, nmax, "User:: Read " XOT " bytes (%lf GiB) in " XOT " ops\n", cs->user.read_bytes, d1, cs->user.read_count);
   d1 = cs->user.write_bytes * 1.0 / (1024.0*1024.0*1024.0);
   n += append_printf(buffer, used, nmax, "User:: Write " XOT " bytes (%lf GiB) in " XOT " ops\n", cs->user.write_bytes, d1, cs->user.write_count);

   tsum1 = cs->system.read_bytes + cs->user.read_bytes;
   tsum2 = cs->system.read_count + cs->user.read_count;
   d1 = tsum1 * 1.0 / (1024.0*1024.0*1024.0);
   n += append_printf(buffer, used, nmax, "Total:: Read " XOT " bytes (%lf GiB) in " XOT " ops\n", tsum1, d1, tsum2);

   sum1 = cs->system.write_bytes + cs->user.write_bytes;
   sum2 = cs->system.write_count + cs->user.write_count;
   d1 = sum1 * 1.0 / (1024.0*1024.0*1024.0);
   n += append_printf(buffer, used, nmax, "Total:: Write " XOT " bytes (%lf GiB) in " XOT " ops\n", sum1, d1, sum2);

   d1 = cs->unused_bytes * 1.0 / (1024.0*1024.0*1024.0);
   n += append_printf(buffer, used, nmax, "Unused " XOT " bytes (%lf GiB)\n", cs->unused_bytes, d1);

//log_printf(0, "hit=" XOT "\n", cs->hit_bytes);
//log_printf(0, "miss=" XOT "\n", cs->miss_bytes);

   dt = cs->hit_time; dt = dt / (1.0*APR_USEC_PER_SEC);
   drate = cs->hit_bytes * 1.0 / (1024.0*1024.0*dt);
   d1 = cs->hit_bytes + cs->miss_bytes;
   d2 = (d1 > 0) ? (100.0*cs->hit_bytes) / d1 : 0;
   d3 = cs->hit_bytes * 1.0 / (1024.0*1024.0*1024.0);
   n += append_printf(buffer, used, nmax, "Hits: " XOT " bytes (%lf GiB) (%lf%% total) (%lf sec %lf MB/s)\n", cs->hit_bytes, d3, d2, dt, drate);

   dt = cs->miss_time; dt = dt / (1.0*APR_USEC_PER_SEC);
   drate = cs->miss_bytes * 1.0 / (1024.0*1024.0*dt);
   d2 = (d1 > 0) ? (100.0*cs->miss_bytes) / d1 : 0;
   d3 = cs->miss_bytes * 1.0 / (1024.0*1024.0*1024.0);
   n += append_printf(buffer, used, nmax, "Misses: " XOT " bytes (%lf GiB) (%lf%% total) (%lf sec %lf MB/s)\n", cs->miss_bytes, d3, d2, dt, drate);

   d3 = cs->dirty_bytes * 1.0 / (1024.0*1024.0*1024.0);
   n += append_printf(buffer, used, nmax, "Dirty: " XOT " bytes (%lf GiB)\n", cs->dirty_bytes, d3);

   return(n);
}

//***********************************************************************
// segcache_inspect - Issues integrity checks for the underlying segments
//***********************************************************************

op_generic_t *segcache_inspect(segment_t *seg, data_attr_t *da, info_fd_t *fd, int mode, ex_off_t bufsize, int timeout)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;

  if ((mode != INSPECT_SOFT_ERRORS) && (mode != INSPECT_HARD_ERRORS)) {
     info_printf(fd, 1, XIDT ": Cache segment maps to child " XIDT "\n", segment_id(seg), segment_id(s->child_seg));
  }
  return(segment_inspect(s->child_seg, da, fd, mode, bufsize, timeout));
}

//*******************************************************************************
// cache_truncate_func - Function for truncating cache pages and actual segment
//*******************************************************************************

op_status_t segcache_truncate_func(void *arg, int id)
{
  cache_truncate_op_t *cop = (cache_truncate_op_t *)arg;
  cache_segment_t *s = (cache_segment_t *)cop->seg->priv;
  ex_off_t old_size;
  op_generic_t *gop;
  int err1, err2;
  op_status_t status;

  //** Adjust the size
  segment_lock(cop->seg);
  _cache_ppages_flush(cop->seg, cop->da); //** Flush any partial pages first
 
  old_size = s->total_size;
  s->total_size = cop->new_size;
  segment_unlock(cop->seg);

  //** If shrinking the file need to destroy excess cache pages
  if (cop->new_size < old_size) {
     cache_page_drop(cop->seg, cop->new_size, old_size+1);
  }

  //** Do a cache flush
  gop = segment_flush(cop->seg, cop->da, 0, cop->new_size, cop->timeout);
  err1 = gop_waitall(gop);
  gop_free(gop, OP_DESTROY);

  //** Perform the truncate on the underlying segment
  gop = segment_truncate(s->child_seg, cop->da, cop->new_size, cop->timeout);
  err2 = gop_waitall(gop);

  segment_lock(cop->seg);
  old_size = segment_size(s->child_seg);
  if (old_size > 0) {
     s->child_last_page = segment_size(s->child_seg) / s->page_size;
     s->child_last_page *= s->page_size;
  } else {
     s->child_last_page = -1;
  }
  segment_unlock(cop->seg);

  gop_free(gop, OP_DESTROY);

  status = ((err1 == OP_STATE_SUCCESS) && (err2 == OP_STATE_SUCCESS)) ? op_success_status : op_failure_status;
  return(status);
}

//***********************************************************************
// segcache_truncate - Truncates the underlying segment and flushes
//     cache as needed.
//***********************************************************************

op_generic_t *segcache_truncate(segment_t *seg, data_attr_t *da, ex_off_t new_size, int timeout)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  cache_truncate_op_t *cop;
  op_generic_t *gop;

  type_malloc(cop, cache_truncate_op_t, 1);
  cop->seg = seg;
  cop->da = da;
  cop->new_size = new_size;
  cop->timeout = timeout;

  gop = new_thread_pool_op(s->tpc_unlimited, NULL, segcache_truncate_func, (void *)cop, free, 1);


  return(gop);
}

//*******************************************************************************
// segcache_clone_func - Does the clone function
//*******************************************************************************

op_status_t segcache_clone_func(void *arg, int id)
{
  cache_clone_t *cop = (cache_clone_t *)arg;
//  cache_segment_t *ss = (cache_segment_t *)cop->sseg->priv;
  cache_segment_t *ds = (cache_segment_t *)cop->dseg->priv;
  op_status_t status;

  status = (gop_waitall(cop->gop) == OP_STATE_SUCCESS) ? op_success_status : op_failure_status;
  gop_free(cop->gop, OP_DESTROY);

  atomic_inc(ds->child_seg->ref_count);
  return(status);
}

//***********************************************************************
// segcache_signature - Generates the segment signature
//***********************************************************************

int segcache_signature(segment_t *seg, char *buffer, int *used, int bufsize)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;

  append_printf(buffer, used, bufsize, "cache()\n");

  return(segment_signature(s->child_seg, buffer, used, bufsize));
}

//***********************************************************************
// segcache_clone - Clones a segment
//***********************************************************************

op_generic_t *segcache_clone(segment_t *seg, data_attr_t *da, segment_t **clone_seg, int mode, void *arg, int timeout)
{
  segment_t *clone;
  cache_segment_t *ss, *sd;
  cache_clone_t *cop;
  int use_existing = (*clone_seg != NULL) ? 1 : 0;

  //** Make the base segment
  if (use_existing == 0) *clone_seg = segment_cache_create(seg->ess);
  clone = *clone_seg;
  sd = (cache_segment_t *)clone->priv;
  ss = (cache_segment_t *)seg->priv;

  //** Copy the header
  if ((seg->header.name != NULL) && (use_existing == 0)) clone->header.name = strdup(seg->header.name);

  //** Basic size info
  sd->total_size = ss->total_size;
  sd->page_size = ss->page_size;

  type_malloc(cop, cache_clone_t, 1);
  cop->sseg = seg;
  cop->dseg = clone;
  if (use_existing == 1) atomic_dec(sd->child_seg->ref_count);
  cop->gop = segment_clone(ss->child_seg, da, &(sd->child_seg), mode, arg, timeout);

  return(new_thread_pool_op(ss->tpc_unlimited, NULL, segcache_clone_func, (void *)cop, free, 1));
}


//***********************************************************************
// seglin_size - Returns the segment size.
//***********************************************************************

ex_off_t segcache_size(segment_t *seg)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  ex_off_t size;

  segment_lock(seg);
  size = (s->total_size > (s->ppage_max+1)) ? s->total_size : s->ppage_max + 1;
  log_printf(5, "seg=" XIDT " total_size=" XOT " ppage_max=" XOT " size=" XOT "\n", segment_id(seg), s->total_size, s->ppage_max, size);
  segment_unlock(seg);
  return(size);
}

//***********************************************************************
// seglin_block_size - Returns the segment block size.
//***********************************************************************

ex_off_t segcache_block_size(segment_t *seg)
{
  return(1);
}

//***********************************************************************
// segcache_remove - DECrements the ref counts for the segment which could
//     result in the data being removed.
//***********************************************************************

op_generic_t *segcache_remove(segment_t *seg, data_attr_t *da, int timeout)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;

  cache_page_drop(seg, 0, s->total_size + 1);
  return(segment_remove(s->child_seg, da, timeout));
}

//***********************************************************************
// segcache_serialize_text -Convert the segment to a text based format
//***********************************************************************

int segcache_serialize_text(segment_t *seg, exnode_exchange_t *exp)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  int bufsize=1024*1024;
  char segbuf[bufsize];
  char *etext;
  int sused;
  exnode_exchange_t child_exp;

  segbuf[0] = 0;

  sused = 0;

  //** Store the segment header
  append_printf(segbuf, &sused, bufsize, "[segment-" XIDT "]\n", seg->header.id);
  if ((seg->header.name != NULL) && (strcmp(seg->header.name, "") != 0)) {
     etext = escape_text("=", '\\', seg->header.name);
     append_printf(segbuf, &sused, bufsize, "name=%s\n", etext);  free(etext);
  }
  append_printf(segbuf, &sused, bufsize, "type=%s\n", SEGMENT_TYPE_CACHE);
  append_printf(segbuf, &sused, bufsize, "ref_count=%d\n", seg->ref_count);

  //** Basic size info
  append_printf(segbuf, &sused, bufsize, "used_size=" XOT "\n", s->total_size);

  //** And the child segment link
  append_printf(segbuf, &sused, bufsize, "segment=" XIDT "\n", segment_id(s->child_seg));

  //** Serialize the child as well
  child_exp.type = EX_TEXT;
  child_exp.text = NULL;
  segment_serialize(s->child_seg, &child_exp);

  //** And merge everything together
  if (child_exp.text != NULL) {
    exnode_exchange_append_text(exp, child_exp.text);
    exnode_exchange_free(&child_exp);
  }
  exnode_exchange_append_text(exp, segbuf);

  return(0);
}


//***********************************************************************
// segcache_serialize_proto -Convert the segment to a protocol buffer
//***********************************************************************

int segcache_serialize_proto(segment_t *seg, exnode_exchange_t *exp)
{
//  cache_segment_t *s = (cache_segment_t *)seg->priv;

  return(-1);
}

//***********************************************************************
// segcache_serialize -Convert the segment to a more portable format
//***********************************************************************

int segcache_serialize(segment_t *seg, exnode_exchange_t *exp)
{
  if (exp->type == EX_TEXT) {
     return(segcache_serialize_text(seg, exp));
  } else if (exp->type == EX_PROTOCOL_BUFFERS) {
     return(segcache_serialize_proto(seg, exp));
  }
 
  return(-1);
}

//***********************************************************************
// segcache_deserialize_text -Read the text based segment
//***********************************************************************

int segcache_deserialize_text(segment_t *seg, ex_id_t myid, exnode_exchange_t *exp)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  int bufsize=1024;
  char seggrp[bufsize];
  char qname[512];
  inip_file_t *fd;
  ex_off_t n, child_size;
  ex_id_t id;
  int i;

  //** Parse the ini text
  fd = inip_read_text(exp->text);

  //** Make the segment section name
  snprintf(seggrp, bufsize, "segment-" XIDT, myid);

  //** Basic size info
  s->total_size = inip_get_integer(fd, seggrp, "used_size", -1);

  //** Load the child
  id = inip_get_integer(fd, seggrp, "segment", 0);
  if (id == 0) {
     log_printf(0, "ERROR missing child segment tag initial sid=" XIDT " myid=" XIDT "\n",segment_id(seg), myid);
     flush_log();
     inip_destroy(fd);
     return (-1);
  }

//s->child_seg = NULL;
//log_printf(0, "ERROR FORCING child_seg = NULL initial sid=" XIDT " myid=" XIDT " cid=" XIDT "\n",segment_id(seg), myid, id);
//flush_log();
//inip_destroy(fd);
//return(-2);

  s->child_seg = load_segment(seg->ess, id, exp);
  if (s->child_seg == NULL) {
     log_printf(0, "ERROR child_seg = NULL initial sid=" XIDT " myid=" XIDT " cid=" XIDT "\n",segment_id(seg), myid, id);
     flush_log();
     inip_destroy(fd);
     return(-2);
  }

  //** Remove my random ID from the segments table
  if (s->c) {
     cache_lock(s->c);
     log_printf(5, "CSEG-I Removing seg=" XIDT " nsegs=%d\n", segment_id(seg), list_key_count(s->c->segments)); flush_log();
     list_remove(s->c->segments, &(segment_id(seg)), seg);
     s->c->fn.removing_segment(s->c, seg);
     cache_unlock(s->c);
  }

  //** Get the segment header info
  seg->header.id = myid;
  if (s->qname != NULL) free(s->qname);
  snprintf(qname, sizeof(qname), XIDT HP_HOSTPORT_SEPARATOR "1" HP_HOSTPORT_SEPARATOR "0" HP_HOSTPORT_SEPARATOR "0", seg->header.id);
  s->qname = strdup(qname);

  seg->header.type = SEGMENT_TYPE_CACHE;
  seg->header.name = inip_get_string(fd, seggrp, "name", "");

  atomic_inc(s->child_seg->ref_count);

  //** Tweak the page size
  s->page_size = segment_block_size(s->child_seg);
  if (s->c != NULL) {
     if (s->page_size < s->c->default_page_size) {
        n = s->c->default_page_size / s->page_size;
        if ((s->c->default_page_size % s->page_size) > 0) n++;
        s->page_size = n * s->page_size;
     }
  }

  //** If total_size is -1 or child is smaller use the size from child
  child_size = segment_size(s->child_seg);
//QWERT CHECK  if ((s->total_size < 0) || (s->total_size > child_size)) s->total_size = child_size;

  //** Determine the child segment size so we don't have to call it
  //** on R/W and risk getting blocked due to child grow operations
  if (child_size > 0) {
     s->child_last_page = child_size / s->page_size;
     s->child_last_page *= s->page_size;
  } else {
     s->child_last_page = -1;  //** No pages
  }
log_printf(5, "seg=" XIDT " Initial child_last_page=" XOT " child_size=" XOT " page_size=" XOT "\n", segment_id(seg), s->child_last_page, child_size, s->page_size);

  //** Make the partial pages table
  s->n_ppages = (s->c != NULL) ? s->c->n_ppages : 0;
  s->ppage_max = -1;
  if (s->n_ppages > 0) {
     type_malloc_clear(s->ppage, cache_partial_page_t, s->n_ppages);
     type_malloc_clear(s->ppages_buffer, char, s->n_ppages*s->page_size);
     for (i=0; i<s->n_ppages; i++) {
       s->ppage[i].data = &(s->ppages_buffer[i*s->page_size]);
       s->ppage[i].range_stack = new_stack();
//log_printf(0, "RSTACK[%d]=%p\n", i, s->ppage[i].range_stack);
     }
  }

  //** and reinsert myself with the new ID
  if (s->c != NULL) {
     cache_lock(s->c);
     log_printf(5, "CSEG Inserting seg=" XIDT " nsegs=%d\n", segment_id(seg), list_key_count(s->c->segments)); flush_log();
     list_insert(s->c->segments, &(segment_id(seg)), seg);
     s->c->fn.adding_segment(s->c, seg);
     cache_unlock(s->c);
  }

  inip_destroy(fd);

  n = (s->c == NULL) ? 0 : s->c->default_page_size;
  log_printf(15, "segcache_deserialize_text: seg=" XIDT " page_size=" XOT " default=" XOT "\n", segment_id(seg), s->page_size, n);
  return(0);
}

//***********************************************************************
// segcache_deserialize_proto - Read the prot formatted segment
//***********************************************************************

int segcache_deserialize_proto(segment_t *seg, ex_id_t id, exnode_exchange_t *exp)
{
  return(-1);
}

//***********************************************************************
// segcache_deserialize -Convert from the portable to internal format
//***********************************************************************

int segcache_deserialize(segment_t *seg, ex_id_t id, exnode_exchange_t *exp)
{
  if (exp->type == EX_TEXT) {
     return(segcache_deserialize_text(seg, id, exp));
  } else if (exp->type == EX_PROTOCOL_BUFFERS) {
     return(segcache_deserialize_proto(seg, id, exp));
  }

  return(-1);
}

//***********************************************************************
// segcache_destroy - Destroys the cache segment
//***********************************************************************

void segcache_destroy(segment_t *seg)
{
  cache_segment_t *s = (cache_segment_t *)seg->priv;
  op_generic_t *gop;
  int i;

  //** Check if it's still in use
log_printf(2, "segcache_destroy: seg->id=" XIDT " ref_count=%d\n", segment_id(seg), seg->ref_count);
//flush_log();

//log_printf(2, "CACHE-PTR seg=" XIDT " s->c=%p\n", segment_id(seg), s->c);

  if (seg->ref_count > 0) return;

//log_printf(0, "Before close/flush seg=" XIDT "\n", segment_id(seg));
CACHE_PRINT;

  //** Flag the segment as being removed and flush any ppages
  segment_lock(seg);
  s->close_requested = 1;
  if (s->c != NULL) _cache_ppages_flush(seg, s->c->da);
  segment_unlock(seg);

  //** IF s->c == NULL then we are just cloning the structure or serial/deserializing an exnode
  //** There should be no data loaded
  if (s->c != NULL) {
     //** Flush everything to backing store
     gop = segment_flush(seg, s->c->da, 0, s->total_size+1, s->c->timeout);
     gop_waitall(gop);
     gop_free(gop, OP_DESTROY);

     //** Remove it from the cache manager
     cache_lock(s->c);
     log_printf(5, "CSEG Removing seg=" XIDT " nsegs=%d\n", segment_id(seg), list_key_count(s->c->segments)); flush_log();
     list_remove(s->c->segments, &(segment_id(seg)), seg);
     cache_unlock(s->c);

     //** Got to check if a dirty thread is trying to do an empty flush
     cache_lock(s->c);
     while (atomic_get(s->cache_check_in_progress) != 0) {
       cache_unlock(s->c);
       log_printf(5, "seg=" XIDT " waiting for dirty flush/prefetch to complete\n", segment_id(seg));
       usleep(10000);
       cache_lock(s->c);
     }
     cache_unlock(s->c);

     //** and drop the cache pages
     cache_page_drop(seg, 0, s->total_size + 1);

     cache_lock(s->c);
     s->c->fn.removing_segment(s->c, seg);  //** Do the final remove
     cache_unlock(s->c);
  }

  //** Drop the flush args
  apr_thread_cond_destroy(s->flush_cond);
  free_stack(s->flush_stack, 0);

//log_printf(0, "After flush/drop seg=" XIDT "\n", segment_id(seg));
CACHE_PRINT;

  log_printf(5, "seg=" XIDT " Starting segment destruction\n", segment_id(seg));

  //** Clean up the list
  list_destroy(s->pages);

  //** Destroy the child segment as well
  if (s->child_seg != NULL) {
     atomic_dec(s->child_seg->ref_count);
     segment_destroy(s->child_seg);
  }

  //** and finally the misc stuff
  if (s->n_ppages > 0) {
     for (i=0; i<s->n_ppages; i++) {
        free_stack(s->ppage[i].range_stack, 1);
     }
     free(s->ppages_buffer);
     free(s->ppage);
  }
  apr_thread_mutex_destroy(seg->lock);
  apr_thread_cond_destroy(seg->cond);
  apr_thread_cond_destroy(s->ppages_cond);
  apr_pool_destroy(seg->mpool);

  free(s->qname);
  free(s);

  ex_header_release(&(seg->header));

  free(seg);
}

//***********************************************************************
// segment_cache_create - Creates a cache segment
//***********************************************************************

segment_t *segment_cache_create(void *arg)
{
  service_manager_t *es = (service_manager_t *)arg;
  cache_segment_t *s;
  segment_t *seg;
  char qname[512];

  //** Make the space
  type_malloc_clear(seg, segment_t, 1);
  type_malloc_clear(s, cache_segment_t, 1);

  assert(apr_pool_create(&(seg->mpool), NULL) == APR_SUCCESS);
  apr_thread_mutex_create(&(seg->lock), APR_THREAD_MUTEX_DEFAULT, seg->mpool);
  apr_thread_cond_create(&(seg->cond), seg->mpool);
  apr_thread_cond_create(&(s->flush_cond), seg->mpool);
  apr_thread_cond_create(&(s->ppages_cond), seg->mpool);

  s->flush_stack = new_stack();
  s->tpc_unlimited = lookup_service(es, ESS_RUNNING, ESS_TPC_UNLIMITED);
  s->tpc_cpu = lookup_service(es, ESS_RUNNING, ESS_TPC_CPU);
  s->pages = list_create(0, &skiplist_compare_ex_off, NULL, NULL, NULL);
  s->c = lookup_service(es, ESS_RUNNING, ESS_CACHE);
  if (s->c != NULL) s->c = cache_get_handle(s->c);
  s->page_size = 64*1024;
  s->n_ppages = 0;

log_printf(2, "CACHE-PTR seg=" XIDT " s->c=%p\n", segment_id(seg), s->c);

  generate_ex_id(&(seg->header.id));
  atomic_set(seg->ref_count, 0);
  seg->header.type = SEGMENT_TYPE_CACHE;

  snprintf(qname, sizeof(qname), XIDT HP_HOSTPORT_SEPARATOR "1" HP_HOSTPORT_SEPARATOR "0" HP_HOSTPORT_SEPARATOR "0", seg->header.id);
  s->qname = strdup(qname);

  seg->ess = es;
  seg->priv = s;
  seg->fn.read = cache_read;
  seg->fn.write = cache_write;
  seg->fn.inspect = segcache_inspect;
  seg->fn.truncate = segcache_truncate;
  seg->fn.remove = segcache_remove;
  seg->fn.flush = cache_flush_range;
  seg->fn.clone = segcache_clone;
  seg->fn.signature = segcache_signature;
  seg->fn.size = segcache_size;
  seg->fn.block_size = segcache_block_size;
  seg->fn.serialize = segcache_serialize;
  seg->fn.deserialize = segcache_deserialize;
  seg->fn.destroy = segcache_destroy;

  if (s->c != NULL) { //** If no cache backend skip this  only used for temporary deseril/serial
     cache_lock(s->c);
 CACHE_PRINT;
     log_printf(5, "CSEG-I Inserting seg=" XIDT " nsegs=%d\n", segment_id(seg), list_key_count(s->c->segments)); flush_log();
     list_insert(s->c->segments, &(segment_id(seg)), seg);
     s->c->fn.adding_segment(s->c, seg);
 CACHE_PRINT;
     cache_unlock(s->c);
  }

  return(seg);
}

//***********************************************************************
// segment_cache_load - Loads a cache segment from ini/ex3
//***********************************************************************

segment_t *segment_cache_load(void *arg, ex_id_t id, exnode_exchange_t *ex)
{
  segment_t *seg = segment_cache_create(arg);
  if (segment_deserialize(seg, id, ex) != 0) {
     segment_destroy(seg);
     seg = NULL;
  }
  return(seg);
}

