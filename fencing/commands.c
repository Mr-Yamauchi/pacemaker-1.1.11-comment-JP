/*
 * Copyright (C) 2009 Andrew Beekhof <andrew@beekhof.net>
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

#include <crm_internal.h>

#include <sys/param.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/utsname.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/ipc.h>
#include <crm/common/ipcs.h>
#include <crm/cluster/internal.h>
#include <crm/common/mainloop.h>

#include <crm/stonith-ng.h>
#include <crm/fencing/internal.h>
#include <crm/common/xml.h>

#if SUPPORT_CIBSECRETS
#  include <crm/common/cib_secrets.h>
#endif

#include <internal.h>

GHashTable *device_list = NULL;
GHashTable *topology = NULL;
GList *cmd_list = NULL;

static int active_children = 0;

struct device_search_s {
    char *host;
    char *action;
    int per_device_timeout;
    int replies_needed;
    int replies_received;

    void *user_data;
    void (*callback) (GList * devices, void *user_data);
    GListPtr capable;
};

static gboolean stonith_device_dispatch(gpointer user_data);
static void st_child_done(GPid pid, int rc, const char *output, gpointer user_data);
static void stonith_send_reply(xmlNode * reply, int call_options, const char *remote_peer,
                               const char *client_id);

static void search_devices_record_result(struct device_search_s *search, const char *device,
                                         gboolean can_fence);

typedef struct async_command_s {

    int id;
    int pid;
    int fd_stdout;
    int options;
    int default_timeout;
    int timeout;

    char *op;
    char *origin;						/* STONITH依頼元ノード名 */
    char *client;
    char *client_name;
    char *remote_op_id;

    char *victim;						/* STONITH対象ノード名 */
    uint32_t victim_nodeid;				/* STONITH対象ノードID */
    char *action;
    char *device;
    char *mode;

    GListPtr device_list;
    GListPtr device_next;

    void *internal_user_data;
    void (*done_cb) (GPid pid, int rc, const char *output, gpointer user_data);
    guint timer_sigterm;
    guint timer_sigkill;
    /*! If the operation timed out, this is the last signal
     *  we sent to the process to get it to terminate */
    int last_timeout_signo;
} async_command_t;

static xmlNode *stonith_construct_async_reply(async_command_t * cmd, const char *output,
                                              xmlNode * data, int rc);

static int
get_action_timeout(stonith_device_t * device, const char *action, int default_timeout)
{
    char buffer[512] = { 0, };
    char *value = NULL;

    CRM_CHECK(action != NULL, return default_timeout);

    if (!device->params) {
        return default_timeout;
    }

    snprintf(buffer, sizeof(buffer) - 1, "pcmk_%s_timeout", action);
    value = g_hash_table_lookup(device->params, buffer);

    if (!value) {
        return default_timeout;
    }

    return atoi(value);
}
/* 非同期コマンドエリアの解放 */
static void
free_async_command(async_command_t * cmd)
{
    if (!cmd) {
        return;
    }
    cmd_list = g_list_remove(cmd_list, cmd);

    g_list_free_full(cmd->device_list, free);
    free(cmd->device);
    free(cmd->action);
    free(cmd->victim);
    free(cmd->remote_op_id);
    free(cmd->client);
    free(cmd->client_name);
    free(cmd->origin);
    free(cmd->mode);
    free(cmd->op);
    free(cmd);
}
/* STONITH 実行非同期コマンドの作成 */
static async_command_t *
create_async_command(xmlNode * msg)
{
    async_command_t *cmd = NULL;
    xmlNode *op = get_xpath_object("//@" F_STONITH_ACTION, msg, LOG_ERR);
    const char *action = crm_element_value(op, F_STONITH_ACTION);

    CRM_CHECK(action != NULL, crm_log_xml_warn(msg, "NoAction"); return NULL);

    crm_log_xml_trace(msg, "Command");
    cmd = calloc(1, sizeof(async_command_t));
    crm_element_value_int(msg, F_STONITH_CALLID, &(cmd->id));
    crm_element_value_int(msg, F_STONITH_CALLOPTS, &(cmd->options));
    crm_element_value_int(msg, F_STONITH_TIMEOUT, &(cmd->default_timeout));
    cmd->timeout = cmd->default_timeout;
	/* 依頼元をセットする */
    cmd->origin = crm_element_value_copy(msg, F_ORIG);
    cmd->remote_op_id = crm_element_value_copy(msg, F_STONITH_REMOTE_OP_ID);
    cmd->client = crm_element_value_copy(msg, F_STONITH_CLIENTID);
    cmd->client_name = crm_element_value_copy(msg, F_STONITH_CLIENTNAME);
    cmd->op = crm_element_value_copy(msg, F_STONITH_OPERATION);
    cmd->action = strdup(action);
    cmd->victim = crm_element_value_copy(op, F_STONITH_TARGET);	/* targetからSTONITH対象をセット */
    cmd->mode = crm_element_value_copy(op, F_STONITH_MODE);
    cmd->device = crm_element_value_copy(op, F_STONITH_DEVICE);

    CRM_CHECK(cmd->op != NULL, crm_log_xml_warn(msg, "NoOp"); free_async_command(cmd); return NULL);
    CRM_CHECK(cmd->client != NULL, crm_log_xml_warn(msg, "NoClient"));

    cmd->done_cb = st_child_done;				/* stonith実行子プロセスの完了コールバック */
    cmd_list = g_list_append(cmd_list, cmd);	/* 非同期コマンドをリストに保存 */
    return cmd;
}
/* STONITHトリガー処理 */
static gboolean
stonith_device_execute(stonith_device_t * device)
{
    int exec_rc = 0;
    async_command_t *cmd = NULL;
    stonith_action_t *action = NULL;

    CRM_CHECK(device != NULL, return FALSE);

    if (device->active_pid) {
		/* 実行中のデバイスは処理しないで、完了待ち */
        crm_trace("%s is still active with pid %u", device->id, device->active_pid);
        return TRUE;
    }

    if (device->pending_ops) {
		/* 実行リストに操作がある場合 */
        GList *first = device->pending_ops;
		/* 先頭操作を取り出す */
        device->pending_ops = g_list_remove_link(device->pending_ops, first);
        cmd = first->data;
        g_list_free_1(first);
    }

    if (cmd == NULL) {
        crm_trace("Nothing further to do for %s", device->id);
        return TRUE;
    }

#if SUPPORT_CIBSECRETS
    if (replace_secret_params(device->id, device->params) < 0) {
        /* replacing secrets failed! */
        if (safe_str_eq(cmd->action,"stop")) {
            /* don't fail on stop! */
            crm_info("proceeding with the stop operation for %s", device->id);

        } else {
            crm_err("failed to get secrets for %s, "
                    "considering resource not configured", device->id);
            exec_rc = PCMK_OCF_NOT_CONFIGURED;
            cmd->done_cb(0, exec_rc, NULL, cmd);
            return TRUE;
        }
    }
#endif
	/* STONITHのアクションを生成する */
    action = stonith_action_create(device->agent,
                                   cmd->action,
                                   cmd->victim,
                                   cmd->victim_nodeid,
                                   cmd->timeout, device->params, device->aliases);

    /* for async exec, exec_rc is pid if positive and error code if negative/zero */
    /* アクションを非同期実行する */
    exec_rc = stonith_action_execute_async(action, (void *)cmd, cmd->done_cb);

    if (exec_rc > 0) {
        crm_debug("Operation %s%s%s on %s now running with pid=%d, timeout=%ds",
                  cmd->action, cmd->victim ? " for node " : "", cmd->victim ? cmd->victim : "",
                  device->id, exec_rc, cmd->timeout);
        device->active_pid = exec_rc;

    } else {
        crm_warn("Operation %s%s%s on %s failed: %s (%d)",
                 cmd->action, cmd->victim ? " for node " : "", cmd->victim ? cmd->victim : "",
                 device->id, pcmk_strerror(exec_rc), exec_rc);
        cmd->done_cb(0, exec_rc, NULL, cmd);				/* コールバックを実行する */
    }
    return TRUE;
}
/* トリガー処理ラッパー */
static gboolean
stonith_device_dispatch(gpointer user_data)
{
    return stonith_device_execute(user_data);
}
/* 実行リストへの追加とトリガーの実行 */
static void
schedule_stonith_command(async_command_t * cmd, stonith_device_t * device)
{
    CRM_CHECK(cmd != NULL, return);
    CRM_CHECK(device != NULL, return);

    if (cmd->device) {
        free(cmd->device);
    }

    if (device->include_nodeid && cmd->victim) {
		/* nodeid判定が必要な場合で、依頼先が設定されている場合は、依頼先の接続node情報を取得する */
        crm_node_t *node = crm_get_peer(0, cmd->victim);
		/* 取得したnode情報を依頼先のnodeidにセットする */
        cmd->victim_nodeid = node->id;
    }

    cmd->device = strdup(device->id);
    cmd->timeout = get_action_timeout(device, cmd->action, cmd->default_timeout);

    if (cmd->remote_op_id) {
        crm_debug("Scheduling %s on %s for remote peer %s with op id (%s) (timeout=%ds)",
                  cmd->action, device->id, cmd->origin, cmd->remote_op_id, cmd->timeout);
    } else {
        crm_debug("Scheduling %s on %s for %s (timeout=%ds)",
                  cmd->action, device->id, cmd->client, cmd->timeout);
    }
	/* 実行リストに追加する */
    device->pending_ops = g_list_append(device->pending_ops, cmd);
    /* トリガーをたたく */
    mainloop_set_trigger(device->work);
}
/* デバイス情報を解放する */
void
free_device(gpointer data)
{
    GListPtr gIter = NULL;
    stonith_device_t *device = data;

    g_hash_table_destroy(device->params);
    g_hash_table_destroy(device->aliases);

    for (gIter = device->pending_ops; gIter != NULL; gIter = gIter->next) {
        async_command_t *cmd = gIter->data;

        crm_warn("Removal of device '%s' purged operation %s", device->id, cmd->action);
        cmd->done_cb(0, -ENODEV, NULL, cmd);
        /* 非同期コマンドエリアの解放 */
        free_async_command(cmd);
    }
    g_list_free(device->pending_ops);

    g_list_free_full(device->targets, free);

    mainloop_destroy_trigger(device->work);

    free_xml(device->agent_metadata);
    free(device->namespace);
    free(device->on_target_actions);
    free(device->agent);
    free(device->id);
    free(device);
}
/* ホストの別名情報をハッシュテーブルに構築する */
static GHashTable *
build_port_aliases(const char *hostmap, GListPtr * targets)
{
    char *name = NULL;
    int last = 0, lpc = 0, max = 0, added = 0;
    /* 別名構築用のハッシュテーブルを生成する */
    GHashTable *aliases =
        g_hash_table_new_full(crm_str_hash, g_str_equal, g_hash_destroy_str, g_hash_destroy_str);

    if (hostmap == NULL) {
		/* パラメータに"pcmk_host_map"が無い場合は、空のハッシュテーブルを戻す */
        return aliases;
    }
	/* パラメータの"pcmk_host_map"の長さを取得する */
    max = strlen(hostmap);
    for (; lpc <= max; lpc++) {
        switch (hostmap[lpc]) {
                /* Assignment chars */
            case '=':
            case ':':
                if (lpc > last) {
            	    /* =,:区切りで別名を作成する */
                    free(name);
                    name = calloc(1, 1 + lpc - last);
                    memcpy(name, hostmap + last, lpc - last);
                }
                last = lpc + 1;
                break;

                /* Delimeter chars */
                /* case ',': Potentially used to specify multiple ports */
            case 0:
            case ';':
            case ' ':
            case '\t':
                if (name) {
					/* ;,blank,tabで生成した別名を登録する */
                    char *value = NULL;

                    value = calloc(1, 1 + lpc - last);
                    memcpy(value, hostmap + last, lpc - last);
					/* 別名構築用のハッシュテーブルに別名を登録する */
                    crm_debug("Adding alias '%s'='%s'", name, value);
                    g_hash_table_replace(aliases, name, value);
                    if (targets) {
                        *targets = g_list_append(*targets, strdup(value));
                    }
                    value = NULL;
                    name = NULL;
                    added++;

                } else if (lpc > last) {
                    crm_debug("Parse error at offset %d near '%s'", lpc - last, hostmap + last);
                }

                last = lpc + 1;
                break;
        }

        if (hostmap[lpc] == 0) {
            break;
        }
    }

    if (added == 0) {
        crm_info("No host mappings detected in '%s'", hostmap);	/* 別名が０件の場合のinfoメッセージ */
    }
	/* 構築した別名構築用のハッシュテーブルを返す */
    free(name);
    return aliases;
}

static void
parse_host_line(const char *line, int max, GListPtr * output)
{
    int lpc = 0;
    int last = 0;

    if (max <= 0) {
        return;
    }

    /* Check for any complaints about additional parameters that the device doesn't understand */
    if (strstr(line, "invalid") || strstr(line, "variable")) {
        crm_debug("Skipping: %s", line);
        return;
    }

    crm_trace("Processing %d bytes: [%s]", max, line);
    /* Skip initial whitespace */
    for (lpc = 0; lpc <= max && isspace(line[lpc]); lpc++) {
        last = lpc + 1;
    }

    /* Now the actual content */
    for (lpc = 0; lpc <= max; lpc++) {
        gboolean a_space = isspace(line[lpc]);

        if (a_space && lpc < max && isspace(line[lpc + 1])) {
            /* fast-forward to the end of the spaces */

        } else if (a_space || line[lpc] == ',' || line[lpc] == 0) {
            int rc = 1;
            char *entry = NULL;

            if (lpc != last) {
                entry = calloc(1, 1 + lpc - last);
                rc = sscanf(line + last, "%[a-zA-Z0-9_-.]", entry);
            }

            if (entry == NULL) {
                /* Skip */
            } else if (rc != 1) {
                crm_warn("Could not parse (%d %d): %s", last, lpc, line + last);
            } else if (safe_str_neq(entry, "on") && safe_str_neq(entry, "off")) {
                crm_trace("Adding '%s'", entry);
                *output = g_list_append(*output, entry);
                entry = NULL;
            }

            free(entry);
            last = lpc + 1;
        }
    }
}
/* hostlistをパースする */
static GListPtr
parse_host_list(const char *hosts)
{
    int lpc = 0;
    int max = 0;
    int last = 0;
    GListPtr output = NULL;

    if (hosts == NULL) {
        return output;
    }

    max = strlen(hosts);
    for (lpc = 0; lpc <= max; lpc++) {
        if (hosts[lpc] == '\n' || hosts[lpc] == 0) {
            char *line = NULL;
            int len = lpc - last;

            if(len > 1) {
                line = malloc(1 + len);
            }

            if(line) {
                snprintf(line, 1 + len, "%s", hosts + last);
                line[len] = 0; /* Because it might be '\n' */
                parse_host_line(line, len, &output);
                free(line);
            }

            last = lpc + 1;
        }
    }

    crm_trace("Parsed %d entries from '%s'", g_list_length(output), hosts);
    return output;
}
/* agentのmetadataを取得する */
static xmlNode *
get_agent_metadata(const char *agent)
{
    stonith_t *st = stonith_api_new();
    xmlNode *xml = NULL;
    char *buffer = NULL;
    int rc = 0;

    rc = st->cmds->metadata(st, st_opt_sync_call, agent, NULL, &buffer, 10);
    if (rc || !buffer) {
        crm_err("Could not retrieve metadata for fencing agent %s", agent);
        return NULL;
    }
    xml = string2xml(buffer);
    free(buffer);
    stonith_api_delete(st);

    return xml;
}

static gboolean
is_nodeid_required(xmlNode * xml)
{
    xmlXPathObjectPtr xpath = NULL;

    if (stand_alone) {
        return FALSE;
    }

    if (!xml) {
        return FALSE;
    }

    xpath = xpath_search(xml, "//parameter[@name='nodeid']");
    if (numXpathResults(xpath)  <= 0) {
        freeXpathObject(xpath);
        return FALSE;
    }

    freeXpathObject(xpath);
    return TRUE;
}
/* 取得したagentのmetadataのxml情報からtagetの実行可能なアクション情報を取得する */
static char *
get_on_target_actions(xmlNode * xml)
{
    char *actions = NULL;
    xmlXPathObjectPtr xpath = NULL;
    int max = 0;
    int lpc = 0;

    if (!xml) {
        return NULL;
    }

    xpath = xpath_search(xml, "//action");
    max = numXpathResults(xpath);

    if (max <= 0) {
        freeXpathObject(xpath);
        return NULL;
    }

    actions = calloc(1, 512);

    for (lpc = 0; lpc < max; lpc++) {
        const char *on_target = NULL;
        const char *action = NULL;
        xmlNode *match = getXpathResult(xpath, lpc);

        CRM_CHECK(match != NULL, continue);

        on_target = crm_element_value(match, "on_target");
        action = crm_element_value(match, "name");

        if (action && crm_is_true(on_target)) {
            if (strlen(actions)) {
                g_strlcat(actions, " ", 512);
            }
            g_strlcat(actions, action, 512);
        }
    }

    freeXpathObject(xpath);

    if (!strlen(actions)) {
        free(actions);
        actions = NULL;
    }

    return actions;
}
/* STONITHデバイスの登録メッセージを元にして、STONITHデバイスの登録データを生成する */
static stonith_device_t *
build_device_from_xml(xmlNode * msg)
{
    const char *value = NULL;
    xmlNode *dev = get_xpath_object("//" F_STONITH_DEVICE, msg, LOG_ERR);
    stonith_device_t *device = NULL;

    device = calloc(1, sizeof(stonith_device_t));
    device->id = crm_element_value_copy(dev, XML_ATTR_ID);
    device->agent = crm_element_value_copy(dev, "agent");
    device->namespace = crm_element_value_copy(dev, "namespace");
    device->params = xml2list(dev);
	/* pcmk_host_listがデバイスリストの展開パラメータにある場合 */
    value = g_hash_table_lookup(device->params, STONITH_ATTR_HOSTLIST);
    if (value) {
		/* デバイスのtargetsにpcmk_host_listをパースしてセットする */
        device->targets = parse_host_list(value);
    }
	/* パラメータから"pcmk_host_map"を取得する */
    value = g_hash_table_lookup(device->params, STONITH_ATTR_HOSTMAP);
    /* パラメータの"pcmk_host_map"から、ホストの別名情報をハッシュテーブルに構築する */
    device->aliases = build_port_aliases(value, &(device->targets));
	/* agentのmetadataを取得し、xml情報にセットする */
    device->agent_metadata = get_agent_metadata(device->agent);
	/* 取得したagentのmetadataのxml情報からtagetの実行可能なアクション情報を取得する */
    device->on_target_actions = get_on_target_actions(device->agent_metadata);
	/* パラメータからnodeidを取得する */
    value = g_hash_table_lookup(device->params, "nodeid");
    if (!value) {
		/* パラメータにnoideidがある場合は、nodeidが必要か判定する */
        device->include_nodeid = is_nodeid_required(device->agent_metadata);
    }

    if (device->on_target_actions) {
        crm_info("The fencing device '%s' requires actions (%s) to be executed on the target node",
                 device->id, device->on_target_actions);
    }
	/* 実行トリガーのセット */
    device->work = mainloop_add_trigger(G_PRIORITY_HIGH, stonith_device_dispatch, device);
    /* TODO: Hook up priority */

    return device;
}
/* デバイスのパラメータ情報からタイプを取得する */
static const char *
target_list_type(stonith_device_t * dev)
{
    const char *check_type = NULL;
	/* デバイスのパラメータから"pcmk_host_check"を取り出す */
    check_type = g_hash_table_lookup(dev->params, STONITH_ATTR_HOSTCHECK);

    if (check_type == NULL) {
		/* pcmk_host_checkがない場合 */
        if (g_hash_table_lookup(dev->params, STONITH_ATTR_HOSTLIST)) {
			/* デバイスのパラメータに"pcmk_host_list"がある場合は、static-list */
            check_type = "static-list";
        } else if (g_hash_table_lookup(dev->params, STONITH_ATTR_HOSTMAP)) {
			/* デバイスのパラメータに"pcmk_host_list"がある場合も、static-list */
            check_type = "static-list";
        } else {
			/* その他の場合(external/sshなど)は、dynamic-list */
            check_type = "dynamic-list";
        }
    }

    return check_type;
}

void
schedule_internal_command(const char *origin,
                          stonith_device_t * device,
                          const char *action,
                          const char *victim,
                          int timeout,
                          void *internal_user_data,
                          void (*done_cb) (GPid pid, int rc, const char *output,
                                           gpointer user_data))
{
    async_command_t *cmd = NULL;

    cmd = calloc(1, sizeof(async_command_t));

    cmd->id = -1;
    cmd->default_timeout = timeout ? timeout : 60;
    cmd->timeout = cmd->default_timeout;
    cmd->action = strdup(action);
    cmd->victim = victim ? strdup(victim) : NULL;	/* STONITH対象をセット */
    cmd->device = strdup(device->id);
    cmd->origin = strdup(origin);					/* 依頼元をセットする */
    cmd->client = strdup(crm_system_name);
    cmd->client_name = strdup(crm_system_name);

    cmd->internal_user_data = internal_user_data;
    cmd->done_cb = done_cb; /* cmd, not internal_user_data, is passed to 'done_cb' as the userdata */

    schedule_stonith_command(cmd, device);
}

gboolean
string_in_list(GListPtr list, const char *item)
{
    int lpc = 0;
    int max = g_list_length(list);

    for (lpc = 0; lpc < max; lpc++) {
        const char *value = g_list_nth_data(list, lpc);

        if (safe_str_eq(item, value)) {
            return TRUE;
        } else {
            crm_trace("%d: '%s' != '%s'", lpc, item, value);
        }
    }
    return FALSE;
}

static void
status_search_cb(GPid pid, int rc, const char *output, gpointer user_data)
{
    async_command_t *cmd = user_data;
    struct device_search_s *search = cmd->internal_user_data;
    stonith_device_t *dev = cmd->device ? g_hash_table_lookup(device_list, cmd->device) : NULL;
    gboolean can = FALSE;
	/* 非同期コマンドエリアの解放 */
    free_async_command(cmd);

    if (!dev) {
		/* デバイスの検索結果を保存する */
        search_devices_record_result(search, NULL, FALSE);
        return;
    }

    dev->active_pid = 0;
    mainloop_set_trigger(dev->work);

    if (rc == 1 /* unkown */ ) {
        crm_trace("Host %s is not known by %s", search->host, dev->id);

    } else if (rc == 0 /* active */  || rc == 2 /* inactive */ ) {
        can = TRUE;

    } else {
        crm_notice("Unkown result when testing if %s can fence %s: rc=%d", dev->id, search->host,
                   rc);
    }
    /* デバイスの検索結果を保存する */
    search_devices_record_result(search, dev->id, can);
}
/* "dynamic-list"タイプのlistコマンド実行完了コールバック */
static void
dynamic_list_search_cb(GPid pid, int rc, const char *output, gpointer user_data)
{
    async_command_t *cmd = user_data;
    struct device_search_s *search = cmd->internal_user_data;
    stonith_device_t *dev = cmd->device ? g_hash_table_lookup(device_list, cmd->device) : NULL;
    gboolean can_fence = FALSE;
	/* 非同期コマンドエリアの解放 */
    free_async_command(cmd);

    /* Host/alias must be in the list output to be eligable to be fenced
     *
     * Will cause problems if down'd nodes aren't listed or (for virtual nodes)
     *  if the guest is still listed despite being moved to another machine
     */
    if (!dev) {
		/* デバイスが存在しない場合の検索結果（ＮＵＬＬ）を保存する */
        search_devices_record_result(search, NULL, FALSE);
        return;
    }

    dev->active_pid = 0;
    /* device用の残処理を実行させる為に、トリガーをたたく */
    mainloop_set_trigger(dev->work);

    /* If we successfully got the targets earlier, don't disable. */
    if (rc != 0 && !dev->targets) {
        crm_notice("Disabling port list queries for %s (%d): %s", dev->id, rc, output);
        /* Fall back to status */
        g_hash_table_replace(dev->params, strdup(STONITH_ATTR_HOSTCHECK), strdup("status"));

        g_list_free_full(dev->targets, free);
        dev->targets = NULL;
    } else if (!rc) {
		/* listコマンドの実行に成功した場合 */
        crm_info("Refreshing port list for %s", dev->id);
        g_list_free_full(dev->targets, free);
        /* list応答outputのhostlistをパースする */
        dev->targets = parse_host_list(output);
        dev->targets_age = time(NULL);
    }

    if (dev->targets) {
		/* hostlistがパース出来た場合 */
        const char *alias = g_hash_table_lookup(dev->aliases, search->host);

        if (!alias) {
            alias = search->host;
        }
        if (string_in_list(dev->targets, alias)) {
			/* hostlistに存在する場合はQUERY結果として応答に積み上げる */
            can_fence = TRUE;
        }
        /* hostlistに存在しない場合はQUERY結果にはつみあがらない */
    }
    /* デバイスの検索結果を保存する */
    search_devices_record_result(search, dev->id, can_fence);
}

/*!
 * \internal
 * \brief Checks to see if an identical device already exists in the device_list
 */
/* device_listリストに登録済のデバイスかどうかをチェックする */
static stonith_device_t *
device_has_duplicate(stonith_device_t * device)
{
    char *key = NULL;
    char *value = NULL;
    GHashTableIter gIter;
    stonith_device_t *dup = g_hash_table_lookup(device_list, device->id);

    if (!dup) {
        crm_trace("No match for %s", device->id);
        return NULL;

    } else if (safe_str_neq(dup->agent, device->agent)) {
        crm_trace("Different agent: %s != %s", dup->agent, device->agent);
        return NULL;
    }

    /* Use calculate_operation_digest() here? */
    g_hash_table_iter_init(&gIter, device->params);
    while (g_hash_table_iter_next(&gIter, (void **)&key, (void **)&value)) {

        if(strstr(key, "CRM_meta") == key) {
            continue;
        } else if(strcmp(key, "crm_feature_set") == 0) {
            continue;
        } else {
            char *other_value = g_hash_table_lookup(dup->params, key);

            if (!other_value || safe_str_neq(other_value, value)) {
                crm_trace("Different value for %s: %s != %s", key, other_value, value);
                return NULL;
            }
        }
    }

    crm_trace("Match");
    return dup;
}
/* STONITHデバイス(device_listリストへ)の登録 */
int
stonith_device_register(xmlNode * msg, const char **desc, gboolean from_cib)
{
    stonith_device_t *dup = NULL;
	/* STONITHデバイスの登録メッセージを元にして、STONITHデバイスの登録データを生成する */
    stonith_device_t *device = build_device_from_xml(msg);	
	/* device_listリストに登録済のデバイスかどうかをチェックする */
    dup = device_has_duplicate(device);
    if (dup) {
        crm_notice("Device '%s' already existed in device list (%d active devices)", device->id,
                   g_hash_table_size(device_list));
        free_device(device);
        device = dup;

    } else {
        stonith_device_t *old = g_hash_table_lookup(device_list, device->id);

        if (from_cib && old && old->api_registered) {
            /* If the cib is writing over an entry that is shared with a stonith client,
             * copy any pending ops that currently exist on the old entry to the new one.
             * Otherwise the pending ops will be reported as failures
             */
            crm_trace("Overwriting an existing entry for %s from the cib", device->id);
            device->pending_ops = old->pending_ops;
            device->api_registered = TRUE;
            old->pending_ops = NULL;
            if (device->pending_ops) {
                mainloop_set_trigger(device->work);
            }
        }
        /* device_listに登録する */
        g_hash_table_replace(device_list, device->id, device);

        crm_notice("Added '%s' to the device list (%d active devices)", device->id,
                   g_hash_table_size(device_list));
    }
    if (desc) {
        *desc = device->id;
    }

    if (from_cib) {
        device->cib_registered = TRUE;
    } else {
        device->api_registered = TRUE;
    }

    return pcmk_ok;
}

int
stonith_device_remove(const char *id, gboolean from_cib)
{
    stonith_device_t *device = g_hash_table_lookup(device_list, id);

    if (!device) {
        crm_info("Device '%s' not found (%d active devices)", id, g_hash_table_size(device_list));
        return pcmk_ok;
    }

    if (from_cib) {
        device->cib_registered = FALSE;
    } else {
        device->verified = FALSE;
        device->api_registered = FALSE;
    }

    if (!device->cib_registered && !device->api_registered) {
        g_hash_table_remove(device_list, id);
        crm_info("Removed '%s' from the device list (%d active devices)",
                 id, g_hash_table_size(device_list));
    }
    return pcmk_ok;
}

static int
count_active_levels(stonith_topology_t * tp)
{
    int lpc = 0;
    int count = 0;

    for (lpc = 0; lpc < ST_LEVEL_MAX; lpc++) {
        if (tp->levels[lpc] != NULL) {
            count++;
        }
    }
    return count;
}

void
free_topology_entry(gpointer data)
{
    stonith_topology_t *tp = data;

    int lpc = 0;

    for (lpc = 0; lpc < ST_LEVEL_MAX; lpc++) {
        if (tp->levels[lpc] != NULL) {
            g_list_free_full(tp->levels[lpc], free);
        }
    }
    free(tp->node);
    free(tp);
}
/* 登録用のxml("st_level")からtopologyハッシュテーブルに登録する */
int
stonith_level_register(xmlNode * msg, char **desc)
{
    int id = 0;
    int rc = pcmk_ok;
    xmlNode *child = NULL;

    xmlNode *level = get_xpath_object("//" F_STONITH_LEVEL, msg, LOG_ERR);
    const char *node = crm_element_value(level, F_STONITH_TARGET);
    /* topologyハッシュテーブルからF_STONITH_TARGET（対象ノード)を検索する */
    stonith_topology_t *tp = g_hash_table_lookup(topology, node);
	/* "st_level"内の"id"を取りだす */
    crm_element_value_int(level, XML_ATTR_ID, &id);
    if (desc) {
        *desc = g_strdup_printf("%s[%d]", node, id);
    }
    if (id <= 0 || id >= ST_LEVEL_MAX) {
		/* index範囲えらー */
        return -EINVAL;
    }

    if (tp == NULL) {
		/* topologyハッシュテーブルに存在しない場合は、新規に登録する */
        tp = calloc(1, sizeof(stonith_topology_t));
        tp->node = strdup(node);
        g_hash_table_replace(topology, tp->node, tp);
        crm_trace("Added %s to the topology (%d active entries)", node,
                  g_hash_table_size(topology));
    }

    if (tp->levels[id] != NULL) {
		/* 存在するレベル(index)の場合はログ出力 */
        crm_info("Adding to the existing %s[%d] topology entry (%d active entries)", node, id,
                 count_active_levels(tp));
    }
	/* 対象ノードのlevelリストをすべて処理する */
    for (child = __xml_first_child(level); child != NULL; child = __xml_next(child)) {
        const char *device = ID(child);
		/* 対象level(index)にデバイスを登録する */
        crm_trace("Adding device '%s' for %s (%d)", device, node, id);
        tp->levels[id] = g_list_append(tp->levels[id], strdup(device));
    }
	/* 登録情報のログ出力 */
    crm_info("Node %s has %d active fencing levels", node, count_active_levels(tp));
    return rc;
}

int
stonith_level_remove(xmlNode * msg, char **desc)
{
    int id = 0;
    xmlNode *level = get_xpath_object("//" F_STONITH_LEVEL, msg, LOG_ERR);
    const char *node = crm_element_value(level, F_STONITH_TARGET);
    stonith_topology_t *tp = g_hash_table_lookup(topology, node);

    if (desc) {
        *desc = g_strdup_printf("%s[%d]", node, id);
    }
    crm_element_value_int(level, XML_ATTR_ID, &id);

    if (tp == NULL) {
        crm_info("Node %s not found (%d active entries)", node, g_hash_table_size(topology));
        return pcmk_ok;

    } else if (id < 0 || id >= ST_LEVEL_MAX) {
        return -EINVAL;
    }

    if (id == 0 && g_hash_table_remove(topology, node)) {
        crm_info("Removed all %s related entries from the topology (%d active entries)",
                 node, g_hash_table_size(topology));

    } else if (id > 0 && tp->levels[id] != NULL) {
        g_list_free_full(tp->levels[id], free);
        tp->levels[id] = NULL;

        crm_info("Removed entry '%d' from %s's topology (%d active entries remaining)",
                 id, node, count_active_levels(tp));
    }
    return pcmk_ok;
}
/* FENCE動作でないSTONITHへのコマンド実行 */
static int
stonith_device_action(xmlNode * msg, char **output)
{
    int rc = pcmk_ok;
    xmlNode *dev = get_xpath_object("//" F_STONITH_DEVICE, msg, LOG_ERR);
    const char *id = crm_element_value(dev, F_STONITH_DEVICE);

    async_command_t *cmd = NULL;
    stonith_device_t *device = NULL;
	/* device_listへの登録を確認する */
    if (id) {
        crm_trace("Looking for '%s'", id);
        device = g_hash_table_lookup(device_list, id);
    }

    if (device) {
		/* 登録されている場合は、コマンドを構築して実行リストに追加する *//* STONITH 実行非同期コマンドの作成 */
        cmd = create_async_command(msg);
        if (cmd == NULL) {
            free_device(device);
            return -EPROTO;
        }
		/* 実行リストへの追加とトリガーをたたく */
        schedule_stonith_command(cmd, device);
        rc = -EINPROGRESS;

    } else {
		/* 未登録の場合は処理しない */
        crm_info("Device %s not found", id ? id : "<none>");
        rc = -ENODEV;
    }
    return rc;
}
/* デバイスの検索結果を保存する */
static void
search_devices_record_result(struct device_search_s *search, const char *device, gboolean can_fence)
{
    search->replies_received++;

    if (can_fence && device) {
		/* stonith可能なデバイスならsearch情報のcapbleリストに追加する */
        search->capable = g_list_append(search->capable, strdup(device));
    }

    if (search->replies_needed == search->replies_received) {

        crm_debug("Finished Search. %d devices can perform action (%s) on node %s",
                  g_list_length(search->capable),
                  search->action ? search->action : "<unknown>",
                  search->host ? search->host : "<anyone>");

        search->callback(search->capable, search->user_data);
        free(search->host);
        free(search->action);
        free(search);
    }
}
/* 単一デバイス情報の検索情報でのフェンスチェック */
static void
can_fence_host_with_device(stonith_device_t * dev, struct device_search_s *search)
{
    gboolean can = FALSE;
    const char *check_type = NULL;
    const char *host = search->host;
    const char *alias = NULL;

    CRM_LOG_ASSERT(dev != NULL);
	/* デバイス、ホストがNULLなら終了 */
    if (dev == NULL) {
        goto search_report_results;
    } else if (host == NULL) {
        can = TRUE;
        goto search_report_results;
    }

    if (dev->on_target_actions &&
        search->action &&
        strstr(dev->on_target_actions, search->action) && safe_str_neq(host, stonith_our_uname)) {
        /* this device can only execute this action on the target node */
        /* agentにtarget指定がある場合(fence_legacy,fenc_pcmkにはない)で、actionが一致して、ホスト名が
        自ノードの場合は、結果に積み上げない */
        goto search_report_results;
    }
	/* デバイスのaliasesリストから対象STONITNノードのaliasを検索 */
    alias = g_hash_table_lookup(dev->aliases, host);
    if (alias == NULL) {
        alias = host;			/* aliasがないなら対象STONITHノードをセット */
    }
	/* １つのデバイスのパラメータ情報からタイプを取得する */
    check_type = target_list_type(dev);

    if (safe_str_eq(check_type, "none")) {
        can = TRUE;

    } else if (safe_str_eq(check_type, "static-list")) {

        /* Presence in the hostmap is sufficient
         * Only use if all hosts on which the device can be active can always fence all listed hosts
         */

        if (string_in_list(dev->targets, host)) {
            can = TRUE;
        } else if (g_hash_table_lookup(dev->params, STONITH_ATTR_HOSTMAP)
                   && g_hash_table_lookup(dev->aliases, host)) {
            can = TRUE;
        }

    } else if (safe_str_eq(check_type, "dynamic-list")) {
        time_t now = time(NULL);

        if (dev->targets == NULL || dev->targets_age + 60 < now) {
			/* targetsがNULLか、前回list実行から60s以上経過している場合（もしくは初期実行)ならlistコマンドを実行する */
            schedule_internal_command(__FUNCTION__, dev, "list", NULL,
                                      search->per_device_timeout, search, dynamic_list_search_cb);

            /* we'll respond to this search request async in the cb */
            return;
        }

        if (string_in_list(dev->targets, alias)) {
            can = TRUE;
        }

    } else if (safe_str_eq(check_type, "status")) {
        schedule_internal_command(__FUNCTION__, dev, "status", search->host,
                                  search->per_device_timeout, search, status_search_cb);
        /* we'll respond to this search request async in the cb */
        return;
    } else {
        crm_err("Unknown check type: %s", check_type);
    }

    if (safe_str_eq(host, alias)) {
        crm_notice("%s can%s fence %s: %s", dev->id, can ? "" : " not", host, check_type);
    } else {
        crm_notice("%s can%s fence %s (aka. '%s'): %s", dev->id, can ? "" : " not", host, alias,
                   check_type);
    }

  search_report_results:
	  /* デバイスの検索結果を保存する */
    search_devices_record_result(search, dev ? dev->id : NULL, can);
}
/* デバイスリストの検索処理 */
static void
search_devices(gpointer key, gpointer value, gpointer user_data)
{
    stonith_device_t *dev = value;
    struct device_search_s *search = user_data;
	/* 単一デバイス情報をサーチ情報でフェンスチェック */
    can_fence_host_with_device(dev, search);
}
/* STONITHデバイスを取得する */
#define DEFAULT_QUERY_TIMEOUT 20
static void
get_capable_devices(const char *host, const char *action, int timeout, void *user_data,
                    void (*callback) (GList * devices, void *user_data))
{
    struct device_search_s *search;
    int per_device_timeout = DEFAULT_QUERY_TIMEOUT;
    int devices_needing_async_query = 0;
    char *key = NULL;
    const char *check_type = NULL;
    GHashTableIter gIter;
    stonith_device_t *device = NULL;

    if (!g_hash_table_size(device_list)) {	/* 自ノードにdevice_listが存在しない場合 */
        callback(NULL, user_data);
        return;
    }

    search = calloc(1, sizeof(struct device_search_s));
    if (!search) {							/* サーチデータが生成できない場合 */
        callback(NULL, user_data);
        return;
    }
	/* このノードのデバイスリストをすべて処理する */
    g_hash_table_iter_init(&gIter, device_list);
    while (g_hash_table_iter_next(&gIter, (void **)&key, (void **)&device)) {
		/* １つのデバイスのパラメータ情報からタイプを取得する */
        check_type = target_list_type(device);
        if (safe_str_eq(check_type, "status") || safe_str_eq(check_type, "dynamic-list")) {
			/* タイプが"status"か"dynamic-list"ならカウントアップ */
            devices_needing_async_query++;
        }
    }

    /* If we have devices that require an async event in order to know what
     * nodes they can fence, we have to give the events a timeout. The total
     * query timeout is divided among those events. */
    if (devices_needing_async_query) {
		/* devices_needing_async_queryがカウントアップされている場合 */
		/* --- このノードのデバイスリストに"status" "dynamic-list"のタイプのデバイスがある場合 --- */
        /* タイムアウトを算出 */
        per_device_timeout = timeout / devices_needing_async_query;
        if (!per_device_timeout) {	/* 算出タイムアウト異常(1) */
            crm_err("stonith-timeout duration %d is too low, raise the duration to %d seconds",
                    timeout, DEFAULT_QUERY_TIMEOUT * devices_needing_async_query);
            per_device_timeout = DEFAULT_QUERY_TIMEOUT;
        } else if (per_device_timeout < DEFAULT_QUERY_TIMEOUT) { /* 算出タイムアウト異常(2) */
            crm_notice
                ("stonith-timeout duration %d is low for the current configuration. Consider raising it to %d seconds",
                 timeout, DEFAULT_QUERY_TIMEOUT * devices_needing_async_query);
        }
    }

    search->host = host ? strdup(host) : NULL;
    search->action = action ? strdup(action) : NULL;
    search->per_device_timeout = per_device_timeout;
    /* We are guaranteed this many replies. Even if a device gets
     * unregistered some how during the async search, we will get
     * the correct number of replies. */
    search->replies_needed = g_hash_table_size(device_list);
    search->callback = callback;
    search->user_data = user_data;
    /* kick off the search */
	/* 検索実行前のデバックメッセージ */
    crm_debug("Searching through %d devices to see what is capable of action (%s) for target %s",
              search->replies_needed,
              search->action ? search->action : "<unknown>",
              search->host ? search->host : "<anyone>");
    /* デバイスリスト検索の実行 */
    g_hash_table_foreach(device_list, search_devices, search);
}

struct st_query_data {
    xmlNode *reply;
    char *remote_peer;
    char *client_id;
    char *target;
    char *action;
    int call_options;
};
/* get_capable_devices処理の実行コールバック */
/*                 devices引数には、STONITH可能なデバイスが積み上げれれている */
static void
stonith_query_capable_device_cb(GList * devices, void *user_data)
{
    struct st_query_data *query = user_data;
    int available_devices = 0;
    xmlNode *dev = NULL;
    xmlNode *list = NULL;
    GListPtr lpc = NULL;

    /* Pack the results into data */
    /* STONITH可能なデバイス情報をQUERY応答用のXMLに積み上げる */
    list = create_xml_node(NULL, __FUNCTION__);
    crm_xml_add(list, F_STONITH_TARGET, query->target);
    for (lpc = devices; lpc != NULL; lpc = lpc->next) {
		/* 結果のデータが自ノードのdevice_list(自ノードに配置可能なデバイス)に存在するか */
        stonith_device_t *device = g_hash_table_lookup(device_list, lpc->data);
        int action_specific_timeout;

        if (!device) {
			/* 存在しないデバイスは処理しない */
            /* It is possible the device got unregistered while
             * determining who can fence the target */
            continue;
        }
		/* 実行可能なデバイス数をカウントあっぷ */
        available_devices++;

        action_specific_timeout = get_action_timeout(device, query->action, 0);
        dev = create_xml_node(list, F_STONITH_DEVICE);
        crm_xml_add(dev, XML_ATTR_ID, device->id);
        crm_xml_add(dev, "namespace", device->namespace);
        crm_xml_add(dev, "agent", device->agent);
        crm_xml_add_int(dev, F_STONITH_DEVICE_VERIFIED, device->verified);
        if (action_specific_timeout) {
            crm_xml_add_int(dev, F_STONITH_ACTION_TIMEOUT, action_specific_timeout);
        }
        if (query->target == NULL) {
            xmlNode *attrs = create_xml_node(dev, XML_TAG_ATTRS);

            g_hash_table_foreach(device->params, hash2field, attrs);
        }
    }
	/* "st-available-devices"に実行可能なデバイス数をセット */
    crm_xml_add_int(list, "st-available-devices", available_devices);
    if (query->target) {
        crm_debug("Found %d matching devices for '%s'", available_devices, query->target);
    } else {
        crm_debug("%d devices installed", available_devices);
    }

    if (list != NULL) {
		/* 積み上げたデバイスのxml情報をQUERY応答にセットする */
        crm_trace("Attaching query list output");
        add_message_xml(query->reply, F_STONITH_CALLDATA, list);
    }
    /* QUERY応答の送信(STONITH可能なデバイス情報を送信) */
    /* --- 対応するデバイスが無い場合は、実行可能なデバイス数は０で応答を送信する --- */
    stonith_send_reply(query->reply, query->call_options, query->remote_peer, query->client_id);

    free_xml(query->reply);
    free(query->remote_peer);
    free(query->client_id);
    free(query->target);
    free(query->action);
    free(query);
    free_xml(list);
    g_list_free_full(devices, free);
}
/* QUERY処理 */
static void
stonith_query(xmlNode * msg, const char *remote_peer, const char *client_id, int call_options)
{
    struct st_query_data *query = NULL;
    const char *action = NULL;
    const char *target = NULL;
    int timeout = 0;
    xmlNode *dev = get_xpath_object("//@" F_STONITH_ACTION, msg, LOG_DEBUG_3);	/* F_STONITH_ACTIONのXMLノードを取得 */

    crm_element_value_int(msg, F_STONITH_TIMEOUT, &timeout);					/* F_STONITH_TIMEOUTを取得 */
    if (dev) {
        const char *device = crm_element_value(dev, F_STONITH_DEVICE);			/* 取得したF_STONITH_ACTIONのXMLノードからF_STONITH_DEVICEを取得 */

        target = crm_element_value(dev, F_STONITH_TARGET);						/* 取得したF_STONITH_ACTIONのXMLノードからF_STONITH_TARGETを取得 */
        action = crm_element_value(dev, F_STONITH_ACTION);						/* 取得したF_STONITH_ACTIONのXMLノードからF_STONITH_TARGETを取得 */
        if (device && safe_str_eq(device, "manual_ack")) {
            /* No query or reply necessary */
            return;
        }
    }
	/* QUERY応答メッセージデータを生成 */
    crm_log_xml_debug(msg, "Query");
    query = calloc(1, sizeof(struct st_query_data));

    query->reply = stonith_construct_reply(msg, NULL, NULL, pcmk_ok);
    query->remote_peer = remote_peer ? strdup(remote_peer) : NULL;
    query->client_id = client_id ? strdup(client_id) : NULL;
    query->target = target ? strdup(target) : NULL;
    query->action = action ? strdup(action) : NULL;
    query->call_options = call_options;
	/* QUERY応答用のSTONITHデバイスを取得する */
    get_capable_devices(target, action, timeout, query, stonith_query_capable_device_cb);
}

#define ST_LOG_OUTPUT_MAX 512
static void
log_operation(async_command_t * cmd, int rc, int pid, const char *next, const char *output)
{
    if (rc == 0) {
        next = NULL;
    }

    if (cmd->victim != NULL) {
        do_crm_log(rc == 0 ? LOG_NOTICE : LOG_ERR,
                   "Operation '%s' [%d] (call %d from %s) for host '%s' with device '%s' returned: %d (%s)%s%s",
                   cmd->action, pid, cmd->id, cmd->client_name, cmd->victim, cmd->device, rc,
                   pcmk_strerror(rc), next ? ". Trying: " : "", next ? next : "");
    } else {
        do_crm_log_unlikely(rc == 0 ? LOG_DEBUG : LOG_NOTICE,
                            "Operation '%s' [%d] for device '%s' returned: %d (%s)%s%s",
                            cmd->action, pid, cmd->device, rc, pcmk_strerror(rc),
                            next ? ". Trying: " : "", next ? next : "");
    }

    if (output) {
        /* Logging the whole string confuses syslog when the string is xml */
        char *prefix = g_strdup_printf("%s:%d", cmd->device, pid);

        crm_log_output(rc == 0 ? LOG_INFO : LOG_WARNING, prefix, output);
        g_free(prefix);
    }
}
/* stonithコマンド実行後の応答処理 */
static void
stonith_send_async_reply(async_command_t * cmd, const char *output, int rc, GPid pid)
{
    xmlNode *reply = NULL;
    gboolean bcast = FALSE;
	/* 非同期応答メッセージ(固定部)の生成 */
    reply = stonith_construct_async_reply(cmd, output, NULL, rc);

    if (safe_str_eq(cmd->action, "metadata")) {
        /* Too verbose to log */
        crm_trace("Metadata query for %s", cmd->device);
        output = NULL;

    } else if (crm_str_eq(cmd->action, "monitor", TRUE) ||
               crm_str_eq(cmd->action, "list", TRUE) || crm_str_eq(cmd->action, "status", TRUE)) {
        crm_trace("Never broadcast %s replies", cmd->action);

    } else if (!stand_alone && safe_str_eq(cmd->origin, cmd->victim) && safe_str_neq(cmd->action, "on")) {
		/* stand_aloneでなく、コマンドの依頼元とFENCING対象が同じで、onコマンドでない場合は、broadcastをセット */
		/* ** 通常、他ノードがSTONITHの依頼元で、FENCING対象の場合は、成功時はFENCING対象に応答出来ないので"broadcast" *** */
        crm_trace("Broadcast %s reply for %s", cmd->action, cmd->victim);
        crm_xml_add(reply, F_SUBTYPE, "broadcast");
        bcast = TRUE;
    }

    log_operation(cmd, rc, pid, NULL, output);
    crm_log_xml_trace(reply, "Reply");

    if (bcast) {
		/* broadcastの場合は、クラスタ内のノードにT_STONITH_NOTIFYメッセージで送信する */
        crm_xml_add(reply, F_STONITH_OPERATION, T_STONITH_NOTIFY);
        send_cluster_message(NULL, crm_msg_stonith_ng, reply, FALSE);

    } else if (cmd->origin) {
		/* broadcastでない場合で依頼元がある場合は、コマンドの依頼元にSTONITH_OP_FENCE応答メッセージで送信する */
        crm_trace("Directed reply to %s", cmd->origin);
        send_cluster_message(crm_get_peer(0, cmd->origin), crm_msg_stonith_ng, reply, FALSE);

    } else {
		/* broadcastでない場合で、コマンドの依頼元が無い場合は、自ノードに応答メッセージを送信する */
		/* 自ノードでの、"monitor","status","list"などが該当 */
        crm_trace("Directed local %ssync reply to %s",
                  (cmd->options & st_opt_sync_call) ? "" : "a-", cmd->client_name);
        do_local_reply(reply, cmd->client, cmd->options & st_opt_sync_call, FALSE);
    }

    if (stand_alone) {
        /* Do notification with a clean data object */
        xmlNode *notify_data = create_xml_node(NULL, T_STONITH_NOTIFY_FENCE);

        crm_xml_add_int(notify_data, F_STONITH_RC, rc);
        crm_xml_add(notify_data, F_STONITH_TARGET, cmd->victim);
        crm_xml_add(notify_data, F_STONITH_OPERATION, cmd->op);
        crm_xml_add(notify_data, F_STONITH_DELEGATE, cmd->device);
        crm_xml_add(notify_data, F_STONITH_REMOTE_OP_ID, cmd->remote_op_id);
        crm_xml_add(notify_data, F_STONITH_ORIGIN, cmd->client);

        do_stonith_notify(0, T_STONITH_NOTIFY_FENCE, rc, notify_data);
    }

    free_xml(reply);
}

void
unfence_cb(GPid pid, int rc, const char *output, gpointer user_data)
{
    async_command_t * cmd = user_data;
    stonith_device_t *dev = g_hash_table_lookup(device_list, cmd->device);

    log_operation(cmd, rc, pid, NULL, output);

    if(dev) {
        dev->active_pid = 0;
        mainloop_set_trigger(dev->work);
    } else {
        crm_trace("Device %s does not exist", cmd->device);
    }

    if(rc != 0) {
        crm_exit(DAEMON_RESPAWN_STOP);
    }
}

static void
cancel_stonith_command(async_command_t * cmd)
{
    stonith_device_t *device;

    CRM_CHECK(cmd != NULL, return);

    if (!cmd->device) {
        return;
    }

    device = g_hash_table_lookup(device_list, cmd->device);

    if (device) {
        crm_trace("Cancel scheduled %s on %s", cmd->action, device->id);
        device->pending_ops = g_list_remove(device->pending_ops, cmd);
    }
}

#define READ_MAX 500
/* stonith実行 子プロセスの完了コールバック */
static void
st_child_done(GPid pid, int rc, const char *output, gpointer user_data)
{
    stonith_device_t *device = NULL;
    async_command_t *cmd = user_data;

    GListPtr gIter = NULL;
    GListPtr gIterNext = NULL;

    CRM_CHECK(cmd != NULL, return);

    active_children--;

    /* The device is ready to do something else now */
    device = g_hash_table_lookup(device_list, cmd->device);
    if (device) {
        device->active_pid = 0;
        if (rc == pcmk_ok &&
            (safe_str_eq(cmd->action, "list") ||
             safe_str_eq(cmd->action, "monitor") || safe_str_eq(cmd->action, "status"))) {
			/* list,monitor,statusが成功した場合にはデバイスを確認済にする */
            device->verified = TRUE;
        }
		/* トリガーをたたく */
        mainloop_set_trigger(device->work);
    }

    crm_trace("Operation %s on %s completed with rc=%d (%d remaining)",
              cmd->action, cmd->device, rc, g_list_length(cmd->device_next));
	/* STONITHが失敗してfencig_topologyからの動作で次のデバイスがある場合 */
    if (rc != 0 && cmd->device_next) {
		/* 実行コマンドがエラーで、次のデバイスがセットされている場合 */
		/* 次のデバイスが自ノードに存在するか検索する */
        stonith_device_t *dev = g_hash_table_lookup(device_list, cmd->device_next->data);

        if (dev) {
			/* 存在する場合は、次のデバイスのコマンドも実行スケジュールする */
            log_operation(cmd, rc, pid, dev->id, output);

            cmd->device_next = cmd->device_next->next;
            schedule_stonith_command(cmd, dev);
            /* Prevent cmd from being freed */
            cmd = NULL;
            goto done;
        }
        /* 自ノードに次のデバイスが存在しなければ次へ進み一旦応答 */
    }
	/* STONITHが成功、もしくは失敗して、fencig_topologyからの動作で次のデバイスがない場合 */
	/* もしくは、fencing_topologyからの設定ではない場合 */
    if (rc > 0) {
        /* Try to provide _something_ useful */
        if(output == NULL) {
            rc = -ENODATA;

        } else if(strstr(output, "imed out")) {
            rc = -ETIMEDOUT;

        } else if(strstr(output, "Unrecognised action")) {
            rc = -EOPNOTSUPP;

        } else {
            rc = -pcmk_err_generic;
        }
    }
	/* 今回実行したコマンドへの応答処理 */
    stonith_send_async_reply(cmd, output, rc, pid);

    if (rc != 0) {
        goto done;
    }

    /* Check to see if any operations are scheduled to do the exact
     * same thing that just completed.  If so, rather than
     * performing the same fencing operation twice, return the result
     * of this operation for all pending commands it matches. */
    for (gIter = cmd_list; gIter != NULL; gIter = gIterNext) {
        async_command_t *cmd_other = gIter->data;

        gIterNext = gIter->next;

        if (cmd == cmd_other) {
            continue;
        }

        /* A pending scheduled command matches the command that just finished if.
         * 1. The client connections are different.
         * 2. The node victim is the same.
         * 3. The fencing action is the same.
         * 4. The device scheduled to execute the action is the same.
         */
        if (safe_str_eq(cmd->client, cmd_other->client) ||
            safe_str_neq(cmd->victim, cmd_other->victim) ||
            safe_str_neq(cmd->action, cmd_other->action) ||
            safe_str_neq(cmd->device, cmd_other->device)) {

            continue;
        }
		/* コマンドの実行完了後に、他の同一デバイス・同一操作の要求処理が来ていた場合は、それらのコマンドにも応答を返す */
        crm_notice
            ("Merging stonith action %s for node %s originating from client %s with identical stonith request from client %s",
             cmd_other->action, cmd_other->victim, cmd_other->client_name, cmd->client_name);

        cmd_list = g_list_remove_link(cmd_list, gIter);

        stonith_send_async_reply(cmd_other, output, rc, pid);
        cancel_stonith_command(cmd_other);
		/* 非同期コマンドエリアの解放 */
        free_async_command(cmd_other);
        g_list_free_1(gIter);
    }

  done:
	/* 非同期コマンドエリアの解放 */
    free_async_command(cmd);
}

static gint
sort_device_priority(gconstpointer a, gconstpointer b)
{
    const stonith_device_t *dev_a = a;
    const stonith_device_t *dev_b = b;

    if (dev_a->priority > dev_b->priority) {
        return -1;
    } else if (dev_a->priority < dev_b->priority) {
        return 1;
    }
    return 0;
}
/* FENCE実行時の検索コールバック */
/*                 devices引数には、STONITH可能なデバイスが積み上げれれている */
static void
stonith_fence_get_devices_cb(GList * devices, void *user_data)
{
    async_command_t *cmd = user_data;
    stonith_device_t *device = NULL;

    crm_info("Found %d matching devices for '%s'", g_list_length(devices), cmd->victim);

    if (g_list_length(devices) > 0) {
        /* Order based on priority */
        devices = g_list_sort(devices, sort_device_priority);
        device = g_hash_table_lookup(device_list, devices->data);

        if (device) {
            cmd->device_list = devices;
            cmd->device_next = devices->next;
            devices = NULL;     /* list owned by cmd now */
        }
    }

    /* we have a device, schedule it for fencing. */
    if (device) {
		/* 実行可能なデバイスでfence動作をスケジュール */
        schedule_stonith_command(cmd, device);
        /* in progress */
        return;
    }
	/* 自ノードに実行可能なデバイスが無い場合は、失敗の応答を送信 */
    /* no device found! */
    stonith_send_async_reply(cmd, NULL, -ENODEV, 0);
	/* 非同期コマンドエリアの解放 */
    free_async_command(cmd);
    g_list_free_full(devices, free);
}
/* STONITHを実行する */
static int
stonith_fence(xmlNode * msg)
{
    const char *device_id = NULL;
    stonith_device_t *device = NULL;
    /* STONITH 実行非同期コマンドの作成 */
    async_command_t *cmd = create_async_command(msg);
    xmlNode *dev = get_xpath_object("//@" F_STONITH_TARGET, msg, LOG_ERR);

    if (cmd == NULL) {
        return -EPROTO;
    }

    device_id = crm_element_value(dev, F_STONITH_DEVICE);
    if (device_id) {
		/* 実行するデバイスが決定している場合(fencing_topologyからの実行の場合) */
        device = g_hash_table_lookup(device_list, device_id);
        if (device == NULL) {
            crm_err("Requested device '%s' is not available", device_id);
            return -ENODEV;
        }
        /* 実行リストにFENCE動作を追加する */
        schedule_stonith_command(cmd, device);

    } else {
		/* fencing_toplogy無の実行の場合 */
        const char *host = crm_element_value(dev, F_STONITH_TARGET);

        if (cmd->options & st_opt_cs_nodeid) {
            int nodeid = crm_atoi(host, NULL);
            crm_node_t *node = crm_get_peer(nodeid, NULL);

            if (node) {
                host = node->uname;
            }
        }
        /* 実行デバイスを取得し、stonith_fence_get_devices_cbコールバックで処理 */
       	/* FENCE用のSTONITHデバイスを取得する */
        get_capable_devices(host, cmd->action, cmd->default_timeout, cmd,
                            stonith_fence_get_devices_cb);
    }

    return -EINPROGRESS;
}

xmlNode *
stonith_construct_reply(xmlNode * request, const char *output, xmlNode * data, int rc)
{
    int lpc = 0;
    xmlNode *reply = NULL;

    const char *name = NULL;
    const char *value = NULL;

    const char *names[] = {
        F_STONITH_OPERATION,
        F_STONITH_CALLID,
        F_STONITH_CLIENTID,
        F_STONITH_CLIENTNAME,
        F_STONITH_REMOTE_OP_ID,
        F_STONITH_CALLOPTS
    };

    crm_trace("Creating a basic reply");
    reply = create_xml_node(NULL, T_STONITH_REPLY);

    crm_xml_add(reply, "st_origin", __FUNCTION__);
    crm_xml_add(reply, F_TYPE, T_STONITH_NG);
    crm_xml_add(reply, "st_output", output);
    crm_xml_add_int(reply, F_STONITH_RC, rc);

    CRM_CHECK(request != NULL, crm_warn("Can't create a sane reply"); return reply);
    for (lpc = 0; lpc < DIMOF(names); lpc++) {
        name = names[lpc];
        value = crm_element_value(request, name);
        crm_xml_add(reply, name, value);
    }

    if (data != NULL) {
        crm_trace("Attaching reply output");
        add_message_xml(reply, F_STONITH_CALLDATA, data);
    }
    return reply;
}
/* 非同期応答メッセージ(固定部)の生成 */
static xmlNode *
stonith_construct_async_reply(async_command_t * cmd, const char *output, xmlNode * data, int rc)
{
    xmlNode *reply = NULL;

    crm_trace("Creating a basic reply");
    reply = create_xml_node(NULL, T_STONITH_REPLY);

    crm_xml_add(reply, "st_origin", __FUNCTION__);
    crm_xml_add(reply, F_TYPE, T_STONITH_NG);

    crm_xml_add(reply, F_STONITH_OPERATION, cmd->op);
    crm_xml_add(reply, F_STONITH_DEVICE, cmd->device);
    crm_xml_add(reply, F_STONITH_REMOTE_OP_ID, cmd->remote_op_id);
    crm_xml_add(reply, F_STONITH_CLIENTID, cmd->client);
    crm_xml_add(reply, F_STONITH_CLIENTNAME, cmd->client_name);
    crm_xml_add(reply, F_STONITH_TARGET, cmd->victim);
    crm_xml_add(reply, F_STONITH_ACTION, cmd->op);
    /* 依頼元ノード名をSTONITHの起点となったノード名にセットする */
    crm_xml_add(reply, F_STONITH_ORIGIN, cmd->origin);
    crm_xml_add_int(reply, F_STONITH_CALLID, cmd->id);
    crm_xml_add_int(reply, F_STONITH_CALLOPTS, cmd->options);

    crm_xml_add_int(reply, F_STONITH_RC, rc);

    crm_xml_add(reply, "st_output", output);

    if (data != NULL) {
        crm_info("Attaching reply output");
        add_message_xml(reply, F_STONITH_CALLDATA, data);	/* 応答メッセージへの付属データのセット */
    }
    return reply;
}

bool fencing_peer_active(crm_node_t *peer)
{
    if (peer == NULL) {
        return FALSE;
    } else if (peer->uname == NULL) {
        return FALSE;
    } else if(peer->processes & (crm_proc_plugin | crm_proc_heartbeat | crm_proc_cpg)) {
        return TRUE;
    }
    return FALSE;
}

/*!
 * \internal
 * \brief Determine if we need to use an alternate node to
 * fence the target. If so return that node's uname
 *
 * \retval NULL, no alternate host
 * \retval uname, uname of alternate host to use
 */
/* STONITH対象をSTONITH可能なノードでactiveなノードをfencing_topologyとcrmdの認識ノードからを検索する */
static const char *
check_alternate_host(const char *target)
{
    const char *alternate_host = NULL;

    if (g_hash_table_lookup(topology, target) && safe_str_eq(target, stonith_our_uname)) {
		/* fencing_topologyに設定されているSTONITH対象で、STONITH対象が自ノードの場合 */
        GHashTableIter gIter;
        crm_node_t *entry = NULL;
		/* crmdの認識している接続ノードハッシュテーブルを検索する */
        g_hash_table_iter_init(&gIter, crm_peer_cache);
        while (g_hash_table_iter_next(&gIter, NULL, (void **)&entry)) {
            crm_trace("Checking for %s.%d != %s", entry->uname, entry->id, target);
            if (fencing_peer_active(entry)
                && safe_str_neq(entry->uname, target)) {
				/* 認識しているノード名とSTONITH対象が一致していない場合で、ノードがactiveな場合は、ノードをセットする */
				/* --- 自ノードのSTONITHなので、自ノード以外のノードを優先する --- */
                alternate_host = entry->uname;
                break;
            }
        }
        if (alternate_host == NULL) {
			/* 自ノードのstontihノードが決定出来ない場合はログ */
            crm_err("No alternate host available to handle complex self fencing request");
            g_hash_table_iter_init(&gIter, crm_peer_cache);
            while (g_hash_table_iter_next(&gIter, NULL, (void **)&entry)) {
                crm_notice("Peer[%d] %s", entry->id, entry->uname);
            }
        }
    }
	/* 他ノードのSTONITHの場合はNULLで返る */
    return alternate_host;
}

static void
stonith_send_reply(xmlNode * reply, int call_options, const char *remote_peer,
                   const char *client_id)
{
    if (remote_peer) {
		/* 他ノードへの応答の場合 */
        send_cluster_message(crm_get_peer(0, remote_peer), crm_msg_stonith_ng, reply, FALSE);
    } else {
		/* 自ノード内の応答の場合 */
        do_local_reply(reply, client_id, is_set(call_options, st_opt_sync_call), remote_peer != NULL);
    }
}
/* 要求メッセージ処理 */
static int
handle_request(crm_client_t * client, uint32_t id, uint32_t flags, xmlNode * request,
               const char *remote_peer)
{
    int call_options = 0;
    int rc = -EOPNOTSUPP;

    xmlNode *data = NULL;
    xmlNode *reply = NULL;

    char *output = NULL;
    /* OPERATION,CLIENTIDを要求メッセージから取得 */
    const char *op = crm_element_value(request, F_STONITH_OPERATION);		
    const char *client_id = crm_element_value(request, F_STONITH_CLIENTID);

    crm_element_value_int(request, F_STONITH_CALLOPTS, &call_options);

    if (is_set(call_options, st_opt_sync_call)) {
        CRM_ASSERT(client == NULL || client->request_id == id);
    }

    if (crm_str_eq(op, CRM_OP_REGISTER, TRUE)) {
		/* クライアント・プロセスからの接続登録メッセージ */
        xmlNode *reply = create_xml_node(NULL, "reply");

        CRM_ASSERT(client);
        crm_xml_add(reply, F_STONITH_OPERATION, CRM_OP_REGISTER);
        crm_xml_add(reply, F_STONITH_CLIENTID, client->id);
        /* 応答のみクライアントに送信 */
        crm_ipcs_send(client, id, reply, flags);
        client->request_id = 0;
        free_xml(reply);
        return 0;

    } else if (crm_str_eq(op, STONITH_OP_EXEC, TRUE)) {
		/* FENCE動作でないSTONITHへのコマンド実行 */
        rc = stonith_device_action(request, &output);

    } else if (crm_str_eq(op, STONITH_OP_TIMEOUT_UPDATE, TRUE)) {
        const char *call_id = crm_element_value(request, F_STONITH_CALLID);
        const char *client_id = crm_element_value(request, F_STONITH_CLIENTID);
        int op_timeout = 0;

        crm_element_value_int(request, F_STONITH_TIMEOUT, &op_timeout);
        do_stonith_async_timeout_update(client_id, call_id, op_timeout);
        return 0;

    } else if (crm_str_eq(op, STONITH_OP_QUERY, TRUE)) {
		/* STONITH_OP_QUERYメッセージの場合 */
        if (remote_peer) {
			/* 接続クライアント以外(ノードのstonithプロセスから)のSTONITH(STONITH_OP_FENCE)要求の場合 */
            /* リモートSTONITH用の操作データを生成して、このノードのstonithの実行リストに追加する */
            create_remote_stonith_op(client_id, request, TRUE); /* Record it for the future notification */
        }
        /* QUERY処理 */
        stonith_query(request, remote_peer, client_id, call_options);
        return 0;

    } else if (crm_str_eq(op, T_STONITH_NOTIFY, TRUE)) {
		/* NOTIFY登録要求 */
        const char *flag_name = NULL;

        CRM_ASSERT(client);
        flag_name = crm_element_value(request, F_STONITH_NOTIFY_ACTIVATE);
        if (flag_name) {
            crm_debug("Setting %s callbacks for %s (%s): ON", flag_name, client->name, client->id);
            client->options |= get_stonith_flag(flag_name);
        }

        flag_name = crm_element_value(request, F_STONITH_NOTIFY_DEACTIVATE);
        if (flag_name) {
            crm_debug("Setting %s callbacks for %s (%s): off", flag_name, client->name, client->id);
            client->options |= get_stonith_flag(flag_name);
        }

        if (flags & crm_ipc_client_response) {
			/* NOTITY登録の応答メッセージをクライアントに送信 */
            crm_ipcs_send_ack(client, id, flags, "ack", __FUNCTION__, __LINE__);
        }
        return 0;

    } else if (crm_str_eq(op, STONITH_OP_RELAY, TRUE)) {
		/* 他ノードからのSTONITH実行ノードへのSTONITH_OP_RELAYメッセージ */
		/* DCノードのstonithプロセスでDCノードをSTONITH可能なノードが見つかった場合に利用される */
        xmlNode *dev = get_xpath_object("//@" F_STONITH_TARGET, request, LOG_TRACE);

        crm_notice("Peer %s has received a forwarded fencing request from %s to fence (%s) peer %s",
                   stonith_our_uname,
                   client ? client->name : remote_peer,
                   crm_element_value(dev, F_STONITH_ACTION),
                   crm_element_value(dev, F_STONITH_TARGET));
		/* FENCEの為にQUERY処理を開始する */
        if (initiate_remote_stonith_op(NULL, request, FALSE) != NULL) {
            rc = -EINPROGRESS;
        }

    } else if (crm_str_eq(op, STONITH_OP_FENCE, TRUE)) {
		/* FENCE動作の要求 */
        if (remote_peer || stand_alone) {
			/* stand_aloneモード(-s起動)、または、他ノードからのSTONITH(STONITH_OP_FENCE)要求の場合 */
            rc = stonith_fence(request);

        } else if (call_options & st_opt_manual_ack) {
			/* stonith_adminからの-CオプションによるSTONITH実行 */
            remote_fencing_op_t *rop = NULL;
            xmlNode *dev = get_xpath_object("//@" F_STONITH_TARGET, request, LOG_TRACE);
            const char *target = crm_element_value(dev, F_STONITH_TARGET);

            crm_notice("Received manual confirmation that %s is fenced", target);
      		/* FENCEの為にQUERY処理を開始する */
            rop = initiate_remote_stonith_op(client, request, TRUE);
            /* manual実行応答処理(stonith_admin -C) */
            rc = stonith_manual_ack(request, rop);

        } else {
			/* 自ノードcrmd(DCノードのtengine)クライアント・・・もしくは・・・・ */
            const char *alternate_host = NULL;
            xmlNode *dev = get_xpath_object("//@" F_STONITH_TARGET, request, LOG_TRACE);
            const char *target = crm_element_value(dev, F_STONITH_TARGET);
            const char *action = crm_element_value(dev, F_STONITH_ACTION);
            const char *device = crm_element_value(dev, F_STONITH_DEVICE);

            if (client) {	/* 接続client(crmdなど)からのFENCE要求の場合 */
                int tolerance = 0;

                crm_notice("Client %s.%.8s wants to fence (%s) '%s' with device '%s'",
                           client->name, client->id, action, target, device ? device : "(any)");

                crm_element_value_int(dev, F_STONITH_TOLERANCE, &tolerance);

                if (stonith_check_fence_tolerance(tolerance, target, action)) {
                    rc = 0;
                    goto done;
                }

            } else {
                crm_notice("Peer %s wants to fence (%s) '%s' with device '%s'",
                           remote_peer, action, target, device ? device : "(any)");
            }
			/* STONITH対象(自ノード)をSTONITH可能なノードでactiveなノードをfencing_topologyとcrmdの認識ノードからを検索する */
			/* STONITH対象が自ノード以外に関しては、alternate_hostはNULLとなる */
            alternate_host = check_alternate_host(target);

            if (alternate_host && client) {
			/* 自ノードのSTONITHの場合で、topologyから実行ホストが決まっていて、クラスタメンバーの場合 */
                const char *client_id = NULL;

                crm_notice("Forwarding complex self fencing request to peer %s", alternate_host);

                if (client) {
                    client_id = client->id;
                } else {
                    client_id = crm_element_value(request, F_STONITH_CLIENTID);
                }
                /* Create a record of it, otherwise call_id will be 0 if we need to notify of failures */
                /* リモートSTONITH用の操作データを生成して、このノードのstonithの実行リストに追加する */
                create_remote_stonith_op(client_id, request, FALSE);

                crm_xml_add(request, F_STONITH_OPERATION, STONITH_OP_RELAY);
                crm_xml_add(request, F_STONITH_CLIENTID, client->id);
                /* STONITH_OP_RELAYメッセージを実行ホストへ送信する */
                send_cluster_message(crm_get_peer(0, alternate_host), crm_msg_stonith_ng, request,
                                     FALSE);
                rc = -EINPROGRESS;
			/* 他ノード及び、自ノードをSONTIH可能な実行ホストが不明の場合ののREMOTE STONITH処理 */
			/* FENCEの為にQUERY処理を開始する */
            } else if (initiate_remote_stonith_op(client, request, FALSE) != NULL) {	
                rc = -EINPROGRESS;
            }
        }

    } else if (crm_str_eq(op, STONITH_OP_FENCE_HISTORY, TRUE)) {
		/* 履歴要求(STONITH_OP_FENCE_HISTORY)メッセージの場合 */
        rc = stonith_fence_history(request, &data);

    } else if (crm_str_eq(op, STONITH_OP_DEVICE_ADD, TRUE)) {
		/* STONITHデバイスの登録(STONITH_OP_DEVICE_ADD)メッセージの場合 */
        const char *id = NULL;
        xmlNode *notify_data = create_xml_node(NULL, op);
		/* STONITHデバイス(device_listリストへ)の登録 */
        rc = stonith_device_register(request, &id, FALSE);

        crm_xml_add(notify_data, F_STONITH_DEVICE, id);
        crm_xml_add_int(notify_data, F_STONITH_ACTIVE, g_hash_table_size(device_list));
		/* STONITHデバイスの登録NOTIFYを送信 */
        do_stonith_notify(call_options, op, rc, notify_data);
        free_xml(notify_data);

    } else if (crm_str_eq(op, STONITH_OP_DEVICE_DEL, TRUE)) {
		/* STONITHデバイスの削除(STONITH_OP_DEVICE_DEL)メッセージの場合 */
        xmlNode *dev = get_xpath_object("//" F_STONITH_DEVICE, request, LOG_ERR);
        const char *id = crm_element_value(dev, XML_ATTR_ID);
        xmlNode *notify_data = create_xml_node(NULL, op);

        rc = stonith_device_remove(id, FALSE);

        crm_xml_add(notify_data, F_STONITH_DEVICE, id);
        crm_xml_add_int(notify_data, F_STONITH_ACTIVE, g_hash_table_size(device_list));
		/* STONITHデバイスの削除NOTIFYを送信 */
        do_stonith_notify(call_options, op, rc, notify_data);
        free_xml(notify_data);

    } else if (crm_str_eq(op, STONITH_OP_LEVEL_ADD, TRUE)) {
		/* fencing_tolopogy levelの登録(STONITH_OP_LEVEL_ADD)メッセージの場合 */
        char *id = NULL;
        xmlNode *notify_data = create_xml_node(NULL, op);
		/* 登録用のxml("st_level")からtopologyハッシュテーブルに登録する */
        rc = stonith_level_register(request, &id);

        crm_xml_add(notify_data, F_STONITH_DEVICE, id);
        crm_xml_add_int(notify_data, F_STONITH_ACTIVE, g_hash_table_size(topology));
		/* fencing_tolopogy levelの登録NOTIFYを送信 */
        do_stonith_notify(call_options, op, rc, notify_data);
        free_xml(notify_data);

    } else if (crm_str_eq(op, STONITH_OP_LEVEL_DEL, TRUE)) {
		/* fencing_tolopogyの削除(STONITH_OP_LEVEL_DEL)メッセージの場合 */
        char *id = NULL;
        xmlNode *notify_data = create_xml_node(NULL, op);

        rc = stonith_level_remove(request, &id);

        crm_xml_add(notify_data, F_STONITH_DEVICE, id);
        crm_xml_add_int(notify_data, F_STONITH_ACTIVE, g_hash_table_size(topology));
		/* fencing_tolopogy levelの削除NOTIFYを送信 */
        do_stonith_notify(call_options, op, rc, notify_data);
        free_xml(notify_data);

    } else if (crm_str_eq(op, STONITH_OP_CONFIRM, TRUE)) {
		/* STONITH 実行非同期コマンドの作成 */
        async_command_t *cmd = create_async_command(request);
        /* 非同期応答メッセージ(固定部)の生成 */
        xmlNode *reply = stonith_construct_async_reply(cmd, NULL, NULL, 0);

        crm_xml_add(reply, F_STONITH_OPERATION, T_STONITH_NOTIFY);
        crm_notice("Broadcasting manual fencing confirmation for node %s", cmd->victim);
        send_cluster_message(NULL, crm_msg_stonith_ng, reply, FALSE);
		/* 非同期コマンドエリアの解放 */
        free_async_command(cmd);
        free_xml(reply);

    } else {
        crm_err("Unknown %s from %s", op, client ? client->name : remote_peer);
        crm_log_xml_warn(request, "UnknownOp");
    }

  done:

    /* Always reply unles the request is in process still.
     * If in progress, a reply will happen async after the request
     * processing is finished */
    if (rc != -EINPROGRESS) {
        crm_trace("Reply handling: %p %u %u %d %d %s", client, client?client->request_id:0,
                  id, is_set(call_options, st_opt_sync_call), call_options,
                  crm_element_value(request, F_STONITH_CALLOPTS));

        if (is_set(call_options, st_opt_sync_call)) {
            CRM_ASSERT(client == NULL || client->request_id == id);
        }
        /* 応答メッセージの送信 */
        reply = stonith_construct_reply(request, output, data, rc);
        stonith_send_reply(reply, call_options, remote_peer, client_id);
    }

    free(output);
    free_xml(data);
    free_xml(reply);

    return rc;
}
/* 応答メッセージ処理 */
static void
handle_reply(crm_client_t * client, xmlNode * request, const char *remote_peer)
{
    const char *op = crm_element_value(request, F_STONITH_OPERATION);

    if (crm_str_eq(op, STONITH_OP_QUERY, TRUE)) {
		/* STONITH_OP_QUERY応答メッセージ */
        process_remote_stonith_query(request);
    } else if (crm_str_eq(op, T_STONITH_NOTIFY, TRUE)) {
		/* T_STONITH_NOTIFY応答メッセージ */
        process_remote_stonith_exec(request);
    } else if (crm_str_eq(op, STONITH_OP_FENCE, TRUE)) {
		/* STONITH_OP_FENCE応答メッセージ(STONITH完了メッセージ、失敗メッセージ) */
        /* Reply to a complex fencing op */
        process_remote_stonith_exec(request);
    } else {
        crm_err("Unknown %s reply from %s", op, client ? client->name : remote_peer);
        crm_log_xml_warn(request, "UnknownOp");
    }
}

void
stonith_command(crm_client_t * client, uint32_t id, uint32_t flags, xmlNode * request,
                const char *remote_peer)
{
    int call_options = 0;
    int rc = 0;
    gboolean is_reply = FALSE;
    char *op = crm_element_value_copy(request, F_STONITH_OPERATION);

    /* F_STONITH_OPERATION can be overwritten in remote_op_done() with crm_xml_add()
     *
     * by 0x4C2E934: crm_xml_add (xml.c:377)
     * by 0x40C5E9: remote_op_done (remote.c:178)
     * by 0x40F1D3: process_remote_stonith_exec (remote.c:1084)
     * by 0x40AD4F: stonith_command (commands.c:1891)
     *
     */

    if (get_xpath_object("//" T_STONITH_REPLY, request, LOG_DEBUG_3)) {
        is_reply = TRUE;
    }

    crm_element_value_int(request, F_STONITH_CALLOPTS, &call_options);
    crm_debug("Processing %s%s %u from %s (%16x)", op, is_reply ? " reply" : "",
              id, client ? client->name : remote_peer, call_options);

    if (is_set(call_options, st_opt_sync_call)) {
        CRM_ASSERT(client == NULL || client->request_id == id);
    }

    if (is_reply) {
		/* 応答メッセージの場合 */
        handle_reply(client, request, remote_peer);
    } else {
		/* 要求メッセージの場合 */
        rc = handle_request(client, id, flags, request, remote_peer);
    }

    do_crm_log_unlikely(rc > 0 ? LOG_DEBUG : LOG_INFO, "Processed %s%s from %s: %s (%d)", op,
                        is_reply ? " reply" : "", client ? client->name : remote_peer,
                        rc > 0 ? "" : pcmk_strerror(rc), rc);

    free(op);
}
