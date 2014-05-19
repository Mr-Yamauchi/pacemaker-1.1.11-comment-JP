/*
 * Copyright (C) 2013 Andrew Beekhof <andrew@beekhof.net>
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

#include <glib.h>

#include <crm/msg_xml.h>
#include <crm/cluster.h>
#include <crm/cib.h>
#include <crm/cluster/internal.h>
#include <crm/cluster/election.h>
#include <crm/cib/internal.h>

#include <internal.h>

#define ATTRD_PROTOCOL_VERSION "1"

int last_cib_op_done = 0;
char *peer_writer = NULL;
GHashTable *attributes = NULL;
/* attributesハッシュテーブルデータ */
typedef struct attribute_s {
    char *uuid; /* TODO: Remove if at all possible */
    char *id;
    char *set;

    GHashTable *values;

    int update;
    int timeout_ms;
    bool changed;
    bool unknown_peer_uuids;
    mainloop_timer_t *timer;

    char *user;

} attribute_t;
/* attributesハッシュテーブルデータ内のvaluesハッシュテーブルデータ */
typedef struct attribute_value_s {
        uint32_t nodeid;
        gboolean is_remote;
        char *nodename;
        char *current;
        char *requested;
        char *stored;
} attribute_value_t;


void write_attribute(attribute_t *a);
void write_or_elect_attribute(attribute_t *a);
void attrd_peer_update(crm_node_t *peer, xmlNode *xml, bool filter);
void attrd_peer_sync(crm_node_t *peer, xmlNode *xml);
void attrd_peer_remove(const char *host, const char *source);
/* クラスタにメッセージを送信する */
static gboolean
send_attrd_message(crm_node_t * node, xmlNode * data)
{
    crm_xml_add(data, F_TYPE, T_ATTRD);
    crm_xml_add(data, F_ATTRD_IGNORE_LOCALLY, "atomic-version"); /* Tell older versions to ignore our messages */
    crm_xml_add(data, F_ATTRD_VERSION, ATTRD_PROTOCOL_VERSION);
    crm_xml_add_int(data, F_ATTRD_WRITER, election_state(writer));

    return send_cluster_message(node, crm_msg_attrd, data, TRUE);
}
/* dampenタイマーコールバック */
static gboolean
attribute_timer_cb(gpointer data)
{
    attribute_t *a = data;
    crm_trace("Dampen interval expired for %s in state %d", a->id, election_state(writer));
    /* 属性更新 or Electionの実行 */
    write_or_elect_attribute(a);
    return FALSE;
}
/* 属性データ解放 */
static void
free_attribute_value(gpointer data)
{
    attribute_value_t *v = data;

    free(v->nodename);
    free(v->current);
    free(v->requested);
    free(v->stored);
    free(v);
}
/* 属性データ解放 */
void
free_attribute(gpointer data)
{
    attribute_t *a = data;
    if(a) {
        free(a->id);
        free(a->set);
        free(a->uuid);
        free(a->user);

        mainloop_timer_del(a->timer);
        g_hash_table_destroy(a->values);

        free(a);
    }
}
/* 属性データから送信XMLデータを生成する */
xmlNode *
build_attribute_xml(
    xmlNode *parent, const char *name, const char *set, const char *uuid, unsigned int timeout_ms, const char *user,
    const char *peer, uint32_t peerid, const char *value)
{
    xmlNode *xml = create_xml_node(parent, __FUNCTION__);

    crm_xml_add(xml, F_ATTRD_ATTRIBUTE, name);
    crm_xml_add(xml, F_ATTRD_SET, set);
    crm_xml_add(xml, F_ATTRD_KEY, uuid);
    crm_xml_add(xml, F_ATTRD_USER, user);
    crm_xml_add(xml, F_ATTRD_HOST, peer);
    crm_xml_add_int(xml, F_ATTRD_HOST_ID, peerid);
    crm_xml_add(xml, F_ATTRD_VALUE, value);
    crm_xml_add_int(xml, F_ATTRD_DAMPEN, timeout_ms/1000);

    return xml;
}
/* 属性更新データを生成する */
static attribute_t *
create_attribute(xmlNode *xml)
{
    int dampen = 0;
    const char *value = crm_element_value(xml, F_ATTRD_DAMPEN);
    attribute_t *a = calloc(1, sizeof(attribute_t));
	/* 属性データをエリアにセット */
    a->id      = crm_element_value_copy(xml, F_ATTRD_ATTRIBUTE);
    a->set     = crm_element_value_copy(xml, F_ATTRD_SET);
    a->uuid    = crm_element_value_copy(xml, F_ATTRD_KEY);
    a->values = g_hash_table_new_full(crm_str_hash, g_str_equal, NULL, free_attribute_value);

#if ENABLE_ACL
    crm_trace("Performing all %s operations as user '%s'", a->id, a->user);
    a->user = crm_element_value_copy(xml, F_ATTRD_USER);
#endif

    if(value) {
        dampen = crm_get_msec(value);
        crm_trace("Created attribute %s with delay %dms (%s)", a->id, dampen, value);
    } else {
        crm_trace("Created attribute %s with no delay", a->id);
    }

    if(dampen > 0) {
		/* dampenが設定されている場合は、属性更新のタイマー（とコールバック）を仕掛ける */
        a->timeout_ms = dampen;
        a->timer = mainloop_timer_add(strdup(a->id), a->timeout_ms, FALSE, attribute_timer_cb, a);
    }
	/* attributesハッシュテーブルに生成したデータを登録 */
    g_hash_table_replace(attributes, a->id, a);
    return a;
}
/* IPCクライアントメッセージ処理 */
void
attrd_client_message(crm_client_t *client, xmlNode *xml)
{
    bool broadcast = FALSE;
    static int plus_plus_len = 5;
    const char *op = crm_element_value(xml, F_ATTRD_TASK);

    if(safe_str_eq(op, "peer-remove")) {
        const char *host = crm_element_value(xml, F_ATTRD_HOST);

        crm_info("Client %s is requesting all values for %s be removed", client->name, host);
        if(host) {
            broadcast = TRUE;
        }

    } else if(safe_str_eq(op, "update")) {
        attribute_t *a = NULL;
        attribute_value_t *v = NULL;
        char *key = crm_element_value_copy(xml, F_ATTRD_KEY);
        char *set = crm_element_value_copy(xml, F_ATTRD_SET);
        char *host = crm_element_value_copy(xml, F_ATTRD_HOST);
        const char *attr = crm_element_value(xml, F_ATTRD_ATTRIBUTE);
        const char *value = crm_element_value(xml, F_ATTRD_VALUE);
		/* 自ノードのattributeハッシュテーブルから更新属性を検索する */
        a = g_hash_table_lookup(attributes, attr);

        if(host == NULL) {
			/* attrd_updaterなどからの処理で、F_ATTRD_HOSTが未設定の場合に、F_ATTRD_HOSTとF_ATTRD_HOST_IDをセットする */
            crm_trace("Inferring host");
            host = strdup(attrd_cluster->uname);
            crm_xml_add(xml, F_ATTRD_HOST, host);
            crm_xml_add_int(xml, F_ATTRD_HOST_ID, attrd_cluster->nodeid);
        }

        if (value) {
            int offset = 1;
            int int_value = 0;
            int value_len = strlen(value);

            if (value_len < (plus_plus_len + 2)
                || value[plus_plus_len] != '+'
                || (value[plus_plus_len + 1] != '+' && value[plus_plus_len + 1] != '=')) {
                goto send;
            }

            if(a) {
				/* 属性情報が存在する場合は、属性のハッシュテーブル(a->value)から対象ホストのデータを検索する */
                v = g_hash_table_lookup(a->values, host);
            }
            if(v) {
				/* 検索出来た場合は、現在の値を属性値を数値化 */
                int_value = char2score(v->current);
            }

            if (value[plus_plus_len + 1] != '+') {
                const char *offset_s = value + (plus_plus_len + 2);

                offset = char2score(offset_s);
            }
            /* 現在値（もしくは初期値(０)に値を加算 */
            int_value += offset;

            if (int_value > INFINITY) {
                int_value = INFINITY;
            }

            crm_info("Expanded %s=%s to %d", attr, value, int_value);
            crm_xml_add_int(xml, F_ATTRD_VALUE, int_value);
        }

      send:

        if(peer_writer == NULL && election_state(writer) != election_in_progress) {
            crm_info("Starting an election to determine the writer");
            /* ELECTION開始 */
            election_vote(writer);
        }

        crm_info("Broadcasting %s[%s] = %s%s", attr, host, value, election_state(writer) == election_won?" (writer)":"");
        broadcast = TRUE;

        free(key);
        free(set);
        free(host);
    }

    if(broadcast) {
        /* クラスタにメッセージを送信する */
        send_attrd_message(NULL, xml);
    }
}
/* CPGメッセージ処理 */
void
attrd_peer_message(crm_node_t *peer, xmlNode *xml)
{
    int peer_state = 0;
    const char *v = crm_element_value(xml, F_ATTRD_VERSION);
    const char *op = crm_element_value(xml, F_ATTRD_TASK);
    const char *election_op = crm_element_value(xml, F_CRM_TASK);

    if(election_op) {
		/* ELECTIONメッセージの場合 */
        enum election_result rc = 0;

        crm_xml_add(xml, F_CRM_HOST_FROM, peer->uname);
        /* 受信メッセージからELECTION状態を確認して、自ノードのELECTION状態を取得 */
        rc = election_count_vote(writer, xml, TRUE);
        switch(rc) {
            case election_start:
                free(peer_writer);
                peer_writer = NULL;
                election_vote(writer);
                break;
            case election_lost:
                free(peer_writer);
                peer_writer = strdup(peer->uname);
                break;
            default:
            	/* ELECTIONの完了チェック */
            	/* election_wonになれるノードはここから、attrd_electin_cbにてelection_wonに遷移 */
                election_check(writer);
                break;
        }
        return;

    } else if(v == NULL) {
		/* 旧タイプのattrd通信の場合 */
        /* From the non-atomic version */
        if(safe_str_eq(op, "update")) {
            const char *name = crm_element_value(xml, F_ATTRD_ATTRIBUTE);

            crm_trace("Compatibility update of %s from %s", name, peer->uname);
            attrd_peer_update(peer, xml, FALSE);

        } else if(safe_str_eq(op, "flush")) {
            const char *name = crm_element_value(xml, F_ATTRD_ATTRIBUTE);
            attribute_t *a = g_hash_table_lookup(attributes, name);

            if(a) {
                crm_trace("Compatibility write-out of %s for %s from %s", a->id, op, peer->uname);
                /* 属性更新 or Electionの実行 */
                write_or_elect_attribute(a);
            }

        } else if(safe_str_eq(op, "refresh")) {
            GHashTableIter aIter;
            attribute_t *a = NULL;

            g_hash_table_iter_init(&aIter, attributes);
            while (g_hash_table_iter_next(&aIter, NULL, (gpointer *) & a)) {
                crm_trace("Compatibility write-out of %s for %s from %s", a->id, op, peer->uname);
                /* 属性更新 or Electionの実行 */
                write_or_elect_attribute(a);
            }
        }
    }
	/* 送信元のELECTION状態を取得する */
    crm_element_value_int(xml, F_ATTRD_WRITER, &peer_state);
    if(election_state(writer) == election_won
       && peer_state == election_won
       && safe_str_neq(peer->uname, attrd_cluster->uname)) {
		/* 受信データが他ノードからのelection_wonのメッセージだった場合 */
		/* -- 自ノードがelection_wonで他ノードからもelection_wonを受信した */
        crm_notice("Detected another attribute writer: %s", peer->uname);
        /* 再ELECTIONを実行 */
        election_vote(writer);

    } else if(peer_state == election_won) {
		/* 上記のif以外で、自ノードもしくは、他ノードからelection_wonを受信した場合 */
        if(peer_writer == NULL) {
			/* 送信元をpeer_writerに設定する */
            peer_writer = strdup(peer->uname);
            crm_notice("Recorded attribute writer: %s", peer->uname);

        } else if(safe_str_neq(peer->uname, peer_writer)) {
			/* peer_writerが変わった場合は、送信元をpeer_writerに再設定する */
            crm_notice("Recorded new attribute writer: %s (was %s)", peer->uname, peer_writer);
            free(peer_writer);
            peer_writer = strdup(peer->uname);
        }
    }

    if(safe_str_eq(op, "update")) {
        attrd_peer_update(peer, xml, FALSE);	/* "update"メッセージ処理 */

    } else if(safe_str_eq(op, "sync")) {
        attrd_peer_sync(peer, xml);				/* "sync"メッセージ受信処理("sync-response"メッセージを応答する) */

    } else if(safe_str_eq(op, "peer-remove")) { /* "peer-remove"メッセージ処理 */
        const char *host = crm_element_value(xml, F_ATTRD_HOST);
        attrd_peer_remove(host, peer->uname);

    } else if(safe_str_eq(op, "sync-response")	/* "sync-response"メッセージ処理 */
              && safe_str_neq(peer->uname, attrd_cluster->uname)) {
        xmlNode *child = NULL;

        crm_notice("Processing %s from %s", op, peer->uname);
        for (child = __xml_first_child(xml); child != NULL; child = __xml_next(child)) {
            attrd_peer_update(peer, child, TRUE);
        }
    }
}
/* "sync"メッセージ受信処理("sync-response"メッセージを応答する) */
void
attrd_peer_sync(crm_node_t *peer, xmlNode *xml)
{
    GHashTableIter aIter;
    GHashTableIter vIter;

    attribute_t *a = NULL;
    attribute_value_t *v = NULL;
    /* 応答用xmlノードを生成 */
    xmlNode *sync = create_xml_node(NULL, __FUNCTION__);
	/* "sync-response"メッセージを積み上げる */
    crm_xml_add(sync, F_ATTRD_TASK, "sync-response");
	/* すべてのattributesハッシュテーブルを処理する*/
    g_hash_table_iter_init(&aIter, attributes);
    while (g_hash_table_iter_next(&aIter, NULL, (gpointer *) & a)) {
        /* attributesハッシュテーブルのvaluesハッシュテーブルをすべて処理する */
        g_hash_table_iter_init(&vIter, a->values);
        while (g_hash_table_iter_next(&vIter, NULL, (gpointer *) & v)) {
            crm_debug("Syncing %s[%s] = %s to %s", a->id, v->nodename, v->current, peer?peer->uname:"everyone");
            /* 属性データから送信XMLデータを生成する */
            build_attribute_xml(sync, a->id, a->set, a->uuid, a->timeout_ms, a->user, v->nodename, v->nodeid, v->current);
        }
    }

    crm_debug("Syncing values to %s", peer?peer->uname:"everyone");
    /* クラスタに"sync-response"メッセージを送信する */
    send_attrd_message(peer, sync);
    free_xml(sync);
}

void
attrd_peer_remove(const char *host, const char *source)
{
    attribute_t *a = NULL;
    GHashTableIter aIter;

    crm_notice("Removing all %s attributes for %s", host, source);
    if(host == NULL) {
        return;
    }
	/* attributesハッシュテーブル内のすべての属性データを処理する */
    g_hash_table_iter_init(&aIter, attributes);
    while (g_hash_table_iter_next(&aIter, NULL, (gpointer *) & a)) {
		/* 属性データのvaluesハッシュテーブルから対象ホストのデータを削除する */
        if(g_hash_table_remove(a->values, host)) {
            crm_debug("Removed %s[%s] for %s", a->id, host, source);
        }
    }

    /* if this matches a remote peer, it will be removed from the cache */
    crm_remote_peer_cache_remove(host);
}
/* "update","sync-response"メッセージ処理 */
void
attrd_peer_update(crm_node_t *peer, xmlNode *xml, bool filter)
{
    bool changed = FALSE;
    attribute_value_t *v = NULL;

    const char *host = crm_element_value(xml, F_ATTRD_HOST);
    const char *attr = crm_element_value(xml, F_ATTRD_ATTRIBUTE);
    const char *value = crm_element_value(xml, F_ATTRD_VALUE);
	/* 自ノードのattributesハッシュテーブルに属性が存在するか検索する */
    attribute_t *a = g_hash_table_lookup(attributes, attr);

    if(a == NULL) {
		/* 存在しない場合は、属性更新データを生成して、attributesハッシュテーブルに属性を追加する */
        a = create_attribute(xml);
    }
	/* attributesハッシュテーブルデータのvaluesハッシュテーブルにホストが存在するか検索する */
    v = g_hash_table_lookup(a->values, host);

    if(v == NULL) {
		/* 属性データに対象ホストが存在しない場合は、valuesハッシュテーブルに属性を追加する */
        crm_trace("Setting %s[%s] to %s from %s", attr, host, value, peer->uname);
        v = calloc(1, sizeof(attribute_value_t));
        if(value) {
            v->current = strdup(value);
        }
        v->nodename = strdup(host);
        crm_element_value_int(xml, F_ATTRD_IS_REMOTE, &v->is_remote);
        /* 属性データのvaluesハッシュテーブルに生成したデータを登録する */
        g_hash_table_replace(a->values, v->nodename, v);

        if (v->is_remote == TRUE) {
            crm_remote_peer_cache_add(host);
        }

        changed = TRUE;

    } else if(filter
              && safe_str_neq(v->current, value)
              && safe_str_eq(host, attrd_cluster->uname)) {
		/* "sync-response"メッセージ(filterがTRUE)で、*/
		/* 自ノードの値変更の場合で、受信した値と保持している値が異なる場合は、*/
		/* 値を変更せずに、クラスタに現在の値を"sync-response"メッセージで送信する */
		/* ---他ノード保持値が間違っている場合の同期通信 ---*/
        xmlNode *sync = create_xml_node(NULL, __FUNCTION__);
        crm_notice("%s[%s]: local value '%s' takes priority over '%s' from %s",
                   a->id, host, v->current, value, peer->uname);

        crm_xml_add(sync, F_ATTRD_TASK, "sync-response");
        v = g_hash_table_lookup(a->values, host);
		/* 属性データから送信XMLデータを生成する */
        build_attribute_xml(sync, a->id, a->set, a->uuid, a->timeout_ms, a->user, v->nodename, v->nodeid, v->current);

        crm_xml_add_int(sync, F_ATTRD_WRITER, election_state(writer));
        /* クラスタにメッセージを送信する */
        send_attrd_message(peer, sync);
        free_xml(sync);

    } else if(safe_str_neq(v->current, value)) {
		/* その他の場合で、ホストが存在した場合で、値が変わった場合は、値を変更する */
		/* --他ノードのデータの場合で値が異なる場合が該当 --- */
        crm_info("Setting %s[%s]: %s -> %s from %s", attr, host, v->current, value, peer->uname);
        free(v->current);
        if(value) {
            v->current = strdup(value);
        } else {
            v->current = NULL;
        }
        changed = TRUE;

    } else {
		/* その他の場合(ホストと値が変わらない場合) */
        crm_trace("Unchanged %s[%s] from %s is %s", attr, host, peer->uname, value);
    }

    a->changed |= changed;

    /* this only involves cluster nodes. */
    if(v->nodeid == 0 && (v->is_remote == FALSE)) {
		/* 初期属性作成時で、リモート以外の場合 */
		/* 2回目の更新では、v->nodeidが取得設定されているので実行されないので注意 */
        if(crm_element_value_int(xml, F_ATTRD_HOST_ID, (int*)&v->nodeid) == 0) {
			/* 受信メッセージのF_ATTRD_HOST_IDをv->nodeidに取得する */
            /* Create the name/id association */
            crm_node_t *peer = crm_get_peer(v->nodeid, host);
            crm_trace("We know %s's node id now: %s", peer->uname, peer->uuid);
            if(election_state(writer) == election_won) {
				/* 自ノードがelection_wonなら、属性を更新する */
                write_attributes(FALSE, TRUE);
                return;
            }
        }
    }

    if(changed) {
        if(a->timer) {
            crm_trace("Delayed write out (%dms) for %s", a->timeout_ms, a->id);
            mainloop_timer_start(a->timer);
        } else {
			/* 属性更新 or Electionの実行 */
            write_or_elect_attribute(a);
        }
    }
}
/* 属性更新 or Electionの実行 */
void
write_or_elect_attribute(attribute_t *a)
{
    enum election_result rc = election_state(writer);
    if(rc == election_won) {
		/* 属性を更新する */
        write_attribute(a);

    } else if(rc == election_in_progress) {
		/* ELECTION中の場合は更新しない */
        crm_trace("Election in progress to determine who will write out %s", a->id);

    } else if(peer_writer == NULL) {
		/* peer_writerが決定していない場合は、ELECTIONする */
        crm_info("Starting an election to determine who will write out %s", a->id);
        election_vote(writer);

    } else {
        crm_trace("%s will write out %s, we are in state %d", peer_writer, a->id, rc);
    }
}
/* ELECTIONコールバック */
gboolean
attrd_election_cb(gpointer user_data)
{
    crm_trace("Election complete");
	/* WRITERに自ノード名をセット */
    free(peer_writer);
    peer_writer = strdup(attrd_cluster->uname);

    /* Update the peers after an election */
    /* "sync"メッセージ受信処理("sync-response"メッセージを応答する) */
    attrd_peer_sync(NULL, NULL);

    /* Update the CIB after an election */
    write_attributes(TRUE, FALSE);
    return FALSE;
}

/* クラスタ接続状態変更コールバック */
void
attrd_peer_change_cb(enum crm_status_type kind, crm_node_t *peer, const void *data)
{
    if(election_state(writer) == election_won
        && kind == crm_status_nstate
        && safe_str_eq(peer->state, CRM_NODE_MEMBER)) {
		/* "sync"メッセージ受信処理("sync-response"メッセージを応答する) */
        attrd_peer_sync(peer, NULL);

    } else if(kind == crm_status_nstate
              && safe_str_neq(peer->state, CRM_NODE_MEMBER)) {

        attrd_peer_remove(peer->uname, __FUNCTION__);
        if(peer_writer && safe_str_eq(peer->uname, peer_writer)) {
			/* peer_writerのノードがクラスタから消失した場合 */
			/* peer_writerをクリアして、再electionを指示 */
            free(peer_writer);
            peer_writer = NULL;
            crm_notice("Lost attribute writer %s", peer->uname);
        }

    } else if(kind == crm_status_processes) {
        if(is_set(peer->processes, crm_proc_cpg)) {
            crm_update_peer_state(__FUNCTION__, peer, CRM_NODE_MEMBER, 0);
        } else {
            crm_update_peer_state(__FUNCTION__, peer, CRM_NODE_LOST, 0);
        }
    }
}
/* CIB更新コールバック */
static void
attrd_cib_callback(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    int level = LOG_ERR;
    GHashTableIter iter;
    const char *peer = NULL;
    attribute_value_t *v = NULL;

    char *name = user_data;
    attribute_t *a = g_hash_table_lookup(attributes, name);

    if(a == NULL) {
		/* 存在しない属性のCIB更新コールバック実行だった場合は、無視する */
        crm_info("Attribute %s no longer exists", name);
        goto done;
    }

    a->update = 0;
    if (rc == pcmk_ok && call_id < 0) {
        rc = call_id;
    }

    switch (rc) {
        case pcmk_ok:
            level = LOG_INFO;
            last_cib_op_done = call_id;
            break;
        case -pcmk_err_diff_failed:    /* When an attr changes while the CIB is syncing */
        case -ETIME:           /* When an attr changes while there is a DC election */
        case -ENXIO:           /* When an attr changes while the CIB is syncing a
                                *   newer config from a node that just came up
                                */
            level = LOG_WARNING;
            break;
    }
	/* 更新ログを出力 */
    do_crm_log(level, "Update %d for %s: %s (%d)", call_id, name, pcmk_strerror(rc), rc);
	/* attributesテーブル内の更新データのvaluesハッシュテーブルをすべて処理する */
    g_hash_table_iter_init(&iter, a->values);
    while (g_hash_table_iter_next(&iter, (gpointer *) & peer, (gpointer *) & v)) {
        crm_notice("Update %d for %s[%s]=%s: %s (%d)", call_id, a->id, peer, v->requested, pcmk_strerror(rc), rc);

        if(rc == pcmk_ok) {
			/* 更新OKだった場合は、requested値で、stored値を更新し、requested値をクリア*/
            free(v->stored);
            v->stored = v->requested;
            v->requested = NULL;

        } else {
			/* 更新失敗だった場合は、requested値をクリアし、再実行の為に、changedをTRUEにセット */
            free(v->requested);
            v->requested = NULL;
            a->changed = TRUE; /* Attempt write out again */
        }
    }
  done:
    free(name);
    if(a && a->changed && election_state(writer) == election_won) {
		/* 存在する更新データの更新通知で、再実行が必要で、自ノードがelection_wonの場合は、属性を再更新する */
        write_attribute(a);
    }
}
/* すべての属性データ(attributesハッシュテーブル内の全データ)を更新する */
void
write_attributes(bool all, bool peer_discovered)
{
    GHashTableIter iter;
    attribute_t *a = NULL;

    g_hash_table_iter_init(&iter, attributes);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *) & a)) {
        if (peer_discovered && a->unknown_peer_uuids) {
            /* a new peer uuid has been discovered, try writing this attribute again. */
            a->changed = TRUE;
        }

        if(all || a->changed) {
            write_attribute(a);
        } else {
            crm_debug("Skipping unchanged attribute %s", a->id);
        }
    }
}
/* cibの属性更新データを生成する */
static void
build_update_element(xmlNode *parent, attribute_t *a, const char *nodeid, const char *value)
{
    char *set = NULL;
    char *uuid = NULL;
    xmlNode *xml_obj = NULL;

    if(a->set) {
        set = g_strdup(a->set);
    } else {
        set = g_strdup_printf("%s-%s", XML_CIB_TAG_STATUS, nodeid);
    }

    if(a->uuid) {
        uuid = g_strdup(a->uuid);
    } else {
        int lpc;
        uuid = g_strdup_printf("%s-%s", set, a->id);

        /* Minimal attempt at sanitizing automatic IDs */
        for (lpc = 0; uuid[lpc] != 0; lpc++) {
            switch (uuid[lpc]) {
                case ':':
                    uuid[lpc] = '.';
            }
        }
    }

    xml_obj = create_xml_node(parent, XML_CIB_TAG_STATE);
    crm_xml_add(xml_obj, XML_ATTR_ID, nodeid);

    xml_obj = create_xml_node(xml_obj, XML_TAG_TRANSIENT_NODEATTRS);
    crm_xml_add(xml_obj, XML_ATTR_ID, nodeid);

    xml_obj = create_xml_node(xml_obj, XML_TAG_ATTR_SETS);
    crm_xml_add(xml_obj, XML_ATTR_ID, set);

    xml_obj = create_xml_node(xml_obj, XML_CIB_TAG_NVPAIR);
    crm_xml_add(xml_obj, XML_ATTR_ID, uuid);
    crm_xml_add(xml_obj, XML_NVPAIR_ATTR_NAME, a->id);

    if(value) {
        crm_xml_add(xml_obj, XML_NVPAIR_ATTR_VALUE, value);

    } else {
        crm_xml_add(xml_obj, XML_NVPAIR_ATTR_VALUE, "");
        crm_xml_add(xml_obj, "__delete__", XML_NVPAIR_ATTR_VALUE);
    }

    g_free(uuid);
    g_free(set);
}
/* 単一属性(attributesハッシュテーブル内のvalueハッシュテーブル)を更新する */
void
write_attribute(attribute_t *a)
{
    int updates = 0;
    xmlNode *xml_top = NULL;
    attribute_value_t *v = NULL;
    GHashTableIter iter;
    enum cib_call_options flags = cib_quorum_override;

    if (a == NULL) {
        return;

    } else if (the_cib == NULL) {
        crm_info("Write out of '%s' delayed: cib not connected", a->id);
        return;

    } else if(a->update && a->update < last_cib_op_done) {
        crm_info("Write out of '%s' continuing: update %d considered lost", a->id, a->update);

    } else if(a->update) {
        crm_info("Write out of '%s' delayed: update %d in progress", a->id, a->update);
        return;

    } else if(mainloop_timer_running(a->timer)) {
        crm_info("Write out of '%s' delayed: timer is running", a->id);
        return;
    }

    a->changed = FALSE;
    a->unknown_peer_uuids = FALSE;
    xml_top = create_xml_node(NULL, XML_CIB_TAG_STATUS);
	/* すべてのノードの属性情報(valuesハッシュテーブルデータ)を処理する */
    g_hash_table_iter_init(&iter, a->values);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *) & v)) {
        crm_node_t *peer = crm_get_peer_full(v->nodeid, v->nodename, CRM_GET_PEER_REMOTE|CRM_GET_PEER_CLUSTER);

        if(peer && peer->id && v->nodeid == 0) {
			/* nodeidを更新する */
            crm_trace("Updating value's nodeid");
            v->nodeid = peer->id;
        }

        if (peer == NULL) {
			/* クラスタに存在しないノードの属性は積み上げない */
            /* If the user is trying to set an attribute on an unknown peer, ignore it. */
            crm_notice("Update error (peer not found): %s[%s]=%s failed (host=%p)", v->nodename, a->id, v->current, peer);

        } else if (peer->uuid == NULL) {
			/* UUIDが決定していないノードの属性は積み上げない */
            /* peer is found, but we don't know the uuid yet. Wait until we discover a new uuid before attempting to write */
            a->unknown_peer_uuids = FALSE;
            crm_notice("Update error (unknown peer uuid, retry will be attempted once uuid is discovered): %s[%s]=%s failed (host=%p)", v->nodename, a->id, v->current, peer);

        } else {
			/* ノードの更新属性データを積み上げる */
            crm_debug("Update: %s[%s]=%s (%s %u %u %s)", v->nodename, a->id, v->current, peer->uuid, peer->id, v->nodeid, peer->uname);
            /* cibの属性更新データを生成する */
            build_update_element(xml_top, a, peer->uuid, v->current);
            /* 更新データカウントアップ */
            updates++;

            free(v->requested);
            v->requested = NULL;

            if(v->current) {
                v->requested = strdup(v->current);

            } else {
                /* Older versions don't know about the cib_mixed_update flag
                 * Make sure it goes to the local cib which does
                 */
                flags |= cib_mixed_update|cib_scope_local;
            }
        }
    }

    if(updates) {	/* 更新データがあった場合 */
        crm_log_xml_trace(xml_top, __FUNCTION__);
		/* 更新データでCIBデータを更新 */
        a->update = cib_internal_op(the_cib, CIB_OP_MODIFY, NULL, XML_CIB_TAG_STATUS, xml_top, NULL,
                                    flags, a->user);

        crm_notice("Sent update %d with %d changes for %s, id=%s, set=%s",
                   a->update, updates, a->id, a->uuid ? a->uuid : "<n/a>", a->set);
		/* CIB更新コールバックをセット */
        the_cib->cmds->register_callback(
            the_cib, a->update, 120, FALSE, strdup(a->id), "attrd_cib_callback", attrd_cib_callback);
    }
    free_xml(xml_top);
}
