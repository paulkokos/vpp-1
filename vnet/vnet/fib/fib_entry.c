/*
 * Copyright (c) 2016 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vlib/vlib.h>
#include <vnet/ip/format.h>
#include <vnet/ip/lookup.h>
#include <vnet/adj/adj.h>
#include <vnet/dpo/load_balance.h>
#include <vnet/dpo/drop_dpo.h>

#include <vnet/fib/fib_entry.h>
#include <vnet/fib/fib_walk.h>
#include <vnet/fib/fib_entry_src.h>
#include <vnet/fib/fib_entry_cover.h>
#include <vnet/fib/fib_table.h>
#include <vnet/fib/fib_internal.h>
#include <vnet/fib/fib_attached_export.h>
#include <vnet/fib/fib_path_ext.h>

/*
 * Array of strings/names for the FIB sources
 */
static const char *fib_source_names[] = FIB_SOURCES;
static const char *fib_attribute_names[] = FIB_ENTRY_ATTRIBUTES;

/*
 * Pool for all fib_entries
 */
static fib_entry_t *fib_entry_pool;

fib_entry_t *
fib_entry_get (fib_node_index_t index)
{
    return (pool_elt_at_index(fib_entry_pool, index));
}

static fib_node_t *
fib_entry_get_node (fib_node_index_t index)
{
    return ((fib_node_t*)fib_entry_get(index));
}

fib_node_index_t
fib_entry_get_index (const fib_entry_t * fib_entry)
{
    return (fib_entry - fib_entry_pool);
}

static fib_protocol_t
fib_entry_get_proto (const fib_entry_t * fib_entry)
{
    return (fib_entry->fe_prefix.fp_proto);
}

/**
 * @brief Turn the chain type requested by the client into the one they
 * really wanted
 */
static fib_forward_chain_type_t
fib_entry_chain_type_fixup (const fib_entry_t *entry,
			    fib_forward_chain_type_t fct)
{
    if (FIB_FORW_CHAIN_TYPE_MPLS_EOS == fct)
    {
	/*
	 * The EOS chain is a tricky since one cannot know the adjacency
	 * to link to without knowing what the packets payload protocol
	 * will be once the label is popped.
	 */
	fib_forward_chain_type_t dfct;

	dfct = fib_entry_get_default_chain_type(entry);

	if (FIB_FORW_CHAIN_TYPE_MPLS_EOS == dfct)
	{
	    /*
	     * If the entry being asked is a eos-MPLS label entry,
	     * then use the payload-protocol field, that we stashed there
	     * for just this purpose
	     */
	    return (fib_forw_chain_type_from_dpo_proto(
			entry->fe_prefix.fp_payload_proto));
	}
	/*
	 * else give them what this entry would be by default. i.e. if it's a v6
	 * entry, then the label its local labelled should be carrying v6 traffic.
	 * If it's a non-EOS label entry, then there are more labels and we want
	 * a non-eos chain.
	 */
	return (dfct);
    }

    return (fct);
}

fib_forward_chain_type_t
fib_entry_get_default_chain_type (const fib_entry_t *fib_entry)
{
    switch (fib_entry->fe_prefix.fp_proto)
    {
    case FIB_PROTOCOL_IP4:
	return (FIB_FORW_CHAIN_TYPE_UNICAST_IP4);
    case FIB_PROTOCOL_IP6:
	return (FIB_FORW_CHAIN_TYPE_UNICAST_IP6);
    case FIB_PROTOCOL_MPLS:
	if (MPLS_EOS == fib_entry->fe_prefix.fp_eos)
	    /*
	     * If the entry being asked is a eos-MPLS label entry,
	     * then use the payload-protocol field, that we stashed there
	     * for just this purpose
	     */
	    return (fib_forw_chain_type_from_dpo_proto(
			fib_entry->fe_prefix.fp_payload_proto));
	else
	    return (FIB_FORW_CHAIN_TYPE_MPLS_NON_EOS);
    }

    return (FIB_FORW_CHAIN_TYPE_UNICAST_IP4);
}

u8 *
format_fib_entry (u8 * s, va_list * args)
{
    fib_forward_chain_type_t fct;
    fib_entry_attribute_t attr;
    fib_path_ext_t *path_ext;
    fib_entry_t *fib_entry;
    fib_entry_src_t *src;
    fib_node_index_t fei;
    fib_source_t source;
    u32 n_covered;
    int level;

    fei = va_arg (*args, fib_node_index_t);
    level = va_arg (*args, int);
    fib_entry = fib_entry_get(fei);

    s = format (s, "%U", format_fib_prefix, &fib_entry->fe_prefix);

    if (level >= FIB_ENTRY_FORMAT_DETAIL)
    {
	s = format (s, " fib:%d", fib_entry->fe_fib_index);
	s = format (s, " index:%d", fib_entry_get_index(fib_entry));
	s = format (s, " locks:%d", fib_entry->fe_node.fn_locks);

	FOR_EACH_SRC_ADDED(fib_entry, src, source,
        ({
	    s = format (s, "\n  src:%s ",
			fib_source_names[source]);
	    s = fib_entry_src_format(fib_entry, source, s);
	    s = format (s, " refs:%d ", src->fes_ref_count);
	    if (FIB_ENTRY_FLAG_NONE != src->fes_entry_flags) {
		s = format(s, "flags:");
		FOR_EACH_FIB_ATTRIBUTE(attr) {
		    if ((1<<attr) & src->fes_entry_flags) {
			s = format (s, "%s,", fib_attribute_names[attr]);
		    }
		}
	    }
	    s = format (s, "\n");
	    if (FIB_NODE_INDEX_INVALID != src->fes_pl)
	    {
		s = fib_path_list_format(src->fes_pl, s);
	    }
	    if (NULL != src->fes_path_exts)
	    {
		s = format(s, "    Extensions:");
		vec_foreach(path_ext, src->fes_path_exts)
		{
		    s = format(s, "\n     %U", format_fib_path_ext, path_ext);
		}
	    }
	}));
    
	n_covered = fib_entry_cover_get_size(fib_entry);
	if (n_covered > 0) {
	    s = format(s, "\n tracking %d covered: ", n_covered);
	    s = fib_entry_cover_list_format(fib_entry, s);
	}
	s = fib_ae_import_format(fib_entry->fe_import, s);
	s = fib_ae_export_format(fib_entry->fe_export, s);

	s = format (s, "\n forwarding: ");
    }
    else
    {
	s = format (s, "\n");
    }

    fct = fib_entry_get_default_chain_type(fib_entry);

    if (!dpo_id_is_valid(&fib_entry->fe_lb[fct]))
    {
	s = format (s, "  UNRESOLVED\n");
	return (s);
    }
    else
    {
        if (level >= FIB_ENTRY_FORMAT_DETAIL2)
        {

            FOR_EACH_FIB_FORW_MPLS_CHAIN(fct)
            {
                s = format(s, "  %U-chain\n  %U",
                           format_fib_forw_chain_type, fct,
                           format_dpo_id,
                           &fib_entry->fe_lb[fct],
                           2);
                s = format(s, "\n");
            }
        }
        else
        {
	    s = format(s, "  %U-chain\n  %U",
		       format_fib_forw_chain_type, fct,
                       format_dpo_id,
                       &fib_entry->fe_lb[fct],
                       2);
            s = format(s, "\n");
        }
    }

    if (level >= FIB_ENTRY_FORMAT_DETAIL2)
    {
        s = format(s, "\nchildren:");
        s = fib_node_children_format(fib_entry->fe_node.fn_children, s);
    }

    /* adj = adj_get(fib_entry->fe_prefix.fp_proto, fib_entry->fe_adj_index); */

    /* ip_multipath_next_hop_t * nhs, tmp_nhs[1]; */
    /* u32 i, j, n_left, n_nhs; */
    /* vlib_counter_t c, sum; */
    /* ip_lookup_main_t *lm = fib_get_lookup_main(fib_entry->fe_prefix.fp_proto); */

    /* if (adj->n_adj == 1) */
    /* { */
    /* 	nhs = &tmp_nhs[0]; */
    /* 	nhs[0].next_hop_adj_index = ~0; /\* not used *\/ */
    /* 	nhs[0].weight = 1; */
    /* 	n_nhs = 1; */
    /* } */
    /* else */
    /* { */
    /* 	ip_multipath_adjacency_t * madj; */
    /* 	madj = vec_elt_at_index (lm->multipath_adjacencies, adj->heap_handle); */
    /* 	nhs = heap_elt_at_index (lm->next_hop_heap, madj->normalized_next_hops.heap_offset); */
    /* 	n_nhs = madj->normalized_next_hops.count; */
    /* } */

    /* n_left = nhs[0].weight; */
    /* vlib_counter_zero (&sum); */
    /* for (i = j = 0; i < adj->n_adj; i++) */
    /* { */
    /* 	n_left -= 1; */
    /* 	vlib_get_combined_counter(&lm->adjacency_counters,  */
    /* 				  fib_entry->fe_adj_index + i, */
    /* 				  &c); */
    /* 	/\* if (clear) *\/ */
    /* 	/\*     vlib_zero_combined_counter (&lm->adjacency_counters,  *\/ */
    /* 	/\* 				fib_entry->fe_adj_index + i); *\/ */

    /* 	vlib_counter_add (&sum, &c); */
    /* 	if (n_left == 0) */
    /* 	{ */
    /* 	    s = format (s, "%16Ld%16Ld ", sum.packets, sum.bytes); */
    /* 	    s = format (s, "weight %d, index %d", */
    /* 			  nhs[j].weight, fib_entry->fe_adj_index + i); */

    /* 	    if (adj->n_adj > 1) */
    /* 		s = format (s, ", multipath"); */

    /* 	    s = format (s, "\n%U", */
    /* 			format_ip_adjacency, */
    /* 			vnet_get_main(), lm, fib_entry->fe_adj_index + i); */

    /* 	    //   vlib_cli_output (vm, "%v", msg); */
    /* 	    //vec_free (msg); */
    /* 	} */
    /* 	else */
    /* 	{ */
    /* 	    j++; */
    /* 	    if (j < n_nhs) */
    /* 	    { */
    /* 		n_left = nhs[j].weight; */
    /* 		vlib_counter_zero (&sum); */
    /* 	    } */
    /* 	} */
    /* } */

    return (s);
}

static fib_entry_t*
fib_entry_from_fib_node (fib_node_t *node)
{
#if CLIB_DEBUG > 0
    ASSERT(FIB_NODE_TYPE_ENTRY == node->fn_type);
#endif
    return ((fib_entry_t*)node);
}

static void
fib_entry_last_lock_gone (fib_node_t *node)
{
    fib_forward_chain_type_t fct;
    fib_entry_t *fib_entry;

    fib_entry = fib_entry_from_fib_node(node);

    FOR_EACH_FIB_FORW_MPLS_CHAIN(fct)
    {
	dpo_reset(&fib_entry->fe_lb[fct]);
    }

    FIB_ENTRY_DBG(fib_entry, "last-lock");

    fib_node_deinit(&fib_entry->fe_node);
    // FIXME -RR Backwalk
    pool_put(fib_entry_pool, fib_entry);
}

static fib_entry_src_t*
fib_entry_get_best_src_i (const fib_entry_t *fib_entry)
{
    fib_entry_src_t *bsrc;

    /*
     * the enum of sources is deliberately arranged in priority order
     */
    if (0 == vec_len(fib_entry->fe_srcs))
    {
	bsrc = NULL;
    }
    else
    {
	bsrc = vec_elt_at_index(fib_entry->fe_srcs, 0);
    }

    return (bsrc);
}

static fib_source_t
fib_entry_src_get_source (const fib_entry_src_t *esrc)
{
    if (NULL != esrc)
    {
	return (esrc->fes_src);
    }
    return (FIB_SOURCE_MAX);
}

static fib_entry_flag_t
fib_entry_src_get_flags (const fib_entry_src_t *esrc)
{
    if (NULL != esrc)
    {
	return (esrc->fes_entry_flags);
    }
    return (FIB_ENTRY_FLAG_NONE);
}

fib_entry_flag_t
fib_entry_get_flags (fib_node_index_t fib_entry_index)
{
    return (fib_entry_get_flags_i(fib_entry_get(fib_entry_index)));
}

/*
 * fib_entry_back_walk_notify
 *
 * A back walk has reach this entry.
 */
static fib_node_back_walk_rc_t
fib_entry_back_walk_notify (fib_node_t *node,
			    fib_node_back_walk_ctx_t *ctx)
{
    fib_entry_t *fib_entry;

    fib_entry = fib_entry_from_fib_node(node);

    if (FIB_NODE_BW_REASON_FLAG_EVALUATE & ctx->fnbw_reason        ||
        FIB_NODE_BW_REASON_FLAG_ADJ_UPDATE & ctx->fnbw_reason      ||
        FIB_NODE_BW_REASON_FLAG_ADJ_DOWN & ctx->fnbw_reason        ||
	FIB_NODE_BW_REASON_FLAG_INTERFACE_UP & ctx->fnbw_reason    ||
	FIB_NODE_BW_REASON_FLAG_INTERFACE_DOWN & ctx->fnbw_reason  ||
	FIB_NODE_BW_REASON_FLAG_INTERFACE_DELETE & ctx->fnbw_reason)
    {
	fib_entry_src_action_reactivate(fib_entry,
                                        fib_entry_get_best_source(
                                            fib_entry_get_index(fib_entry)));
    }

    /*
     * all other walk types can be reclassifed to a re-evaluate to
     * all recursive dependents.
     * By reclassifying we ensure that should any of these walk types meet
     * they can be merged.
     */
    ctx->fnbw_reason = FIB_NODE_BW_REASON_FLAG_EVALUATE;

    /*
     * ... and nothing is forced sync from now on.
     */
    ctx->fnbw_flags &= ~FIB_NODE_BW_FLAG_FORCE_SYNC;

    /*
     * propagate the backwalk further if we haven't already reached the
     * maximum depth.
     */
    fib_walk_sync(FIB_NODE_TYPE_ENTRY,
		  fib_entry_get_index(fib_entry),
		  ctx);

    return (FIB_NODE_BACK_WALK_CONTINUE);
}

static void
fib_entry_show_memory (void)
{
    u32 n_srcs = 0, n_exts = 0;
    fib_entry_src_t *esrc;
    fib_entry_t *entry;

    fib_show_memory_usage("Entry",
			  pool_elts(fib_entry_pool),
			  pool_len(fib_entry_pool),
			  sizeof(fib_entry_t));

    pool_foreach(entry, fib_entry_pool,
    ({
	n_srcs += vec_len(entry->fe_srcs);
	vec_foreach(esrc, entry->fe_srcs)
	{
	    n_exts += vec_len(esrc->fes_path_exts);
	}
    }));

    fib_show_memory_usage("Entry Source",
			  n_srcs, n_srcs, sizeof(fib_entry_src_t));
    fib_show_memory_usage("Entry Path-Extensions",
			  n_exts, n_exts,
			  sizeof(fib_path_ext_t));
}

/*
 * The FIB path-list's graph node virtual function table
 */
static const fib_node_vft_t fib_entry_vft = {
    .fnv_get = fib_entry_get_node,
    .fnv_last_lock = fib_entry_last_lock_gone,
    .fnv_back_walk = fib_entry_back_walk_notify,
    .fnv_mem_show = fib_entry_show_memory,
};

/**
 * @brief Contribute the set of Adjacencies that this entry forwards with
 * to build the uRPF list of its children
 */
void
fib_entry_contribute_urpf (fib_node_index_t entry_index,
			   index_t urpf)
{
    fib_entry_t *fib_entry;

    fib_entry = fib_entry_get(entry_index);

    return (fib_path_list_contribute_urpf(fib_entry->fe_parent, urpf));
}

/*
 * fib_entry_contribute_forwarding
 *
 * Get an lock the forwarding information (DPO) contributed by the FIB entry.
 */
void
fib_entry_contribute_forwarding (fib_node_index_t fib_entry_index,
				 fib_forward_chain_type_t type,
				 dpo_id_t *dpo)
{
    fib_entry_t *fib_entry;

    fib_entry = fib_entry_get(fib_entry_index);

    /*
     * these are not the droids you are looking for...
     */
    type = fib_entry_chain_type_fixup(fib_entry, type);

    if (!dpo_id_is_valid(&fib_entry->fe_lb[type]))
    {
	/*
	 * on-demand create eos/non-eos.
	 * There is no on-demand delete because:
	 *   - memory versus complexity & reliability:
	 *      leaving unrequired [n]eos LB arounds wastes memory, cleaning
	 *      then up on the right trigger is more code. i favour the latter.
	 */
	fib_entry_src_mk_lb(fib_entry,
			    fib_entry_get_best_src_i(fib_entry),
			    type,
			    &fib_entry->fe_lb[type]);
    }

    dpo_copy(dpo, &fib_entry->fe_lb[type]);
}

const dpo_id_t *
fib_entry_contribute_ip_forwarding (fib_node_index_t fib_entry_index)
{
    fib_entry_t *fib_entry;

    fib_entry = fib_entry_get(fib_entry_index);

    return (&fib_entry->fe_lb[fib_entry_get_default_chain_type(fib_entry)]);
}

adj_index_t
fib_entry_get_adj (fib_node_index_t fib_entry_index)
{
    const dpo_id_t *dpo;

    dpo = fib_entry_contribute_ip_forwarding(fib_entry_index);
    dpo = load_balance_get_bucket(dpo->dpoi_index, 0);

    if (dpo_is_adj(dpo))
    {
	return (dpo->dpoi_index);
    }
    return (ADJ_INDEX_INVALID);
}

fib_node_index_t
fib_entry_get_path_list (fib_node_index_t fib_entry_index)
{
    fib_entry_t *fib_entry;

    fib_entry = fib_entry_get(fib_entry_index);

    return (fib_entry->fe_parent);
}

u32
fib_entry_child_add (fib_node_index_t fib_entry_index,
		     fib_node_type_t child_type,
		     fib_node_index_t child_index)
{
    return (fib_node_child_add(FIB_NODE_TYPE_ENTRY,
                               fib_entry_index,
                               child_type,
                               child_index));
};

void
fib_entry_child_remove (fib_node_index_t fib_entry_index,
			u32 sibling_index)
{
    fib_node_child_remove(FIB_NODE_TYPE_ENTRY,
                          fib_entry_index,
                          sibling_index);
}

static fib_entry_t *
fib_entry_alloc (u32 fib_index,
		 const fib_prefix_t *prefix,
		 fib_node_index_t *fib_entry_index)
{
    fib_forward_chain_type_t fct;
    fib_entry_t *fib_entry;

    pool_get(fib_entry_pool, fib_entry);
    memset(fib_entry, 0, sizeof(*fib_entry));

    fib_node_init(&fib_entry->fe_node,
		  FIB_NODE_TYPE_ENTRY);

    fib_entry->fe_fib_index = fib_index;
    fib_entry->fe_prefix = *prefix;
    if (FIB_PROTOCOL_MPLS == fib_entry->fe_prefix.fp_proto)
    {
	fib_entry->fe_prefix.fp_len = 21;
    	ASSERT(DPO_PROTO_NONE != fib_entry->fe_prefix.fp_payload_proto);
    }

    fib_entry->fe_export = FIB_NODE_INDEX_INVALID;
    fib_entry->fe_import = FIB_NODE_INDEX_INVALID;
    fib_entry->fe_covered = FIB_NODE_INDEX_INVALID;
    FOR_EACH_FIB_FORW_MPLS_CHAIN(fct)
    {
	dpo_reset(&fib_entry->fe_lb[fct]);
    }

    *fib_entry_index = fib_entry_get_index(fib_entry);

    FIB_ENTRY_DBG(fib_entry, "alloc");

    return (fib_entry);
}

static void
fib_entry_post_flag_update_actions (fib_entry_t *fib_entry,
				    fib_source_t source,
				    fib_entry_flag_t old_flags)
{
    /*
     * handle changes to attached export for import entries
     */
    int is_import  = (FIB_ENTRY_FLAG_IMPORT & fib_entry_get_flags_i(fib_entry));
    int was_import = (FIB_ENTRY_FLAG_IMPORT & old_flags);

    if (!was_import && is_import)
    {
	/*
	 * transition from not exported to exported
	 */

	/*
	 * there is an assumption here that the entry resolves via only
	 * one interface and that it is the cross VRF interface.
	 */
	u32 sw_if_index = fib_path_list_get_resolving_interface(fib_entry->fe_parent);

	fib_attached_export_import(fib_entry,
				   fib_table_get_index_for_sw_if_index(
				       fib_entry_get_proto(fib_entry),
                                       sw_if_index));
    }
    else if (was_import && !is_import)
    {
	/*
	 * transition from exported to not exported
	 */
	fib_attached_export_purge(fib_entry);
    }
    /*
     * else
     *   no change. nothing to do.
     */

    /*
     * handle changes to attached export for export entries
     */
    int is_attached  = (FIB_ENTRY_FLAG_ATTACHED & fib_entry_get_flags_i(fib_entry));
    int was_attached = (FIB_ENTRY_FLAG_ATTACHED & old_flags);

    if (!was_attached && is_attached)
    {
	/*
	 * transition to attached. time to export
	 */
	// FIXME
    }
    // else FIXME
}

static void
fib_entry_post_install_actions (fib_entry_t *fib_entry,
				fib_source_t source,
				fib_entry_flag_t old_flags)
{
    fib_entry_post_flag_update_actions(fib_entry, source, old_flags);
    fib_entry_src_action_installed(fib_entry, source);
}

fib_node_index_t
fib_entry_create (u32 fib_index,
		  const fib_prefix_t *prefix,
		  fib_source_t source,
		  fib_entry_flag_t flags,
		  const fib_route_path_t *paths)
{
    fib_node_index_t fib_entry_index;
    fib_entry_t *fib_entry;

    ASSERT(0 < vec_len(paths));

    fib_entry = fib_entry_alloc(fib_index, prefix, &fib_entry_index);

    /*
     * since this is a new entry create, we don't need to check for winning
     * sources - there is only one.
     */
    fib_entry = fib_entry_src_action_add(fib_entry, source, flags,
                                         drop_dpo_get(
                                             fib_proto_to_dpo(
                                                 fib_entry_get_proto(fib_entry))));
    fib_entry_src_action_path_swap(fib_entry,
				   source,
				   flags,
				   paths);
    /*
     * handle possible realloc's by refetching the pointer
     */
    fib_entry = fib_entry_get(fib_entry_index);
    fib_entry_src_action_activate(fib_entry, source);

    fib_entry_post_install_actions(fib_entry, source, FIB_ENTRY_FLAG_NONE);

    return (fib_entry_index);
}

fib_node_index_t
fib_entry_create_special (u32 fib_index,
			  const fib_prefix_t *prefix,
			  fib_source_t source,
			  fib_entry_flag_t flags,
			  const dpo_id_t *dpo)
{
    fib_node_index_t fib_entry_index;
    fib_entry_t *fib_entry;

    /*
     * create and initiliase the new enty
     */
    fib_entry = fib_entry_alloc(fib_index, prefix, &fib_entry_index);

    /*
     * create the path-list
     */
    fib_entry = fib_entry_src_action_add(fib_entry, source, flags, dpo);
    fib_entry_src_action_activate(fib_entry, source);

    fib_entry_post_install_actions(fib_entry, source, FIB_ENTRY_FLAG_NONE);

    return (fib_entry_index);
}

static void
fib_entry_post_update_actions (fib_entry_t *fib_entry,
			       fib_source_t source,
			       fib_entry_flag_t old_flags)
{
    /*
     * backwalk to children to inform then of the change to forwarding.
     */
    fib_node_back_walk_ctx_t bw_ctx = {
	.fnbw_reason = FIB_NODE_BW_REASON_FLAG_EVALUATE,
    };

    fib_walk_sync(FIB_NODE_TYPE_ENTRY, fib_entry_get_index(fib_entry), &bw_ctx);

    /*
     * then inform any covered prefixes
     */
    fib_entry_cover_update_notify(fib_entry);

    fib_entry_post_install_actions(fib_entry, source, old_flags);
}

static void
fib_entry_source_change (fib_entry_t *fib_entry,
			 fib_source_t best_source,
			 fib_source_t new_source,
			 fib_entry_flag_t old_flags)
{
    /*
     * if the path list for the source passed is invalid,
     * then we need to create a new one. else we are updating
     * an existing.
     */
    if (new_source < best_source)
    {
	/*
	 * we have a new winning source.
	 */
	fib_entry_src_action_deactivate(fib_entry, best_source);
	fib_entry_src_action_activate(fib_entry, new_source);
    }
    else if (new_source > best_source)
    {
	/*
	 * the new source loses. nothing to do here.
	 * the data from the source is saved in the path-list created
	 */
	return;
    }
    else
    {
	/*
	 * the new source is one this entry already has.
	 * But the path-list was updated, which will contribute new forwarding,
	 * so install it.
	 */
	fib_entry_src_action_deactivate(fib_entry, new_source);
	fib_entry_src_action_activate(fib_entry, new_source);
    }

    fib_entry_post_update_actions(fib_entry, new_source, old_flags);
}

void
fib_entry_special_add (fib_node_index_t fib_entry_index,
		       fib_source_t source,
		       fib_entry_flag_t flags,
		       const dpo_id_t *dpo)
{
    fib_source_t best_source;
    fib_entry_flag_t bflags;
    fib_entry_t *fib_entry;
    fib_entry_src_t *bsrc;

    fib_entry = fib_entry_get(fib_entry_index);

    bsrc = fib_entry_get_best_src_i(fib_entry);
    best_source = fib_entry_src_get_source(bsrc);
    bflags = fib_entry_src_get_flags(bsrc);

    fib_entry = fib_entry_src_action_add(fib_entry, source, flags, dpo);
    fib_entry_source_change(fib_entry, best_source, source, bflags);
}

void
fib_entry_special_update (fib_node_index_t fib_entry_index,
			  fib_source_t source,
			  fib_entry_flag_t flags,
			  const dpo_id_t *dpo)
{
    fib_source_t best_source;
    fib_entry_flag_t bflags;
    fib_entry_t *fib_entry;
    fib_entry_src_t *bsrc;

    fib_entry = fib_entry_get(fib_entry_index);

    bsrc = fib_entry_get_best_src_i(fib_entry);
    best_source = fib_entry_src_get_source(bsrc);
    bflags = fib_entry_src_get_flags(bsrc);

    fib_entry = fib_entry_src_action_update(fib_entry, source, flags, dpo);
    fib_entry_source_change(fib_entry, best_source, source, bflags);
}


void
fib_entry_path_add (fib_node_index_t fib_entry_index,
		    fib_source_t source,
		    fib_entry_flag_t flags,
		    const fib_route_path_t *rpath)
{
    fib_source_t best_source;
    fib_entry_flag_t bflags;
    fib_entry_t *fib_entry;
    fib_entry_src_t *bsrc;

    ASSERT(1 == vec_len(rpath));

    fib_entry = fib_entry_get(fib_entry_index);
    ASSERT(NULL != fib_entry);

    bsrc = fib_entry_get_best_src_i(fib_entry);
    best_source = fib_entry_src_get_source(bsrc);
    bflags = fib_entry_src_get_flags(bsrc);
    
    fib_entry = fib_entry_src_action_path_add(fib_entry, source, flags, rpath);

    /*
     * if the path list for the source passed is invalid,
     * then we need to create a new one. else we are updating
     * an existing.
     */
    if (source < best_source)
    {
	/*
	 * we have a new winning source.
	 */
	fib_entry_src_action_deactivate(fib_entry, best_source);
	fib_entry_src_action_activate(fib_entry, source);
    }
    else if (source > best_source)
    {
	/*
	 * the new source loses. nothing to do here.
	 * the data from the source is saved in the path-list created
	 */
	return;
    }
    else
    {
	/*
	 * the new source is one this entry already has.
	 * But the path-list was updated, which will contribute new forwarding,
	 * so install it.
	 */
	fib_entry_src_action_deactivate(fib_entry, source);
	fib_entry_src_action_activate(fib_entry, source);
    }

    fib_entry_post_update_actions(fib_entry, source, bflags);
}

/*
 * fib_entry_path_remove
 *
 * remove a path from the entry.
 * return the fib_entry's index if it is still present, INVALID otherwise.
 */
fib_entry_src_flag_t
fib_entry_path_remove (fib_node_index_t fib_entry_index,
		       fib_source_t source,
		       const fib_route_path_t *rpath)
{
    fib_entry_src_flag_t sflag;
    fib_source_t best_source;
    fib_entry_flag_t bflags;
    fib_entry_t *fib_entry;
    fib_entry_src_t *bsrc;

    ASSERT(1 == vec_len(rpath));

    fib_entry = fib_entry_get(fib_entry_index);
    ASSERT(NULL != fib_entry);

    bsrc = fib_entry_get_best_src_i(fib_entry);
    best_source = fib_entry_src_get_source(bsrc);
    bflags = fib_entry_src_get_flags(bsrc);

    sflag = fib_entry_src_action_path_remove(fib_entry, source, rpath);

    /*
     * if the path list for the source passed is invalid,
     * then we need to create a new one. else we are updating
     * an existing.
     */
    if (source < best_source )
    {
	/*
	 * Que! removing a path from a source that is better than the
	 * one this entry is using.
	 */
	ASSERT(0);
    }
    else if (source > best_source )
    {
	/*
	 * the source is not the best. nothing to do.
	 */
	return (FIB_ENTRY_SRC_FLAG_ADDED);
    }
    else
    {
	/*
	 * removing a path from the path-list we were using.
	 */
	if (!(FIB_ENTRY_SRC_FLAG_ADDED & sflag))
	{
	    /*
	     * the last path from the source was removed.
	     * fallback to lower source
	     */
	    bsrc = fib_entry_get_best_src_i(fib_entry);
	    best_source = fib_entry_src_get_source(bsrc);

	    if (FIB_SOURCE_MAX == best_source) {
		/*
		 * no more sources left. this entry is toast.
		 */
		fib_entry_src_action_uninstall(fib_entry);
		fib_entry_post_flag_update_actions(fib_entry, source, bflags);

		return (FIB_ENTRY_SRC_FLAG_NONE);
	    }
	    else
	    {
		fib_entry_src_action_activate(fib_entry, best_source);
		source = best_source;
	    }
	}
	else
	{
	    /*
	     * re-install the new forwarding information
	     */
	    fib_entry_src_action_deactivate(fib_entry, source);
	    fib_entry_src_action_activate(fib_entry, source);
	}
    }

    fib_entry_post_update_actions(fib_entry, source, bflags);

    /*
     * still have sources
     */
    return (FIB_ENTRY_SRC_FLAG_ADDED);
}

/*
 * fib_entry_special_remove
 *
 * remove a special source from the entry.
 * return the fib_entry's index if it is still present, INVALID otherwise.
 */
fib_entry_src_flag_t
fib_entry_special_remove (fib_node_index_t fib_entry_index,
			  fib_source_t source)
{
    fib_entry_src_flag_t sflag;
    fib_source_t best_source;
    fib_entry_flag_t bflags;
    fib_entry_t *fib_entry;
    fib_entry_src_t *bsrc;

    fib_entry = fib_entry_get(fib_entry_index);
    ASSERT(NULL != fib_entry);

    bsrc = fib_entry_get_best_src_i(fib_entry);
    best_source = fib_entry_src_get_source(bsrc);
    bflags = fib_entry_src_get_flags(bsrc);

    sflag = fib_entry_src_action_remove(fib_entry, source);

    /*
     * if the path list for the source passed is invalid,
     * then we need to create a new one. else we are updating
     * an existing.
     */
    if (source < best_source )
    {
	/*
	 * Que! removing a path from a source that is better than the
	 * one this entry is using. This can only mean it is a source
         * this prefix does not have.
	 */
        return (FIB_ENTRY_SRC_FLAG_ADDED);
    }
    else if (source > best_source ) {
	/*
	 * the source is not the best. nothing to do.
	 */
	return (FIB_ENTRY_SRC_FLAG_ADDED);
    }
    else
    {
	if (!(FIB_ENTRY_SRC_FLAG_ADDED & sflag))
	{
	    /*
	     * the source was removed. use the next best.
	     */
	    bsrc = fib_entry_get_best_src_i(fib_entry);
	    best_source = fib_entry_src_get_source(bsrc);

	    if (FIB_SOURCE_MAX == best_source) {
		/*
		 * no more sources left. this entry is toast.
		 */
		fib_entry_src_action_uninstall(fib_entry);
		fib_entry_post_flag_update_actions(fib_entry, source, bflags);

		return (FIB_ENTRY_SRC_FLAG_NONE);
	    }
	    else
	    {
		fib_entry_src_action_activate(fib_entry, best_source);
		source = best_source;
	    }
	}
	else
	{
	    /*
	     * re-install the new forwarding information
	     */
	    fib_entry_src_action_reactivate(fib_entry, source);
	}
    }

    fib_entry_post_update_actions(fib_entry, source, bflags);

    /*
     * still have sources
     */
    return (FIB_ENTRY_SRC_FLAG_ADDED);
}

/**
 * fib_entry_delete
 *
 * The source is withdrawing all the paths it provided
 */
fib_entry_src_flag_t
fib_entry_delete (fib_node_index_t fib_entry_index,
		  fib_source_t source)
{
    return (fib_entry_special_remove(fib_entry_index, source));
}

/**
 * fib_entry_update
 *
 * The source has provided a new set of paths that will replace the old.
 */
void
fib_entry_update (fib_node_index_t fib_entry_index,
		  fib_source_t source,
		  fib_entry_flag_t flags,
		  const fib_route_path_t *paths)
{
    fib_source_t best_source;
    fib_entry_flag_t bflags;
    fib_entry_t *fib_entry;
    fib_entry_src_t *bsrc;

    fib_entry = fib_entry_get(fib_entry_index);
    ASSERT(NULL != fib_entry);

    bsrc = fib_entry_get_best_src_i(fib_entry);
    best_source = fib_entry_src_get_source(bsrc);
    bflags = fib_entry_src_get_flags(bsrc);

    fib_entry_src_action_path_swap(fib_entry,
				   source,
				   flags,
				   paths);
    /*
     * handle possible realloc's by refetching the pointer
     */
    fib_entry = fib_entry_get(fib_entry_index);

    /*
     * if the path list for the source passed is invalid,
     * then we need to create a new one. else we are updating
     * an existing.
     */
    if (source < best_source)
    {
	/*
	 * we have a new winning source.
	 */
	fib_entry_src_action_deactivate(fib_entry, best_source);
	fib_entry_src_action_activate(fib_entry, source);
    }
    else if (source > best_source) {
	/*
	 * the new source loses. nothing to do here.
	 * the data from the source is saved in the path-list created
	 */
	return;
    }
    else
    {
	/*
	 * the new source is one this entry already has.
	 * But the path-list was updated, which will contribute new forwarding,
	 * so install it.
	 */
	fib_entry_src_action_deactivate(fib_entry, source);
	fib_entry_src_action_activate(fib_entry, source);
    }

    fib_entry_post_update_actions(fib_entry, source, bflags);
}


/*
 * fib_entry_cover_changed
 *
 * this entry is tracking its cover and that cover has changed.
 */
void
fib_entry_cover_changed (fib_node_index_t fib_entry_index)
{
    fib_entry_src_cover_res_t res = {
	.install = !0,
	.bw_reason = FIB_NODE_BW_REASON_FLAG_NONE,
    };
    fib_source_t source, best_source;
    fib_entry_flag_t bflags;
    fib_entry_t *fib_entry;
    fib_entry_src_t *esrc;
    u32 index;

    bflags = FIB_ENTRY_FLAG_NONE;
    best_source = FIB_SOURCE_FIRST;
    fib_entry = fib_entry_get(fib_entry_index);

    fib_attached_export_cover_change(fib_entry);

    /*
     * propagate the notificuation to each of the added sources
     */
    index = 0;
    FOR_EACH_SRC_ADDED(fib_entry, esrc, source,
    ({
	if (0 == index)
	{
	    /*
	     * only the best source gets to set the back walk flags
	     */
	    res = fib_entry_src_action_cover_change(fib_entry, source);
            bflags = fib_entry_src_get_flags(esrc);
            best_source = fib_entry_src_get_source(esrc);
	}
	else
	{
	    fib_entry_src_action_cover_change(fib_entry, source);
	}
	index++;
    }));

    if (res.install)
    {
	fib_entry_src_action_reactivate(fib_entry,
					fib_entry_src_get_source(
					    fib_entry_get_best_src_i(fib_entry)));
        fib_entry_post_install_actions(fib_entry, best_source, bflags);
    }
    else
    {
	fib_entry_src_action_uninstall(fib_entry);
    }

    if (FIB_NODE_BW_REASON_FLAG_NONE != res.bw_reason)
    {
	/*
	 * time for walkies fido.
	 */
	fib_node_back_walk_ctx_t bw_ctx = {
	    .fnbw_reason = res.bw_reason,
        };

	fib_walk_sync(FIB_NODE_TYPE_ENTRY, fib_entry_index, &bw_ctx);
    }
}

/*
 * fib_entry_cover_updated
 *
 * this entry is tracking its cover and that cover has been updated
 * (i.e. its forwarding information has changed).
 */
void
fib_entry_cover_updated (fib_node_index_t fib_entry_index)
{
    fib_entry_src_cover_res_t res = {
	.install = !0,
	.bw_reason = FIB_NODE_BW_REASON_FLAG_NONE,
    };
    fib_source_t source, best_source;
    fib_entry_flag_t bflags;
    fib_entry_t *fib_entry;
    fib_entry_src_t *esrc;
    u32 index;

    bflags = FIB_ENTRY_FLAG_NONE;
    best_source = FIB_SOURCE_FIRST;
    fib_entry = fib_entry_get(fib_entry_index);

    fib_attached_export_cover_update(fib_entry);

    /*
     * propagate the notificuation to each of the added sources
     */
    index = 0;
    FOR_EACH_SRC_ADDED(fib_entry, esrc, source,
    ({
	if (0 == index)
	{
	    /*
	     * only the best source gets to set the back walk flags
	     */
	    res = fib_entry_src_action_cover_update(fib_entry, source);
            bflags = fib_entry_src_get_flags(esrc);
            best_source = fib_entry_src_get_source(esrc);
	}
	else
	{
	    fib_entry_src_action_cover_update(fib_entry, source);
	}
	index++;
    }));

    if (res.install)
    {
	fib_entry_src_action_reactivate(fib_entry,
					fib_entry_src_get_source(
					    fib_entry_get_best_src_i(fib_entry)));
        fib_entry_post_install_actions(fib_entry, best_source, bflags);
    }
    else
    {
	fib_entry_src_action_uninstall(fib_entry);
    }

    if (FIB_NODE_BW_REASON_FLAG_NONE != res.bw_reason)
    {
	/*
	 * time for walkies fido.
	 */
	fib_node_back_walk_ctx_t bw_ctx = {
	    .fnbw_reason = res.bw_reason,
        };

	fib_walk_sync(FIB_NODE_TYPE_ENTRY, fib_entry_index, &bw_ctx);
    }
}

int
fib_entry_recursive_loop_detect (fib_node_index_t entry_index,
				 fib_node_index_t **entry_indicies)
{
    fib_entry_t *fib_entry;
    int was_looped, is_looped;

    fib_entry = fib_entry_get(entry_index);

    if (FIB_NODE_INDEX_INVALID != fib_entry->fe_parent)
    {
	fib_node_index_t *entries = *entry_indicies;
	fib_forward_chain_type_t fct;

	vec_add1(entries, entry_index);
	was_looped = fib_path_list_is_looped(fib_entry->fe_parent);
	is_looped = fib_path_list_recursive_loop_detect(fib_entry->fe_parent,
							&entries);

	*entry_indicies = entries;

	if (!!was_looped != !!is_looped)
	{
	    /*
	     * re-evaluate all the entry's forwarding
	     * NOTE: this is an inplace modify
	     */
	    FOR_EACH_FIB_FORW_MPLS_CHAIN(fct)
	    {
		if (dpo_id_is_valid(&fib_entry->fe_lb[fct]))
		{
		    fib_entry_src_mk_lb(fib_entry,
					fib_entry_get_best_src_i(fib_entry),
					fct,
					&fib_entry->fe_lb[fct]);
		}
	    }
	}
    }
    else
    {
	/*
	 * the entry is currently not linked to a path-list. this happens
	 * when it is this entry that is re-linking path-lists and has thus
	 * broken the loop
	 */
	is_looped = 0;
    }

    return (is_looped);
}

u32
fib_entry_get_resolving_interface (fib_node_index_t entry_index)
{
    fib_entry_t *fib_entry;

    fib_entry = fib_entry_get(entry_index);

    return (fib_path_list_get_resolving_interface(fib_entry->fe_parent));
}

fib_source_t
fib_entry_get_best_source (fib_node_index_t entry_index)
{
    fib_entry_t *fib_entry;
    fib_entry_src_t *bsrc;

    fib_entry = fib_entry_get(entry_index);

    bsrc = fib_entry_get_best_src_i(fib_entry);
    return (fib_entry_src_get_source(bsrc));
}

static int
fib_ip4_address_compare (ip4_address_t * a1,
                         ip4_address_t * a2)
{
    /*
     * IP addresses are unsiged ints. the return value here needs to be signed
     * a simple subtraction won't cut it.
     * If the addresses are the same, the sort order is undefiend, so phoey.
     */
    return ((clib_net_to_host_u32(a1->data_u32) >
	     clib_net_to_host_u32(a2->data_u32) ) ?
	    1 : -1);
}

static int
fib_ip6_address_compare (ip6_address_t * a1,
                         ip6_address_t * a2)
{
  int i;
  for (i = 0; i < ARRAY_LEN (a1->as_u16); i++)
  {
      int cmp = (clib_net_to_host_u16 (a1->as_u16[i]) -
		 clib_net_to_host_u16 (a2->as_u16[i]));
      if (cmp != 0)
	  return cmp;
  }
  return 0;
}

static int
fib_entry_cmp (fib_node_index_t fib_entry_index1,
	       fib_node_index_t fib_entry_index2)
{
    fib_entry_t *fib_entry1, *fib_entry2;
    int cmp = 0;

    fib_entry1 = fib_entry_get(fib_entry_index1);
    fib_entry2 = fib_entry_get(fib_entry_index2);

    switch (fib_entry1->fe_prefix.fp_proto)
    {
    case FIB_PROTOCOL_IP4:
        cmp = fib_ip4_address_compare(&fib_entry1->fe_prefix.fp_addr.ip4,
                                      &fib_entry2->fe_prefix.fp_addr.ip4);
        break;
    case FIB_PROTOCOL_IP6:
        cmp = fib_ip6_address_compare(&fib_entry1->fe_prefix.fp_addr.ip6,
                                      &fib_entry2->fe_prefix.fp_addr.ip6);
        break;
    case FIB_PROTOCOL_MPLS:
	cmp = (fib_entry1->fe_prefix.fp_label - fib_entry2->fe_prefix.fp_label);

	if (0 == cmp)
	{
	    cmp = (fib_entry1->fe_prefix.fp_eos - fib_entry2->fe_prefix.fp_eos);
	}
        break;
    }

    if (0 == cmp) {
	cmp = (fib_entry1->fe_prefix.fp_len - fib_entry2->fe_prefix.fp_len);
    }
    return (cmp);   
}

int
fib_entry_cmp_for_sort (void *i1, void *i2)
{
    fib_node_index_t *fib_entry_index1 = i1, *fib_entry_index2 = i2;

    return (fib_entry_cmp(*fib_entry_index1,
			  *fib_entry_index2));
}

void
fib_entry_lock (fib_node_index_t fib_entry_index)
{
    fib_entry_t *fib_entry;

    fib_entry = fib_entry_get(fib_entry_index);

    fib_node_lock(&fib_entry->fe_node);
}

void
fib_entry_unlock (fib_node_index_t fib_entry_index)
{
    fib_entry_t *fib_entry;

    fib_entry = fib_entry_get(fib_entry_index);

    fib_node_unlock(&fib_entry->fe_node);
}

void
fib_entry_module_init (void)
{
    fib_node_register_type (FIB_NODE_TYPE_ENTRY, &fib_entry_vft);
}

void
fib_entry_encode (fib_node_index_t fib_entry_index,
		  fib_route_path_encode_t **api_rpaths)
{
    fib_entry_t *fib_entry;

    fib_entry = fib_entry_get(fib_entry_index);
    fib_path_list_walk(fib_entry->fe_parent, fib_path_encode, api_rpaths);
}

void
fib_entry_get_prefix (fib_node_index_t fib_entry_index,
		      fib_prefix_t *pfx)
{
    fib_entry_t *fib_entry;

    fib_entry = fib_entry_get(fib_entry_index);
    *pfx = fib_entry->fe_prefix;
}

u32
fib_entry_get_fib_index (fib_node_index_t fib_entry_index)
{
    fib_entry_t *fib_entry;

    fib_entry = fib_entry_get(fib_entry_index);

    return (fib_entry->fe_fib_index);
}

u32
fib_entry_pool_size (void)
{
    return (pool_elts(fib_entry_pool));
}

static clib_error_t *
show_fib_entry_command (vlib_main_t * vm,
			unformat_input_t * input,
			vlib_cli_command_t * cmd)
{
    fib_node_index_t fei;

    if (unformat (input, "%d", &fei))
    {
	/*
	 * show one in detail
	 */
	if (!pool_is_free_index(fib_entry_pool, fei))
	{
	    vlib_cli_output (vm, "%d@%U",
			     fei,
			     format_fib_entry, fei,
			     FIB_ENTRY_FORMAT_DETAIL2);
	}
	else
	{
	    vlib_cli_output (vm, "entry %d invalid", fei);
	}
    }
    else
    {
	/*
	 * show all
	 */
	vlib_cli_output (vm, "FIB Entries:");
	pool_foreach_index(fei, fib_entry_pool,
        ({
	    vlib_cli_output (vm, "%d@%U",
			     fei,
			     format_fib_entry, fei,
			     FIB_ENTRY_FORMAT_BRIEF);
	}));
    }

    return (NULL);
}

VLIB_CLI_COMMAND (show_fib_entry, static) = {
  .path = "show fib entry",
  .function = show_fib_entry_command,
  .short_help = "show fib entry",
};
