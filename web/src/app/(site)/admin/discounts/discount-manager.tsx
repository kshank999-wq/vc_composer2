'use client';

import { useState } from 'react';
import {
  advertisedLink,
  describeNewDiscount,
  describeOff,
  describeStanding,
  type DiscountOffer,
  isRedeemable,
  type NewDiscount,
  newDiscountRefusal,
  normaliseCode,
} from '@/domain';

/**
 * Making a code, and the advertisement's link beside it.
 *
 * Every sentence on this screen is the domain's — what a press would do, what
 * is wrong with a plan, how a code is standing — so the screen and the act
 * cannot disagree about any of it.
 */
export function DiscountManager({
  initialOffers,
  siteUrl,
}: {
  initialOffers: DiscountOffer[];
  siteUrl: string;
}) {
  const [offers, setOffers] = useState(initialOffers);
  const [code, setCode] = useState('');
  const [kind, setKind] = useState<'percent' | 'amount'>('percent');
  const [percent, setPercent] = useState('20');
  const [amount, setAmount] = useState('10');
  const [currency, setCurrency] = useState('usd');
  const [limit, setLimit] = useState('');
  const [ends, setEnds] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [copied, setCopied] = useState<string | null>(null);

  const plan: NewDiscount = {
    code,
    off:
      kind === 'percent'
        ? { kind: 'percent', percent: Number(percent) }
        : { kind: 'amount', amountCents: Math.round(Number(amount) * 100), currency },
    maxRedemptions: limit.trim() === '' ? null : Number(limit),
    expiresAt: ends.trim() === '' ? null : new Date(`${ends}T23:59:59Z`).toISOString(),
  };
  // Said before the press, and said again by the act itself.
  const refusal = newDiscountRefusal(plan, new Date());

  const make = async () => {
    setBusy(true);
    setError(null);
    try {
      const response = await fetch('/api/admin/discounts', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(plan),
      });
      const payload = (await response.json()) as { offer?: DiscountOffer; error?: string };
      if (!response.ok || !payload.offer) throw new Error(payload.error ?? 'The code could not be made.');
      setOffers((was) => [payload.offer as DiscountOffer, ...was]);
      setCode('');
      setLimit('');
      setEnds('');
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : 'The code could not be made.');
    } finally {
      setBusy(false);
    }
  };

  const flip = async (offer: DiscountOffer) => {
    const response = await fetch('/api/admin/discounts', {
      method: 'PATCH',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ promotionCodeId: offer.promotionCodeId, active: !offer.active }),
    });
    if (!response.ok) {
      setError('Stripe would not change it.');
      return;
    }
    setOffers((was) =>
      was.map((one) =>
        one.promotionCodeId === offer.promotionCodeId ? { ...one, active: !one.active } : one,
      ),
    );
  };

  const copy = async (text: string, which: string) => {
    try {
      await navigator.clipboard.writeText(text);
      setCopied(which);
      window.setTimeout(() => setCopied(null), 2000);
    } catch {
      setError('The link could not be copied.');
    }
  };

  const now = new Date();

  return (
    <>
      <section className="card">
        <h2>Make a code</h2>
        <div className="discount-fields">
          <label className="field">
            <span>Code</span>
            <input
              value={code}
              onChange={(event) => setCode(normaliseCode(event.target.value))}
              placeholder="LAUNCH20"
              autoComplete="off"
              spellCheck={false}
            />
          </label>

          <label className="field">
            <span>Takes off</span>
            <select value={kind} onChange={(event) => setKind(event.target.value as 'percent' | 'amount')}>
              <option value="percent">A percentage</option>
              <option value="amount">A fixed amount</option>
            </select>
          </label>

          {kind === 'percent' ? (
            <label className="field">
              <span>Percent</span>
              <input
                type="number"
                min={1}
                max={100}
                value={percent}
                onChange={(event) => setPercent(event.target.value)}
              />
            </label>
          ) : (
            <>
              <label className="field">
                <span>Amount</span>
                <input
                  type="number"
                  min={0}
                  step="0.01"
                  value={amount}
                  onChange={(event) => setAmount(event.target.value)}
                />
              </label>
              <label className="field">
                <span>Currency</span>
                <input
                  value={currency}
                  onChange={(event) => setCurrency(event.target.value.toLowerCase())}
                  maxLength={3}
                />
              </label>
            </>
          )}

          <label className="field">
            <span>Limit</span>
            <input
              type="number"
              min={1}
              value={limit}
              onChange={(event) => setLimit(event.target.value)}
              placeholder="No limit"
            />
          </label>

          <label className="field">
            <span>Ends</span>
            <input type="date" value={ends} onChange={(event) => setEnds(event.target.value)} />
          </label>
        </div>

        {/* What the press would do, before it can be asked for. */}
        <p className="field-note">
          {refusal ?? describeNewDiscount(plan)}
        </p>

        <button type="button" className="button" onClick={make} disabled={busy || refusal !== null}>
          {busy ? 'Making it…' : 'Make the code'}
        </button>

        {error ? (
          <p className="error" role="alert">
            {error}
          </p>
        ) : null}
      </section>

      <section className="card">
        <h2>Codes</h2>
        {offers.length === 0 ? (
          <p className="lede">No codes yet. One made here works at checkout straight away.</p>
        ) : (
          <div className="data-table discounts-table">
            <table>
              <thead>
                <tr>
                  <th>Code</th>
                  <th>Takes off</th>
                  <th>Standing</th>
                  <th>For an advertisement</th>
                  <th />
                </tr>
              </thead>
              <tbody>
                {offers.map((offer) => {
                  const link = advertisedLink(siteUrl, offer.code);
                  return (
                    <tr key={offer.promotionCodeId} className={isRedeemable(offer, now) ? undefined : 'spent'}>
                      <td>
                        <strong>{offer.code}</strong>
                      </td>
                      <td>{describeOff(offer.off)}</td>
                      <td>{describeStanding(offer, now)}</td>
                      {/* `wrap` is the table's own opt-in to wrapping — its
                          cells are `nowrap` by default. Taking it off is what
                          made the link run straight through the column beside
                          it and print over `Switch off`. */}
                      <td className="wrap">
                        <div className="discount-cell">
                        {/* The link **is** the advertising half of the ask: a
                            code with no link beside it is six characters a
                            reader has to carry from the page to the checkout. */}
                          <code className="discount-link">{link}</code>
                          <button type="button" className="text-act" onClick={() => void copy(link, offer.code)}>
                            {copied === offer.code ? 'Copied' : 'Copy'}
                          </button>
                        </div>
                      </td>
                      <td>
                        <button type="button" className="text-act" onClick={() => void flip(offer)}>
                          {offer.active ? 'Switch off' : 'Switch on'}
                        </button>
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </section>
    </>
  );
}
