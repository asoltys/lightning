#include "bitcoin/script.h"
#include "bitcoin/tx.h"
#include "chaintopology.h"
#include "close_tx.h"
#include "commit_tx.h"
#include "controlled_time.h"
#include "cryptopkt.h"
#include "htlc.h"
#include "lightningd.h"
#include "log.h"
#include "names.h"
#include "packets.h"
#include "peer.h"
#include "protobuf_convert.h"
#include "secrets.h"
#include "state.h"
#include "utils.h"
#include <ccan/array_size/array_size.h>
#include <ccan/crypto/sha256/sha256.h>
#include <ccan/io/io.h>
#include <ccan/mem/mem.h>
#include <ccan/ptrint/ptrint.h>
#include <ccan/str/hex/hex.h>
#include <ccan/structeq/structeq.h>
#include <ccan/tal/str/str.h>
#include <inttypes.h>

/* Wrap (and own!) member inside Pkt */
static Pkt *make_pkt(const tal_t *ctx, Pkt__PktCase type, const void *msg)
{
	Pkt *pkt = tal(ctx, Pkt);

	pkt__init(pkt);
	pkt->pkt_case = type;
	/* This is a union, so doesn't matter which we assign. */
	pkt->error = (Error *)tal_steal(pkt, msg);

	/* This makes sure all packets are valid. */
#ifndef NDEBUG
	{
		size_t len;
		u8 *packed;
		Pkt *cpy;
		
		len = pkt__get_packed_size(pkt);
		packed = tal_arr(pkt, u8, len);
		pkt__pack(pkt, packed);
		cpy = pkt__unpack(NULL, len, memcheck(packed, len));
		assert(cpy);
		pkt__free_unpacked(cpy, NULL);
		tal_free(packed);
	}
#endif
	return pkt;
}

static void queue_raw_pkt(struct peer *peer, Pkt *pkt)
{
	size_t n = tal_count(peer->outpkt);
	tal_resize(&peer->outpkt, n+1);
	peer->outpkt[n] = pkt;

	log_debug(peer->log, "Queued pkt %s (order=%"PRIu64")",
		  pkt_name(pkt->pkt_case), peer->order_counter);

	/* In case it was waiting for output. */
	io_wake(peer);
}

static void queue_pkt(struct peer *peer, Pkt__PktCase type, const void *msg)
{
	queue_raw_pkt(peer, make_pkt(peer, type, msg));
}

void queue_pkt_open(struct peer *peer, OpenChannel__AnchorOffer anchor)
{
	OpenChannel *o = tal(peer, OpenChannel);

	open_channel__init(o);
	o->revocation_hash = sha256_to_proto(o, &peer->local.commit->revocation_hash);
	o->next_revocation_hash = sha256_to_proto(o, &peer->local.next_revocation_hash);
	o->commit_key = pubkey_to_proto(o, peer->dstate->secpctx,
					&peer->local.commitkey);
	o->final_key = pubkey_to_proto(o, peer->dstate->secpctx,
				       &peer->local.finalkey);
	o->delay = tal(o, Locktime);
	locktime__init(o->delay);
	o->delay->locktime_case = LOCKTIME__LOCKTIME_BLOCKS;
	o->delay->blocks = rel_locktime_to_blocks(&peer->local.locktime);
	o->initial_fee_rate = peer->local.commit_fee_rate;
	if (anchor == OPEN_CHANNEL__ANCHOR_OFFER__WILL_CREATE_ANCHOR)
		assert(peer->local.offer_anchor == CMD_OPEN_WITH_ANCHOR);
	else {
		assert(anchor == OPEN_CHANNEL__ANCHOR_OFFER__WONT_CREATE_ANCHOR);
		assert(peer->local.offer_anchor == CMD_OPEN_WITHOUT_ANCHOR);
	}
		
	o->anch = anchor;
	o->min_depth = peer->local.mindepth;
	queue_pkt(peer, PKT__PKT_OPEN, o);
}

void queue_pkt_anchor(struct peer *peer)
{
	OpenAnchor *a = tal(peer, OpenAnchor);

	open_anchor__init(a);
	a->txid = sha256_to_proto(a, &peer->anchor.txid.sha);
	a->output_index = peer->anchor.index;
	a->amount = peer->anchor.satoshis;

	queue_pkt(peer, PKT__PKT_OPEN_ANCHOR, a);
}

void queue_pkt_open_commit_sig(struct peer *peer)
{
	OpenCommitSig *s = tal(peer, OpenCommitSig);

	open_commit_sig__init(s);

	s->sig = signature_to_proto(s, peer->dstate->secpctx,
				    &peer->remote.commit->sig->sig);

	queue_pkt(peer, PKT__PKT_OPEN_COMMIT_SIG, s);
}

void queue_pkt_open_complete(struct peer *peer)
{
	OpenComplete *o = tal(peer, OpenComplete);

	open_complete__init(o);
	queue_pkt(peer, PKT__PKT_OPEN_COMPLETE, o);
}

void queue_pkt_htlc_add(struct peer *peer, struct htlc *htlc)
{
	UpdateAddHtlc *u = tal(peer, UpdateAddHtlc);

	update_add_htlc__init(u);

	u->id = htlc->id;
	u->amount_msat = htlc->msatoshis;
	u->r_hash = sha256_to_proto(u, &htlc->rhash);
	u->expiry = abs_locktime_to_proto(u, &htlc->expiry);
	u->route = tal(u, Routing);
	routing__init(u->route);
	u->route->info.data = tal_dup_arr(u, u8,
					  htlc->routing,
					  tal_count(htlc->routing),
					  0);
	u->route->info.len = tal_count(u->route->info.data);

	queue_pkt(peer, PKT__PKT_UPDATE_ADD_HTLC, u);
}

void queue_pkt_htlc_fulfill(struct peer *peer, struct htlc *htlc)
{
	UpdateFulfillHtlc *f = tal(peer, UpdateFulfillHtlc);

	update_fulfill_htlc__init(f);
	f->id = htlc->id;
	f->r = rval_to_proto(f, htlc->r);

	queue_pkt(peer, PKT__PKT_UPDATE_FULFILL_HTLC, f);
}

void queue_pkt_htlc_fail(struct peer *peer, struct htlc *htlc)
{
	UpdateFailHtlc *f = tal(peer, UpdateFailHtlc);

	update_fail_htlc__init(f);
	f->id = htlc->id;

	/* FIXME: reason! */
	f->reason = tal(f, FailReason);
	fail_reason__init(f->reason);

	queue_pkt(peer, PKT__PKT_UPDATE_FAIL_HTLC, f);
}

/* OK, we're sending a signature for their pending changes. */
void queue_pkt_commit(struct peer *peer, const struct bitcoin_signature *sig)
{
	UpdateCommit *u = tal(peer, UpdateCommit);

	/* Now send message */
	update_commit__init(u);
	if (sig)
		u->sig = signature_to_proto(u, peer->dstate->secpctx,
					    &sig->sig);
	else
		u->sig = NULL;

	queue_pkt(peer, PKT__PKT_UPDATE_COMMIT, u);
}


/* Send a preimage for the old commit tx.  The one we've just committed to is
 * in peer->local.commit. */
void queue_pkt_revocation(struct peer *peer,
			  const struct sha256 *preimage,
			  const struct sha256 *next_hash)
{
	UpdateRevocation *u = tal(peer, UpdateRevocation);

	update_revocation__init(u);

	u->revocation_preimage = sha256_to_proto(u, preimage);
	u->next_revocation_hash	= sha256_to_proto(u, next_hash);
	queue_pkt(peer, PKT__PKT_UPDATE_REVOCATION, u);
}

Pkt *pkt_err(struct peer *peer, const char *msg, ...)
{
	Error *e = tal(peer, Error);
	va_list ap;

	error__init(e);
	va_start(ap, msg);
	e->problem = tal_vfmt(e, msg, ap);
	va_end(ap);

	log_unusual(peer->log, "Sending PKT_ERROR: %s", e->problem);
	return make_pkt(peer, PKT__PKT_ERROR, e);
}

Pkt *pkt_reconnect(struct peer *peer, u64 ack)
{
	Reconnect *r = tal(peer, Reconnect);
	reconnect__init(r);
	r->ack = ack;
	return make_pkt(peer, PKT__PKT_RECONNECT, r);
}

void queue_pkt_err(struct peer *peer, Pkt *err)
{
	queue_raw_pkt(peer, err);
}

void queue_pkt_close_shutdown(struct peer *peer)
{
	u8 *redeemscript;
	CloseShutdown *c = tal(peer, CloseShutdown);

	close_shutdown__init(c);
	redeemscript = bitcoin_redeem_single(c, peer->dstate->secpctx,
					     &peer->local.finalkey);
	peer->closing.our_script = scriptpubkey_p2sh(peer, redeemscript);

	c->scriptpubkey.data = tal_dup_arr(c, u8,
					   peer->closing.our_script,
					   tal_count(peer->closing.our_script),
					   0);
	c->scriptpubkey.len = tal_count(c->scriptpubkey.data);

	queue_pkt(peer, PKT__PKT_CLOSE_SHUTDOWN, c);
}

void queue_pkt_close_signature(struct peer *peer)
{
	CloseSignature *c = tal(peer, CloseSignature);
	struct bitcoin_tx *close_tx;
	struct signature our_close_sig;

	close_signature__init(c);
	close_tx = peer_create_close_tx(peer, peer->closing.our_fee);

	peer_sign_mutual_close(peer, close_tx, &our_close_sig);
	c->sig = signature_to_proto(c, peer->dstate->secpctx, &our_close_sig);
	c->close_fee = peer->closing.our_fee;
	log_info(peer->log, "queue_pkt_close_signature: offered close fee %"
		 PRIu64, c->close_fee);

	queue_pkt(peer, PKT__PKT_CLOSE_SIGNATURE, c);
}

Pkt *pkt_err_unexpected(struct peer *peer, const Pkt *pkt)
{
	return pkt_err(peer, "Unexpected packet %s", pkt_name(pkt->pkt_case));
}

/* Process various packets: return an error packet on failure. */
Pkt *accept_pkt_open(struct peer *peer, const Pkt *pkt,
		     struct sha256 *revocation_hash,
		     struct sha256 *next_revocation_hash)
{
	struct rel_locktime locktime;
	const OpenChannel *o = pkt->open;
	u64 feerate = get_feerate(peer->dstate);

	if (!proto_to_rel_locktime(o->delay, &locktime))
		return pkt_err(peer, "Invalid delay");
	if (o->delay->locktime_case != LOCKTIME__LOCKTIME_BLOCKS)
		return pkt_err(peer, "Delay in seconds not accepted");
	if (o->delay->blocks > peer->dstate->config.locktime_max)
		return pkt_err(peer, "Delay too great");
	if (o->min_depth > peer->dstate->config.anchor_confirms_max)
		return pkt_err(peer, "min_depth too great");
	if (o->initial_fee_rate
	    < feerate * peer->dstate->config.commitment_fee_min_percent / 100)
		return pkt_err(peer, "Commitment fee rate too low");
	if (o->initial_fee_rate
	    > feerate * peer->dstate->config.commitment_fee_max_percent / 100)
		return pkt_err(peer, "Commitment fee rate too low");
	if (o->anch == OPEN_CHANNEL__ANCHOR_OFFER__WILL_CREATE_ANCHOR)
		peer->remote.offer_anchor = CMD_OPEN_WITH_ANCHOR;
	else if (o->anch == OPEN_CHANNEL__ANCHOR_OFFER__WONT_CREATE_ANCHOR)
		peer->remote.offer_anchor = CMD_OPEN_WITHOUT_ANCHOR;
	else
		return pkt_err(peer, "Unknown offer anchor value");

	if (peer->remote.offer_anchor == peer->local.offer_anchor)
		return pkt_err(peer, "Only one side can offer anchor");

	if (!proto_to_rel_locktime(o->delay, &peer->remote.locktime))
		return pkt_err(peer, "Malformed locktime");
	peer->remote.mindepth = o->min_depth;
	peer->remote.commit_fee_rate = o->initial_fee_rate;
	if (!proto_to_pubkey(peer->dstate->secpctx,
			     o->commit_key, &peer->remote.commitkey))
		return pkt_err(peer, "Bad commitkey");
	if (!proto_to_pubkey(peer->dstate->secpctx,
			     o->final_key, &peer->remote.finalkey))
		return pkt_err(peer, "Bad finalkey");

	proto_to_sha256(o->revocation_hash, revocation_hash);
	proto_to_sha256(o->next_revocation_hash, next_revocation_hash);
	return NULL;
}

Pkt *accept_pkt_anchor(struct peer *peer, const Pkt *pkt)
{
	const OpenAnchor *a = pkt->open_anchor;

	/* They must be offering anchor for us to try accepting */
	assert(peer->local.offer_anchor == CMD_OPEN_WITHOUT_ANCHOR);
	assert(peer->remote.offer_anchor == CMD_OPEN_WITH_ANCHOR);

	proto_to_sha256(a->txid, &peer->anchor.txid.sha);
	peer->anchor.index = a->output_index;
	peer->anchor.satoshis = a->amount;
	return NULL;
}

Pkt *accept_pkt_open_commit_sig(struct peer *peer, const Pkt *pkt,
				struct bitcoin_signature **sig)
{
	const OpenCommitSig *s = pkt->open_commit_sig;
	struct signature signature;

	if (!proto_to_signature(peer->dstate->secpctx, s->sig, &signature))
		return pkt_err(peer, "Malformed signature");

	*sig = tal(peer, struct bitcoin_signature);	
	(*sig)->stype = SIGHASH_ALL;
	(*sig)->sig = signature;	
	return NULL;
}

Pkt *accept_pkt_open_complete(struct peer *peer, const Pkt *pkt)
{
	return NULL;
}

/*
 * We add changes to both our staging cstate (as they did when they sent
 * it) and theirs (as they will when we ack it).
 */
Pkt *accept_pkt_htlc_add(struct peer *peer, const Pkt *pkt, struct htlc **h)
{
	const UpdateAddHtlc *u = pkt->update_add_htlc;
	struct sha256 rhash;
	struct abs_locktime expiry;

	/* BOLT #2:
	 *
	 * `amount_msat` MUST BE greater than 0.
	 */
	if (u->amount_msat == 0)
		return pkt_err(peer, "Invalid amount_msat");

	proto_to_sha256(u->r_hash, &rhash);
	if (!proto_to_abs_locktime(u->expiry, &expiry))
		return pkt_err(peer, "Invalid HTLC expiry");

	if (abs_locktime_is_seconds(&expiry))
		return pkt_err(peer, "HTLC expiry in seconds not supported!");

	/* BOLT #2:
	 *
	 * A node MUST NOT add a HTLC if it would result in it
	 * offering more than 300 HTLCs in the remote commitment transaction.
	 */
	if (peer->remote.staging_cstate->side[THEIRS].num_htlcs == 300)
		return pkt_err(peer, "Too many HTLCs");

	/* BOLT #2:
	 *
	 * A node MUST set `id` to a unique identifier for this HTLC
	 * amongst all past or future `update_add_htlc` messages.
	 */
	/* Note that it's not *our* problem if they do this, it's
	 * theirs (future confusion).  Nonetheless, we detect and
	 * error for them. */
	if (htlc_get(&peer->htlcs, u->id, REMOTE))
		return pkt_err(peer, "HTLC id %"PRIu64" clashes for you", u->id);

	/* BOLT #2:
	 *
	 * ...and the receiving node MUST add the HTLC addition to the
	 * unacked changeset for its local commitment. */
	*h = peer_new_htlc(peer, u->id, u->amount_msat, &rhash,
			   abs_locktime_to_blocks(&expiry),
			   u->route->info.data, u->route->info.len,
			   NULL, RCVD_ADD_HTLC);
	return NULL;
}

static Pkt *find_commited_htlc(struct peer *peer, uint64_t id,
			       struct htlc **local_htlc)
{
	*local_htlc = htlc_get(&peer->htlcs, id, LOCAL);

	/* BOLT #2:
	 *
	 * A node MUST check that `id` corresponds to an HTLC in its
	 * current commitment transaction, and MUST fail the
	 * connection if it does not.
	 */
	if (!(*local_htlc))
		return pkt_err(peer, "Did not find HTLC %"PRIu64, id);

	if ((*local_htlc)->state != SENT_ADD_ACK_REVOCATION)
		return pkt_err(peer, "HTLC %"PRIu64" state %s", id,
			       htlc_state_name((*local_htlc)->state));

	return NULL;
}

Pkt *accept_pkt_htlc_fail(struct peer *peer, const Pkt *pkt, struct htlc **h)
{
	const UpdateFailHtlc *f = pkt->update_fail_htlc;
	Pkt *err;

	err = find_commited_htlc(peer, f->id, h);
	if (err)
		return err;

	/* FIXME: Save reason. */
	return NULL;
}

Pkt *accept_pkt_htlc_fulfill(struct peer *peer, const Pkt *pkt, struct htlc **h,
			     bool *was_already_fulfilled)
{
	const UpdateFulfillHtlc *f = pkt->update_fulfill_htlc;
	struct sha256 rhash;
	struct rval r;
	Pkt *err;

	err = find_commited_htlc(peer, f->id, h);
	if (err)
		return err;

	/* Now, it must solve the HTLC rhash puzzle. */
	proto_to_rval(f->r, &r);
	sha256(&rhash, &r, sizeof(r));

	if (!structeq(&rhash, &(*h)->rhash))
		return pkt_err(peer, "Invalid r for %"PRIu64, f->id);

	if ((*h)->r) {
		*was_already_fulfilled = true;
	} else {
		*was_already_fulfilled = false;
		(*h)->r = tal_dup(*h, struct rval, &r);
	}
	return NULL;
}

Pkt *accept_pkt_commit(struct peer *peer, const Pkt *pkt,
		       struct bitcoin_signature *sig)
{
	const UpdateCommit *c = pkt->update_commit;

	if (!c->sig && sig)
		return pkt_err(peer, "Expected signature");

	if (!sig && c->sig)
		return pkt_err(peer, "Unexpected signature");

	if (!sig && !c->sig)
		return NULL;

	sig->stype = SIGHASH_ALL;
	if (!proto_to_signature(peer->dstate->secpctx, c->sig, &sig->sig))
		return pkt_err(peer, "Malformed signature");
	return NULL;
}

Pkt *accept_pkt_revocation(struct peer *peer, const Pkt *pkt)
{
	const UpdateRevocation *r = pkt->update_revocation;
	struct sha256 h, preimage;

	assert(peer->their_prev_revocation_hash);
	proto_to_sha256(r->revocation_preimage, &preimage);

	/* BOLT #2:
	 *
	 * The receiver of `update_revocation` MUST check that the
	 * SHA256 hash of `revocation_preimage` matches the previous commitment
	 * transaction, and MUST fail if it does not.
	 */
	sha256(&h, &preimage, sizeof(preimage));
	if (!structeq(&h, peer->their_prev_revocation_hash)) {
		log_unusual(peer->log, "Incorrect preimage for %"PRIu64,
			    peer->remote.commit->commit_num - 1);
		return pkt_err(peer, "complete preimage incorrect");
	}

	// save revocation preimages in shachain
	if (!shachain_add_hash(&peer->their_preimages,
			       0xFFFFFFFFFFFFFFFFL
			       - (peer->remote.commit->commit_num - 1),
			       &preimage))
		return pkt_err(peer, "preimage not next in shachain");

	log_debug(peer->log, "Got revocation preimage %"PRIu64,
		  peer->remote.commit->commit_num - 1);

	/* Clear the previous revocation hash. */
	peer->their_prev_revocation_hash
		= tal_free(peer->their_prev_revocation_hash);

	/* Save next revocation hash. */
	proto_to_sha256(r->next_revocation_hash,
			&peer->remote.next_revocation_hash);
	return NULL;
}
	
Pkt *accept_pkt_close_shutdown(struct peer *peer, const Pkt *pkt)
{
	const CloseShutdown *c = pkt->close_shutdown;

	/* FIXME: Filter for non-standardness? */
	peer->closing.their_script = tal_dup_arr(peer, u8,
						 c->scriptpubkey.data,
						 c->scriptpubkey.len, 0);

	return NULL;
}
