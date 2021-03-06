/*   
 *   File: skiplist-optik.c
 *   Author: Vasileios Trigonakis <vasileios.trigonakis@epfl.ch>
 *  	     Vincent Gramoli <vincent.gramoli@sydney.edu.au>, 
 *   Description:  Fine-grained locking skip list.
 *   Optimized with OPTIK.
 *   C implementation of the Herlihy et al. algorithm originally 
 *   designed for managed programming language.
 *   "A Simple Optimistic Skiplist Algorithm" 
 *   M. Herlihy, Y. Lev, V. Luchangco, N. Shavit 
 *   p.124-138, SIROCCO 2007
 *   skiplist-optik.c is part of ASCYLIB
 *
 * Copyright (c) 2014 Vasileios Trigonakis <vasileios.trigonakis@epfl.ch>,
 * 	     	      Tudor David <tudor.david@epfl.ch>
 *	      	      Distributed Programming Lab (LPD), EPFL
 *
 * ASCYLIB is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, version 2
 * of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "skiplist-optik.h"
#include "utils.h"

RETRY_STATS_VARS;

#include "latency.h"
#if LATENCY_PARSING == 1
__thread size_t lat_parsing_get = 0;
__thread size_t lat_parsing_put = 0;
__thread size_t lat_parsing_rem = 0;
#endif	/* LATENCY_PARSING == 1 */

extern ALIGNED(CACHE_LINE_SIZE) unsigned int levelmax;

#define MAX_BACKOFF 131071
#define HERLIHY_MAX_MAX_LEVEL 64 /* covers up to 2^64 elements */

inline int 
ok_to_delete(sl_node_t *node, int found)
{
  return (node->fullylinked && ((node->toplevel-1) == found) && !node->marked);
}

inline int
sl_optik_search(sl_intset_t *set, skey_t key, sl_node_t **preds, sl_node_t **succs, optik_t* predsv)
{
 restart:
  PARSE_TRY();
  int found, i;
  sl_node_t *pred, *curr;
	
  found = -1;
  pred = set->head;
  optik_t predv = set->head->lock;
	
  for (i = (pred->toplevel - 1); i >= 0; i--)
    {
      curr = pred->next[i];
      optik_t currv = curr->lock; 

      while (key > curr->key)
	{
	  predv = currv;
	  pred = curr;

	  curr = pred->next[i];
	  currv = curr->lock;  
	}

      if (unlikely(pred->marked))
	{
	  goto restart;
	}
      preds[i] = pred;
      predsv[i] = predv;
      succs[i] = curr;
      if (found == -1 && key == curr->key)
	{
	  found = i;
	}
    }
  return found;
}

inline sl_node_t*
sl_optik_left_search(sl_intset_t *set, skey_t key)
{
  PARSE_TRY();
  int i;
  sl_node_t *pred, *curr, *nd = NULL;
	
  pred = set->head;
	
  for (i = (pred->toplevel - 1); i >= 0; i--)
    {
      curr = pred->next[i];
      while (key > curr->key)
	{
	  pred = curr;
	  curr = pred->next[i];
	}

      if (key == curr->key)
	{
	  nd = curr;
	  break;
	}
    }

  return nd;
}

sval_t
sl_optik_find(sl_intset_t *set, skey_t key)
{ 
  sval_t result = 0;

  PARSE_START_TS(0);
  sl_node_t* nd = sl_optik_left_search(set, key);
  PARSE_END_TS(0, lat_parsing_get++);

  if (nd != NULL && !nd->marked && nd->fullylinked)
    {
      result = nd->val;
    }
  return result;
}

inline void
unlock_levels(sl_intset_t* set, sl_node_t **nodes, int highestlevel)
{

  int i;
  sl_node_t *old = NULL;
  for (i = 0; i <= highestlevel; i++)
    {
      if (old != nodes[i])
	{
	  optik_unlock(&nodes[i]->lock);
	}
      old = nodes[i];
    }
}

int
sl_optik_insert(sl_intset_t *set, skey_t key, sval_t val)
{
  sl_node_t *succs[HERLIHY_MAX_MAX_LEVEL], *preds[HERLIHY_MAX_MAX_LEVEL];
  optik_t predsv[HERLIHY_MAX_MAX_LEVEL];
  sl_node_t  *node_found, *prev_pred, *new_node;
  sl_node_t *pred, *succ;
  int toplevel, highest_locked, i, valid, found;
  unsigned int backoff;

  toplevel = get_rand_level();
  backoff = 1;
	
  PARSE_START_TS(1);
  while (1) 
    {
      UPDATE_TRY();
      found = sl_optik_search(set, key, preds, succs, predsv);
      PARSE_END_TS(1, lat_parsing_put);

      if (found != -1)
	{
	  node_found = succs[found];
	  if (!node_found->marked)
	    {
	      while (!node_found->fullylinked)
		{
		  PAUSE;
		}
	      PARSE_END_INC(lat_parsing_put);
	      return 0;
	    }
	  continue;
	}

      highest_locked = -1;
      prev_pred = NULL;
      valid = 1;
      for (i = 0; valid && (i < toplevel); i++)
	{
	  pred = preds[i];
	  succ = succs[i];
	  if (pred != prev_pred)
	    {
	      //	      optik_lock(&pred->lock);
	      if (!optik_lock_version(&pred->lock, predsv[i]))
		{
		  valid = !pred->marked;
		}
	      highest_locked = i;
	      prev_pred = pred;
	    }	

	  valid = valid && !succ->marked;
	  valid = valid && (pred->next[i] == succ);
	}
	
      if (!valid) 
	{			 /* Unlock the predecessors before leaving */ 
	  unlock_levels(set, preds, highest_locked); /* unlocks the global-lock in the GL case */
	  if (backoff > 5000) 
	    {
	      nop_rep(backoff & MAX_BACKOFF);
	    }
	  backoff <<= 1;
	  continue;
	}
		
      new_node = sl_new_simple_node(key, val, toplevel, 0);

      for (i = 0; i < toplevel; i++)
	{
	  new_node->next[i] = succs[i];
	}

#if defined(__tile__)
      MEM_BARRIER;
#endif

      for (i = 0; i < toplevel; i++)
	{
	  preds[i]->next[i] = new_node;
	}
		
      new_node->fullylinked = 1;

      unlock_levels(set, preds, highest_locked);
      PARSE_END_INC(lat_parsing_put);
      return 1;
    }
}

sval_t
sl_optik_delete(sl_intset_t *set, skey_t key)
{
  sl_node_t *succs[HERLIHY_MAX_MAX_LEVEL], *preds[HERLIHY_MAX_MAX_LEVEL];
  optik_t predsv[HERLIHY_MAX_MAX_LEVEL];
  sl_node_t *node_todel, *prev_pred; 
  sl_node_t *pred, *succ;
  int is_marked, toplevel, highest_locked, i, valid, found;	
  unsigned int backoff;

  node_todel = NULL;
  is_marked = 0;
  toplevel = -1;
  backoff = 1;
	
  PARSE_START_TS(2);
  while(1)
    {
      UPDATE_TRY();
      found = sl_optik_search(set, key, preds, succs, predsv);
      PARSE_END_TS(2, lat_parsing_rem);

      /* If not marked and ok to delete, then mark it */
      if (is_marked || (found != -1 && ok_to_delete(succs[found], found)))
	{	
	  if (!is_marked)
	    {
	      node_todel = succs[found];

	      optik_lock(&node_todel->lock);
	      toplevel = node_todel->toplevel;
	      /* Unless it has been marked meanwhile */

	      if (node_todel->marked)
		{
		  optik_unlock(&node_todel->lock);
		  PARSE_END_INC(lat_parsing_rem);
		  return 0;
		}

	      node_todel->marked = 1;
	      is_marked = 1;
	    }

	  /* Physical deletion */
	  highest_locked = -1;
	  prev_pred = NULL;
	  valid = 1;
	  for (i = 0; valid && (i < toplevel); i++)
	    {
	      pred = preds[i];
	      succ = succs[i];
	      if (pred != prev_pred)
		{
		  if (!optik_lock_version(&pred->lock, predsv[i]))
		    {
		      valid = !pred->marked;
		      valid = valid && (pred->next[i] == succ);
		    }
		  highest_locked = i;
		  prev_pred = pred;
		}

	    }

	  if (!valid)
	    {	
	      unlock_levels(set, preds, highest_locked);
	      if (backoff > 5000) 
		{
		  nop_rep(backoff & MAX_BACKOFF);
		}
	      backoff <<= 1;
	      continue;
	    }
			
	  for (i = (toplevel-1); i >= 0; i--)
	    {
	      preds[i]->next[i] = node_todel->next[i];
	    }

	  sval_t val = node_todel->val;

	  optik_unlock(&node_todel->lock);
	  unlock_levels(set, preds, highest_locked);

#if GC == 1
	  ssmem_free(alloc, (void*) node_todel);
#endif

	  PARSE_END_INC(lat_parsing_rem);
	  return val;
	}
      else
	{
	  PARSE_END_INC(lat_parsing_rem);
	  return 0;
	}
    }
}
