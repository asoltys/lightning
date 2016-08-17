#ifndef LIGHTNING_DAEMON_CHANNEL_H
#define LIGHTNING_DAEMON_CHANNEL_H
#include "config.h"
#include "bitcoin/locktime.h"
#include <ccan/crypto/sha256/sha256.h>
#include <ccan/tal/tal.h>
#include <stdbool.h>

struct htlc;

struct channel_oneside {
	/* Payment and fee is in millisatoshi. */
	uint32_t pay_msat, fee_msat;
	/* Number of HTLCs (required for limiting total number) */
	unsigned int num_htlcs;
};

enum channel_side {
	/* Output for us, htlcs we offered to them. */
	OURS,
	/* Output for them, htlcs they offered to us. */
	THEIRS
};

struct channel_state {
	/* Satoshis paid by anchor. */
	uint64_t anchor;
	/* Satoshis per 1000 bytes. */
	uint32_t fee_rate;
	/* Number of non-dust htlcs (to calculate txsize) */
	unsigned int num_nondust;
	struct channel_oneside side[2];
};

/**
 * initial_cstate: Given initial fees and funding anchor, what is initial state?
 * @ctx: tal context to allocate return value from.
 * @anchor_satoshis: The anchor amount.
 * @fee_rate: amount to pay in fees per kb (in satoshi).
 * @dir: which side paid for the anchor.
 *
 * Returns state, or NULL if malformed.
 */
struct channel_state *initial_cstate(const tal_t *ctx,
				     uint64_t anchor_satoshis,
				     uint32_t fee_rate,
				     enum channel_side side);

/**
 * copy_cstate: Make a deep copy of channel_state
 * @ctx: tal context to allocate return value from.
 * @cstate: state to copy.
 */
struct channel_state *copy_cstate(const tal_t *ctx,
				  const struct channel_state *cstate);

/**
 * cstate_add_htlc: append an HTLC to cstate if it can afford it
 * @cstate: The channel state
 * @htlc: the htlc pointer.
 *
 * If that direction can't afford the HTLC (or still owes its half of the fees),
 * this will return false and leave @cstate unchanged.  Otherwise, pay_msat and
 * fee_msat are adjusted accordingly; true is returned.
 */
bool cstate_add_htlc(struct channel_state *cstate, const struct htlc *htlc);
/**
 * cstate_fail_htlc: remove an HTLC, funds to the side which offered it.
 * @cstate: The channel state
 * @htlc: the htlc to remove.
 *
 * This will remove the @index'th entry in cstate->side[dir].htlcs[], and credit
 * the value of the HTLC (back) to cstate->side[dir].
 */
void cstate_fail_htlc(struct channel_state *cstate, const struct htlc *htlc);

/**
 * cstate_fulfill_htlc: remove an HTLC, funds to side which accepted it.
 * @cstate: The channel state
 * @htlc: the htlc to remove
 *
 * This will remove the @index'th entry in cstate->side[dir].htlcs[], and credit
 * the value of the HTLC to cstate->side[!dir].
 */
void cstate_fulfill_htlc(struct channel_state *cstate, const struct htlc *htlc);

/**
 * adjust_fee: Change fee rate.
 * @cstate: The channel state
 * @fee_rate: fee in satoshi per 1000 bytes.
 */
void adjust_fee(struct channel_state *cstate, uint32_t fee_rate);

/**
 * force_fee: Change fee to a specific value.
 * @cstate: The channel state
 * @fee: fee in satoshi.
 *
 * This is used for the close transaction, which specifies an exact fee.
 * If the fee cannot be paid in full, this return false (but cstate will
 * still be altered).
 */
bool force_fee(struct channel_state *cstate, uint64_t fee);

/**
 * fee_for_feerate: calculate the fee (in satoshi) for a given fee_rate.
 * @txsize: transaction size in bytes.
 * @fee_rate: satoshi per 1000 bytes.
 */
uint64_t fee_by_feerate(size_t txsize, uint32_t fee_rate);

#endif /* LIGHTNING_DAEMON_CHANNEL_H */
