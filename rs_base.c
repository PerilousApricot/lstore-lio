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

//***********************************************************************
// Routines for managing the layout loading framework
//***********************************************************************

#define _log_module_index 157

#include "ex3_abstract.h"
#include "list.h"
#include "random.h"
#include "type_malloc.h"
#include "log.h"

typedef struct {
  resource_service_fn_t *driver;
} rs_driver_t;

typedef struct {
  list_t *table;
} rs_table_t;

rs_table_t *rs_driver_table = NULL;

//***********************************************************************
// install_layout- Installs a resource_service driver into the table
//***********************************************************************

int install_resource_service(char *type, resource_service_fn_t *driver)
{
  rs_driver_t *d;

  //** 1st time so create the struct
  if (rs_driver_table == NULL) {
     type_malloc_clear(rs_driver_table, rs_table_t, 1);
     rs_driver_table->table = list_create(0, &list_string_compare, list_string_dup, list_simple_free, list_no_data_free);
  }

  d = list_search(rs_driver_table->table, type);
  if (d != NULL) {
    log_printf(0, "install_layout: Matching driver for type=%s already exists!\n", type);
    return(1);
  }
  
  type_malloc_clear(d, rs_driver_t, 1);
  d->driver = driver;
  list_insert(rs_driver_table->table, type, (void *)d);

  return(0);
}

//***********************************************************************
// lookup_resource_service - Looks up the resource service driver
//***********************************************************************

resource_service_fn_t *lookup_resource_service(char *type)
{
  rs_driver_t *d;

  d = (rs_driver_t *)list_search(rs_driver_table->table, type);
  if (d == NULL) {
    log_printf(0, "lookup_resrouce_Service:  No matching driver for type=%s\n", type);
    return(NULL);
  }

  return(d->driver);
}
