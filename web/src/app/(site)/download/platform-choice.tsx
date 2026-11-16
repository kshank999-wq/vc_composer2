'use client';

import { useCallback, useEffect, useRef, useState } from 'react';
import { codeRefusal, normaliseCode, PLANS, planWords, type Platform, type SubscriptionPlan } from '@/domain';

/** What a plan costs, as the shop answered — never computed here. */
export interface PlanOffer {
  plan: SubscriptionPlan;
  label: string;
  per: string;
  formatted: string;
  savingPercent: number | null;
}

const OPTIONS: ReadonlyArray<{ platform: Platform; label: string; detail: string }> = [
  { platform: 'windows', label: 'Windows', detail: 'Windows 10 and Windows 11, 64-bit' },
  { platform: 'macos', label: 'Mac', detail: 'macOS' },
];

/** What the shop said a code is worth. */
interface Checked {
  code: string;
  takes: string;
  was: string | null;
  now: string | null;
}

export function PlatformChoice({
  advertisedCode = null,
  plans = [],
}: {
  advertisedCode?: string | null;
  plans?: PlanOffer[];
}) {
  const [platform, setPlatform] = useState<Platform>('windows');
  // Monthly where the shop could answer for it, otherwise whatever it could —
  // **a plan Stripe has never heard of is absent rather than offered**, so the
  // chosen one is always one of the plans actually drawn.
  const [plan, setPlan] = useState<SubscriptionPlan>(plans[0]?.plan ?? 'monthly');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const [typed, setTyped] = useState(advertisedCode ? normaliseCode(advertisedCode) : '');
  const [checking, setChecking] = useState(false);
  const [applied, setApplied] = useState<Checked | null>(null);
  const [codeError, setCodeError] = useState<string | null>(null);

  /**
   * Ask the shop what a code is worth. The shop decides, every time — this
   * never computes a discount and never remembers one, so what is drawn is
   * always what the till will charge.
   */
  const check = useCallback(async (said: string): Promise<void> => {
    const word = codeRefusal(said);
    if (word) {
      setApplied(null);
      setCodeError(word);
      return;
    }
    setChecking(true);
    setCodeError(null);
    try {
      // The plan goes with it: what a code takes off is the same, but what is
      // then owed is the chosen plan's, so asking without it would print a
      // yearly code's saving against a monthly price.
      const response = await fetch(
        `/api/discount?code=${encodeURIComponent(normaliseCode(said))}&plan=${plan}`,
      );
      const payload = (await response.json()) as Partial<Checked> & { ok?: boolean; error?: string };
      if (payload.ok && payload.code && payload.takes) {
        setApplied({
          code: payload.code,
          takes: payload.takes,
          was: payload.was ?? null,
          now: payload.now ?? null,
        });
      } else {
        setApplied(null);
        setCodeError(payload.error ?? 'That code is not valid.');
      }
    } catch {
      setApplied(null);
      setCodeError('That code could not be checked.');
    } finally {
      setChecking(false);
    }
  }, [plan]);

  /**
   * **A code carried in from an advertisement applies itself.** A reader who
   * followed the link has already said the code.
   */
  const asked = useRef(false);
  useEffect(() => {
    if (!advertisedCode || asked.current) return;
    asked.current = true;
    void check(advertisedCode);
  }, [advertisedCode, check]);

  /**
   * **An applied code is asked again when the plan changes**: `was` and `now`
   * are the plan's, and leaving them would print monthly figures beside a
   * chosen yearly plan. Keyed on the plan alone, so it does not re-ask on
   * every render.
   */
  const appliedCode = applied?.code ?? null;
  const pricedFor = useRef<SubscriptionPlan | null>(null);
  useEffect(() => {
    if (!appliedCode) {
      pricedFor.current = null;
      return;
    }
    if (pricedFor.current === plan) return;
    pricedFor.current = plan;
    void check(appliedCode);
  }, [appliedCode, plan, check]);

  const startCheckout = async () => {
    setBusy(true);
    setError(null);
    try {
      const response = await fetch('/api/checkout', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        // The word, never the discount: the server asks the shop again for
        // itself, so nothing here can decide what anybody is charged.
        body: JSON.stringify({ platform, plan, ...(applied ? { code: applied.code } : {}) }),
      });
      const payload = (await response.json()) as { url?: string; error?: string };
      if (!response.ok || !payload.url) {
        throw new Error(payload.error ?? 'Checkout could not be started');
      }
      window.location.assign(payload.url);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : 'Checkout could not be started');
      setBusy(false);
    }
  };

  return (
    <section>
      {plans.length > 0 ? (
        <div className="plan-choice" role="radiogroup" aria-label="Plan">
          {plans.map((offer) => (
            <button
              key={offer.plan}
              type="button"
              className="plan-option"
              role="radio"
              aria-checked={plan === offer.plan}
              onClick={() => setPlan(offer.plan)}
            >
              <strong>{offer.label}</strong>
              <span className="plan-figure">{offer.formatted}</span>
              <span>{offer.per}</span>
              {/* Read off the two prices rather than typed, so it cannot
                  disagree with the till the day a price changes. */}
              {offer.savingPercent ? <em className="plan-saving">Save {offer.savingPercent}%</em> : null}
            </button>
          ))}
        </div>
      ) : (
        // Stripe could not be asked for the figures (or is not configured on
        // this deployment). The plans are still offered; the price is shown
        // at checkout rather than made up here.
        <div className="plan-choice" role="radiogroup" aria-label="Plan">
          {PLANS.map((one) => (
            <button
              key={one}
              type="button"
              className="plan-option"
              role="radio"
              aria-checked={plan === one}
              onClick={() => setPlan(one)}
            >
              <strong>{planWords(one).label}</strong>
              <span>Billed {planWords(one).per}. Price shown at checkout.</span>
            </button>
          ))}
        </div>
      )}

      <p className="field-note">Which computer are you installing on first? Your licence covers both.</p>
      <div className="platform-choice">
        {OPTIONS.map((option) => (
          <button
            key={option.platform}
            type="button"
            className="platform-option"
            aria-pressed={platform === option.platform}
            onClick={() => setPlatform(option.platform)}
          >
            <strong>{option.label}</strong>
            <span>{option.detail}</span>
          </button>
        ))}
      </div>

      <div className="discount-box">
        {applied ? (
          // Applied: the box is gone and what is left is the fact and a way
          // out. A field still offering to take a code, beside a code that has
          // been taken, is two answers to whether it worked.
          <p className="discount-applied" role="status">
            <strong>{applied.code}</strong> — {applied.takes}
            {applied.was && applied.now ? (
              <>
                {' '}
                · <s>{applied.was}</s> <strong>{applied.now}</strong>
              </>
            ) : null}{' '}
            <button
              type="button"
              className="text-act"
              onClick={() => {
                setApplied(null);
                setTyped('');
                setCodeError(null);
              }}
            >
              Remove
            </button>
          </p>
        ) : (
          <form
            className="discount-form"
            onSubmit={(event) => {
              event.preventDefault();
              void check(typed);
            }}
          >
            <label htmlFor="discount-code">Discount code</label>
            <div className="discount-row">
              <input
                id="discount-code"
                value={typed}
                // Upper-cased as it is typed, because the program only ever
                // makes upper-case codes and a box that quietly disagrees with
                // what is printed on the advertisement reads as a refusal.
                onChange={(event) => setTyped(normaliseCode(event.target.value))}
                placeholder="If you have one"
                autoComplete="off"
                spellCheck={false}
              />
              <button type="submit" className="button secondary" disabled={checking || typed.length === 0}>
                {checking ? 'Checking…' : 'Apply'}
              </button>
            </div>
            {codeError ? (
              <p className="field-note error" role="alert">
                {codeError}
              </p>
            ) : null}
          </form>
        )}
      </div>

      <button type="button" className="button" onClick={startCheckout} disabled={busy}>
        {busy ? 'Opening checkout…' : 'Continue to secure checkout'}
      </button>

      {error ? (
        <p className="error" role="alert" style={{ marginTop: 16 }}>
          {error}
        </p>
      ) : null}

      <p className="lede" style={{ marginTop: 24 }}>
        You get your download straight away, plus an email with your licence and a link to download either build
        at any time. Change plan or cancel whenever you like from your account.
      </p>
    </section>
  );
}
