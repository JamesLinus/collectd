/**
 * collectd - src/utils_threshold.c
 * Copyright (C) 2007  Florian octo Forster
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Author:
 *   Florian octo Forster <octo at verplant.org>
 **/

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "utils_avltree.h"
#include "utils_cache.h"

#include <assert.h>
#include <pthread.h>

/*
 * Private data structures
 */
typedef struct threshold_s
{
  char host[DATA_MAX_NAME_LEN];
  char plugin[DATA_MAX_NAME_LEN];
  char plugin_instance[DATA_MAX_NAME_LEN];
  char type[DATA_MAX_NAME_LEN];
  char type_instance[DATA_MAX_NAME_LEN];
  gauge_t min;
  gauge_t max;
  int invert;
} threshold_t;

/*
 * Private (static) variables
 */
static avl_tree_t     *threshold_tree = NULL;
static pthread_mutex_t threshold_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Threshold management
 * ====================
 * The following functions add, delete, search, etc. configured thresholds to
 * the underlying AVL trees.
 */
static int ut_threshold_add (const threshold_t *th)
{
  char name[6 * DATA_MAX_NAME_LEN];
  char *name_copy;
  threshold_t *th_copy;
  int status = 0;

  if (format_name (name, sizeof (name), th->host,
	th->plugin, th->plugin_instance,
	th->type, th->type_instance) != 0)
  {
    ERROR ("ut_threshold_add: format_name failed.");
    return (-1);
  }

  name_copy = strdup (name);
  if (name_copy == NULL)
  {
    ERROR ("ut_threshold_add: strdup failed.");
    return (-1);
  }

  th_copy = (threshold_t *) malloc (sizeof (threshold_t));
  if (th_copy == NULL)
  {
    sfree (name_copy);
    ERROR ("ut_threshold_add: malloc failed.");
    return (-1);
  }
  memcpy (th_copy, th, sizeof (threshold_t));

  DEBUG ("ut_threshold_add: Adding entry `%s'", name);

  pthread_mutex_lock (&threshold_lock);
  status = avl_insert (threshold_tree, name_copy, th_copy);
  pthread_mutex_unlock (&threshold_lock);

  if (status != 0)
  {
    ERROR ("ut_threshold_add: avl_insert (%s) failed.", name);
    sfree (name_copy);
    sfree (th_copy);
  }

  return (status);
} /* int ut_threshold_add */
/*
 * End of the threshold management functions
 */

/*
 * Configuration
 * =============
 * The following approximately two hundred functions are used to convert the
 * threshold values..
 */
static int ut_config_type_instance (threshold_t *th, oconfig_item_t *ci)
{
  if ((ci->values_num != 1)
      || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("threshold values: The `Instance' option needs exactly one "
	"string argument.");
    return (-1);
  }

  strncpy (th->type_instance, ci->values[0].value.string,
      sizeof (th->type_instance));
  th->type_instance[sizeof (th->type_instance) - 1] = '\0';

  return (0);
} /* int ut_config_type_instance */

static int ut_config_type_max (threshold_t *th, oconfig_item_t *ci)
{
  if ((ci->values_num != 1)
      || (ci->values[0].type != OCONFIG_TYPE_NUMBER))
  {
    WARNING ("threshold values: The `Max' option needs exactly one "
	"number argument.");
    return (-1);
  }

  th->max = ci->values[0].value.number;

  return (0);
} /* int ut_config_type_max */

static int ut_config_type_min (threshold_t *th, oconfig_item_t *ci)
{
  if ((ci->values_num != 1)
      || (ci->values[0].type != OCONFIG_TYPE_NUMBER))
  {
    WARNING ("threshold values: The `Min' option needs exactly one "
	"number argument.");
    return (-1);
  }

  th->min = ci->values[0].value.number;

  return (0);
} /* int ut_config_type_min */

static int ut_config_type_invert (threshold_t *th, oconfig_item_t *ci)
{
  if ((ci->values_num != 1)
      || (ci->values[0].type != OCONFIG_TYPE_BOOLEAN))
  {
    WARNING ("threshold values: The `Invert' option needs exactly one "
	"boolean argument.");
    return (-1);
  }

  th->invert = (ci->values[0].value.boolean) ? 1 : 0;

  return (0);
} /* int ut_config_type_invert */

static int ut_config_type (const threshold_t *th_orig, oconfig_item_t *ci)
{
  int i;
  threshold_t th;
  int status = 0;

  if ((ci->values_num != 1)
      || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("threshold values: The `Type' block needs exactly one string "
	"argument.");
    return (-1);
  }

  if (ci->children_num < 1)
  {
    WARNING ("threshold values: The `Type' block needs at least one option.");
    return (-1);
  }

  memcpy (&th, th_orig, sizeof (th));
  strncpy (th.type, ci->values[0].value.string, sizeof (th.type));
  th.type[sizeof (th.type) - 1] = '\0';

  for (i = 0; i < ci->children_num; i++)
  {
    oconfig_item_t *option = ci->children + i;
    status = 0;

    if (strcasecmp ("Instance", option->key) == 0)
      status = ut_config_type_instance (&th, option);
    else if (strcasecmp ("Max", option->key) == 0)
      status = ut_config_type_max (&th, option);
    else if (strcasecmp ("Min", option->key) == 0)
      status = ut_config_type_min (&th, option);
    else if (strcasecmp ("Invert", option->key) == 0)
      status = ut_config_type_invert (&th, option);
    else
    {
      WARNING ("threshold values: Option `%s' not allowed inside a `Type' "
	  "block.", option->key);
      status = -1;
    }

    if (status != 0)
      break;
  }

  if (status == 0)
  {
    status = ut_threshold_add (&th);
  }

  return (status);
} /* int ut_config_type */

static int ut_config_plugin_instance (threshold_t *th, oconfig_item_t *ci)
{
  if ((ci->values_num != 1)
      || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("threshold values: The `Instance' option needs exactly one "
	"string argument.");
    return (-1);
  }

  strncpy (th->plugin_instance, ci->values[0].value.string,
      sizeof (th->plugin_instance));
  th->plugin_instance[sizeof (th->plugin_instance) - 1] = '\0';

  return (0);
} /* int ut_config_plugin_instance */

static int ut_config_plugin (const threshold_t *th_orig, oconfig_item_t *ci)
{
  int i;
  threshold_t th;
  int status = 0;

  if ((ci->values_num != 1)
      || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("threshold values: The `Plugin' block needs exactly one string "
	"argument.");
    return (-1);
  }

  if (ci->children_num < 1)
  {
    WARNING ("threshold values: The `Plugin' block needs at least one nested "
	"block.");
    return (-1);
  }

  memcpy (&th, th_orig, sizeof (th));
  strncpy (th.plugin, ci->values[0].value.string, sizeof (th.plugin));
  th.plugin[sizeof (th.plugin) - 1] = '\0';

  for (i = 0; i < ci->children_num; i++)
  {
    oconfig_item_t *option = ci->children + i;
    status = 0;

    if (strcasecmp ("Type", option->key) == 0)
      status = ut_config_type (&th, option);
    else if (strcasecmp ("Instance", option->key) == 0)
      status = ut_config_plugin_instance (&th, option);
    else
    {
      WARNING ("threshold values: Option `%s' not allowed inside a `Plugin' "
	  "block.", option->key);
      status = -1;
    }

    if (status != 0)
      break;
  }

  return (status);
} /* int ut_config_plugin */

static int ut_config_host (const threshold_t *th_orig, oconfig_item_t *ci)
{
  int i;
  threshold_t th;
  int status = 0;

  if ((ci->values_num != 1)
      || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("threshold values: The `Host' block needs exactly one string "
	"argument.");
    return (-1);
  }

  if (ci->children_num < 1)
  {
    WARNING ("threshold values: The `Host' block needs at least one nested "
	"block.");
    return (-1);
  }

  memcpy (&th, th_orig, sizeof (th));
  strncpy (th.host, ci->values[0].value.string, sizeof (th.host));
  th.host[sizeof (th.host) - 1] = '\0';

  for (i = 0; i < ci->children_num; i++)
  {
    oconfig_item_t *option = ci->children + i;
    status = 0;

    if (strcasecmp ("Type", option->key) == 0)
      status = ut_config_type (&th, option);
    else if (strcasecmp ("Plugin", option->key) == 0)
      status = ut_config_plugin (&th, option);
    else
    {
      WARNING ("threshold values: Option `%s' not allowed inside a `Host' "
	  "block.", option->key);
      status = -1;
    }

    if (status != 0)
      break;
  }

  return (status);
} /* int ut_config_host */

int ut_config (const oconfig_item_t *ci)
{
  int i;
  int status = 0;

  threshold_t th;

  memset (&th, '\0', sizeof (th));
  th.min = NAN;
  th.max = NAN;
    
  for (i = 0; i < ci->children_num; i++)
  {
    oconfig_item_t *option = ci->children + i;
    status = 0;

    if (strcasecmp ("Type", option->key) == 0)
      status = ut_config_type (&th, option);
    else if (strcasecmp ("Plugin", option->key) == 0)
      status = ut_config_plugin (&th, option);
    else if (strcasecmp ("Host", option->key) == 0)
      status = ut_config_host (&th, option);
    else
    {
      WARNING ("threshold values: Option `%s' not allowed here.", option->key);
      status = -1;
    }

    if (status != 0)
      break;
  }

  return (status);
} /* int um_config */
/*
 * End of the functions used to configure threshold values.
 */

/* vim: set sw=2 ts=8 sts=2 tw=78 : */
