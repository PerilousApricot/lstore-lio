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

#define _log_module_index 193

#include "lio.h"
#include "type_malloc.h"
#include "log.h"
#include "hwinfo.h"
#include "apr_wrapper.h"
#include "log.h"
#include "string_token.h"

typedef struct {
  int count;
  void *object;
  char *key;
} lc_object_container_t;

int lio_parallel_task_count = 100;

//** Define the global LIO config
lio_config_t *lio_gc = NULL;
cache_t *_lio_cache = NULL;
info_fd_t *lio_ifd = NULL;

apr_pool_t *_lc_mpool = NULL;
apr_thread_mutex_t *_lc_lock = NULL;
list_t *_lc_object_list = NULL;

lio_config_t *lio_create_nl(char *fname, char *section, char *user);
void lio_destroy_nl(lio_config_t *lio);

//***************************************************************
//  _lc_object_destroy - Decrements the LC object and removes it
//       if no other references exist.  It returns the number of
//       remaining references.  So it can be safely destroyed
//       when 0 is returned.
//***************************************************************

int _lc_object_destroy(char *key)
{
  int count = 0;
  lc_object_container_t *lcc;

  lcc = list_search(_lc_object_list, key);
  if (lcc != NULL) {
    lcc->count--;
    count = lcc->count;
    log_printf(15, "REMOVE key=%s count=%d lcc=%p\n", key, count, lcc);

    if (lcc->count <= 0) {
       list_remove(_lc_object_list, key, lcc);
       free(lcc->key);
       free(lcc);
    }
  } else {
    log_printf(15, "REMOVE-FAIL key=%s\n", key);
  }

  return(count);
}

//***************************************************************
//  _lc_object_get - Retrieves an LC object
//***************************************************************

void *_lc_object_get(char *key)
{
  void *obj = NULL;
  lc_object_container_t *lcc;

  lcc = list_search(_lc_object_list, key);
  if (lcc != NULL) {
    lcc->count++;
    obj = lcc->object;
  }

if (obj != NULL) {
  log_printf(15, "GET key=%s count=%d lcc=%p\n", key, lcc->count, lcc);
} else {
  log_printf(15, "GET-FAIL key=%s\n", key);
}
  return(obj);
}

//***************************************************************
//  lc_object_put - Adds an LC object
//***************************************************************

void _lc_object_put(char *key, void *obj)
{
  lc_object_container_t *lcc;

  type_malloc(lcc, lc_object_container_t, 1);

  log_printf(15, "PUT key=%s count=1 lcc=%p\n", key, lcc);

  lcc->count = 1;
  lcc->object = obj;
  lcc->key = strdup(key);
  list_insert(_lc_object_list, lcc->key, lcc);

  return;
}

//***************************************************************
//  lio_path_release - Decrs a path tuple object
//***************************************************************

void lio_path_release(lio_path_tuple_t *tuple)
{
  char buffer[1024];

  if (tuple->path != NULL) free(tuple->path);
  if (tuple->lc == NULL) return;

  apr_thread_mutex_lock(_lc_lock);
  snprintf(buffer, sizeof(buffer), "tuple:%s@%s", an_cred_get_id(tuple->creds), tuple->lc->section_name);
  _lc_object_destroy(buffer);

  snprintf(buffer, sizeof(buffer), "lc:%s", tuple->lc->section_name);
  _lc_object_destroy(buffer);

  apr_thread_mutex_unlock(_lc_lock);

  return;
}

//***************************************************************
//  lio_path_resolve - Returns a  path tuple object
//      containing the cred, lc, and path
//***************************************************************

lio_path_tuple_t lio_path_resolve(char *lpath)
{
  char *userid, *section_name, *fname;
  char *cred_args[2];
  lio_path_tuple_t tuple, *tuple2;
  char buffer[1024];
  int n, is_lio;

  is_lio = lio_parse_path(lpath, &userid, &section_name, &fname);

log_printf(15, "lpath=%s user=%s lc=%s path=%s\n", lpath, userid, section_name, fname);

  apr_thread_mutex_lock(_lc_lock);

  n = 0;
  if ((userid == NULL) && (section_name == NULL)) {
     n = 1;
     snprintf(buffer, sizeof(buffer), "tuple:%s@%s", an_cred_get_id(lio_gc->creds), lio_gc->section_name);
  } else if ((userid != NULL) && (section_name != NULL)) {
     n = 1;
     snprintf(buffer, sizeof(buffer), "tuple:%s@%s", userid, section_name);
  }

  if (n == 1) {
     tuple2 = _lc_object_get(buffer);
     if (tuple2 != NULL) { //** Already exists!
       tuple = *tuple2;
       tuple.path = fname;
       goto finished;
     }
  }

  //** Get the LC
  n = 0;
  if (section_name == NULL) {
     section_name = lio_gc->section_name;
     tuple.lc = lio_gc;
     snprintf(buffer, sizeof(buffer), "lc%s", section_name);
     _lc_object_get(buffer);
  } else {
     snprintf(buffer, sizeof(buffer), "lc:%s", section_name);
     tuple.lc = _lc_object_get(buffer);
     if (tuple.lc == NULL) { //** Doesn't exist so need to load it
        tuple.lc = lio_create_nl(lio_gc->cfg_name, section_name, userid);  //** USe the non-locking routine
        if (tuple.lc == NULL) {
           memset(&tuple, 0, sizeof(tuple));
           if (fname != NULL) free(fname);
           goto finished;
        }
        tuple.lc->anonymous_creation = 1;
        _lc_object_put(buffer, tuple.lc);
        n = 1; //** Flag as anon for cred check
     }
  }

  //** Now determine the user
  if (userid == NULL) {
     userid = an_cred_get_id(tuple.lc->creds);  //** IF not specified default to the one in the LC
  }

  snprintf(buffer, sizeof(buffer), "tuple:%s@%s", userid, tuple.lc->section_name);
  tuple2 = _lc_object_get(buffer);
  if (tuple2 == NULL) { //** Doesn't exist so insert the tuple
     cred_args[0] = tuple.lc->cfg_name;
     cred_args[1] = userid;
     tuple.creds = os_cred_init(tuple.lc->os, OS_CREDS_INI_TYPE, (void **)cred_args);
     type_malloc_clear(tuple2, lio_path_tuple_t, 1);
     tuple2->creds = tuple.creds;
     tuple2->lc = tuple.lc;
     tuple2->path = "ANONYMOUS";
     log_printf(15, "adding anon creds tuple=%s\n", buffer);
     _lc_object_put(buffer, tuple2);  //** Add it to the table
  } else if (n==1) {//** This is default user tuple just created so mark it as anon as well
     log_printf(15, "marking anon creds tuple=%s\n", buffer);
     tuple2->path = "ANONYMOUS-DEFAULT";
  }

  tuple.creds = tuple2->creds;
  tuple.path = fname;

finished:
  apr_thread_mutex_unlock(_lc_lock);

  if (section_name != NULL) free(section_name);
  if (userid != NULL) free(userid);

  tuple.is_lio = is_lio;
  return(tuple);
}

//***************************************************************
// lc_object_remove_unused  - Removes unused LC's from the global
//     table.  The default, remove_all_unsed=0, is to only
//     remove anonymously created LC's.
//***************************************************************

void lc_object_remove_unused(int remove_all_unused)
{
  list_t *user_lc;
  list_iter_t it;
  lc_object_container_t *lcc, *lcc2;
  lio_path_tuple_t *tuple;
  lio_config_t *lc;
  char *key;
  Stack_t *stack;

  stack = new_stack();

  apr_thread_mutex_lock(_lc_lock);

  //** Make a list of all the different LC's in use from the tuples
  //** Keep track of the anon creds for deletion
  user_lc = list_create(0, &list_string_compare, NULL, list_no_key_free, list_no_data_free);
  it = list_iter_search(_lc_object_list, "tuple:", 0);
  while ((list_next(&it, (list_key_t **)&key, (list_data_t **)&lcc)) == 0) {
     if (strncmp(lcc->key, "tuple:", 6) != 0) break;  //** No more tuples
     tuple = lcc->object;
     if (tuple->path == NULL) {
        list_insert(user_lc, tuple->lc->section_name, tuple->lc);
        log_printf(15, "user_lc adding key=%s lc=%s\n", lcc->key, tuple->lc->section_name);
     } else {
        log_printf(15, "user_lc marking creds key=%s for removal\n", lcc->key);
        if (strcmp(tuple->path, "ANONYMOUS") == 0) push(stack, lcc);
     }
  }

  //** Go ahead and delete the anonymously created creds (as long as they aren't the LC default
  while ((lcc = pop(stack)) != NULL) {
     tuple = lcc->object;
     _lc_object_destroy(lcc->key);
//     if (strcmp(tuple->path, "ANONYMOUS") == 0) os_cred_destroy(tuple->lc->os, tuple->creds);
     os_cred_destroy(tuple->lc->os, tuple->creds);
  }

  //** Now iterate through all the LC's
  it = list_iter_search(_lc_object_list, "lc:", 0);
  while ((list_next(&it, (list_key_t **)&key, (list_data_t **)&lcc)) == 0) {
     if (strncmp(lcc->key, "lc:", 3) != 0) break;  //** No more LCs
     lc = lcc->object;
     log_printf(15, "checking key=%s lc=%s anon=%d count=%d\n", lcc->key, lc->section_name, lc->anonymous_creation, lcc->count);
     lcc2 = list_search(user_lc, lc->section_name);
     if (lcc2 == NULL) {  //** No user@lc reference so safe to delete from that standpoint
        log_printf(15, "not in user_lc key=%s lc=%s anon=%d count=%d\n", lcc->key, lc->section_name, lc->anonymous_creation, lcc->count);
        if (((lc->anonymous_creation == 1) && (lcc->count <= 1)) ||
            ((remove_all_unused == 1) && (lcc->count <= 0))) {
            push(stack, lcc);
        }
     }
  }

  while ((lcc = pop(stack)) != NULL) {
     lc = lcc->object;
     _lc_object_destroy(lcc->key);
     lio_destroy_nl(lc);
  }

  apr_thread_mutex_unlock(_lc_lock);

  free_stack(stack, 0);
  list_destroy(user_lc);

  return;
}


//***************************************************************
// lio_print_options - Prints the standard supported lio options
//   Use "LIO_COMMON_OPTIONS" in the arg list
//***************************************************************

void lio_print_options(FILE *fd)
{
 fprintf(fd, "    LIO_COMMON_OPTIONS\n");
 fprintf(fd, "       -d level        - Set the debug level (0-20).  Defaults to 0\n");
 fprintf(fd, "       -c config       - Configuration file\n");
 fprintf(fd, "       -lc user@config - Use the user and config section specified for making the default LIO\n");
 fprintf(fd, "       -np N           - Number of simultaneous operations. Default is %d.\n", lio_parallel_task_count);
 fprintf(fd, "       -i N            - Print information messages of level N or greater. No header is printed\n");
 fprintf(fd, "       -it N           - Print information messages of level N or greater. Thread ID header is used\n");
 fprintf(fd, "       -if N           - Print information messages of level N or greater. Full header is used\n");
}

//***************************************************************
//  lio_destroy_nl - Destroys a LIO config object - NO locking
//***************************************************************

void lio_destroy_nl(lio_config_t *lio)
{
  lc_object_container_t *lcc;
  lio_path_tuple_t *tuple;
  char buffer[128];


  snprintf(buffer, sizeof(buffer), "lc:%s", lio->section_name);
  if (_lc_object_destroy(buffer) > 0) {  //** Still in use so return
     apr_thread_mutex_unlock(_lc_lock);
     return;
  }

  log_printf(15, "removing lio=%s\n", lio->section_name);


  if (_lc_object_destroy(lio->rs_section) <= 0) {
     rs_destroy_service(lio->rs);
  }
  free(lio->rs_section);

  ds_attr_destroy(lio->ds, lio->da);
  if (_lc_object_destroy(lio->ds_section) <= 0) {
//log_printf(15, "FLAG removing ds_section=%s\n", lio->ds_section);
     ds_destroy_service(lio->ds);
  }
  free(lio->ds_section);

  if (_lc_object_destroy(lio->tpc_unlimited_section) <= 0) {
     thread_pool_destroy_context(lio->tpc_unlimited);
  }
  free(lio->tpc_unlimited_section);

  if (_lc_object_destroy(lio->tpc_cpu_section) <= 0) {
     thread_pool_destroy_context(lio->tpc_cpu);
  }
  free(lio->tpc_cpu_section);

  //** The creds is a little tricky cause we need to get the tuple first
  lcc = list_search(_lc_object_list, lio->creds_name);
  tuple = (lcc != NULL) ? lcc->object : NULL;
  if (_lc_object_destroy(lio->creds_name) <= 0) {
     os_cred_destroy(lio->os, lio->creds);
     free(tuple);
  }
  free(lio->creds_name);

  if (_lc_object_destroy(lio->os_section) <= 0) {
     os_destroy_service(lio->os);
  }
  free(lio->os_section);

  lio_core_destroy(lio);

  if (lio->cfg_name != NULL) free(lio->cfg_name);
  if (lio->section_name != NULL) free(lio->section_name);

  exnode_service_set_destroy(lio->ess);

  lio_core_destroy(lio);

  inip_destroy(lio->ifd);

  free(lio);

  return;
}

//***************************************************************
//  lio_destroy - Destroys a LIO config object
//***************************************************************

void lio_destroy(lio_config_t *lio)
{
  apr_thread_mutex_lock(_lc_lock);
  lio_destroy_nl(lio);
  apr_thread_mutex_unlock(_lc_lock);
}

//***************************************************************
// lio_create_nl - Creates a lio configuration according to the config file
//   NOTE:  No locking is used
//***************************************************************

lio_config_t *lio_create_nl(char *fname, char *section, char *user)
{
  lio_config_t *lio;
  int sockets, cores, vcores;
  int err;
  char buffer[1024];
  char *cred_args[2];
  char *ctype, *stype;
  ds_create_t *ds_create;
  rs_create_t *rs_create;
  os_create_t *os_create;
  lio_path_tuple_t *tuple;

  //** Add the LC first cause it may already exist
  snprintf(buffer, sizeof(buffer), "lc:%s", section);
  lio = _lc_object_get(buffer);
  if (lio != NULL) {  //** Already loaded so can skip this part
     return(lio);
  }

  type_malloc_clear(lio, lio_config_t, 1);
  lio->ess = exnode_service_set_create();

  //** Add it to the table for ref counting
  snprintf(buffer, sizeof(buffer), "lc:%s", section);
  _lc_object_put(buffer, lio);


  lio->lio = lio_core_create();

  lio->cfg_name = strdup(fname);
  lio->section_name = strdup(section);

  lio->ifd = inip_read(lio->cfg_name);
  lio->timeout = inip_get_integer(lio->ifd, section, "timeout", 120);
  lio->max_attr = inip_get_integer(lio->ifd, section, "max_attr_size", 10*1024*1024);

  proc_info(&sockets, &cores, &vcores);
  cores = inip_get_integer(lio->ifd, section, "tpc_cpu", cores);
  sprintf(buffer, "tpc:%d", cores);
  stype = buffer;
  lio->tpc_cpu_section = strdup(stype);
  lio->tpc_cpu = _lc_object_get(stype);
  if (lio->tpc_cpu == NULL) {  //** Need to load it
     lio->tpc_cpu = thread_pool_create_context("CPU", 1, cores);
     if (lio->tpc_cpu == NULL) {
        err = 5;
        log_printf(0, "Error loading tpc_cpu threadpool!  n=%d\n", cores);
     }

     _lc_object_put(stype, lio->tpc_cpu);  //** Add it to the table
  }
  lio->ess->tpc_cpu = lio->tpc_cpu;

  cores = inip_get_integer(lio->ifd, section, "tpc_unlimited", 10000);
  sprintf(buffer, "tpc:%d", cores);
  stype = buffer;
  lio->tpc_unlimited_section = strdup(stype);
  lio->tpc_unlimited = _lc_object_get(stype);
  if (lio->tpc_unlimited == NULL) {  //** Need to load it
     lio->tpc_unlimited = thread_pool_create_context("UNLIMITED", 1, cores);
     if (lio->tpc_unlimited == NULL) {
        err = 6;
        log_printf(0, "Error loading tpc_unlimited threadpool!  n=%d\n", cores);
     }

     _lc_object_put(stype, lio->tpc_unlimited);  //** Add it to the table
  }
  lio->ess->tpc_unlimited = lio->tpc_unlimited;

  stype = inip_get_string(lio->ifd, section, "ds", DS_TYPE_IBP);
  lio->ds_section = stype;
  lio->ds = _lc_object_get(stype);
  if (lio->ds == NULL) {  //** Need to load it
     ctype = inip_get_string(lio->ifd, stype, "type", DS_TYPE_IBP);
     ds_create = lookup_service(lio->ess->dsm, DS_SM_AVAILABLE, ctype);
     lio->ds = (*ds_create)(lio->ess, lio->cfg_name, stype);
     if (lio->ds == NULL) {
        err = 2;
        log_printf(1, "Error loading data service!  type=%s\n", ctype);
     }
     free(ctype);

     _lc_object_put(stype, lio->ds);  //** Add it to the table
  }
  lio->da = ds_attr_create(lio->ds);

  lio->ess->ds = lio->ds;  //** This is needed by the RS service

  stype = inip_get_string(lio->ifd, section, "rs", RS_TYPE_SIMPLE);
  lio->rs_section = stype;
  lio->rs = _lc_object_get(stype);
  if (lio->rs == NULL) {  //** Need to load it
     ctype = inip_get_string(lio->ifd, stype, "type", RS_TYPE_SIMPLE);
     rs_create = lookup_service(lio->ess->rsm, RS_SM_AVAILABLE, ctype);
     lio->rs = (*rs_create)(lio->ess, lio->cfg_name, stype);
     if (lio->rs == NULL) {
        err = 3;
        log_printf(1, "Error loading resource service!  type=%s section=%s\n", ctype, stype);
     }
     free(ctype);

     _lc_object_put(stype, lio->rs);  //** Add it to the table
  }

  stype = inip_get_string(lio->ifd, section, "os", "osfile");
  lio->os_section = stype;
  lio->os = _lc_object_get(stype);
  if (lio->os == NULL) {  //** Need to load it
     ctype = inip_get_string(lio->ifd, stype, "type", OS_TYPE_FILE);
     os_create = lookup_service(lio->ess->osm, 0, ctype);
     lio->os = (*os_create)(lio->ess->authn_sm, lio->ess->osaz_sm, lio->tpc_cpu, lio->tpc_unlimited, lio->cfg_name, stype);
     if (lio->os == NULL) {
        err = 4;
        log_printf(1, "Error loading object service!  type=%s section=%s\n", ctype, stype);
     }
     free(ctype);

     _lc_object_put(stype, lio->os);  //** Add it to the table
  }

  cred_args[0] = lio->cfg_name;
  cred_args[1] = (user == NULL) ? inip_get_string(lio->ifd, section, "user", "guest") : strdup(user);
  snprintf(buffer, sizeof(buffer), "tuple:%s@%s", cred_args[1], lio->section_name);
  stype = buffer;
  lio->creds_name = strdup(buffer);
  tuple = _lc_object_get(stype);
  if (tuple == NULL) {  //** Need to load it
     lio->creds = os_cred_init(lio->os, OS_CREDS_INI_TYPE, (void **)cred_args);
     type_malloc_clear(tuple, lio_path_tuple_t, 1);
     tuple->creds = lio->creds;
     tuple->lc = lio;
     _lc_object_put(stype, tuple);  //** Add it to the table
  } else {
     lio->creds = tuple->creds;
  }
  if (cred_args[1] != NULL) free(cred_args[1]);

  //** Update the lc count for the creds
  snprintf(buffer, sizeof(buffer), "lc:%s", lio->section_name);
  _lc_object_get(buffer);

  if (_lio_cache == NULL) {
     stype = inip_get_string(lio->ifd, section, "cache", CACHE_TYPE_AMP);
     ctype = inip_get_string(lio->ifd, section, stype, CACHE_TYPE_AMP);
     _lio_cache = load_cache(ctype, lio->da, lio->timeout, lio->cfg_name, stype);
     if (_lio_cache == NULL) {
        err = 4;
        log_printf(0, "Error loading cache service!  type=%s\n", ctype);
     }
     free(stype); free(ctype);
  }
  lio->cache = _lio_cache;

  exnode_system_config(lio->ess, lio->ds, lio->rs, lio->os, lio->tpc_unlimited, lio->tpc_cpu, lio->cache);

  return(lio);
}


//***************************************************************
// lio_create - Creates a lio configuration according to the config file
//***************************************************************

lio_config_t *lio_create(char *fname, char *section, char *user)
{
  lio_config_t *lc;

  apr_thread_mutex_unlock(_lc_lock);
  lc = lio_create_nl(fname, section, user);
  apr_thread_mutex_unlock(_lc_lock);

  return(lc);
}

//***************************************************************
// lio_print_path_options - Prints the path options to the device
//***************************************************************

void lio_print_path_options(FILE *fd)
{
 fprintf(fd, "    LIO_PATH_OPTIONS: -rp regex_path | -gp glob_path   -ro regex_objext | -go glob_object\n");
 fprintf(fd, "       -rp regex_path  - Regex of path to scan\n");
 fprintf(fd, "       -gp glob_path   - Glob of path to scan\n");
 fprintf(fd, "       -ro regex_obj   - Regex for final object selection.\n");
 fprintf(fd, "       -go glob_obj    - Glob for final object selection.\n");
}

//***************************************************************
// lio_parse_path_options - Parses the path options
//***************************************************************

int lio_parse_path_options(int *argc, char **argv, lio_path_tuple_t *tuple, os_regex_table_t **rp, os_regex_table_t **ro)
{
  int nargs, i;
//  char *myargv[*argc];

  *rp = NULL; *ro = NULL;

  nargs = 1;  //** argv[0] is preserved as the calling name
//  myargv[0] = argv[0];

  i=1;
  do {
//log_printf(0, "argv[%d]=%s\n", i, argv[i]);
     if (strcmp(argv[i], "-rp") == 0) { //** Regex for path
        i++;
        *tuple = lio_path_resolve(argv[i]);  //** Pick off the user/host
        *rp = os_regex2table(tuple->path); i++;
     } else if (strcmp(argv[i], "-ro") == 0) {  //** Regex for object
        i++;
        *ro = os_regex2table(argv[i]); i++;
     } else if (strcmp(argv[i], "-gp") == 0) {  //** Glob for path
        i++;
        *tuple = lio_path_resolve(argv[i]);  //** Pick off the user/host
        *rp = os_path_glob2regex(tuple->path); i++;
     } else if (strcmp(argv[i], "-go") == 0) {  //** Glob for object
        i++;
        *ro = os_path_glob2regex(argv[i]); i++;
     } else {
//       myargv[nargs] = argv[i];
       if (i!=nargs)argv[nargs] = argv[i];
       nargs++;
       i++;
     }
  } while (i<*argc);

  if (*argc == nargs) return(0);  //** Nothing was processed

  //** Adjust argv to reflect the parsed arguments
//  memcpy(argv, myargv, sizeof(char *)*nargs);
  *argc = nargs;

//for (i=0; i<nargs; i++) log_printf(0, "myargv[%d]=%s\n", i, argv[i]);
  return(1);
}

//***************************************************************
// env2args - Converts an env string to argc/argv
//***************************************************************

int env2args(char *env, int *argc, char ***eargv)
{
  int i, n, fin;
  char *bstate, **argv;

  n = 100;
  type_malloc_clear(argv, char *, n);

  i = 0;
  argv[i] = string_token(env, " ", &bstate, &fin);
  while (fin == 0) {
    i++;
    if (i==n) { n += 10; type_realloc(argv, char *, n); }
    argv[i] = string_token(NULL, " ", &bstate, &fin);
  }

  *argc = i;
  if (i == 0) {
     *argv = NULL;
  } else {
    type_realloc(argv, char *, i);
  }

  *eargv = argv;
  return(0);
}

//***************************************************************
// lio_init - Initializes LIO for use.  argc and argv are
//    modified by removing LIO common options.
//***************************************************************

//char **t2 = NULL;
int lio_init(int *argc, char ***argvp)
{
  int i, ll, neargs, nargs, ptype;
  char *env;
  char **eargv;
  char **myargv;
  char **argv;
  char *dummy;
  char *cfg_name = NULL;
  char *section_name = "lio";
  char  *userid = NULL;

  argv = *argvp;

  apr_wrapper_start();

  exnode_system_init();

  //** Create the lio object container
  apr_pool_create(&_lc_mpool, NULL);
  apr_thread_mutex_create(&_lc_lock, APR_THREAD_MUTEX_DEFAULT, _lc_mpool);
  _lc_object_list = list_create(0, &list_string_compare, NULL, list_no_key_free, list_no_data_free);

//argv = *argvp;
//printf("start argc=%d\n", *argc);
//for (i=0; i<*argc; i++) {
//  printf("start argv[%d]=%s\n", i, argv[i]);
//}

  //** Get default args from the environment
  env = getenv("LIO_OPTIONS");
  if (env != NULL) {  //** Got args so prepend them to the front of the list
     env = strdup(env);  //** Don't want to mess up the actual env variable
     eargv = NULL;
     env2args(env, &neargs, &eargv);

     if (neargs > 0) {
        ll = *argc + neargs;
        type_malloc_clear(myargv, char *, ll);
        myargv[0] = argv[0];
        memcpy(&(myargv[1]), eargv, sizeof(char *)*neargs);
        if (*argc > 1) memcpy(&(myargv[neargs+1]), &(argv[1]), sizeof(char *)*(*argc - 1));
        argv = myargv;
        *argvp = myargv;
        *argc = ll;
        free(eargv);
//printf("after merge argc=%d\n", *argc);
//for (i=0; i<*argc; i++) {
//  printf("merge argv[%d]=%s\n", i, argv[i]);
//}
     }
  }

  type_malloc_clear(myargv, char *, *argc);


  //** Parse any arguments
  nargs = 1;  //** argv[0] is preserved as the calling name
  i=1;
  ll = -1;
  do {
//printf("argv[%d]=%s\n", i, argv[i]);
     if (strcmp(argv[i], "-d") == 0) { //** Enable debugging
        i++;
        ll = atoi(argv[i]); i++;
     } else if (strcmp(argv[i], "-np") == 0) { //** Parallel task count
        i++;
        lio_parallel_task_count = atoi(argv[i]); i++;
     } else if (strcmp(argv[i], "-i") == 0) { //** Info level w/o header
        i++;
        lio_ifd = info_create(stdout, INFO_HEADER_NONE, atoi(argv[i])); i++;
     } else if (strcmp(argv[i], "-it") == 0) { //** Info level w thread header
        i++;
        lio_ifd = info_create(stdout, INFO_HEADER_THREAD, atoi(argv[i])); i++;
     } else if (strcmp(argv[i], "-if") == 0) { //** Info level w full header
        i++;
        lio_ifd = info_create(stdout, INFO_HEADER_FULL, atoi(argv[i])); i++;
     } else if (strcmp(argv[i], "-c") == 0) { //** Load a config file
        i++;
        cfg_name = argv[i]; i++;
     } else if (strcmp(argv[i], "-lc") == 0) { //** Default LIO config section
        i++;
        ptype = lio_parse_path(argv[i], &userid, &section_name, &dummy);
        //printf("parse: arg=%s user=%s section=%s path=%s ptype=%d\n", argv[i], userid, section_name, dummy, ptype);
        if (section_name == NULL) section_name = "lio";
        if (dummy != NULL) free(dummy);
        i++;
     } else {
       myargv[nargs] = argv[i];
//printf("myargv[%d]=%s\n", nargs, argv[i]);
       nargs++;
       i++;
     }
  } while (i<*argc);

  //** If not specified create a default
  if (lio_ifd == NULL) lio_ifd = info_create(stdout, INFO_HEADER_NONE, 0);


  //** Adjust argv to reflect the parsed arguments
//  memcpy(argv, myargv, sizeof(char *)*nargs);
  *argvp = myargv;
  *argc = nargs;

//  free(myargv);

//for (i=0; i<nargs; i++) log_printf(0, "myargv[%d]=%s\n", i, argv[i]);

//for (i=0; i<*argc; i++) {
//  log_printf(0, "argv[%d]=%s\n", i, argv[i]);
//}
//flush_log();
//exit(1);

  //** TRy to see if we can find a default config somewhere
  if (cfg_name == NULL) {
    if (os_local_filetype("lio.cfg") != 0) {
       cfg_name = "lio.cfg";
    } else if (os_local_filetype("~/lio.cfg") != 0) {
       cfg_name = "~/lio.cfg";
    } else if (os_local_filetype("/etc/lio.cfg") != 0) {
       cfg_name = "/etc/lio.cfg";
    }
  }


  if (cfg_name != NULL) {
     mlog_load(cfg_name);

     if (ll > -1) set_log_level(ll);

     lio_gc = lio_create(cfg_name, section_name, userid);
  } else {
     log_printf(0, "Error missing config file!\n");
  }

  if (userid != NULL) free(userid);

//argv = *argvp;
//printf("end argc=%d\n", *argc);
//for (i=0; i<*argc; i++) {
//  printf("end argv[%d]=%s\n", i, argv[i]);
//}

  return(0);
}

//***************************************************************
//  lio_shutdown - Shuts down the LIO system
//***************************************************************

int lio_shutdown()
{
  cache_destroy(_lio_cache);
  cache_system_destroy();

  exnode_system_destroy();

  lio_destroy(lio_gc);

  lc_object_remove_unused(0);

  apr_thread_mutex_destroy(_lc_lock);
  apr_pool_destroy(_lc_mpool);
//  list_destroy(_lc_object_list);

  apr_wrapper_stop();

  return(0);
}
