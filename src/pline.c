/** @file pline.c
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <syslog.h>
#include <limits.h>

#include <faux/faux.h>
#include <faux/str.h>
#include <faux/list.h>
#include <faux/argv.h>
#include <faux/ini.h>
#include <faux/conv.h>

#include <sysrepo.h>
#include <sysrepo/xpath.h>
#include <sysrepo/values.h>
#include <libyang/tree_edit.h>

#include "klish_plugin_sysrepo.h"


static int sr_ly_module_is_internal(const struct lys_module *ly_mod)
{
	if (!ly_mod->revision) {
		return 0;
	}

	if (!strcmp(ly_mod->name, "ietf-yang-metadata")
	    && !strcmp(ly_mod->revision, "2016-08-05")) {
		return 1;
	} else if (!strcmp(ly_mod->name, "yang")
		   && !strcmp(ly_mod->revision, "2021-04-07")) {
		return 1;
	} else if (!strcmp(ly_mod->name, "ietf-inet-types")
		   && !strcmp(ly_mod->revision, "2013-07-15")) {
		return 1;
	} else if (!strcmp(ly_mod->name, "ietf-yang-types")
		   && !strcmp(ly_mod->revision, "2013-07-15")) {
		return 1;
	}

	return 0;
}


static int sr_module_is_internal(const struct lys_module *ly_mod, bool_t enable_nacm)
{
	if (!ly_mod->revision) {
		return 0;
	}

	if (sr_ly_module_is_internal(ly_mod)) {
		return 1;
	}

	if (!strcmp(ly_mod->name, "ietf-datastores")
	    && !strcmp(ly_mod->revision, "2018-02-14")) {
		return 1;
	} else if (!strcmp(ly_mod->name, "ietf-yang-schema-mount")) {
		return 1;
	} else if (!strcmp(ly_mod->name, "ietf-yang-library")) {
		return 1;
	} else if (!strcmp(ly_mod->name, "ietf-netconf")) {
		return 1;
	} else if (!strcmp(ly_mod->name, "ietf-netconf-with-defaults")
		   && !strcmp(ly_mod->revision, "2011-06-01")) {
		return 1;
	} else if (!strcmp(ly_mod->name, "ietf-origin")
		   && !strcmp(ly_mod->revision, "2018-02-14")) {
		return 1;
	} else if (!strcmp(ly_mod->name, "ietf-netconf-notifications")
		   && !strcmp(ly_mod->revision, "2012-02-06")) {
		return 1;
	} else if (!strcmp(ly_mod->name, "sysrepo")) {
		return 1;
	} else if (!strcmp(ly_mod->name, "sysrepo-monitoring")) {
		return 1;
	} else if (!strcmp(ly_mod->name, "sysrepo-plugind")) {
		return 1;
	} else if (!strcmp(ly_mod->name, "ietf-netconf-acm") && !enable_nacm) {
		return 1;
	}

	return 0;
}


static pexpr_t *pexpr_new(void)
{
	pexpr_t *pexpr = NULL;

	pexpr = faux_zmalloc(sizeof(*pexpr));
	assert(pexpr);
	if (!pexpr)
		return NULL;

	// Initialize
	pexpr->xpath = NULL;
	pexpr->value = NULL;
	pexpr->active = BOOL_FALSE;
	pexpr->pat = PAT_NONE;
	pexpr->args_num = 0;
	pexpr->list_pos = 0;
	pexpr->last_keys = NULL;

	return pexpr;
}


static void pexpr_free(pexpr_t *pexpr)
{
	if (!pexpr)
		return;

	faux_str_free(pexpr->xpath);
	faux_str_free(pexpr->value);
	faux_str_free(pexpr->last_keys);

	free(pexpr);
}


static pcompl_t *pcompl_new(void)
{
	pcompl_t *pcompl = NULL;

	pcompl = faux_zmalloc(sizeof(*pcompl));
	assert(pcompl);
	if (!pcompl)
		return NULL;

	// Initialize
	pcompl->type = PCOMPL_NODE;
	pcompl->node = NULL;
	pcompl->xpath = NULL;
	pcompl->xpath_ds = SRP_REPO_EDIT;
	pcompl->pat = PAT_NONE;

	return pcompl;
}


static void pcompl_free(pcompl_t *pcompl)
{
	if (!pcompl)
		return;

	faux_str_free(pcompl->xpath);

	free(pcompl);
}


pline_t *pline_new(sr_session_ctx_t *sess)
{
	pline_t *pline = NULL;

	pline = faux_zmalloc(sizeof(*pline));
	assert(pline);
	if (!pline)
		return NULL;

	// Init
	pline->sess = sess;
	pline->invalid = BOOL_FALSE;
	pline->exprs = faux_list_new(FAUX_LIST_UNSORTED, FAUX_LIST_NONUNIQUE,
		NULL, NULL, (faux_list_free_fn)pexpr_free);
	pline->compls = faux_list_new(FAUX_LIST_UNSORTED, FAUX_LIST_NONUNIQUE,
		NULL, NULL, (faux_list_free_fn)pcompl_free);

	return pline;
}


void pline_free(pline_t *pline)
{
	if (!pline)
		return;

	faux_list_free(pline->exprs);
	faux_list_free(pline->compls);

	faux_free(pline);
}


static pexpr_t *pline_add_expr(pline_t *pline, const char *xpath,
	size_t args_num, size_t list_pos, size_t tree_depth)
{
	pexpr_t *pexpr = NULL;

	assert(pline);

	pexpr = pexpr_new();
	if (xpath)
		pexpr->xpath = faux_str_dup(xpath);
	pexpr->args_num = args_num;
	pexpr->list_pos = list_pos;
	pexpr->tree_depth = tree_depth;
	faux_list_add(pline->exprs, pexpr);

	return pexpr;
}


pexpr_t *pline_current_expr(pline_t *pline)
{
	assert(pline);

	if (faux_list_len(pline->exprs) == 0)
		pline_add_expr(pline, NULL, 0, 0, 0);

	return (pexpr_t *)faux_list_data(faux_list_tail(pline->exprs));
}


static void pline_add_compl(pline_t *pline,
	pcompl_type_e type, const struct lysc_node *node,
	const char *xpath, sr_datastore_t ds, pat_e pat)
{
	pcompl_t *pcompl = NULL;

	assert(pline);

	pcompl = pcompl_new();
	pcompl->type = type;
	pcompl->node = node;
	pcompl->pat = pat;
	if (xpath) {
		pcompl->xpath = faux_str_dup(xpath);
		pcompl->xpath_ds = ds;
	}
	faux_list_add(pline->compls, pcompl);
}


static void pline_add_compl_subtree(pline_t *pline, const struct lys_module *module,
	const struct lysc_node *node, const char *xpath)
{
	const struct lysc_node *subtree = NULL;
	const struct lysc_node *iter = NULL;

	assert(pline);
	assert(module);
	if (node)
		subtree = lysc_node_child(node);
	else
		subtree = module->compiled->data;

	LY_LIST_FOR(subtree, iter) {
		pat_e pat = PAT_NONE;
		char *node_xpath = NULL;

		if (!(iter->nodetype & SRP_NODETYPE_CONF))
			continue;
		if (!(iter->flags & LYS_CONFIG_W))
			continue;
		if ((iter->nodetype & LYS_LEAF) && (iter->flags & LYS_KEY))
			continue;
		if (iter->nodetype & (LYS_CHOICE | LYS_CASE)) {
			pline_add_compl_subtree(pline, module, iter, xpath);
			continue;
		}
		switch(iter->nodetype) {
		case LYS_CONTAINER:
			pat = PAT_CONTAINER;
			break;
		case LYS_LEAF:
			pat = PAT_LEAF;
			break;
		case LYS_LEAFLIST:
			pat = PAT_LEAFLIST;
			break;
		case LYS_LIST:
			pat = PAT_LIST;
			break;
		default:
			continue;
			break;
		}

		node_xpath = faux_str_sprintf("%s/%s:%s", xpath ? xpath : "",
			iter->module->name, iter->name);
		pline_add_compl(pline, PCOMPL_NODE, iter, node_xpath,
			SRP_REPO_EDIT, pat);
		faux_str_free(node_xpath);
	}
}


static const char *pat2str(pat_e pat)
{
	const char *str = NULL;

	switch (pat) {
	case PAT_NONE:
		str = "NONE";
		break;
	case PAT_CONTAINER:
		str = "CONTAINER";
		break;
	case PAT_LIST:
		str = "LIST";
		break;
	case PAT_LIST_KEY:
		str = "LIST_KEY";
		break;
	case PAT_LIST_KEY_INCOMPLETED:
		str = "LIST_KEY_INCOMPLETED";
		break;
	case PAT_LEAF:
		str = "LEAF";
		break;
	case PAT_LEAF_VALUE:
		str = "LEAF_VALUE";
		break;
	case PAT_LEAF_EMPTY:
		str = "LEAF_EMPTY";
		break;
	case PAT_LEAFLIST:
		str = "LEAFLIST";
		break;
	case PAT_LEAFLIST_VALUE:
		str = "LEAFLIST_VALUE";
		break;
	default:
		str = "UNKNOWN";
		break;
	}

	return str;
}


void pline_debug(const pline_t *pline)
{
	faux_list_node_t *iter = NULL;
	pexpr_t *pexpr = NULL;
	pcompl_t *pcompl = NULL;

	syslog(LOG_ERR, "====== Pline:");
	syslog(LOG_ERR, "invalid = %s", pline->invalid ? "true" : "false");

	syslog(LOG_ERR, "=== Expressions:");

	iter = faux_list_head(pline->exprs);
	while ((pexpr = (pexpr_t *)faux_list_each(&iter))) {
		syslog(LOG_ERR, "pexpr.xpath = %s", pexpr->xpath ? pexpr->xpath : "NULL");
		syslog(LOG_ERR, "pexpr.value = %s", pexpr->value ? pexpr->value : "NULL");
		syslog(LOG_ERR, "pexpr.active = %s", pexpr->active ? "true" : "false");
		syslog(LOG_ERR, "pexpr.pat = %s", pat2str(pexpr->pat));
		syslog(LOG_ERR, "pexpr.args_num = %lu", pexpr->args_num);
		syslog(LOG_ERR, "pexpr.list_pos = %lu", pexpr->list_pos);
		syslog(LOG_ERR, "pexpr.last_keys = %s", pexpr->last_keys ? pexpr->last_keys : "NULL");
		syslog(LOG_ERR, "pexpr.tree_depth = %lu", pexpr->tree_depth);
		syslog(LOG_ERR, "---");
	}

	syslog(LOG_ERR, "=== Completions:");

	iter = faux_list_head(pline->compls);
	while ((pcompl = (pcompl_t *)faux_list_each(&iter))) {
		syslog(LOG_ERR, "pcompl.type = %s", (pcompl->type == PCOMPL_NODE) ?
			"PCOMPL_NODE" : "PCOMPL_TYPE");
		syslog(LOG_ERR, "pcompl.node = %s", pcompl->node ? pcompl->node->name : "NULL");
		syslog(LOG_ERR, "pcompl.xpath = %s", pcompl->xpath ? pcompl->xpath : "NULL");
		syslog(LOG_ERR, "pcompl.pat = %s", pat2str(pcompl->pat));
		syslog(LOG_ERR, "---");
	}
}


static bool_t pexpr_xpath_add_node(pexpr_t *pexpr,
	const char *prefix, const char *name)
{
	char *tmp = NULL;

	assert(pexpr);
	assert(prefix);
	assert(name);

	tmp = faux_str_sprintf("/%s:%s", prefix, name);
	faux_str_cat(&pexpr->xpath, tmp);
	faux_str_free(tmp);
	pexpr->args_num++;
	// Activate current expression. Because it really has
	// new component
	pexpr->active = BOOL_TRUE;

	return BOOL_TRUE;
}


static bool_t pexpr_xpath_add_list_key(pexpr_t *pexpr,
	const char *key, const char *value, bool_t inc_args_num)
{
	char *tmp = NULL;
	char *escaped = NULL;

	assert(pexpr);
	assert(key);
	assert(value);

	escaped = faux_str_c_esc(value);
	tmp = faux_str_sprintf("[%s=\"%s\"]", key, escaped);
	faux_str_free(escaped);
	faux_str_cat(&pexpr->xpath, tmp);
	faux_str_cat(&pexpr->last_keys, tmp);
	faux_str_free(tmp);
	if (inc_args_num)
		pexpr->args_num++;

	return BOOL_TRUE;
}


static bool_t pexpr_xpath_add_leaflist_key(pexpr_t *pexpr,
	const char *prefix, const char *value)
{
	char *tmp = NULL;

	assert(pexpr);
	assert(value);

	tmp = faux_str_sprintf("[.='%s%s%s']",
		prefix ? prefix : "", prefix ? ":" : "", value);
	faux_str_cat(&pexpr->xpath, tmp);
	faux_str_cat(&pexpr->last_keys, value);
	faux_str_free(tmp);
	pexpr->args_num++;

	return BOOL_TRUE;
}


static void pline_add_compl_leafref(pline_t *pline, const struct lysc_node *node,
	const struct lysc_type *type, const char *xpath, pat_e pat)
{
	if (!type)
		return;
	if (!node)
		return;
	if (!(node->nodetype & (LYS_LEAF | LYS_LEAFLIST)))
		return;

	switch (type->basetype) {

	case LY_TYPE_UNION: {
		struct lysc_type_union *t =
			(struct lysc_type_union *)type;
		LY_ARRAY_COUNT_TYPE u = 0;
		LY_ARRAY_FOR(t->types, u) {
			pline_add_compl_leafref(pline, node, t->types[u], xpath, pat);
		}
		break;
	}

	case LY_TYPE_LEAFREF: {
		char *compl_xpath = klysc_leafref_xpath(node, type, xpath);
		pline_add_compl(pline, PCOMPL_TYPE, NULL, compl_xpath, SRP_REPO_EDIT, pat);
		faux_str_free(compl_xpath);
		break;
	}

	default:
		break;
	}
}


static void pline_add_compl_leaf(pline_t *pline, const struct lysc_node *node,
	const char *xpath, pat_e pat)
{
	struct lysc_type *type = NULL;
	const char *ext_xpath = NULL;

	assert(pline);
	if (!pline)
		return;
	assert(node);
	if (!node)
		return;

	switch (node->nodetype) {
	case LYS_LEAF:
		type = ((struct lysc_node_leaf *)node)->type;
		break;
	case LYS_LEAFLIST:
		type = ((struct lysc_node_leaflist *)node)->type;
		break;
	default:
		return;
	}

	ext_xpath = klysc_node_ext_completion(node);
	if (ext_xpath) {
		const char *raw_xpath = NULL;
		sr_datastore_t ds = SRP_REPO_EDIT;
		if (kly_parse_ext_xpath(ext_xpath, &raw_xpath, &ds))
			pline_add_compl(pline, PCOMPL_TYPE, NULL, raw_xpath, ds, pat);
	}
	pline_add_compl(pline, PCOMPL_TYPE, node, xpath, SRP_REPO_EDIT, pat);
	pline_add_compl_leafref(pline, node, type, xpath, pat);
}


static bool_t pline_parse_module(const struct lys_module *module,
	const faux_argv_t *argv, pline_t *pline, const pline_opts_t *opts)
{
	faux_argv_node_t *arg = faux_argv_iter(argv);
	const struct lysc_node *node = NULL;
	char *rollback_xpath = NULL;
	size_t rollback_args_num = 0;
	size_t rollback_list_pos = 0;
	size_t rollback_tree_depth = 0;
	// Rollback is a mechanism to roll to previous node while
	// oneliners parsing
	bool_t rollback = BOOL_FALSE;
	pexpr_t *first_pexpr = NULL;

	// It's necessary because upper function can use the same pline object
	// for another modules before. It uses the same object to collect
	// possible completions. But pline is really invalid only when all
	// modules don't recognize argument.
	pline->invalid = BOOL_FALSE;

	do {
		pexpr_t *pexpr = pline_current_expr(pline);
		const char *str = (const char *)faux_argv_current(arg);
		bool_t is_rollback = rollback;
		bool_t next_arg = BOOL_TRUE;

		rollback = BOOL_FALSE;

		if (node && !is_rollback) {

			// Save rollback Xpath (for oneliners) before leaf node
			// Only leaf and leaf-list node allows to "rollback"
			// the path and add additional statements
			if (node->nodetype & (LYS_LEAF | LYS_LEAFLIST)) {
				faux_str_free(rollback_xpath);
				rollback_xpath = faux_str_dup(pexpr->xpath);
				rollback_args_num = pexpr->args_num;
				rollback_list_pos = pexpr->list_pos;
				rollback_tree_depth = pexpr->tree_depth;
			}

			// Add current node to Xpath
			pexpr_xpath_add_node(pexpr,
				node->module->name, node->name);
		}

		// Root of the module
		if (!node) {

			// Completion
			if (!str) {
				pline_add_compl_subtree(pline, module, node, pexpr->xpath);
				break;
			}

			// Next element
			node = klysc_find_child(module->compiled->data, str);
			if (!node)
				break;

		// Container
		} else if (node->nodetype & LYS_CONTAINER) {

			pexpr->pat = PAT_CONTAINER;
			pexpr->tree_depth++;

			// Completion
			if (!str) {
				pline_add_compl_subtree(pline, module, node, pexpr->xpath);
				break;
			}

			// Next element
			node = klysc_find_child(lysc_node_child(node), str);

		// List
		} else if (node->nodetype & LYS_LIST) {
			const struct lysc_node *iter = NULL;

			pexpr->pat = PAT_LIST;
			pexpr->list_pos = pexpr->args_num;
			faux_str_free(pexpr->last_keys);
			pexpr->last_keys = NULL;

			// Next element
			if (!is_rollback) {
				bool_t break_upper_loop = BOOL_FALSE;

				// Keys without statement. Positional parameters.
				if (!opts->keys_w_stmt) {

					LY_LIST_FOR(lysc_node_child(node), iter) {
						struct lysc_node_leaf *leaf =
							(struct lysc_node_leaf *)iter;

						if (!(iter->nodetype & LYS_LEAF))
							continue;
						if (!(iter->flags & LYS_KEY))
							continue;
						assert (leaf->type->basetype != LY_TYPE_EMPTY);

						// Completion
						if (!str) {
							char *tmp = faux_str_sprintf("%s/%s",
								pexpr->xpath, leaf->name);
							pline_add_compl_leaf(pline, iter,
								tmp, PAT_LIST_KEY);
							faux_str_free(tmp);
							break_upper_loop = BOOL_TRUE;
							break;
						}

						pexpr_xpath_add_list_key(pexpr,
							leaf->name, str, BOOL_TRUE);
						faux_argv_each(&arg);
						str = (const char *)faux_argv_current(arg);
						pexpr->pat = PAT_LIST_KEY;
					}

				// Keys with statements. Arbitrary order of keys.
				} else {
					faux_list_t *keys = NULL;
					unsigned int specified_keys_num = 0;
					klysc_key_t *cur_key = NULL;
					char *xpath_wo_default_keys = NULL;
					bool_t first_key = BOOL_TRUE;
					bool_t first_key_is_optional = BOOL_FALSE;
					faux_list_node_t *key_iter = NULL;

					// List keys
					keys = faux_list_new(FAUX_LIST_UNSORTED,
						FAUX_LIST_UNIQUE,
						klysc_key_compare,
						klysc_key_kcompare,
						(faux_list_free_fn)faux_free);

					LY_LIST_FOR(lysc_node_child(node), iter) {
						struct lysc_node_leaf *leaf =
							(struct lysc_node_leaf *)iter;
						klysc_key_t *key = NULL;

						if (!(iter->nodetype & LYS_LEAF))
							continue;
						if (!(iter->flags & LYS_KEY))
							continue;
						assert (leaf->type->basetype != LY_TYPE_EMPTY);
						key = faux_zmalloc(sizeof(*key));
						assert(key);
						key->node = iter;
						if (opts->default_keys &&
							(key->dflt = klysc_node_ext_default(iter))) {
							if (first_key)
								first_key_is_optional = BOOL_TRUE;
						}
						faux_list_add(keys, key);
						first_key = BOOL_FALSE;
					}

					while (specified_keys_num < faux_list_len(keys)) {

						// First key without statement. Must be mandatory.
						if ((0 == specified_keys_num) &&
							!opts->first_key_w_stmt &&
							!first_key_is_optional) {
							cur_key = (klysc_key_t *)faux_list_data(faux_list_head(keys));
						} else {
							if (!str)
								break;
							cur_key = faux_list_kfind(keys, str);
							if (!cur_key || cur_key->value)
								break;
							pexpr->args_num++;
							faux_argv_each(&arg);
							str = (const char *)faux_argv_current(arg);
							pexpr->pat = PAT_LIST_KEY_INCOMPLETED;
						}

						// Completion
						if (!str) {
							char *tmp = faux_str_sprintf("%s/%s",
								pexpr->xpath,
								cur_key->node->name);
							pline_add_compl_leaf(pline, cur_key->node,
								tmp, PAT_LIST_KEY);
							faux_str_free(tmp);
							break_upper_loop = BOOL_TRUE;
							break;
						}

						pexpr_xpath_add_list_key(pexpr,
							cur_key->node->name, str, BOOL_TRUE);
						cur_key->value = str;
						specified_keys_num++;
						faux_argv_each(&arg);
						str = (const char *)faux_argv_current(arg);
						pexpr->pat = PAT_LIST_KEY;
					}
					if (break_upper_loop) {
						faux_list_free(keys);
						break;
					}

					xpath_wo_default_keys = faux_str_dup(pexpr->xpath);
					key_iter = faux_list_head(keys);
					while((cur_key = (klysc_key_t *)faux_list_each(&key_iter))) {
						if (cur_key->value)
							continue;

						// Completion
						if (!str) {
							char *tmp = faux_str_sprintf("%s/%s",
								xpath_wo_default_keys,
								cur_key->node->name);
							pline_add_compl(pline, PCOMPL_NODE,
								cur_key->node, tmp,
								SRP_REPO_EDIT, PAT_LIST_KEY_INCOMPLETED);
							faux_str_free(tmp);
						}

						if (opts->default_keys && cur_key->dflt) {
							pexpr_xpath_add_list_key(pexpr,
								cur_key->node->name,
								cur_key->dflt, BOOL_FALSE);
							pexpr->pat = PAT_LIST_KEY;
						} else { // Mandatory key is not specified
							break_upper_loop = BOOL_TRUE;
						}
					}
					faux_str_free(xpath_wo_default_keys);
					faux_list_free(keys);
				}
				if (break_upper_loop)
					break;
			}

			pexpr->tree_depth++;

 			// Completion
			if (!str) {
				pline_add_compl_subtree(pline, module, node, pexpr->xpath);
				break;
			}

			// Next element
			node = klysc_find_child(lysc_node_child(node), str);

		// Leaf
		} else if (node->nodetype & LYS_LEAF) {
			struct lysc_node_leaf *leaf =
				(struct lysc_node_leaf *)node;

			// Next element
			if (LY_TYPE_EMPTY == leaf->type->basetype) {

				pexpr->pat = PAT_LEAF_EMPTY;

				// Completion
				if (!str) {
					pline_add_compl_subtree(pline,
						module, node->parent, pexpr->xpath);
					break;
				}
				// Don't get next argument when argument is not
				// really consumed
				next_arg = BOOL_FALSE;
			} else {

				pexpr->pat = PAT_LEAF;

				// Completion
				if (!str) {
					pline_add_compl_leaf(pline, node,
						pexpr->xpath, PAT_LEAF_VALUE);
					break;
				}

				pexpr->pat = PAT_LEAF_VALUE;

				// Idenity must have prefix
				if (LY_TYPE_IDENT == leaf->type->basetype) {
					const char *prefix = NULL;
					prefix = klysc_identityref_prefix(
						(struct lysc_type_identityref *)
						leaf->type, str);
					if (prefix)
						pexpr->value = faux_str_sprintf(
							"%s:", prefix);
				}
				faux_str_cat(&pexpr->value, str);
			}
			// Expression was completed
			// So rollback (for oneliners)
			node = node->parent;
			pline_add_expr(pline, rollback_xpath,
				rollback_args_num, rollback_list_pos,
				rollback_tree_depth);
			rollback = BOOL_TRUE;

		// Leaf-list
		} else if (node->nodetype & LYS_LEAFLIST) {
			const char *prefix = NULL;
			struct lysc_node_leaflist *leaflist =
				(struct lysc_node_leaflist *)node;

			pexpr->pat = PAT_LEAFLIST;
			pexpr->list_pos = pexpr->args_num;
			faux_str_free(pexpr->last_keys);
			pexpr->last_keys = NULL;

			// Completion
			if (!str) {
				pline_add_compl_leaf(pline, node,
					pexpr->xpath, PAT_LEAFLIST_VALUE);
				break;
			}

			pexpr->pat = PAT_LEAFLIST_VALUE;

			// Idenity must have prefix
			if (LY_TYPE_IDENT == leaflist->type->basetype) {
				prefix = klysc_identityref_prefix(
					(struct lysc_type_identityref *)
					leaflist->type, str);
			}

			pexpr_xpath_add_leaflist_key(pexpr, prefix, str);

			// Expression was completed
			// So rollback (for oneliners)
			node = node->parent;
			pline_add_expr(pline, rollback_xpath,
				rollback_args_num, rollback_list_pos,
				rollback_tree_depth);
			rollback = BOOL_TRUE;

		// LYS_CHOICE and LYS_CASE can appear while rollback only
		} else if (node->nodetype & (LYS_CHOICE | LYS_CASE)) {

			// Don't set pexpr->pat because CHOICE and CASE can't
			// appear within data tree (schema only)

			// Completion
			if (!str) {
				pline_add_compl_subtree(pline, module, node, pexpr->xpath);
				break;
			}

			// Next element
			node = klysc_find_child(lysc_node_child(node), str);

		} else {
			break;
		}

		// Current argument was not consumed.
		// Break before getting next arg.
		if (!node && !rollback)
			break;

		if (next_arg)
			faux_argv_each(&arg);
	} while (BOOL_TRUE);

	// There is not-consumed argument so whole pline is invalid
	if (faux_argv_current(arg))
		pline->invalid = BOOL_TRUE;

	faux_str_free(rollback_xpath);

	first_pexpr = (pexpr_t *)faux_list_data(faux_list_head(pline->exprs));
	if (!first_pexpr || !first_pexpr->xpath)
		return BOOL_FALSE; // Not found

	return BOOL_TRUE;
}


pline_t *pline_parse(sr_session_ctx_t *sess, const faux_argv_t *argv,
	const pline_opts_t *opts)
{
	const struct ly_ctx *ctx = NULL;
	struct lys_module *module = NULL;
	pline_t *pline = NULL;
	uint32_t i = 0;
	faux_list_node_t *last_expr_node = NULL;

	assert(sess);
	if (!sess)
		return NULL;

	pline = pline_new(sess);
	if (!pline)
		return NULL;
	ctx = sr_session_acquire_context(pline->sess);
	if (!ctx)
		return NULL;

	// Iterate all modules
	i = 0;
	while ((module = ly_ctx_get_module_iter(ctx, &i))) {
		if (sr_module_is_internal(module, opts->enable_nacm))
			continue;
		if (!module->compiled)
			continue;
		if (!module->implemented)
			continue;
		if (!module->compiled->data)
			continue;
		if (pline_parse_module(module, argv, pline, opts))
			break; // Found
	}

	sr_session_release_context(pline->sess);

	// Last parsed expression can be inactive so remove it from list
	last_expr_node = faux_list_tail(pline->exprs);
	if (last_expr_node) {
		pexpr_t *expr = (pexpr_t *)faux_list_data(last_expr_node);
		if (!expr->active)
			faux_list_del(pline->exprs, last_expr_node);
	}

	return pline;
}


static void identityref_compl(struct lysc_ident *ident)
{
	LY_ARRAY_COUNT_TYPE u = 0;

	if (!ident)
		return;

	if (!ident->derived) {
		printf("%s\n", ident->name);
		return;
	}

	LY_ARRAY_FOR(ident->derived, u) {
		identityref_compl(ident->derived[u]);
	}
}


static void identityref_help(struct lysc_ident *ident)
{
	LY_ARRAY_COUNT_TYPE u = 0;

	if (!ident)
		return;

	if (!ident->derived) {
		if (ident->dsc) {
			char *dsc = faux_str_getline(ident->dsc, NULL);
			printf("%s\n%s\n", ident->name, dsc);
			faux_str_free(dsc);
		} else {
			printf("%s\n%s\n", ident->name, ident->name);
		}
		return;
	}

	LY_ARRAY_FOR(ident->derived, u) {
		identityref_help(ident->derived[u]);
	}
}



static void pline_print_type_completions(const struct lysc_type *type)
{
	assert(type);

	switch (type->basetype) {

	case LY_TYPE_BOOL: {
		printf("true\nfalse\n");
		break;
	}

	case LY_TYPE_ENUM: {
		const struct lysc_type_enum *t =
			(const struct lysc_type_enum *)type;
		LY_ARRAY_COUNT_TYPE u = 0;

		LY_ARRAY_FOR(t->enums, u) {
			printf("%s\n",t->enums[u].name);
		}
		break;
	}

	case LY_TYPE_IDENT: {
		struct lysc_type_identityref *t =
			(struct lysc_type_identityref *)type;
		LY_ARRAY_COUNT_TYPE u = 0;

		LY_ARRAY_FOR(t->bases, u) {
			identityref_compl(t->bases[u]);
		}
		break;
	}

	case LY_TYPE_UNION: {
		struct lysc_type_union *t =
			(struct lysc_type_union *)type;
		LY_ARRAY_COUNT_TYPE u = 0;

		LY_ARRAY_FOR(t->types, u) {
			pline_print_type_completions(t->types[u]);
		}
		break;
	}

	case LY_TYPE_LEAFREF: {
		struct lysc_type_leafref *t =
			(struct lysc_type_leafref *)type;
		pline_print_type_completions(t->realtype);
		break;
	}

	default:
		break;
	}
}


static void uint_range(const struct lysc_type *type, uint64_t def_min, uint64_t def_max)
{
	struct lysc_range *range = NULL;
	LY_ARRAY_COUNT_TYPE u = 0;
	char *r = NULL;

	assert(type);
	range = ((struct lysc_type_num *)type)->range;

	// Show defaults
	if (!range) {
		printf("[%" PRIu64 "..%" PRIu64 "]\n", def_min, def_max);
		return;
	}

	// Range
	faux_str_cat(&r, "[");
	LY_ARRAY_FOR(range->parts, u) {
		char *t = NULL;
		if (u != 0)
			faux_str_cat(&r, "|");
		t = faux_str_sprintf("%" PRIu64 "..%" PRIu64,
			range->parts[u].min_u64, range->parts[u].max_u64);
		faux_str_cat(&r, t);
		faux_str_free(t);
	}
	faux_str_cat(&r, "]\n");
	printf("%s", r);
	faux_free(r);
}


static void int_range(const struct lysc_type *type, int64_t def_min, int64_t def_max)
{
	struct lysc_range *range = NULL;
	LY_ARRAY_COUNT_TYPE u = 0;
	char *r = NULL;

	assert(type);
	range = ((struct lysc_type_num *)type)->range;

	// Show defaults
	if (!range) {
		printf("[%" PRId64 "..%" PRId64 "]\n", def_min, def_max);
		return;
	}

	// Range
	faux_str_cat(&r, "[");
	LY_ARRAY_FOR(range->parts, u) {
		char *t = NULL;
		if (u != 0)
			faux_str_cat(&r, "|");
		t = faux_str_sprintf("%" PRId64 "..%" PRId64,
			range->parts[u].min_64, range->parts[u].max_64);
		faux_str_cat(&r, t);
		faux_str_free(t);
	}
	faux_str_cat(&r, "]\n");
	printf("%s", r);
	faux_free(r);
}


static void dec_range(const struct lysc_type *type, int64_t def_min, int64_t def_max)
{
	struct lysc_range *range = NULL;
	uint8_t fraction_digits = 0;
	LY_ARRAY_COUNT_TYPE u = 0;
	char *r = NULL;
	int64_t div = 1;
	uint8_t i = 0;

	assert(type);
	range = ((struct lysc_type_dec *)type)->range;
	fraction_digits = ((struct lysc_type_dec *)type)->fraction_digits;
	for (i = 0; i < fraction_digits; i++)
		div = div * 10;

	// Show defaults
	if (!range) {
		printf("[%.*f..%.*f]\n",
			fraction_digits, (double)def_min / div,
			fraction_digits, (double)def_max / div);
		return;
	}

	// Range
	faux_str_cat(&r, "[");
	LY_ARRAY_FOR(range->parts, u) {
		char *t = NULL;
		if (u != 0)
			faux_str_cat(&r, "|");
		t = faux_str_sprintf("%.*f..%.*f",
			fraction_digits, (double)range->parts[u].min_64 / div,
			fraction_digits, (double)range->parts[u].max_64 / div);
		faux_str_cat(&r, t);
		faux_str_free(t);
	}
	faux_str_cat(&r, "]\n");
	printf("%s", r);
	faux_free(r);
}


static void str_range(const struct lysc_type *type)
{
	struct lysc_range *range = NULL;
	LY_ARRAY_COUNT_TYPE u = 0;
	char *r = NULL;

	assert(type);
	range = ((struct lysc_type_str *)type)->length;

	// Show defaults
	if (!range) {
		printf("<string>\n");
		return;
	}

	// Range
	faux_str_cat(&r, "<string[");
	LY_ARRAY_FOR(range->parts, u) {
		char *t = NULL;
		if (u != 0)
			faux_str_cat(&r, "|");
		t = faux_str_sprintf("%" PRIu64 "..%" PRIu64,
			range->parts[u].min_u64, range->parts[u].max_u64);
		faux_str_cat(&r, t);
		faux_str_free(t);
	}
	faux_str_cat(&r, "]>\n");
	printf("%s", r);
	faux_free(r);
}


static void pline_print_type_help(const struct lysc_node *node,
	const struct lysc_type *type)
{
	const char *units = NULL;

	assert(type);
	assert(node);

	if (node->nodetype & LYS_LEAF)
		units = ((struct lysc_node_leaf *)node)->units;
	else if (node->nodetype & LYS_LEAFLIST)
		units = ((struct lysc_node_leaflist *)node)->units;
	else
		return;

	if (units) {
		printf("%s\n", units);
	} else {
		switch (type->basetype) {

		case LY_TYPE_UINT8:
			uint_range(type, 0, UCHAR_MAX);
			break;

		case LY_TYPE_UINT16:
			uint_range(type, 0, USHRT_MAX);
			break;

		case LY_TYPE_UINT32:
			uint_range(type, 0, UINT_MAX);
			break;

		case LY_TYPE_UINT64:
			uint_range(type, 0, ULLONG_MAX);
			break;

		case LY_TYPE_INT8:
			int_range(type, CHAR_MIN, CHAR_MAX);
			break;

		case LY_TYPE_INT16:
			int_range(type, SHRT_MIN, SHRT_MAX);
			break;

		case LY_TYPE_INT32:
			int_range(type, INT_MIN, INT_MAX);
			break;

		case LY_TYPE_INT64:
			int_range(type, LLONG_MIN, LLONG_MAX);
			break;

		case LY_TYPE_DEC64:
			dec_range(type, LLONG_MIN, LLONG_MAX);
			break;

		case LY_TYPE_STRING:
			str_range(type);
			break;

		case LY_TYPE_BOOL:
			printf("<true/false>\n");
			break;

		case LY_TYPE_LEAFREF: {
			const struct lysc_type_leafref *t =
				(const struct lysc_type_leafref *)type;
			const struct lysc_node *ref_node = NULL;
			const struct lysc_type *ref_type = NULL;
			char *node_path = lysc_path(node, LYSC_PATH_LOG, NULL, 0);
			char *path = klysc_leafref_xpath(node, type, node_path);
			faux_str_free(node_path);
			ref_node = lys_find_path(NULL, node, path, 0);
			faux_str_free(path);
			if (!ref_node) {
				pline_print_type_help(node, t->realtype);
				return; // Because it prints whole info itself
			}
			if (ref_node->nodetype & LYS_LEAF)
				ref_type = ((struct lysc_node_leaf *)ref_node)->type;
			else
				ref_type = ((struct lysc_node_leaflist *)ref_node)->type;
			pline_print_type_help(ref_node, ref_type);
			return; // Because it prints whole info itself
		}

		case LY_TYPE_UNION: {
			const struct lysc_type_union *t =
				(const struct lysc_type_union *)type;
			LY_ARRAY_COUNT_TYPE u = 0;
			LY_ARRAY_FOR(t->types, u)
				pline_print_type_help(node, t->types[u]);
			return; // Because it prints whole info itself
		}

		case LY_TYPE_ENUM: {
			const struct lysc_type_enum *t =
				(const struct lysc_type_enum *)type;
			LY_ARRAY_COUNT_TYPE u = 0;
			LY_ARRAY_FOR(t->enums, u)
				if (t->enums[u].dsc) {
					char *dsc = faux_str_getline(
						t->enums[u].dsc, NULL);
					printf("%s\n%s\n",
						t->enums[u].name, dsc);
					faux_str_free(dsc);
				} else {
					printf("%s\n%s\n",
						t->enums[u].name,
						t->enums[u].name);
				}
			return; // Because it prints whole info itself
		}

		case LY_TYPE_IDENT: {
			struct lysc_type_identityref *t =
				(struct lysc_type_identityref *)type;
			LY_ARRAY_COUNT_TYPE u = 0;
			LY_ARRAY_FOR(t->bases, u)
				identityref_help(t->bases[u]);
			return; // Because it prints whole info itself
		}

		default:
			printf("<unknown>\n");
			break;
		}
	}

	if (node->dsc) {
		char *dsc = faux_str_getline(node->dsc, NULL);
		printf("%s\n", dsc);
		faux_str_free(dsc);
	} else {
		printf("%s\n", node->name);
	}
}


static bool_t pline_find_node_within_tree(const struct lyd_node *nodes_list,
	const struct lysc_node *node)
{
	const struct lyd_node *iter = NULL;

	if (!nodes_list)
		return BOOL_FALSE;

	LY_LIST_FOR(nodes_list, iter) {
		const char *default_value = NULL;
		char *value = NULL;

		if (iter->schema != node) {
			if (pline_find_node_within_tree(lyd_child(iter),
				node))
				return BOOL_TRUE;
			continue;
		}
		if (iter->flags & LYD_DEFAULT) {
			continue;
		}
		default_value = klysc_node_ext_default(iter->schema);
		value = klyd_node_value(iter);
		// Don't show "default" keys with default values
		if (default_value && faux_str_cmp(default_value, value) == 0) {
			faux_str_free(value);
			continue;
		}
		faux_str_free(value);
		return BOOL_TRUE;
	}

	return BOOL_FALSE;
}


static bool_t pline_node_exists(sr_session_ctx_t* sess, const char *xpath,
	const struct lysc_node *node)
{
	sr_data_t *data = NULL;
	bool_t found = BOOL_FALSE;

	if (!xpath)
		return BOOL_FALSE;
	if (sr_get_data(sess, xpath, 1, 0, 0, &data) != SR_ERR_OK)
		return BOOL_FALSE;
	if (!data) // Not found
		return BOOL_FALSE;

	if (pline_find_node_within_tree(data->tree, node))
		found = BOOL_TRUE;
	sr_release_data(data);

	return found;
}


void pline_print_completions(const pline_t *pline, bool_t help,
	pt_e enabled_types, bool_t existing_nodes_only)
{
	faux_list_node_t *iter = NULL;
	pcompl_t *pcompl = NULL;
	sr_datastore_t current_ds = SRP_REPO_EDIT;

	iter = faux_list_head(pline->compls);
	while ((pcompl = (pcompl_t *)faux_list_each(&iter))) {
		struct lysc_type *type = NULL;
		const struct lysc_node *node = pcompl->node;

		if (!(pcompl->pat & enabled_types))
			continue;

		// Switch to necessary DS
		if (pcompl->xpath && (current_ds != pcompl->xpath_ds)) {
			sr_session_switch_ds(pline->sess, pcompl->xpath_ds);
			current_ds = pcompl->xpath_ds;
		}

		// Help
		if (help) {

			// Note we can't show help without valid node
			if (!node)
				continue;

			// Type (help)
			if (pcompl->type == PCOMPL_TYPE) {
				if (node->nodetype & LYS_LEAF)
					type = ((struct lysc_node_leaf *)node)->type;
				else if (node->nodetype & LYS_LEAFLIST)
					type = ((struct lysc_node_leaflist *)node)->type;
				else
					continue;
				pline_print_type_help(node, type);
				continue;
			}

			// Check node for existing if necessary
			if (existing_nodes_only &&
				!pline_node_exists(pline->sess, pcompl->xpath,
				node)) {
					continue;
			}

			// Node (help)
			if (!node->dsc) {
				printf("%s\n%s\n", node->name, node->name);
			} else {
				char *dsc = faux_str_getline(node->dsc,
					NULL);
				printf("%s\n%s\n", node->name, dsc);
				faux_str_free(dsc);
			}

		// Completion
		} else {

			// Type (completion)
			if (pcompl->type == PCOMPL_TYPE) {

				// Existing entries
				if (pcompl->xpath) {
					size_t i = 0;
					sr_val_t *vals = NULL;
					size_t val_num = 0;

					sr_get_items(pline->sess, pcompl->xpath,
						0, 0, &vals, &val_num);
					for (i = 0; i < val_num; i++) {
						char *tmp = sr_val_to_str(&vals[i]);
						char *esc_tmp = NULL;
						if (!tmp)
							continue;
						esc_tmp = faux_str_c_esc_space(tmp);
						free(tmp);
						printf("%s\n", esc_tmp);
						free(esc_tmp);
					}
					sr_free_values(vals, val_num);
				}

				if (!node)
					continue;
				if (existing_nodes_only)
					continue;

				// All entries
				if (node->nodetype & LYS_LEAF)
					type = ((struct lysc_node_leaf *)node)->type;
				else if (node->nodetype & LYS_LEAFLIST)
					type = ((struct lysc_node_leaflist *)node)->type;
				else
					continue;
				pline_print_type_completions(type);
				continue;
			}

			// Node (completion)
			if (!node)
				continue;

			// Existing entries
			if (existing_nodes_only &&
				!pline_node_exists(pline->sess, pcompl->xpath,
				node)) {
					continue;
			}

			printf("%s\n", node->name);

		} // Completion

	} // while

	// Restore default DS
	if (current_ds != SRP_REPO_EDIT)
		sr_session_switch_ds(pline->sess, SRP_REPO_EDIT);
}


void pline_opts_init(pline_opts_t *opts)
{
	opts->begin_bracket = '{';
	opts->end_bracket = '}';
	opts->show_brackets = BOOL_TRUE;
	opts->show_semicolons = BOOL_TRUE;
	opts->first_key_w_stmt = BOOL_FALSE;
	opts->keys_w_stmt = BOOL_TRUE;
	opts->colorize = BOOL_TRUE;
	opts->indent = 2;
	opts->default_keys = BOOL_FALSE;
	opts->show_default_keys = BOOL_FALSE;
	opts->hide_passwords = BOOL_TRUE;
	opts->enable_nacm = BOOL_FALSE;
	opts->oneliners = BOOL_TRUE;
}


static int pline_opts_parse_ini(const faux_ini_t *ini, pline_opts_t *opts)
{
	const char *val = NULL;

	if (!opts)
		return -1;
	if (!ini)
		return 0; // Nothing to parse

	if ((val = faux_ini_find(ini, "ShowBrackets"))) {
		if (faux_str_cmp(val, "y") == 0)
			opts->show_brackets = BOOL_TRUE;
		else if (faux_str_cmp(val, "n") == 0)
			opts->show_brackets = BOOL_FALSE;
	}

	if ((val = faux_ini_find(ini, "ShowSemicolons"))) {
		if (faux_str_cmp(val, "y") == 0)
			opts->show_semicolons = BOOL_TRUE;
		else if (faux_str_cmp(val, "n") == 0)
			opts->show_semicolons = BOOL_FALSE;
	}

	if ((val = faux_ini_find(ini, "FirstKeyWithStatement"))) {
		if (faux_str_cmp(val, "y") == 0)
			opts->first_key_w_stmt = BOOL_TRUE;
		else if (faux_str_cmp(val, "n") == 0)
			opts->first_key_w_stmt = BOOL_FALSE;
	}

	if ((val = faux_ini_find(ini, "KeysWithStatement"))) {
		if (faux_str_cmp(val, "y") == 0)
			opts->keys_w_stmt = BOOL_TRUE;
		else if (faux_str_cmp(val, "n") == 0)
			opts->keys_w_stmt = BOOL_FALSE;
	}

	if ((val = faux_ini_find(ini, "Colorize"))) {
		if (faux_str_cmp(val, "y") == 0)
			opts->colorize = BOOL_TRUE;
		else if (faux_str_cmp(val, "n") == 0)
			opts->colorize = BOOL_FALSE;
	}

	if ((val = faux_ini_find(ini, "Indent"))) {
		unsigned char indent = 0;
		if (faux_conv_atouc(val, &indent, 10))
			opts->indent = indent;
	}

	if ((val = faux_ini_find(ini, "DefaultKeys"))) {
		if (faux_str_cmp(val, "y") == 0)
			opts->default_keys = BOOL_TRUE;
		else if (faux_str_cmp(val, "n") == 0)
			opts->default_keys = BOOL_FALSE;
	}

	if ((val = faux_ini_find(ini, "ShowDefaultKeys"))) {
		if (faux_str_cmp(val, "y") == 0)
			opts->show_default_keys = BOOL_TRUE;
		else if (faux_str_cmp(val, "n") == 0)
			opts->show_default_keys = BOOL_FALSE;
	}

	if ((val = faux_ini_find(ini, "HidePasswords"))) {
		if (faux_str_cmp(val, "y") == 0)
			opts->hide_passwords = BOOL_TRUE;
		else if (faux_str_cmp(val, "n") == 0)
			opts->hide_passwords = BOOL_FALSE;
	}

	if ((val = faux_ini_find(ini, "EnableNACM"))) {
		if (faux_str_cmp(val, "y") == 0)
			opts->enable_nacm = BOOL_TRUE;
		else if (faux_str_cmp(val, "n") == 0)
			opts->enable_nacm = BOOL_FALSE;
	}

	if ((val = faux_ini_find(ini, "Oneliners"))) {
		if (faux_str_cmp(val, "y") == 0)
			opts->oneliners = BOOL_TRUE;
		else if (faux_str_cmp(val, "n") == 0)
			opts->oneliners = BOOL_FALSE;
	}

	return 0;
}


int pline_opts_parse(const char *conf, pline_opts_t *opts)
{
	faux_ini_t *ini = NULL;
	int rc = -1;

	if (!opts)
		return -1;
	if (!conf)
		return 0; // Use defaults

	ini = faux_ini_new();
	if (!faux_ini_parse_str(ini, conf)) {
		faux_ini_free(ini);
		return -1;
	}
	rc = pline_opts_parse_ini(ini, opts);
	faux_ini_free(ini);

	return rc;
}


int pline_opts_parse_file(const char *conf_name, pline_opts_t *opts)
{
	faux_ini_t *ini = NULL;
	int rc = -1;

	if (!opts)
		return -1;
	if (!conf_name)
		return 0; // Use defaults

	ini = faux_ini_new();
	if (!faux_ini_parse_file(ini, conf_name)) {
		faux_ini_free(ini);
		return -1;
	}
	rc = pline_opts_parse_ini(ini, opts);
	faux_ini_free(ini);

	return rc;
}
