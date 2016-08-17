#include "bitcoin/pullpush.h"
#include "commit_tx.h"
#include "db.h"
#include "htlc.h"
#include "lightningd.h"
#include "log.h"
#include "names.h"
#include "netaddr.h"
#include "routing.h"
#include "secrets.h"
#include "utils.h"
#include "wallet.h"
#include <ccan/array_size/array_size.h>
#include <ccan/cast/cast.h>
#include <ccan/mem/mem.h>
#include <ccan/str/hex/hex.h>
#include <ccan/tal/str/str.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <unistd.h>

#define DB_FILE "lightning.sqlite3"

/* They don't use stdint types. */
#define PRIuSQLITE64 "llu"

struct db {
	bool in_transaction;
	sqlite3 *sql;
};

static void close_db(struct db *db)
{
	sqlite3_close(db->sql);
}

/* We want a string, not an 'unsigned char *' thanks! */
static const char *sqlite3_column_str(sqlite3_stmt *stmt, int iCol)
{
	return cast_signed(const char *, sqlite3_column_text(stmt, iCol));
}

#define SQL_PUBKEY	"BINARY(33)"
#define SQL_PRIVKEY	"BINARY(32)"
#define SQL_SIGNATURE	"BINARY(64)"
#define SQL_TXID	"BINARY(32)"
#define SQL_RHASH	"BINARY(32)"
#define SQL_SHA256	"BINARY(32)"
#define SQL_R		"BINARY(32)"

/* 8 + 4 + (8 + 32) * (64 + 1) */
#define SHACHAIN_SIZE	2612
#define SQL_SHACHAIN	"BINARY(2612)"

/* FIXME: Should be fixed size. */
#define SQL_ROUTING	"BLOB"

static char *PRINTF_FMT(3,4)
	db_exec(const tal_t *ctx,
		struct lightningd_state *dstate, const char *fmt, ...)
{
	va_list ap;
	char *cmd, *errmsg;
	int err;

	va_start(ap, fmt);
	cmd = tal_vfmt(ctx, fmt, ap);
	va_end(ap);

	err = sqlite3_exec(dstate->db->sql, cmd, NULL, NULL, &errmsg);
	if (err != SQLITE_OK) {
		char *e = tal_fmt(ctx, "%s:%s:%s",
				  sqlite3_errstr(err), cmd, errmsg);
		sqlite3_free(errmsg);
		tal_free(cmd);
		return e;
	}
	tal_free(cmd);
	return NULL;
}

static char *sql_hex_or_null(const tal_t *ctx, const void *buf, size_t len)
{
	char *r;

	if (!buf)
		return "NULL";
	r = tal_arr(ctx, char, 3 + hex_str_size(len));
	r[0] = 'x';
	r[1] = '\'';
	hex_encode(buf, len, r+2, hex_str_size(len));
	r[2+hex_str_size(len)-1] = '\'';
	r[2+hex_str_size(len)] = '\0';
	return r;
}

static void from_sql_blob(sqlite3_stmt *stmt, int idx, void *p, size_t n)
{
	if (sqlite3_column_bytes(stmt, idx) != n)
		fatal("db:wrong bytes %i not %zu",
		      sqlite3_column_bytes(stmt, idx), n);
	memcpy(p, sqlite3_column_blob(stmt, idx), n);
}

static u8 *tal_sql_blob(const tal_t *ctx, sqlite3_stmt *stmt, int idx)
{
	u8 *p;

	if (sqlite3_column_type(stmt, idx) == SQLITE_NULL)
		return NULL;

	p = tal_arr(ctx, u8, sqlite3_column_bytes(stmt, idx));
	from_sql_blob(stmt, idx, p, tal_count(p));
	return p;
}

static void pubkey_from_sql(secp256k1_context *secpctx,
			    sqlite3_stmt *stmt, int idx, struct pubkey *pk)
{
	if (!pubkey_from_der(secpctx, sqlite3_column_blob(stmt, idx),
			     sqlite3_column_bytes(stmt, idx), pk))
		fatal("db:bad pubkey length %i",
		      sqlite3_column_bytes(stmt, idx));
}

static void sha256_from_sql(sqlite3_stmt *stmt, int idx, struct sha256 *sha)
{
	from_sql_blob(stmt, idx, sha, sizeof(*sha));
}

static void sig_from_sql(secp256k1_context *secpctx,
			 sqlite3_stmt *stmt, int idx,
			 struct bitcoin_signature *sig)
{
	u8 compact[64];

	from_sql_blob(stmt, idx, compact, sizeof(compact));
	if (secp256k1_ecdsa_signature_parse_compact(secpctx, &sig->sig.sig,
						    compact) != 1)
		fatal("db:bad signature blob");
	sig->stype = SIGHASH_ALL;
}

static char *sig_to_sql(const tal_t *ctx,
			secp256k1_context *secpctx,
			const struct bitcoin_signature *sig)
{
	u8 compact[64];

	if (!sig)
		return sql_hex_or_null(ctx, NULL, 0);

	assert(sig->stype == SIGHASH_ALL);
	secp256k1_ecdsa_signature_serialize_compact(secpctx, compact,
						    &sig->sig.sig);
	return sql_hex_or_null(ctx, compact, sizeof(compact));
}

static void db_load_wallet(struct lightningd_state *dstate)
{
	int err;
	sqlite3_stmt *stmt;

	err = sqlite3_prepare_v2(dstate->db->sql, "SELECT * FROM wallet;", -1,
				 &stmt, NULL);

	if (err != SQLITE_OK)
		fatal("db_load_wallet:prepare gave %s:%s",
		      sqlite3_errstr(err), sqlite3_errmsg(dstate->db->sql));

	while ((err = sqlite3_step(stmt)) != SQLITE_DONE) {
		struct privkey privkey;
		if (err != SQLITE_ROW)
			fatal("db_load_wallet:step gave %s:%s",
			      sqlite3_errstr(err),
			      sqlite3_errmsg(dstate->db->sql));
		if (sqlite3_column_count(stmt) != 1)
			fatal("db_load_wallet:step gave %i cols, not 1",
			      sqlite3_column_count(stmt));
		from_sql_blob(stmt, 0, &privkey, sizeof(privkey));
		if (!restore_wallet_address(dstate, &privkey))
			fatal("db_load_wallet:bad privkey");
	}
	err = sqlite3_finalize(stmt);
	if (err != SQLITE_OK)
		fatal("db_load_wallet:finalize gave %s:%s",
		      sqlite3_errstr(err),
		      sqlite3_errmsg(dstate->db->sql));
}

void db_add_wallet_privkey(struct lightningd_state *dstate,
			   const struct privkey *privkey)
{
	char *ctx = tal(dstate, char);
	char *err;

	log_debug(dstate->base_log, "%s", __func__);
	err = db_exec(ctx, dstate,
		      "INSERT INTO wallet VALUES (x'%s');",
		      tal_hexstr(ctx, privkey, sizeof(*privkey)));
	if (err)
		fatal("db_add_wallet_privkey:%s", err);
}

static void load_peer_address(struct peer *peer)
{
	int err;
	sqlite3_stmt *stmt;
	sqlite3 *sql = peer->dstate->db->sql;
	char *ctx = tal(peer, char);
	const char *select;
	bool addr_set = false;

	select = tal_fmt(ctx,
			 "SELECT * FROM peer_address WHERE peer = x'%s';",
			 pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id));

	err = sqlite3_prepare_v2(sql, select, -1, &stmt, NULL);
	if (err != SQLITE_OK)
		fatal("load_peer_address:prepare gave %s:%s",
		      sqlite3_errstr(err), sqlite3_errmsg(sql));

	while ((err = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (err != SQLITE_ROW)
			fatal("load_peer_address:step gave %s:%s",
			      sqlite3_errstr(err), sqlite3_errmsg(sql));
		if (addr_set)
			fatal("load_peer_address: two addresses for '%s'",
			      select);
		if (!netaddr_from_blob(sqlite3_column_blob(stmt, 1),
				       sqlite3_column_bytes(stmt, 1),
				       &peer->addr))
			fatal("load_peer_address: unparsable addresses for '%s'",
			      select);
		addr_set = true;
		peer->log = new_log(peer, peer->dstate->log_record, "%s%s:",
				    log_prefix(peer->dstate->base_log),
				    netaddr_name(peer, &peer->addr));
	}

	if (!addr_set)
		fatal("load_peer_address: no addresses for '%s'", select);

	tal_free(ctx);
}

static void load_peer_secrets(struct peer *peer)
{
	int err;
	sqlite3_stmt *stmt;
	sqlite3 *sql = peer->dstate->db->sql;
	char *ctx = tal(peer, char);
	const char *select;
	bool secrets_set = false;

	select = tal_fmt(ctx,
			 "SELECT * FROM peer_secrets WHERE peer = x'%s';",
			 pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id));

	err = sqlite3_prepare_v2(sql, select, -1, &stmt, NULL);
	if (err != SQLITE_OK)
		fatal("load_peer_secrets:prepare gave %s:%s",
		      sqlite3_errstr(err), sqlite3_errmsg(sql));

	while ((err = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (err != SQLITE_ROW)
			fatal("load_peer_secrets:step gave %s:%s",
			      sqlite3_errstr(err), sqlite3_errmsg(sql));
		if (secrets_set)
			fatal("load_peer_secrets: two secrets for '%s'",
			      select);
		peer_set_secrets_from_db(peer,
					 sqlite3_column_blob(stmt, 1),
					 sqlite3_column_bytes(stmt, 1),
					 sqlite3_column_blob(stmt, 2),
					 sqlite3_column_bytes(stmt, 2),
					 sqlite3_column_blob(stmt, 3),
					 sqlite3_column_bytes(stmt, 3));
		secrets_set = true;
	}

	if (!secrets_set)
		fatal("load_peer_secrets: no secrets for '%s'", select);
	tal_free(ctx);
}

static void load_peer_anchor(struct peer *peer)
{
	int err;
	sqlite3_stmt *stmt;
	sqlite3 *sql = peer->dstate->db->sql;
	char *ctx = tal(peer, char);
	const char *select;
	bool anchor_set = false;

	/* CREATE TABLE anchors (peer "SQL_PUBKEY", txid "SQL_TXID", idx INT, amount INT, ok_depth INT, min_depth INT, bool ours); */
	select = tal_fmt(ctx,
			 "SELECT * FROM anchors WHERE peer = x'%s';",
			 pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id));

	err = sqlite3_prepare_v2(sql, select, -1, &stmt, NULL);
	if (err != SQLITE_OK)
		fatal("load_peer_anchor:prepare gave %s:%s",
		      sqlite3_errstr(err), sqlite3_errmsg(sql));

	while ((err = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (err != SQLITE_ROW)
			fatal("load_peer_anchor:step gave %s:%s",
			      sqlite3_errstr(err), sqlite3_errmsg(sql));
		if (anchor_set)
			fatal("load_peer_anchor: two anchors for '%s'",
			      select);
		from_sql_blob(stmt, 1,
			      &peer->anchor.txid, sizeof(peer->anchor.txid));
		peer->anchor.index = sqlite3_column_int64(stmt, 2);
		peer->anchor.satoshis = sqlite3_column_int64(stmt, 3);
		peer->anchor.ours = sqlite3_column_int(stmt, 6);

		/* FIXME: Do timeout! */
		peer_watch_anchor(peer,
				  sqlite3_column_int(stmt, 4),
				  BITCOIN_ANCHOR_DEPTHOK, INPUT_NONE);
		peer->anchor.min_depth = sqlite3_column_int(stmt, 5);
		anchor_set = true;
	}

	if (!anchor_set)
		fatal("load_peer_anchor: no anchor for '%s'", select);
	tal_free(ctx);
}

static void load_peer_visible_state(struct peer *peer)
{
	int err;
	sqlite3_stmt *stmt;
	sqlite3 *sql = peer->dstate->db->sql;
	char *ctx = tal(peer, char);
	const char *select;
	bool visible_set = false;

	/* "CREATE TABLE their_visible_state (peer "SQL_PUBKEY", offered_anchor BOOLEAN, commitkey "SQL_PUBKEY", finalkey "SQL_PUBKEY", locktime INT, mindepth INT, commit_fee_rate INT, next_revocation_hash "SQL_SHA256", PRIMARY KEY(peer)); */
	select = tal_fmt(ctx,
			 "SELECT * FROM their_visible_state WHERE peer = x'%s';",
			 pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id));

	err = sqlite3_prepare_v2(sql, select, -1, &stmt, NULL);
	if (err != SQLITE_OK)
		fatal("load_peer_visible_state:prepare gave %s:%s",
		      sqlite3_errstr(err), sqlite3_errmsg(sql));

	while ((err = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (err != SQLITE_ROW)
			fatal("load_peer_visible_state:step gave %s:%s",
			      sqlite3_errstr(err), sqlite3_errmsg(sql));

		if (sqlite3_column_count(stmt) != 8)
			fatal("load_peer_visible_state:step gave %i cols, not 8",
			      sqlite3_column_count(stmt));

		if (visible_set)
			fatal("load_peer_visible_state: two states for %s", select);
		visible_set = true;
		
		if (sqlite3_column_int64(stmt, 1))
			peer->remote.offer_anchor = CMD_OPEN_WITH_ANCHOR;
		else
			peer->remote.offer_anchor = CMD_OPEN_WITHOUT_ANCHOR;
		pubkey_from_sql(peer->dstate->secpctx, stmt, 2,
				&peer->remote.commitkey);
		pubkey_from_sql(peer->dstate->secpctx, stmt, 3,
				&peer->remote.finalkey);
		peer->remote.locktime.locktime = sqlite3_column_int(stmt, 4);
		peer->remote.mindepth = sqlite3_column_int(stmt, 5);
		peer->remote.commit_fee_rate = sqlite3_column_int64(stmt, 6);
		sha256_from_sql(stmt, 7, &peer->remote.next_revocation_hash);
		log_debug(peer->log, "%s:next_revocation_hash=%s",
			  __func__,
			  tal_hexstr(ctx, &peer->remote.next_revocation_hash,
				     sizeof(peer->remote.next_revocation_hash)));

		/* Now we can fill in anchor witnessscript. */
		peer->anchor.witnessscript
			= bitcoin_redeem_2of2(peer, peer->dstate->secpctx,
					      &peer->local.commitkey,
					      &peer->remote.commitkey);
	}

	if (!visible_set)
		fatal("load_peer_visible_state: no result '%s'", select);

	err = sqlite3_finalize(stmt);
	if (err != SQLITE_OK)
		fatal("load_peer_visible_state:finalize gave %s:%s",
		      sqlite3_errstr(err), sqlite3_errmsg(sql));
	tal_free(ctx);
}

static void load_peer_commit_info(struct peer *peer)
{
	int err;
	sqlite3_stmt *stmt;
	sqlite3 *sql = peer->dstate->db->sql;
	char *ctx = tal(peer, char);
	const char *select;

	select = tal_fmt(ctx,
			 "SELECT * FROM commit_info WHERE peer = x'%s';",
			 pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id));

	err = sqlite3_prepare_v2(sql, select, -1, &stmt, NULL);
	if (err != SQLITE_OK)
		fatal("load_peer_commit_info:prepare gave %s:%s",
		      sqlite3_errstr(err), sqlite3_errmsg(sql));

	while ((err = sqlite3_step(stmt)) != SQLITE_DONE) {
		struct commit_info **cip, *ci;

		if (err != SQLITE_ROW)
			fatal("load_peer_commit_info:step gave %s:%s",
			      sqlite3_errstr(err), sqlite3_errmsg(sql));

		/* peer "SQL_PUBKEY", side TEXT, commit_num INT, revocation_hash "SQL_SHA256", sig "SQL_SIGNATURE", xmit_order INT, prev_revocation_hash "SQL_SHA256",  */
		if (sqlite3_column_count(stmt) != 7)
			fatal("load_peer_commit_info:step gave %i cols, not 7",
			      sqlite3_column_count(stmt));

		if (streq(sqlite3_column_str(stmt, 1), "OURS"))
			cip = &peer->local.commit;
		else {
			if (!streq(sqlite3_column_str(stmt, 1), "THEIRS"))
				fatal("load_peer_commit_info:bad side %s",
				      sqlite3_column_str(stmt, 1));
			cip = &peer->remote.commit;
			/* This is a hack where we temporarily store their
			 * previous revocation hash before we get their
			 * revocation. */
			if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
				peer->their_prev_revocation_hash
					= tal(peer, struct sha256);
				sha256_from_sql(stmt, 6,
						peer->their_prev_revocation_hash);
			}
		}

		/* Do we already have this one? */
		if (*cip)
			fatal("load_peer_commit_info:duplicate side %s",
			      sqlite3_column_str(stmt, 1));

		*cip = ci = new_commit_info(peer, sqlite3_column_int64(stmt, 2));
		sha256_from_sql(stmt, 3, &ci->revocation_hash);
		ci->order = sqlite3_column_int64(stmt, 4);

		if (sqlite3_column_type(stmt, 5) == SQLITE_NULL)
			ci->sig = NULL;
		else {
			ci->sig = tal(ci, struct bitcoin_signature);
			sig_from_sql(peer->dstate->secpctx, stmt, 5, ci->sig);
		}

		/* Set once we have updated HTLCs. */
		ci->cstate = NULL;
		ci->tx = NULL;
	}

	err = sqlite3_finalize(stmt);
	if (err != SQLITE_OK)
		fatal("load_peer_commit_info:finalize gave %s:%s",
		      sqlite3_errstr(err), sqlite3_errmsg(sql));
	tal_free(ctx);

	if (!peer->local.commit)
		fatal("load_peer_commit_info:no local commit info found");
	if (!peer->remote.commit)
		fatal("load_peer_commit_info:no remote commit info found");
}

/* This htlc no longer committed; either resolved or failed. */
static void htlc_resolved(struct channel_state *cstate, const struct htlc *htlc)
{
	if (htlc->r)
		cstate_fulfill_htlc(cstate, htlc);
	else
		cstate_fail_htlc(cstate, htlc);
}

/* As we load the HTLCs, we apply them to get the final channel_state.
 * We also get the last used htlc id.
 * This is slow, but sure. */
static void load_peer_htlcs(struct peer *peer)
{
	int err;
	sqlite3_stmt *stmt;
	sqlite3 *sql = peer->dstate->db->sql;
	char *ctx = tal(peer, char);
	const char *select;
	bool to_them_only, to_us_only;

	select = tal_fmt(ctx,
			 "SELECT * FROM htlcs WHERE peer = x'%s' ORDER BY id;",
			 pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id));

	err = sqlite3_prepare_v2(sql, select, -1, &stmt, NULL);
	if (err != SQLITE_OK)
		fatal("load_peer_htlcs:prepare gave %s:%s",
		      sqlite3_errstr(err), sqlite3_errmsg(sql));

	peer->local.commit->cstate = initial_cstate(peer,
						    peer->anchor.satoshis,
						    peer->local.commit_fee_rate,
						    peer->local.offer_anchor
						    == CMD_OPEN_WITH_ANCHOR ?
						    OURS : THEIRS);
	peer->remote.commit->cstate = initial_cstate(peer,
						     peer->anchor.satoshis,
						     peer->remote.commit_fee_rate,
						     peer->local.offer_anchor
						     == CMD_OPEN_WITH_ANCHOR ?
						     OURS : THEIRS);

	/* We rebuild cstate by running *every* HTLC through. */
	while ((err = sqlite3_step(stmt)) != SQLITE_DONE) {
		struct htlc *htlc;
		struct sha256 rhash;
		enum htlc_state hstate;

		if (err != SQLITE_ROW)
			fatal("load_peer_htlcs:step gave %s:%s",
			      sqlite3_errstr(err), sqlite3_errmsg(sql));

		if (sqlite3_column_count(stmt) != 10)
			fatal("load_peer_htlcs:step gave %i cols, not 10",
			      sqlite3_column_count(stmt));
		/* CREATE TABLE htlcs (peer "SQL_PUBKEY", id INT, state TEXT, msatoshis INT, expiry INT, rhash "SQL_RHASH", r "SQL_R", routing "SQL_ROUTING", src_peer "SQL_PUBKEY", src_id INT, PRIMARY KEY(peer, id)); */
		sha256_from_sql(stmt, 5, &rhash);

		hstate = htlc_state_from_name(sqlite3_column_str(stmt, 2));
		if (hstate == HTLC_STATE_INVALID)
			fatal("load_peer_htlcs:invalid state %s",
			      sqlite3_column_str(stmt, 2));
		htlc = peer_new_htlc(peer,
				     sqlite3_column_int64(stmt, 1),
				     sqlite3_column_int64(stmt, 3),
				     &rhash,
				     sqlite3_column_int64(stmt, 4),
				     sqlite3_column_blob(stmt, 7),
				     sqlite3_column_bytes(stmt, 7),
				     NULL,
				     hstate);

		if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
			htlc->r = tal(htlc, struct rval);
			from_sql_blob(stmt, 6, htlc->r, sizeof(*htlc->r));
		}

		log_debug(peer->log, "Loaded %s HTLC %"PRIu64" (%s)",
			  htlc_owner(htlc) == LOCAL ? "local" : "remote",
			  htlc->id, htlc_state_name(htlc->state));

		if (htlc_owner(htlc) == LOCAL
		    && htlc->id >= peer->htlc_id_counter)
			peer->htlc_id_counter = htlc->id + 1;

		/* Update cstate with this HTLC. */
		if (htlc_has(htlc, HTLC_LOCAL_F_WAS_COMMITTED)) {
			log_debug(peer->log, "  Local committed");
			if (!cstate_add_htlc(peer->local.commit->cstate, htlc))
				fatal("load_peer_htlcs:can't add local HTLC");

			if (!htlc_has(htlc, HTLC_LOCAL_F_COMMITTED)) {
				log_debug(peer->log, "  Local %s",
					  htlc->r ? "resolved" : "failed");
				htlc_resolved(peer->local.commit->cstate, htlc);
			}
		}

		if (htlc_has(htlc, HTLC_REMOTE_F_WAS_COMMITTED)) {
			log_debug(peer->log, "  Remote committed");
			if (!cstate_add_htlc(peer->remote.commit->cstate, htlc)) 
				fatal("load_peer_htlcs:can't add remote HTLC");
			if (!htlc_has(htlc, HTLC_REMOTE_F_COMMITTED)) {
				log_debug(peer->log, "  Remote %s",
					  htlc->r ? "resolved" : "failed");
				htlc_resolved(peer->remote.commit->cstate, htlc);
			}
		}
	}

	err = sqlite3_finalize(stmt);
	if (err != SQLITE_OK)
		fatal("load_peer_htlcs:finalize gave %s:%s",
		      sqlite3_errstr(err), sqlite3_errmsg(sql));

	/* Update commit->tx and commit->map */
	peer->local.commit->tx = create_commit_tx(peer->local.commit,
						  peer,
						  &peer->local.commit->revocation_hash,
						  peer->local.commit->cstate,
						  LOCAL, &to_them_only);
	bitcoin_txid(peer->local.commit->tx, &peer->local.commit->txid);

	peer->remote.commit->tx = create_commit_tx(peer->remote.commit,
						   peer,
						   &peer->remote.commit->revocation_hash,
						   peer->remote.commit->cstate,
						   REMOTE, &to_us_only);
	bitcoin_txid(peer->remote.commit->tx, &peer->remote.commit->txid);

	peer->remote.staging_cstate = copy_cstate(peer, peer->remote.commit->cstate);
	peer->local.staging_cstate = copy_cstate(peer, peer->local.commit->cstate);
	log_debug(peer->log, "Local staging: pay %u/%u fee %u/%u htlcs %u/%u",
		  peer->local.staging_cstate->side[OURS].pay_msat,
		  peer->local.staging_cstate->side[THEIRS].pay_msat,
		  peer->local.staging_cstate->side[OURS].fee_msat,
		  peer->local.staging_cstate->side[THEIRS].fee_msat,
		  peer->local.staging_cstate->side[OURS].num_htlcs,
		  peer->local.staging_cstate->side[THEIRS].num_htlcs);
	log_debug(peer->log, "Remote staging: pay %u/%u fee %u/%u htlcs %u/%u",
		  peer->remote.staging_cstate->side[OURS].pay_msat,
		  peer->remote.staging_cstate->side[THEIRS].pay_msat,
		  peer->remote.staging_cstate->side[OURS].fee_msat,
		  peer->remote.staging_cstate->side[THEIRS].fee_msat,
		  peer->remote.staging_cstate->side[OURS].num_htlcs,
		  peer->remote.staging_cstate->side[THEIRS].num_htlcs);
	
	tal_free(ctx);
}

/* FIXME: A real database person would do this in a single clause along
 * with loading the htlcs in the first place! */
static void connect_htlc_src(struct lightningd_state *dstate)
{
	sqlite3 *sql = dstate->db->sql;
	int err;
	sqlite3_stmt *stmt;
	char *ctx = tal(dstate, char);
	const char *select;

	select = tal_fmt(ctx,
			 "SELECT peer,id,state,src_peer,src_id FROM htlcs WHERE src_peer IS NOT NULL AND state <> 'RCVD_REMOVE_ACK_REVOCATION' AND state <> 'SENT_REMOVE_ACK_REVOCATION';");

	err = sqlite3_prepare_v2(sql, select, -1, &stmt, NULL);
	if (err != SQLITE_OK)
		fatal("connect_htlc_src:%s gave %s:%s",
		      select, sqlite3_errstr(err), sqlite3_errmsg(sql));

	while ((err = sqlite3_step(stmt)) != SQLITE_DONE) {
		struct pubkey id;
		struct peer *peer;
		struct htlc *htlc;
		enum htlc_state s;

		if (err != SQLITE_ROW)
			fatal("connect_htlc_src:step gave %s:%s",
			      sqlite3_errstr(err), sqlite3_errmsg(sql));

		pubkey_from_sql(dstate->secpctx, stmt, 0, &id);
		peer = find_peer(dstate, &id);
		if (!peer)
			continue;

		s = htlc_state_from_name(sqlite3_column_str(stmt, 2));
		if (s == HTLC_STATE_INVALID)
			fatal("connect_htlc_src:unknown state %s",
			      sqlite3_column_str(stmt, 2));

		htlc = htlc_get(&peer->htlcs, sqlite3_column_int64(stmt, 1),
				htlc_state_owner(s));
		if (!htlc)
			fatal("connect_htlc_src:unknown htlc %"PRIuSQLITE64" state %s",
			      sqlite3_column_int64(stmt, 1),
			      sqlite3_column_str(stmt, 2));

		pubkey_from_sql(dstate->secpctx, stmt, 4, &id);
		peer = find_peer(dstate, &id);
		if (!peer)
			fatal("connect_htlc_src:unknown src peer %s",
			      tal_hexstr(dstate, &id, sizeof(id)));

		/* Source must be a HTLC they offered. */
		htlc->src = htlc_get(&peer->htlcs,
				     sqlite3_column_int64(stmt, 4),
				     REMOTE);
		if (!htlc->src)
			fatal("connect_htlc_src:unknown src htlc");
	}

	err = sqlite3_finalize(stmt);
	if (err != SQLITE_OK)
		fatal("load_peer_htlcs:finalize gave %s:%s",
		      sqlite3_errstr(err),
		      sqlite3_errmsg(dstate->db->sql));
	tal_free(ctx);
}

/* FIXME: Expose pull/push and use that here. */
static const char *linearize_shachain(const tal_t *ctx,
				      const struct shachain *shachain)
{
	size_t i;
	u8 *p = tal_arr(ctx, u8, 0);
	const char *str;

	push_le64(shachain->min_index, push, &p);
	push_le32(shachain->num_valid, push, &p);
	for (i = 0; i < shachain->num_valid; i++) {
		push_le64(shachain->known[i].index, push, &p);
		push(&shachain->known[i].hash, sizeof(shachain->known[i].hash),
		     &p);
	}
	for (i = shachain->num_valid; i < ARRAY_SIZE(shachain->known); i++) {
		static u8 zeroes[sizeof(shachain->known[0].hash)];
		push_le64(0, push, &p);
		push(zeroes, sizeof(zeroes), &p);
	}
		
	assert(tal_count(p) == SHACHAIN_SIZE);
	str = tal_hexstr(ctx, p, tal_count(p));
	tal_free(p);
	return str;
}

static bool delinearize_shachain(struct shachain *shachain,
				 const void *data, size_t len)
{
	size_t i;
	const u8 *p = data;

	shachain->min_index = pull_le64(&p, &len);
	shachain->num_valid = pull_le32(&p, &len);
	for (i = 0; i < ARRAY_SIZE(shachain->known); i++) {
		shachain->known[i].index = pull_le64(&p, &len);
		pull(&p, &len, &shachain->known[i].hash,
		     sizeof(shachain->known[i].hash));
	}
	return p && len == 0;
}

static void load_peer_shachain(struct peer *peer)
{
	int err;
	sqlite3_stmt *stmt;
	sqlite3 *sql = peer->dstate->db->sql;
	char *ctx = tal(peer, char);
	bool shachain_found = false;
	const char *select;

	select = tal_fmt(ctx,
			 "SELECT * FROM shachain WHERE peer = x'%s';",
			 pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id));

	err = sqlite3_prepare_v2(sql, select, -1, &stmt, NULL);
	if (err != SQLITE_OK)
		fatal("load_peer_shachain:prepare gave %s:%s",
		      sqlite3_errstr(err), sqlite3_errmsg(sql));

	while ((err = sqlite3_step(stmt)) != SQLITE_DONE) {
		const char *hexstr;

		if (err != SQLITE_ROW)
			fatal("load_peer_shachain:step gave %s:%s",
			      sqlite3_errstr(err), sqlite3_errmsg(sql));

		/* shachain (peer "SQL_PUBKEY", shachain BINARY(%zu) */
		if (sqlite3_column_count(stmt) != 2)
			fatal("load_peer_shachain:step gave %i cols, not 2",
			      sqlite3_column_count(stmt));

		if (shachain_found)
			fatal("load_peer_shachain:multiple shachains?");

		hexstr = tal_hexstr(ctx, sqlite3_column_blob(stmt, 1),
				    sqlite3_column_bytes(stmt, 1));
		if (!delinearize_shachain(&peer->their_preimages,
					  sqlite3_column_blob(stmt, 1),
					  sqlite3_column_bytes(stmt, 1)))
			fatal("load_peer_shachain:invalid shachain %s",
			      hexstr);
		shachain_found = true;
	}

	if (!shachain_found)
		fatal("load_peer_shachain:no shachain");
	tal_free(ctx);
}

/* We may not have one, and that's OK. */
static void load_peer_closing(struct peer *peer)
{
	int err;
	sqlite3_stmt *stmt;
	sqlite3 *sql = peer->dstate->db->sql;
	char *ctx = tal(peer, char);
	bool closing_found = false;
	const char *select;

	select = tal_fmt(ctx,
			 "SELECT * FROM closing WHERE peer = x'%s';",
			 pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id));

	err = sqlite3_prepare_v2(sql, select, -1, &stmt, NULL);
	if (err != SQLITE_OK)
		fatal("load_peer_closing:prepare gave %s:%s",
		      sqlite3_errstr(err), sqlite3_errmsg(sql));

	while ((err = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (err != SQLITE_ROW)
			fatal("load_peer_closing:step gave %s:%s",
			      sqlite3_errstr(err), sqlite3_errmsg(sql));

		/* CREATE TABLE closing (peer "SQL_PUBKEY", our_fee INTEGER, their_fee INTEGER, their_sig "SQL_SIGNATURE", our_script BLOB, their_script BLOB, shutdown_order INTEGER, closing_order INTEGER, sigs_in INTEGER, PRIMARY KEY(peer)); */
		if (sqlite3_column_count(stmt) != 9)
			fatal("load_peer_closing:step gave %i cols, not 9",
			      sqlite3_column_count(stmt));

		if (closing_found)
			fatal("load_peer_closing:multiple closing?");

		peer->closing.our_fee = sqlite3_column_int64(stmt, 1);
		peer->closing.their_fee = sqlite3_column_int64(stmt, 2);
		if (sqlite3_column_type(stmt, 3) == SQLITE_NULL)
			peer->closing.their_sig = NULL;
		else {
			peer->closing.their_sig = tal(peer,
						      struct bitcoin_signature);
			sig_from_sql(peer->dstate->secpctx, stmt, 3,
				     peer->closing.their_sig);
		}
		peer->closing.our_script = tal_sql_blob(peer, stmt, 4);
		peer->closing.their_script = tal_sql_blob(peer, stmt, 5);
		peer->closing.shutdown_order = sqlite3_column_int64(stmt, 6);
		peer->closing.closing_order = sqlite3_column_int64(stmt, 7);
		peer->closing.sigs_in = sqlite3_column_int64(stmt, 8);
		closing_found = true;
	}
	tal_free(ctx);
}

/* FIXME: much of this is redundant. */
static void restore_peer_local_visible_state(struct peer *peer)
{
	if (peer->remote.offer_anchor == CMD_OPEN_WITH_ANCHOR)
		peer->local.offer_anchor = CMD_OPEN_WITHOUT_ANCHOR;
	else
		peer->local.offer_anchor = CMD_OPEN_WITH_ANCHOR;

	/* peer->local.commitkey and peer->local.finalkey set by
	 * peer_set_secrets_from_db(). */
	memcheck(&peer->local.commitkey, sizeof(peer->local.commitkey));
	memcheck(&peer->local.finalkey, sizeof(peer->local.finalkey));
	/* These set in new_peer */
	memcheck(&peer->local.locktime, sizeof(peer->local.locktime));
	memcheck(&peer->local.mindepth, sizeof(peer->local.mindepth));
	/* This set in db_load_peers */
	memcheck(&peer->local.commit_fee_rate,
		 sizeof(peer->local.commit_fee_rate));

	peer_get_revocation_hash(peer,
				 peer->local.commit->commit_num + 1,
				 &peer->local.next_revocation_hash);

	if (state_is_normal(peer->state))
		peer->nc = add_connection(peer->dstate,
					  &peer->dstate->id, peer->id,
					  peer->dstate->config.fee_base,
					  peer->dstate->config.fee_per_satoshi,
					  peer->dstate->config.min_htlc_expiry,
					  peer->dstate->config.min_htlc_expiry);

	peer->their_commitsigs = peer->local.commit->commit_num + 1;
	/* If they created anchor, they didn't send a sig for first commit */
	if (!peer->anchor.ours)
		peer->their_commitsigs--;

	peer->order_counter = 0;
	if (peer->local.commit->order + 1 > peer->order_counter)
		peer->order_counter = peer->local.commit->order + 1;
	if (peer->remote.commit->order + 1 > peer->order_counter)
		peer->order_counter = peer->remote.commit->order + 1;
	if (peer->closing.closing_order + 1 > peer->order_counter)
		peer->order_counter = peer->closing.closing_order + 1;
	if (peer->closing.shutdown_order + 1 > peer->order_counter)
		peer->order_counter = peer->closing.shutdown_order + 1;
}

static void db_load_peers(struct lightningd_state *dstate)
{
	int err;
	sqlite3_stmt *stmt;
	struct peer *peer;

	err = sqlite3_prepare_v2(dstate->db->sql, "SELECT * FROM peers;", -1,
				 &stmt, NULL);

	if (err != SQLITE_OK)
		fatal("db_load_peers:prepare gave %s:%s",
		      sqlite3_errstr(err), sqlite3_errmsg(dstate->db->sql));

	while ((err = sqlite3_step(stmt)) != SQLITE_DONE) {
		enum state state;

		if (err != SQLITE_ROW)
			fatal("db_load_peers:step gave %s:%s",
			      sqlite3_errstr(err),
			      sqlite3_errmsg(dstate->db->sql));
		if (sqlite3_column_count(stmt) != 4)
			fatal("db_load_peers:step gave %i cols, not 4",
			      sqlite3_column_count(stmt));
		state = name_to_state(sqlite3_column_str(stmt, 1));
		if (state == STATE_MAX)
			fatal("db_load_peers:unknown state %s",
			      sqlite3_column_str(stmt, 1));
		peer = new_peer(dstate, state, sqlite3_column_int(stmt, 2) ?
				CMD_OPEN_WITH_ANCHOR : CMD_OPEN_WITHOUT_ANCHOR);
		peer->htlc_id_counter = 0;
		peer->id = tal(peer, struct pubkey);
		pubkey_from_sql(dstate->secpctx, stmt, 0, peer->id);
		peer->local.commit_fee_rate = sqlite3_column_int64(stmt, 3);
		log_debug(dstate->base_log, "%s:%s:",
			  __func__, state_name(peer->state));
		log_add_struct(dstate->base_log, "%s", struct pubkey, peer->id);
	}
	err = sqlite3_finalize(stmt);
	if (err != SQLITE_OK)
		fatal("db_load_peers:finalize gave %s:%s",
		      sqlite3_errstr(err),
		      sqlite3_errmsg(dstate->db->sql));

	list_for_each(&dstate->peers, peer, list) {
		load_peer_address(peer);
		load_peer_secrets(peer);
		load_peer_closing(peer);
		peer->anchor.min_depth = 0;
		if (peer->state >= STATE_OPEN_WAITING_OURANCHOR
		    && !state_is_error(peer->state)) {
			load_peer_anchor(peer);
			load_peer_visible_state(peer);
			load_peer_shachain(peer);
			load_peer_commit_info(peer);
			load_peer_htlcs(peer);
			restore_peer_local_visible_state(peer);
		}
	}

	connect_htlc_src(dstate);
}

static void db_load(struct lightningd_state *dstate)
{
	db_load_wallet(dstate);

	db_load_peers(dstate);
}

void db_init(struct lightningd_state *dstate)
{
	int err;
	char *errmsg;
	bool created = false;

	if (SQLITE_VERSION_NUMBER != sqlite3_libversion_number())
		fatal("SQLITE version mistmatch: compiled %u, now %u",
		      SQLITE_VERSION_NUMBER, sqlite3_libversion_number());

	dstate->db = tal(dstate, struct db);

	err = sqlite3_open_v2(DB_FILE, &dstate->db->sql,
			      SQLITE_OPEN_READWRITE, NULL);
	if (err != SQLITE_OK) {
		log_unusual(dstate->base_log,
			    "Error opening %s (%s), trying to create",
			    DB_FILE, sqlite3_errstr(err));
		err = sqlite3_open_v2(DB_FILE, &dstate->db->sql,
				      SQLITE_OPEN_READWRITE
				      | SQLITE_OPEN_CREATE, NULL);
		if (err != SQLITE_OK)
			fatal("failed creating %s: %s",
			      DB_FILE, sqlite3_errstr(err));
		created = true;
	}

	tal_add_destructor(dstate->db, close_db);
	dstate->db->in_transaction = false;
	
	if (!created) {
		db_load(dstate);
		return;
	}

	/* Set up tables. */
	errmsg = db_exec(dstate, dstate,
			 "CREATE TABLE wallet (privkey "SQL_PRIVKEY");"
			 "CREATE TABLE anchors (peer "SQL_PUBKEY", txid "SQL_TXID", idx INT, amount INT, ok_depth INT, min_depth INT, bool ours, PRIMARY KEY(peer));"
			 "CREATE TABLE htlcs (peer "SQL_PUBKEY", id INT, state TEXT, msatoshis INT, expiry INT, rhash "SQL_RHASH", r "SQL_R", routing "SQL_ROUTING", src_peer "SQL_PUBKEY", src_id INT, PRIMARY KEY(peer, id));"
			 "CREATE TABLE commit_info (peer "SQL_PUBKEY", side TEXT, commit_num INT, revocation_hash "SQL_SHA256", xmit_order INT, sig "SQL_SIGNATURE", prev_revocation_hash "SQL_SHA256", PRIMARY KEY(peer, side));"
			 "CREATE TABLE shachain (peer "SQL_PUBKEY", shachain BINARY(%zu), PRIMARY KEY(peer));"
			 "CREATE TABLE their_visible_state (peer "SQL_PUBKEY", offered_anchor BOOLEAN, commitkey "SQL_PUBKEY", finalkey "SQL_PUBKEY", locktime INT, mindepth INT, commit_fee_rate INT, next_revocation_hash "SQL_SHA256", PRIMARY KEY(peer));"
			 "CREATE TABLE their_commitments (peer "SQL_PUBKEY", txid "SQL_SHA256", INT commit_num, PRIMARY KEY(peer, txid));"
			 "CREATE TABLE peer_secrets (peer "SQL_PUBKEY", commitkey "SQL_PRIVKEY", finalkey "SQL_PRIVKEY", revocation_seed "SQL_SHA256", PRIMARY KEY(peer));"
			 "CREATE TABLE peer_address (peer "SQL_PUBKEY", addr BLOB, PRIMARY KEY(peer));"
			 "CREATE TABLE closing (peer "SQL_PUBKEY", our_fee INTEGER, their_fee INTEGER, their_sig "SQL_SIGNATURE", our_script BLOB, their_script BLOB, shutdown_order INTEGER, closing_order INTEGER, sigs_in INTEGER, PRIMARY KEY(peer));"
			 "CREATE TABLE peers (peer "SQL_PUBKEY", state TEXT, offered_anchor BOOLEAN, our_feerate INT, PRIMARY KEY(peer));",
			 sizeof(struct shachain));

	if (errmsg) {
		unlink(DB_FILE);
		fatal("%s", errmsg);
	}
}

bool db_set_anchor(struct peer *peer)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid;

	assert(peer->dstate->db->in_transaction);
	peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);
	log_debug(peer->log, "%s(%s)", __func__, peerid);

	errmsg = db_exec(ctx, peer->dstate, 
			 "INSERT INTO anchors VALUES (x'%s', x'%s', %u, %"PRIu64", %i, %u, %u);",
			 peerid,
			 tal_hexstr(ctx, &peer->anchor.txid,
				    sizeof(peer->anchor.txid)),
			 peer->anchor.index,
			 peer->anchor.satoshis,
			 peer->anchor.ok_depth,
			 peer->anchor.min_depth,
			 peer->anchor.ours);
	if (errmsg)
		goto out;

	errmsg = db_exec(ctx, peer->dstate, 
			 "INSERT INTO commit_info VALUES(x'%s', 'OURS', 0, x'%s', %"PRIi64", %s, NULL);",
			 peerid,
			 tal_hexstr(ctx, &peer->local.commit->revocation_hash,
				    sizeof(peer->local.commit->revocation_hash)),
			 peer->local.commit->order,
			 sig_to_sql(ctx, peer->dstate->secpctx,
				    peer->local.commit->sig));
	if (errmsg)
		goto out;

	errmsg = db_exec(ctx, peer->dstate, 
			 "INSERT INTO commit_info VALUES(x'%s', 'THEIRS', 0, x'%s', %"PRIi64", %s, NULL);",
			 peerid,
			 tal_hexstr(ctx, &peer->remote.commit->revocation_hash,
				    sizeof(peer->remote.commit->revocation_hash)),
			 peer->remote.commit->order,
			 sig_to_sql(ctx, peer->dstate->secpctx,
				    peer->remote.commit->sig));

	if (errmsg)
		goto out;

	errmsg = db_exec(ctx, peer->dstate, "INSERT INTO shachain VALUES (x'%s', x'%s');",
			 peerid,
			 linearize_shachain(ctx, &peer->their_preimages));

out:
	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	
	tal_free(ctx);
	return !errmsg;
}

bool db_set_visible_state(struct peer *peer)
{
	/* "CREATE TABLE their_visible_state (peer "SQL_PUBKEY", offered_anchor BOOLEAN, commitkey "SQL_PUBKEY", finalkey "SQL_PUBKEY", locktime INT, mindepth INT, commit_fee_rate INT, next_revocation_hash "SQL_SHA256", PRIMARY KEY(peer));" */
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);
	if (!db_start_transaction(peer)) {
		tal_free(ctx);
		return false;
	}

	errmsg = db_exec(ctx, peer->dstate, 
			 "INSERT INTO their_visible_state VALUES (x'%s', %u, x'%s', x'%s', %u, %u, %"PRIu64", x'%s');",
			 peerid,
			 peer->remote.offer_anchor == CMD_OPEN_WITH_ANCHOR,
			 pubkey_to_hexstr(ctx, peer->dstate->secpctx,
					  &peer->remote.commitkey),
			 pubkey_to_hexstr(ctx, peer->dstate->secpctx,
					  &peer->remote.finalkey),
			 peer->remote.locktime.locktime,
			 peer->remote.mindepth,
			 peer->remote.commit_fee_rate,
			 tal_hexstr(ctx, &peer->remote.next_revocation_hash,
				    sizeof(peer->remote.next_revocation_hash)));
	if (errmsg)
		goto out;

	if (!db_commit_transaction(peer))
		errmsg = "Commit failed";

out:
	if (errmsg) {
		log_broken(peer->log, "%s:%s", __func__, errmsg);
		db_abort_transaction(peer);
	}
	
	tal_free(ctx);
	return !errmsg;
}

bool db_update_next_revocation_hash(struct peer *peer)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s):%s", __func__, peerid,
		tal_hexstr(ctx, &peer->remote.next_revocation_hash,
			   sizeof(peer->remote.next_revocation_hash)));
	assert(peer->dstate->db->in_transaction);
	errmsg = db_exec(ctx, peer->dstate, 
			 "UPDATE their_visible_state SET next_revocation_hash=x'%s' WHERE peer=x'%s';",
			 tal_hexstr(ctx, &peer->remote.next_revocation_hash,
				    sizeof(peer->remote.next_revocation_hash)),
			 peerid);
	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}

bool db_create_peer(struct peer *peer)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);
	if (!db_start_transaction(peer)) {
		tal_free(ctx);
		return false;
	}
	errmsg = db_exec(ctx, peer->dstate, 
			 "INSERT INTO peers VALUES (x'%s', '%s', %u, %"PRIi64");",
			 peerid,
			 state_name(peer->state),
			 peer->local.offer_anchor == CMD_OPEN_WITH_ANCHOR,
			 peer->local.commit_fee_rate);
	if (errmsg)
		goto out;
	
	errmsg = db_exec(ctx, peer->dstate, 
			 "INSERT INTO peer_secrets VALUES (x'%s', %s);",
			 peerid, peer_secrets_for_db(ctx, peer));
	if (errmsg)
		goto out;

	errmsg = db_exec(ctx, peer->dstate, 
			 "INSERT INTO peer_address VALUES (x'%s', x'%s');",
			 peerid,
			 netaddr_to_hex(ctx, &peer->addr));

	if (errmsg)
		goto out;

	if (!db_commit_transaction(peer))
		errmsg = "Commit failed";

out:
	if (errmsg) {
		log_broken(peer->log, "%s:%s", __func__, errmsg);
		db_abort_transaction(peer);
	}
	
	tal_free(ctx);
	return !errmsg;
}

bool db_start_transaction(struct peer *peer)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);
	assert(!peer->dstate->db->in_transaction);
	errmsg = db_exec(ctx, peer->dstate, "BEGIN IMMEDIATE;");
	if (!errmsg)
		peer->dstate->db->in_transaction = true;
	else
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}

void db_abort_transaction(struct peer *peer)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);
	assert(peer->dstate->db->in_transaction);
	peer->dstate->db->in_transaction = false;
	errmsg = db_exec(ctx, peer->dstate, "ROLLBACK;");
	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
}

bool db_commit_transaction(struct peer *peer)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);
	assert(peer->dstate->db->in_transaction);
	peer->dstate->db->in_transaction = false;
	errmsg = db_exec(ctx, peer->dstate, "COMMIT;");
	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}

bool db_new_htlc(struct peer *peer, const struct htlc *htlc)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);
	assert(peer->dstate->db->in_transaction);

	if (htlc->src) {
		errmsg = db_exec(ctx, peer->dstate, 
				 "INSERT INTO htlcs VALUES"
				 " (x'%s', %"PRIu64", '%s', %"PRIu64", %u, x'%s', NULL, x'%s', x'%s', %"PRIu64");",
				 pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id),
				 htlc->id,
				 htlc_state_name(htlc->state),
				 htlc->msatoshis,
				 abs_locktime_to_blocks(&htlc->expiry),
				 tal_hexstr(ctx, &htlc->rhash, sizeof(htlc->rhash)),
				 tal_hexstr(ctx, htlc->routing, tal_count(htlc->routing)),
				 peerid,
				 htlc->src->id);
	} else {
		errmsg = db_exec(ctx, peer->dstate, 
				 "INSERT INTO htlcs VALUES"
				 " (x'%s', %"PRIu64", '%s', %"PRIu64", %u, x'%s', NULL, x'%s', NULL, NULL);",
				 peerid,
				 htlc->id,
				 htlc_state_name(htlc->state),
				 htlc->msatoshis,
				 abs_locktime_to_blocks(&htlc->expiry),
				 tal_hexstr(ctx, &htlc->rhash, sizeof(htlc->rhash)),
				 tal_hexstr(ctx, htlc->routing, tal_count(htlc->routing)));
	}

	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}

bool db_update_htlc_state(struct peer *peer, const struct htlc *htlc,
			  enum htlc_state oldstate)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s): %"PRIu64" %s->%s", __func__, peerid,
		  htlc->id, htlc_state_name(oldstate),
		  htlc_state_name(htlc->state));
	assert(peer->dstate->db->in_transaction);
	errmsg = db_exec(ctx, peer->dstate, 
			 "UPDATE htlcs SET state='%s' WHERE peer=x'%s' AND id=%"PRIu64" AND state='%s';",
			 htlc_state_name(htlc->state), peerid,
			 htlc->id, htlc_state_name(oldstate));

	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}

bool db_update_state(struct peer *peer)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);

	assert(peer->dstate->db->in_transaction);
	errmsg = db_exec(ctx, peer->dstate, 
			 "UPDATE peers SET state='%s' WHERE peer=x'%s';",
			 state_name(peer->state), peerid);

	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}

bool db_htlc_fulfilled(struct peer *peer, const struct htlc *htlc)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);

	/* When called from their_htlc_added() and it's a payment to
	 * us, we are in a transaction.  When called due to
	 * PKT_UPDATE_FULFILL_HTLC we are not. */
	errmsg = db_exec(ctx, peer->dstate, 
			 "UPDATE htlcs SET r=x'%s' WHERE peer=x'%s' AND id=%"PRIu64" AND state='%s';",
			 tal_hexstr(ctx, htlc->r, sizeof(*htlc->r)),
			 peerid,
			 htlc->id,
			 htlc_state_name(htlc->state));

	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}

bool db_new_commit_info(struct peer *peer, enum channel_side side,
			const struct sha256 *prev_rhash)
{
	struct commit_info *ci;
	const char *sidestr;
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);

	assert(peer->dstate->db->in_transaction);
	if (side == OURS) {
		sidestr = "OURS";
		ci = peer->local.commit;
	} else {
		sidestr = "THEIRS";
		ci = peer->remote.commit;
	}

	// CREATE TABLE commit_info (peer "SQL_PUBKEY", side TEXT, commit_num INT, revocation_hash "SQL_SHA256", xmit_order INT, sig "SQL_SIGNATURE", prev_revocation_hash "SQL_SHA256", PRIMARY KEY(peer, side));
	errmsg = db_exec(ctx, peer->dstate, "UPDATE commit_info SET commit_num=%"PRIu64", revocation_hash=x'%s', sig=%s, xmit_order=%"PRIi64", prev_revocation_hash=%s WHERE peer=x'%s' AND side='%s';",
			 ci->commit_num,
			 tal_hexstr(ctx, &ci->revocation_hash,
				    sizeof(ci->revocation_hash)),
			 sig_to_sql(ctx, peer->dstate->secpctx, ci->sig),
			 ci->order,
			 sql_hex_or_null(ctx, prev_rhash, sizeof(*prev_rhash)),
			 peerid, sidestr);
	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}

/* FIXME: Is this strictly necessary? */
bool db_remove_their_prev_revocation_hash(struct peer *peer)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);

	assert(peer->dstate->db->in_transaction);

	// CREATE TABLE commit_info (peer "SQL_PUBKEY", side TEXT, commit_num INT, revocation_hash "SQL_SHA256", xmit_order INT, sig "SQL_SIGNATURE", prev_revocation_hash "SQL_SHA256", PRIMARY KEY(peer, side));
	errmsg = db_exec(ctx, peer->dstate, "UPDATE commit_info SET prev_revocation_hash=NULL WHERE peer=x'%s' AND side='THEIRS' and prev_revocation_hash IS NOT NULL;",
			 peerid);
	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}
	

bool db_save_shachain(struct peer *peer)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);

	assert(peer->dstate->db->in_transaction);
	// "CREATE TABLE shachain (peer "SQL_PUBKEY", shachain BINARY(%zu), PRIMARY KEY(peer));"
	errmsg = db_exec(ctx, peer->dstate, "UPDATE shachain SET shachain=x'%s' WHERE peer=x'%s';",
			 linearize_shachain(ctx, &peer->their_preimages),
			 peerid);
	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}

bool db_add_commit_map(struct peer *peer,
		       const struct sha256_double *txid, u64 commit_num)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s),commit_num=%"PRIu64, __func__, peerid,
		  commit_num);

	assert(peer->dstate->db->in_transaction);
	// "CREATE TABLE their_commitments (peer "SQL_PUBKEY", txid "SQL_SHA256", INT commit_num, PRIMARY KEY(peer, txid));"
	errmsg = db_exec(ctx, peer->dstate, "INSERT INTO their_commitments VALUES (x'%s', x'%s', %"PRIu64");",
			 peerid,
			 tal_hexstr(ctx, txid, sizeof(*txid)),
			 commit_num);
	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}

void db_forget_peer(struct peer *peer)
{
	const char *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);
	size_t i;
	const char *const tables[] = { "anchors", "htlcs", "commit_info", "shachain", "their_visible_state", "their_commitments", "peer_secrets", "closing", "peers" };
	log_debug(peer->log, "%s(%s)", __func__, peerid);

	assert(peer->state == STATE_CLOSED);

	if (!db_start_transaction(peer))
		fatal("%s:db_start_transaction failed", __func__);

	for (i = 0; i < ARRAY_SIZE(tables); i++) {
		const char *errmsg;
		errmsg = db_exec(ctx, peer->dstate,
				 "DELETE from %s WHERE peer=x'%s';",
				 tables[i], peerid);
		if (errmsg)
			fatal("%s:%s", __func__, errmsg);
	}
	if (!db_commit_transaction(peer))
		fatal("%s:db_commi_transaction failed", __func__);

	tal_free(ctx);
}

bool db_begin_shutdown(struct peer *peer)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);

	assert(peer->dstate->db->in_transaction);
	//  "CREATE TABLE closing (peer "SQL_PUBKEY", our_fee INTEGER, their_fee INTEGER, their_sig "SQL_SIGNATURE", our_script BLOB, their_script BLOB, shutdown_order INTEGER, closing_order INTEGER, sigs_in INTEGER, PRIMARY KEY(peer));"
	errmsg = db_exec(ctx, peer->dstate, "INSERT INTO closing VALUES (x'%s', 0, 0, NULL, NULL, NULL, 0, 0, 0);",
			 peerid);
	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}

bool db_set_our_closing_script(struct peer *peer)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);

	assert(peer->dstate->db->in_transaction);
	//  "CREATE TABLE closing (peer "SQL_PUBKEY", our_fee INTEGER, their_fee INTEGER, their_sig "SQL_SIGNATURE", our_script BLOB, their_script BLOB, shutdown_order INTEGER, closing_order INTEGER, sigs_in INTEGER, PRIMARY KEY(peer));"
	errmsg = db_exec(ctx, peer->dstate, "UPDATE closing SET our_script=x'%s',shutdown_order=%"PRIu64" WHERE peer=x'%s';",
			 tal_hexstr(ctx, peer->closing.our_script,
				    tal_count(peer->closing.our_script)),
			 peer->closing.shutdown_order,
			 peerid);
	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}

bool db_set_their_closing_script(struct peer *peer)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);

	assert(!peer->dstate->db->in_transaction);
	//  "CREATE TABLE closing (peer "SQL_PUBKEY", our_fee INTEGER, their_fee INTEGER, their_sig "SQL_SIGNATURE", our_script BLOB, their_script BLOB, shutdown_order INTEGER, closing_order INTEGER, sigs_in INTEGER, PRIMARY KEY(peer));"
	errmsg = db_exec(ctx, peer->dstate, "UPDATE closing SET their_script=x'%s' WHERE peer=x'%s';",
			 tal_hexstr(ctx, peer->closing.their_script,
				    tal_count(peer->closing.their_script)),
			 peerid);
	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}

/* For first time, we are in transaction to make it atomic with peer->state
 * update.  Later calls are not. */
bool db_update_our_closing(struct peer *peer)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);

	//  "CREATE TABLE closing (peer "SQL_PUBKEY", our_fee INTEGER, their_fee INTEGER, their_sig "SQL_SIGNATURE", our_script BLOB, their_script BLOB, shutdown_order INTEGER, closing_order INTEGER, sigs_in INTEGER, PRIMARY KEY(peer));"
	errmsg = db_exec(ctx, peer->dstate, "UPDATE closing SET our_fee=%"PRIu64", closing_order=%"PRIi64" WHERE peer=x'%s';",
			 peer->closing.our_fee,
			 peer->closing.closing_order,
			 peerid);
	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}

bool db_update_their_closing(struct peer *peer)
{
	const char *errmsg, *ctx = tal(peer, char);
	const char *peerid = pubkey_to_hexstr(ctx, peer->dstate->secpctx, peer->id);

	log_debug(peer->log, "%s(%s)", __func__, peerid);

	assert(!peer->dstate->db->in_transaction);
	//  "CREATE TABLE closing (peer "SQL_PUBKEY", our_fee INTEGER, their_fee INTEGER, their_sig "SQL_SIGNATURE", our_script BLOB, their_script BLOB, shutdown_order INTEGER, closing_order INTEGER, sigs_in INTEGER, PRIMARY KEY(peer));"
	errmsg = db_exec(ctx, peer->dstate, "UPDATE closing SET their_fee=%"PRIu64", their_sig=x'%s', sigs_in=%u WHERE peer=x'%s';",
			 peer->closing.their_fee,
			 tal_hexstr(ctx, peer->closing.their_sig,
				    tal_count(peer->closing.their_sig)),
			 peer->closing.sigs_in,
			 peerid);
	if (errmsg)
		log_broken(peer->log, "%s:%s", __func__, errmsg);
	tal_free(ctx);
	return !errmsg;
}
