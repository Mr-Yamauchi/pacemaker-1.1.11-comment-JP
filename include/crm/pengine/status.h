/*
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#ifndef PENGINE_STATUS__H
#  define PENGINE_STATUS__H

#  include <glib.h>
#  include <crm/common/iso8601.h>
#  include <crm/pengine/common.h>

typedef struct node_s pe_node_t;
typedef struct node_s node_t;
typedef struct pe_action_s action_t;
typedef struct pe_action_s pe_action_t;
typedef struct resource_s resource_t;
typedef struct ticket_s ticket_t;

typedef enum no_quorum_policy_e {
    no_quorum_freeze,
    no_quorum_stop,
    no_quorum_ignore,
    no_quorum_suicide
} no_quorum_policy_t;

enum node_type {
    node_ping,
    node_member,
    node_remote
};

enum pe_restart {
    pe_restart_restart,
    pe_restart_ignore
};

enum pe_find {
    pe_find_renamed = 0x001,
    pe_find_clone = 0x004,
    pe_find_current = 0x008,
    pe_find_inactive = 0x010,
};

#  define pe_flag_have_quorum		0x00000001ULL
#  define pe_flag_symmetric_cluster	0x00000002ULL
#  define pe_flag_is_managed_default	0x00000004ULL
#  define pe_flag_maintenance_mode	0x00000008ULL

#  define pe_flag_stonith_enabled	0x00000010ULL
#  define pe_flag_have_stonith_resource	0x00000020ULL

#  define pe_flag_stop_rsc_orphans	0x00000100ULL
#  define pe_flag_stop_action_orphans	0x00000200ULL
#  define pe_flag_stop_everything	0x00000400ULL

#  define pe_flag_start_failure_fatal	0x00001000ULL
#  define pe_flag_remove_after_stop	0x00002000ULL

#  define pe_flag_startup_probes	0x00010000ULL
#  define pe_flag_have_status		0x00020000ULL
#  define pe_flag_have_remote_nodes	0x00040000ULL

#  define pe_flag_quick_location  	0x00100000ULL

typedef struct pe_working_set_s {
    xmlNode *input;						/* 処理対象の生のCIB(XML)情報 */
    crm_time_t *now;

    /* options extracted from the input */
    char *dc_uuid;
    node_t *dc_node;
    const char *stonith_action;
    const char *placement_strategy;

    unsigned long long flags;

    int stonith_timeout;
    int default_resource_stickiness;
    no_quorum_policy_t no_quorum_policy;

    GHashTable *config_hash;
    GHashTable *domains;
    GHashTable *tickets;

    GListPtr nodes;						/* ノード情報のリスト */
    GListPtr resources;					/* リソース情報のリスト */
    GListPtr placement_constraints;		/* location制約情報のリスト */
    GListPtr ordering_constraints;		/* order制約情報のリスト */
    GListPtr colocation_constraints;	/* colocation制約情報のリスト */
    GListPtr ticket_constraints;

    GListPtr actions;					/* peningeの処理で生成されたすべて(psedo,resourceなど)の実行するべきアクション情報のリスト */
    xmlNode *failed;					/* 故障情報のXML */
    xmlNode *op_defaults;				/* CIBのop_defaultのXML */
    xmlNode *rsc_defaults;				/* CIBのrsc_defaultのXML */

    /* stats */
    int num_synapse;
    int max_valid_nodes;
    int order_id;
    int action_id;

    /* final output */
    xmlNode *graph;						/* 生成するグラフ情報 */

    GHashTable *template_rsc_sets;
    const char *localhost;

} pe_working_set_t;

struct node_shared_s {
    const char *id;
    const char *uname;
    gboolean online;
    gboolean standby;
    gboolean standby_onfail;
    gboolean pending;
    gboolean unclean;
    gboolean unseen;
    gboolean shutdown;
    gboolean expected_up;
    gboolean is_dc;
    int num_resources;
    GListPtr running_rsc;       /* resource_t* */	/* ノードで実行しているリソース情報のリスト */
    GListPtr allocated_rsc;     /* resource_t* */

    resource_t *remote_rsc;

    GHashTable *attrs;          /* char* => char* */
    enum node_type type;

    GHashTable *utilization;

    /*! cache of calculated rsc digests for this node. */
    GHashTable *digest_cache;

    gboolean maintenance;
};

struct node_s {
    int weight;
    gboolean fixed;
    int count;
    struct node_shared_s *details;
};

#  include <crm/pengine/complex.h>

#  define pe_rsc_orphan		0x00000001ULL
#  define pe_rsc_managed	0x00000002ULL
#  define pe_rsc_block          0x00000004ULL   /* Further operations are prohibited due to failure policy */
#  define pe_rsc_orphan_container_filler	0x00000008ULL

#  define pe_rsc_notify		0x00000010ULL
#  define pe_rsc_unique		0x00000020ULL

#  define pe_rsc_provisional	0x00000100ULL
#  define pe_rsc_allocating	0x00000200ULL
#  define pe_rsc_merging	0x00000400ULL
#  define pe_rsc_munging	0x00000800ULL

#  define pe_rsc_try_reload     0x00001000ULL
#  define pe_rsc_reload         0x00002000ULL

#  define pe_rsc_failed		0x00010000ULL
#  define pe_rsc_shutdown	0x00020000ULL
#  define pe_rsc_runnable	0x00040000ULL
#  define pe_rsc_start_pending	0x00080000ULL

#  define pe_rsc_starting	0x00100000ULL
#  define pe_rsc_stopping	0x00200000ULL
#  define pe_rsc_migrating	0x00400000ULL

#  define pe_rsc_failure_ignored 0x01000000ULL
#  define pe_rsc_unexpectedly_running 0x02000000ULL

#  define pe_rsc_needs_quorum	 0x10000000ULL
#  define pe_rsc_needs_fencing	 0x20000000ULL
#  define pe_rsc_needs_unfencing 0x40000000ULL

enum pe_graph_flags {
    pe_graph_none = 0x00000,
    pe_graph_updated_first = 0x00001,
    pe_graph_updated_then = 0x00002,
    pe_graph_disable = 0x00004,
};

/* *INDENT-OFF* */
enum pe_action_flags {
    pe_action_pseudo = 0x00001,								/* PSEUDOなアクション(リソースを伴わない遷移アクション) */
    pe_action_runnable = 0x00002,							/* 実行可能なアクション */
    pe_action_optional = 0x00004,							/* オプション実行なアクション */
    pe_action_print_always = 0x00008,

    pe_action_have_node_attrs = 0x00010,
    pe_action_failure_is_fatal = 0x00020,
    pe_action_implied_by_stonith = 0x00040,

    pe_action_dumped = 0x00100,
    pe_action_processed = 0x00200,
    pe_action_clear = 0x00400,
    pe_action_dangle = 0x00800,

    pe_action_requires_any = 0x01000, /* This action requires one or mre of its dependancies to be runnable
                                       * We use this to clear the runnable flag before checking dependancies
                                       */
};
/* *INDENT-ON* */

struct resource_s {											/* 単一リソースの情報 */
    char *id;
    char *clone_name;
    xmlNode *xml;
    xmlNode *orig_xml;
    xmlNode *ops_xml;

    resource_t *parent;										/* 親リソース情報へのポインタ */
    void *variant_opaque;
    enum pe_obj_types variant;
    resource_object_functions_t *fns;						/* リソース種別毎の処理へのポインタ(resource_class_functions[])*/
    resource_alloc_functions_t *cmds;						/* リソース種別毎のコマンド処理へのポインタ(resource_class_alloc_functions[]) */

    enum rsc_recovery_type recovery_type;					/* リカバリタイプ */
    														/* デフォルト:recovery_stop_start */
    														/* リソースのmultiple-activeメタ情報で指定可能 */
    														/*           stop_only,block */
    enum pe_restart restart_type;							/* リスタートタイプ */
    														/* デフォルト:recovery_stop_start */
    														/* リソースのrestart-typeメタ情報で指定可能 */
    														/*			 restart,ignore */

    int priority;
    int stickiness;											/* リソースの重み */
    														/* デフォルトは、rsc_defaultsのresource-stickiness */
    														/* リソースのresource-stickinessメタ情報で個別セット可能 */
    														
    int sort_index;
    int failure_timeout;
    int effective_priority;
    int migration_threshold;								/* 故障閾値(故障数管理) */

    gboolean is_remote_node;

    unsigned long long flags;

    GListPtr rsc_cons_lhs;      /* rsc_colocation_t* */		/* このリソースがwith-rsc指定されているcolocation情報のリスト */
    GListPtr rsc_cons;          /* rsc_colocation_t* */		/* このリソースがwith-rsc指定しているcolocation情報のリスト */
    GListPtr rsc_location;      /* rsc_to_node_t*    */		/* このリソースのlocation情報のリスト */
    GListPtr actions;           /* action_t*         */		/* peningeの処理で生成されたリソース単位の実行するべきアクション情報のリスト */
    GListPtr rsc_tickets;       /* rsc_ticket*       */

    node_t *allocated_to;									/* 決定されたリソースの配置先ノード情報 */
    GListPtr running_on;        /* node_t*   */				/* リソースを実行中のノード情報のリスト */
    GHashTable *known_on;       /* node_t*   */
    GHashTable *allowed_nodes;  /* node_t*   */				/* リソースの配置候補ノード情報のリスト */

    enum rsc_role_e role;									/* 現在のロール */
    enum rsc_role_e next_role;								/* 次の遷移先のロール */

    GHashTable *meta;
    GHashTable *parameters;
    GHashTable *utilization;

    GListPtr children;          /* resource_t*   */			/* リソースの子リソース情報のリスト */
    GListPtr dangling_migrations;       /* node_t*       */

    node_t *partial_migration_target;
    node_t *partial_migration_source;

    resource_t *container;
    GListPtr fillers;
};

struct pe_action_s {										/* アクション情報 */
    int id;
    int priority;

    resource_t *rsc;										/* アクションの対象リソース情報へのポインタ */
    node_t *node;
    xmlNode *op_entry;										/* リソースの対象アクションのoperation-op-XML情報 */

    char *task;
    char *uuid;

    enum pe_action_flags flags;
    enum rsc_start_requirement needs;
    enum action_fail_response on_fail;
    enum rsc_role_e fail_role;

    action_t *pre_notify;
    action_t *pre_notified;
    action_t *post_notify;
    action_t *post_notified;

    int seen_count;

    GHashTable *meta;
    GHashTable *extra;

    GListPtr actions_before;    /* action_warpper_t* */		/* このアクションの前に実行するアクション情報のリスト */
    GListPtr actions_after;     /* action_warpper_t* */		/* このアクションの後に実行するアクション情報のリスト */
};

struct ticket_s {
    char *id;
    gboolean granted;
    time_t last_granted;
    gboolean standby;
    GHashTable *state;
};

enum pe_link_state {
    pe_link_not_dumped,
    pe_link_dumped,
    pe_link_dup,
};

/* *INDENT-OFF* */
enum pe_ordering {
    pe_order_none                  = 0x0,        /* deleted */
    pe_order_optional              = 0x1,    /* pure ordering, nothing implied */
    /* ----firstリソースのアクションを実行可能にする */
    /* 基本的に、orderのfirstリソースのfirst-actionがoptionalな場合には、optionalを解除して実行可能になる */
    pe_order_implies_first         = 0x10,      /* If 'first' is required, ensure 'then' is too */
    /* ----thenリソースのアクションを実行可能にする */
    /* firstリソースのactionがoptinalでない場合で(実行される)、thenリソースのアクションのoptionalの場合*/
    /* thenリソースのアクションのoptionalフラグをクリアして実行できるようにする */
    pe_order_implies_then          = 0x20,       /* If 'then' is required, ensure 'first' is too */
    pe_order_implies_first_master  = 0x40,      /* Imply 'first' is required when 'then' is required and then's rsc holds Master role. */
	/* ----thenリソースはfirstリソースがpe_action_runnableでなければ実行不可能になる */
	/* 基本的に、thenリソースのアクションがpe_action_runnableで、firstリソースがpe_action_runnableでない場合 */
    /* thenリソースのアクションのrunnableフラグをクリアして実行できないようにする */
    pe_order_runnable_left         = 0x100,     /* 'then' requires 'first' to be runnable */

    pe_order_restart               = 0x1000,    /* 'then' is runnable if 'first' is optional or runnable */
    pe_order_stonith_stop          = 0x2000,     /* only applies if the action is non-pseudo */
    pe_order_serialize_only        = 0x4000,   /* serialize */

    pe_order_implies_first_printed = 0x10000,   /* Like ..implies_first but only ensures 'first' is printed, not manditory */
    pe_order_implies_then_printed  = 0x20000,    /* Like ..implies_then but only ensures 'then' is printed, not manditory */

    pe_order_asymmetrical          = 0x100000,    /* Indicates asymmetrical one way ordering constraint. */
    pe_order_load                  = 0x200000,    /* Only relevant if... */
    pe_order_one_or_more           = 0x400000,    /* 'then' is only runnable if one or more of it's dependancies are too */

    pe_order_trace                 = 0x4000000  /* test marker */
};
/* *INDENT-ON* */

typedef struct action_wrapper_s action_wrapper_t;
struct action_wrapper_s {
    enum pe_ordering type;
    enum pe_link_state state;
    action_t *action;
};

const char *rsc_printable_id(resource_t *rsc);
gboolean cluster_status(pe_working_set_t * data_set);
void set_working_set_defaults(pe_working_set_t * data_set);
void cleanup_calculations(pe_working_set_t * data_set);
resource_t *pe_find_resource(GListPtr rsc_list, const char *id_rh);
node_t *pe_find_node(GListPtr node_list, const char *uname);
node_t *pe_find_node_id(GListPtr node_list, const char *id);
node_t *pe_find_node_any(GListPtr node_list, const char *id, const char *uname);
GListPtr find_operations(const char *rsc, const char *node, gboolean active_filter,
                         pe_working_set_t * data_set);
#endif
