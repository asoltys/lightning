#include "channel.h"
#include "htlc.h"
#include "remove_dust.h"
#include <assert.h>
#include <ccan/array_size/array_size.h>
#include <ccan/mem/mem.h>
#include <ccan/structeq/structeq.h>
#include <string.h>

uint64_t fee_by_feerate(size_t txsize, uint64_t fee_rate)
{
	/* BOLT #2:
	 * 
	 * The fee for a transaction MUST be calculated by multiplying this
	 * bytecount by the fee rate, dividing by 1000 and truncating
	 * (rounding down) the result to an even number of satoshis.
	 */
	return txsize * fee_rate / 2000 * 2;
}

static uint64_t calculate_fee_msat(size_t num_nondust_htlcs,
				   uint64_t fee_rate)
{
	uint64_t bytes;

	/* BOLT #2:
	 *
	 * A node MUST use the formula 338 + 32 bytes for every
	 * non-dust HTLC as the bytecount for calculating commitment
	 * transaction fees.  Note that the fee requirement is
	 * unchanged, even if the elimination of dust HTLC outputs has
	 * caused a non-zero fee already.
	*/
	bytes = 338 + 32 * num_nondust_htlcs;

	/* milli-satoshis */
	return fee_by_feerate(bytes, fee_rate) * 1000;
}

/* Pay this much fee, if possible.  Return amount unpaid. */
static uint64_t pay_fee(struct channel_oneside *side, uint64_t fee_msat)
{
	if (side->pay_msat >= fee_msat) {
		side->pay_msat -= fee_msat;
		side->fee_msat += fee_msat;
		return 0;
	} else {
		uint64_t remainder = fee_msat - side->pay_msat;
		side->fee_msat += side->pay_msat;
		side->pay_msat = 0;
		return remainder;
	}
}

/* Charge the fee as per BOLT #2 */
static void recalculate_fees(struct channel_oneside *a,
			     struct channel_oneside *b,
			     uint64_t fee_msat)
{
	uint64_t remainder;

	/* Fold in fees, to recalcuate again below. */
	a->pay_msat += a->fee_msat;
	b->pay_msat += b->fee_msat;
	a->fee_msat = b->fee_msat = 0;

	/* BOLT #2:
	 *
	 * 1. If each nodes can afford half the fee from their
	 *    to-`final_key` output, reduce the two to-`final_key`
	 *    outputs accordingly.
	 *
	 * 2. Otherwise, reduce the to-`final_key` output of one node
	 *    which cannot afford the fee to zero (resulting in that
	 *    entire output paying fees).  If the remaining
	 *    to-`final_key` output is greater than the fee remaining,
	 *    reduce it accordingly, otherwise reduce it to zero to
	 *    pay as much fee as possible.
	 */
	remainder = pay_fee(a, fee_msat / 2) + pay_fee(b, fee_msat / 2);

	/* If there's anything left, the other side tries to pay for it. */
	remainder = pay_fee(a, remainder);
	pay_fee(b, remainder);
}

/* a transfers htlc_msat to a HTLC (gains it, if -ve) */
static bool change_funding(uint64_t anchor_satoshis,
			   uint64_t fee_rate,
			   int64_t htlc_msat,
			   struct channel_oneside *a,
			   struct channel_oneside *b,
			   size_t num_nondust_htlcs)
{
	uint64_t fee_msat;
	uint64_t htlcs_total;

	htlcs_total = anchor_satoshis * 1000
		- (a->pay_msat + a->fee_msat + b->pay_msat + b->fee_msat);

	fee_msat = calculate_fee_msat(num_nondust_htlcs, fee_rate);

	/* If A is paying, can it afford it? */
	if (htlc_msat > 0) {
		if (htlc_msat + fee_msat / 2 > a->pay_msat + a->fee_msat)
			return false;
	}

	/* OK, now adjust funds for A, then recalculate fees. */
	a->pay_msat -= htlc_msat;
	recalculate_fees(a, b, fee_msat);

	htlcs_total += htlc_msat;
	assert(htlcs_total == anchor_satoshis * 1000
	       - (a->pay_msat + a->fee_msat + b->pay_msat + b->fee_msat));
	return true;
}

struct channel_state *initial_cstate(const tal_t *ctx,
				      uint64_t anchor_satoshis,
				      uint64_t fee_rate,
				      enum channel_side funding)
{
	uint64_t fee_msat;
	struct channel_state *cstate = talz(ctx, struct channel_state);
	struct channel_oneside *funder, *fundee;

	cstate->fee_rate = fee_rate;
	cstate->anchor = anchor_satoshis;
	cstate->num_nondust = 0;

	/* Anchor must fit in 32 bit. */
	if (anchor_satoshis >= (1ULL << 32) / 1000)
		return tal_free(cstate);

	fee_msat = calculate_fee_msat(0, fee_rate);
	if (fee_msat > anchor_satoshis * 1000)
		return tal_free(cstate);

	funder = &cstate->side[funding];
	fundee = &cstate->side[!funding];

	/* Neither side has HTLCs. */
	funder->num_htlcs = fundee->num_htlcs = 0;

	/* Initially, all goes back to funder. */
	funder->pay_msat = anchor_satoshis * 1000 - fee_msat;
	funder->fee_msat = fee_msat;

	/* Make sure it checks out. */
	assert(change_funding(anchor_satoshis, fee_rate, 0, funder, fundee, 0));
	assert(funder->fee_msat == fee_msat);
	assert(fundee->fee_msat == 0);

	return cstate;
}

void adjust_fee(struct channel_state *cstate, uint64_t fee_rate)
{
	uint64_t fee_msat;

	fee_msat = calculate_fee_msat(cstate->num_nondust, fee_rate);

	recalculate_fees(&cstate->side[OURS], &cstate->side[THEIRS], fee_msat);
}

bool force_fee(struct channel_state *cstate, uint64_t fee)
{
	/* Beware overflow! */
	if (fee > 0xFFFFFFFFFFFFFFFFULL / 1000)
		return false;
	recalculate_fees(&cstate->side[OURS], &cstate->side[THEIRS], fee * 1000);
	return cstate->side[OURS].fee_msat + cstate->side[THEIRS].fee_msat == fee * 1000;
}

/* Add a HTLC to @creator if it can afford it. */
bool cstate_add_htlc(struct channel_state *cstate, const struct htlc *htlc)
{
	size_t nondust;
	struct channel_oneside *creator, *recipient;

	creator = &cstate->side[htlc_channel_side(htlc)];
	recipient = &cstate->side[!htlc_channel_side(htlc)];
	
	/* Remember to count the new one in total txsize if not dust! */
	nondust = cstate->num_nondust;
	if (!is_dust(htlc->msatoshis / 1000))
		nondust++;
	
	if (!change_funding(cstate->anchor, cstate->fee_rate,
			    htlc->msatoshis, creator, recipient, nondust))
		return false;

	cstate->num_nondust = nondust;
	creator->num_htlcs++;
	return true;
}

/* Remove htlc from creator, credit it to beneficiary. */
static void remove_htlc(struct channel_state *cstate,
			enum channel_side creator,
			enum channel_side beneficiary,
			const struct htlc *htlc)
{
	size_t nondust;

	/* Remember to remove this one in total txsize if not dust! */
	nondust = cstate->num_nondust;
	if (!is_dust(htlc->msatoshis / 1000)) {
		assert(nondust > 0);
		nondust--;
	}

	/* Can't fail since msatoshis is positive. */
	if (!change_funding(cstate->anchor, cstate->fee_rate,
			    -(int64_t)htlc->msatoshis,
			    &cstate->side[beneficiary],
			    &cstate->side[!beneficiary], nondust))
		abort();

	/* Actually remove the HTLC. */
	assert(cstate->side[creator].num_htlcs > 0);
	cstate->side[creator].num_htlcs--;
	cstate->num_nondust = nondust;
}

void cstate_fail_htlc(struct channel_state *cstate, const struct htlc *htlc)
{
	remove_htlc(cstate, htlc_channel_side(htlc), htlc_channel_side(htlc),
		    htlc);
}

void cstate_fulfill_htlc(struct channel_state *cstate, const struct htlc *htlc)
{
	remove_htlc(cstate, htlc_channel_side(htlc), !htlc_channel_side(htlc),
		    htlc);
}

struct channel_state *copy_cstate(const tal_t *ctx,
				  const struct channel_state *cstate)
{
	return tal_dup(ctx, struct channel_state, cstate);
}
