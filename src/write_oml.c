#include "string.h"
/*
 * Copyright 2012 National ICT Australia (NICTA), Australia
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */
#include <time.h>
#include <pthread.h>

#include <oml2/omlc.h>

#include "collectd.h"
#include "plugin.h"
#include "common.h"
#include "utils_cache.h"
#include "utils_parse_option.h"

static const char *config_keys[] =
{
   "ServerURL",
   "ContextName",
   "NodeName",
   "StartupDelay"
};
static int config_keys_num = STATIC_ARRAY_SIZE(config_keys);


typedef struct _mpoint {
  char   name[DATA_MAX_NAME_LEN];

  // From value_list_t (plugin.h)
  char     plugin[DATA_MAX_NAME_LEN];
  char     plugin_instance[DATA_MAX_NAME_LEN];
  char     type[DATA_MAX_NAME_LEN];
  char     type_instance[DATA_MAX_NAME_LEN];

  OmlMP*     oml_mp;
  OmlMPDef*  mp_defs;
  struct _mpoint* next;
} MPoint;

typedef struct {
  char* server_url;
  char* context_name;
  char* node_id;
  int   startup_delay;

  MPoint* mpoint;  // linked list of mpoint definitions
  int     oml_intialized;
  pthread_mutex_t init_lock;

} Session;

static Session session;
static time_t start_time;

static MPoint*
find_mpoint_struct(
    const char* name
) {
  MPoint* first_mp = session.mpoint;
  MPoint* mp = first_mp->next;

  while (mp != first_mp) {
    if (mp == NULL) return NULL;
    if (strncmp(mp->name, name, DATA_MAX_NAME_LEN) == 0) {
      return mp;
    }
    mp = mp->next;
  } 
  return NULL;
}

static void
configure_mpoint(
    MPoint* mp,
    const data_set_t *ds,
    const value_list_t *vl
) {

//  strncpy(mp->plugin, vl->plugin, sizeof(mp->plugin));
//  strncpy(mp->plugin_instance, vl->plugin_instance, sizeof(mp->plugin_instance));
//  strncpy(mp->type, vl->type, sizeof (mp->type));
//  strncpy(mp->type_instance, vl->type_instance, sizeof (mp->type_instance));

  mp->mp_defs = (OmlMPDef*)malloc((ds->ds_num + 6 + 1) * sizeof(OmlMPDef));

  int i = 0;
  mp->mp_defs[i].name = "time"; mp->mp_defs[i].param_types = OML_INT64_VALUE;
  mp->mp_defs[++i].name = "host"; mp->mp_defs[i].param_types = OML_STRING_VALUE;
  mp->mp_defs[++i].name = "plugin"; mp->mp_defs[i].param_types = OML_STRING_VALUE;
  mp->mp_defs[++i].name = "plugin_instance"; mp->mp_defs[i].param_types = OML_STRING_VALUE;
  mp->mp_defs[++i].name = "type"; mp->mp_defs[i].param_types = OML_STRING_VALUE;
  mp->mp_defs[++i].name = "type_instance"; mp->mp_defs[i].param_types = OML_STRING_VALUE;

  int offset = ++i;
  for (i = 0; i < ds->ds_num; i++) {
    data_source_t* d = &ds->ds[i];
    OmlMPDef* md = &mp->mp_defs[i + offset];
    char* s = (char*)malloc(sizeof(d->name) + 1);
    strncpy(s, d->name, sizeof(d->name));
    md->name = s;
    assert(d->type <= 3);
    switch(d->type) {
    //  see plugin.h
    //    typedef unsigned long long counter_t;
    //    typedef double gauge_t;
    //    typedef int64_t derive_t;
    //    typedef uint64_t absolute_t;
    case 0: md->param_types = OML_INT64_VALUE; break;
    case 1: md->param_types = OML_DOUBLE_VALUE; break;
    case 2: md->param_types = OML_INT64_VALUE; break;
    case 3: md->param_types = OML_INT64_VALUE; break;  // TODO: Should really be u64, but sqlite doesn't handle well
    }
  }
  // NULL out last one
  OmlMPDef* md = &mp->mp_defs[ds->ds_num + offset];
  md->name = 0; md->param_types = (OmlValueT)0;
  mp->oml_mp = omlc_add_mp(mp->name, mp->mp_defs);
}

static MPoint*
create_mpoint(
    const char* name,
    const data_set_t *ds,
    const value_list_t *vl
) {
  MPoint* mp;
  // Create MPoint and insert it into session's existing MP chain.
  mp = (MPoint*)malloc(sizeof(MPoint));
  strncpy(mp->name, name, DATA_MAX_NAME_LEN);
  MPoint* pmp = session.mpoint;
  if (pmp == NULL) {
    // first one created
    mp->next = mp;
  } else {
    mp->next = pmp;
    pmp = mp;
  }
  session.mpoint = mp;
  configure_mpoint(mp, ds, vl);
  return mp;
}

static MPoint*
find_mpoint(
    const char* name,
    const data_set_t *ds,
    const value_list_t *vl
) {
  MPoint* mp = find_mpoint_struct(name);

  if (mp == NULL) {
    if (session.oml_intialized) {
      ERROR("oml_writer plugin: We assumed that all collectors already checked in, but now we found '%s'", name);
    }
    mp = create_mpoint(name, ds, vl);
  }   
  if (! session.oml_intialized) {
      // We are waiting for some time before we commit to a set of
      // reportable measurements.
      //
      time_t now;
      time(&now);
      if ((now - start_time) < session.startup_delay) {
        return NULL;  // let's wait a bit longer
      }
      // OK it's time to commit
      pthread_mutex_lock(&session.init_lock);
      if (! session.oml_intialized) { // just make sure nothing has changed since we aquired the lock
        DEBUG("oml_writer plugin: Starting OML");
        omlc_start();
        session.oml_intialized = 1;
      }
      pthread_mutex_unlock(&session.init_lock);

  }
  return mp;
}


static int
oml_write (
    const data_set_t *ds,
    const value_list_t *vl,
    user_data_t __attribute__((unused)) *user_data
) {
  MPoint* mp = find_mpoint(ds->type, ds, vl);
  if (mp == NULL || !session.oml_intialized) return(0);

  OmlValueU v[64];
  int header = 6;
  if (vl->values_len >= 64 - header) {
    ERROR("oml_writer plugin: Can't handle more than 64 values per measurement");
    return(-1);
  }

  omlc_set_int64(v[0], vl->time);
  omlc_set_string(v[1], vl->host != NULL ? (char*)vl->host : "");
  omlc_set_string(v[2], vl->plugin != NULL ? (char*)vl->plugin : "");
  omlc_set_string(v[3], vl->plugin_instance != NULL ? (char*)vl->plugin_instance : "");
  omlc_set_string(v[4], vl->type != NULL ? (char*)vl->type : "");
  omlc_set_string(v[5], vl->type_instance != NULL ? (char*)vl->type_instance : "");

  int i = 0;
  for (; i < ds->ds_num; i++) {
    data_source_t* d = &ds->ds[i];
    value_t* vi = &vl->values[i];

    assert(d->type <= 3);
    switch(d->type) {
    //  see configure_mpoint()
    case 0: omlc_set_int64(v[i + header], vi->counter); break;
    case 1: omlc_set_double(v[i + header], vi->gauge); break;
    case 2: omlc_set_int64(v[i + header], vi->derive); break;
    case 3: omlc_set_int64(v[i + header], vi->absolute); break;
    }
  }
  omlc_inject(mp->oml_mp, v);
  return(0);
}

static int
oml_config(
    const char *key,
    const char *value
) {
  if (strcasecmp ("ServerURL", key) == 0) {
    session.server_url = (char*)malloc(strlen(value) + 1);
    strncpy(session.server_url, value, strlen(value));
  } else if (strcasecmp ("ContextName", key) == 0) {
    session.context_name = (char*)malloc(strlen(value) + 1);
    strncpy(session.context_name, value, strlen(value));
  } else if (strcasecmp ("NodeName", key) == 0) {
    session.node_id = (char*)malloc(strlen(value) + 1);
    strncpy(session.node_id, value, strlen(value));
  } else if (strcasecmp ("StartupDelay", key) == 0) {
    session.startup_delay = atoi(value);
  } else {
    return(-1);
  }
  return(0);
}

static int
oml_init(
    void
) {
  MPoint* mp;
  mp = (MPoint*)malloc(sizeof(MPoint));
  mp->next = 0;
  session.mpoint = mp;
  
  const char* app_name = "collectd";
  const char* argv[] = {"--oml-server", "file:-", "--oml-id", hostname_g, "--oml-exp-id", "collectd"};
  int argc = 6;

  if (session.server_url != NULL) argv[1] = session.server_url;
  if (session.node_id != NULL) argv[3] = session.node_id;
  if (session.context_name != NULL) argv[5] = session.context_name;
  omlc_init(app_name, &argc, argv, NULL);

  return 0;
}


void
module_register(void)

{
  memset(&session, 0, sizeof(Session));
  time(&start_time);
  session.startup_delay = 10;
  pthread_mutex_init(&session.init_lock, /* attr = */NULL);

  plugin_register_config("write_oml", oml_config, config_keys, config_keys_num);
  plugin_register_init("write_oml", oml_init);
  plugin_register_write("write_oml", oml_write, /* user_data = */ NULL);
} /* void module_register */