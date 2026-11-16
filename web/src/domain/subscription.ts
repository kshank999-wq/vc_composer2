import { licenseLive } from './commerce';
import type { License, LicenseGrant, LicenseStatus } from './commerce';

/**
 * The desktop subscription.
 *
 * VC Composer 2 is sold as two recurring plans. **A subscription is not a
 * second kind of entitlement**: it is an ordinary licence row with its expiry
 * written down. Every gate in the site asks `licenseLive` (activation, the
 * download route, the account page, the admin console), so pointing a lapse at
 * the licence's status and expiry carries it everywhere with nothing else told.
 *
 * **There are no prices in this file.** Stripe is where the price lives
 * because Stripe is what charges the customer; a copy in a constant would
 * eventually disagree with the till. What a plan costs is read; what a plan
 * *is* lives here.
 *
 * The standing functions further down (`deskStandingFrom`, `noteStanding`,
 * `writingStanding`) are the reference for how the desktop app reads the
 * answer of `GET /api/licenses/standing`. The C++ app implements the same
 * rules; these tests pin them.
 */

/** The two plans. A word the client may say, never an amount it may name. */
export const PLANS = ['monthly', 'yearly'] as const;
export type SubscriptionPlan = (typeof PLANS)[number];

export const isPlan = (said: string): said is SubscriptionPlan => (PLANS as readonly string[]).includes(said);

/** What each plan is called where a composer chooses one. */
export const planWords = (plan: SubscriptionPlan): { label: string; per: string } =>
  plan === 'monthly' ? { label: 'Monthly', per: 'per month' } : { label: 'Yearly', per: 'per year' };

/**
 * What the yearly plan saves, as a whole percent, or null.
 *
 * Read off the two Stripe prices rather than typed on the page, so if the
 * yearly price changes the badge changes with it. Null where the saving is
 * nothing or the figures cannot be compared: a "Save 0%" badge is worse than
 * none, and two currencies cannot be subtracted.
 */
export const yearlySaving = (
  monthlyCents: number,
  yearlyCents: number,
  monthlyCurrency: string,
  yearlyCurrency: string,
): number | null => {
  if (monthlyCurrency.toLowerCase() !== yearlyCurrency.toLowerCase()) return null;
  const twelve = monthlyCents * 12;
  if (twelve <= 0 || yearlyCents >= twelve) return null;
  const percent = Math.round(((twelve - yearlyCents) / twelve) * 100);
  return percent > 0 ? percent : null;
};

/**
 * Stripe's subscription statuses, and which of them entitle.
 *
 * **A word this build has never heard of is not a reason to hand anything
 * over**, so the entitling ones are named and everything else (`canceled`,
 * `unpaid`, `paused`, `incomplete`, and whatever Stripe adds next) reads as
 * lapsed.
 *
 * **`past_due` entitles, deliberately.** It means the latest invoice failed and
 * Stripe is still retrying, which is overwhelmingly an expired card rather
 * than somebody leaving. Stripe stops retrying soon enough and the
 * subscription then says `canceled` or `unpaid`, which do not entitle.
 */
const ENTITLING = new Set(['active', 'trialing', 'past_due']);

export const subscriptionEntitles = (stripeStatus: string): boolean => ENTITLING.has(stripeStatus);

/** What the licence's own status becomes when Stripe says this. */
export const licenseStatusForSubscription = (stripeStatus: string): LicenseStatus =>
  subscriptionEntitles(stripeStatus) ? 'active' : 'expired';

/**
 * What a lapse reaches, said once. Projects are never taken away: what stops
 * is changing them, installing on another computer, and the features that ask
 * the website for themselves.
 */
export const DESKTOP_LAPSE_PROMISE =
  'Your projects stay on your computer and go on opening, playing and exporting. What stops is changing them, installing on another computer, and the parts that reach vc-composer.com.';

// ---------------------------------------------------------------------------
// What this machine may do
// ---------------------------------------------------------------------------

/**
 * What a machine last heard about its licence.
 *
 * Read-only cannot depend on a live answer (a composer on a plane must not be
 * refused their own score because the machine could not ask), so what the
 * server said is **written down with the day it said it**, and the reading
 * below is over that record.
 */
export interface DeskStanding {
  /** The licence's status, as the server last said it. */
  status: LicenseStatus;
  /** What it is paid up to, or null for a licence that never ends. */
  expiresAt: string | null;
  /** When this machine was last told. */
  checkedAt: string;
  /**
   * When this machine **first** saw it had lapsed, which the grace period is
   * measured from. Carried forward rather than rewritten, or every check would
   * start the week over.
   */
  seenLapsedAt: string | null;
  /** How the licence came to be, so a given one is not told to renew. Absent reads as bought. */
  grant?: LicenseGrant;
}

/** Of two live licences, the one that never ends outlasts any date. */
const endsAtLive = (license: Pick<License, 'expiresAt'>): number =>
  license.expiresAt ? new Date(license.expiresAt).getTime() : Number.POSITIVE_INFINITY;

/** A licence with no date ended whenever its status said so. */
const endsAt = (license: Pick<License, 'expiresAt'>): number =>
  license.expiresAt ? new Date(license.expiresAt).getTime() : Number.NEGATIVE_INFINITY;

export type StandingClaim = Pick<License, 'status' | 'expiresAt'> & { grant?: LicenseGrant };

const claimOf = (one: StandingClaim): StandingClaim =>
  one.grant && one.grant !== 'purchase'
    ? { status: one.status, expiresAt: one.expiresAt ?? null, grant: one.grant }
    : { status: one.status, expiresAt: one.expiresAt ?? null };

/**
 * The account's liveliest licence, or null where the account has none.
 *
 * **An account with two licences is as live as its liveliest.** Where none is
 * live, the one that ended last is the answer, its date being what the
 * customer is owed an explanation about. Null (no licence at all) is **not** a
 * lapse.
 */
export const deskStandingFrom = (licenses: readonly StandingClaim[], now: Date): StandingClaim | null => {
  if (licenses.length === 0) return null;
  // A licence that never ends answers before one that runs to a date.
  const live = licenses.filter((one) => licenseLive(one, now)).sort((a, b) => endsAtLive(b) - endsAtLive(a))[0];
  if (live) return claimOf(live);
  const ended = [...licenses].sort((a, b) => endsAt(b) - endsAt(a))[0]!;
  return claimOf(ended);
};

/**
 * Write down what the server just said, keeping what only this machine knows.
 *
 * Pure, so the host stores and never decides. A live answer clears
 * `seenLapsedAt`, so a later lapse gets its grace period again.
 */
export const noteStanding = (previous: DeskStanding | null, claim: StandingClaim, now: Date): DeskStanding => {
  const checkedAt = now.toISOString();
  return {
    status: claim.status,
    expiresAt: claim.expiresAt ?? null,
    checkedAt,
    seenLapsedAt: licenseLive(claim, now) ? null : (previous?.seenLapsedAt ?? checkedAt),
    ...(claim.grant && claim.grant !== 'purchase' ? { grant: claim.grant } : {}),
  };
};

/**
 * How long a copy stays writable after this machine first sees the lapse. A
 * refusal is announced before it bites.
 */
export const WRITING_GRACE_DAYS = 7;

/**
 * How long a recorded answer is worth acting on. Past this, **being unable to
 * ask is not a lapse**: read-only is a fresh refusal and never a remembered one.
 */
export const STANDING_GOOD_FOR_DAYS = 30;

const DAY_MS = 86_400_000;
const daysAfter = (iso: string, days: number): number => new Date(iso).getTime() + days * DAY_MS;
const sayDay = (at: number): string => new Date(at).toISOString().slice(0, 10);

export interface WritingStanding {
  /** Whether the project may be changed at all. */
  writable: boolean;
  /** What to say about it, or null where there is nothing to say. */
  notice: string | null;
  /** A warning of something coming (dismissable) rather than the explanation of a refusal. */
  warning: boolean;
  /** Whether what stands in the way is activating this computer rather than paying. */
  needsActivation: boolean;
}

/** What an installation that has never been activated says. */
export const NOT_ACTIVATED_NOTICE =
  'This computer is not activated yet. Sign in and activate VC Composer 2 to edit. Until then your projects open, play and export, but cannot be changed.';

/** Why it ended, in the customer's terms. */
const lapseWords = (standing: DeskStanding): string => {
  if (standing.status === 'expired' || standing.status === 'active') {
    const on = standing.expiresAt ? ` on ${standing.expiresAt.slice(0, 10)}` : '';
    if (standing.grant === 'academic') return `Your VC Composer academic licence ended${on}.`;
    return `Your VC Composer subscription ended${on}.`;
  }
  return 'This copy’s licence is no longer active.';
};

/**
 * Whether this copy may be edited, and what to say. Generous wherever it is
 * uncertain, and the order of the clauses is the design.
 *
 * `activated`: an explicit `false` means this install has never been activated;
 * null is a host that does not licence editing at all (a development run).
 *
 * Preview builds are not governed by this: a preview stops saving after its
 * own end date, which the desktop build carries.
 */
export const writingStanding = (
  standing: DeskStanding | null,
  now: Date,
  activated: boolean | null = null,
): WritingStanding => {
  if (activated === false) {
    return { writable: false, notice: NOT_ACTIVATED_NOTICE, warning: false, needsActivation: true };
  }

  // Activated, but nothing heard yet. Being unable to ask is never a lapse.
  if (!standing) return { writable: true, notice: null, warning: false, needsActivation: false };

  // Paid up. Nothing is said: a renewal reminder is the shop's to send.
  if (licenseLive(standing, now)) return { writable: true, notice: null, warning: false, needsActivation: false };

  const ended = lapseWords(standing);

  // An answer too old to act on: say the licence ended, claim nothing about read-only.
  if (now.getTime() > daysAfter(standing.checkedAt, STANDING_GOOD_FOR_DAYS)) {
    return {
      writable: true,
      notice:
        standing.grant === 'academic'
          ? `${ended} Ask whoever gave it to you to extend it, to install on another computer.`
          : `${ended} Renew it to install on another computer.`,
      warning: true,
      needsActivation: false,
    };
  }

  const bitesAt = daysAfter(standing.seenLapsedAt ?? standing.checkedAt, WRITING_GRACE_DAYS);
  if (now.getTime() < bitesAt) {
    return {
      writable: true,
      notice: `${ended} This copy stays editable until ${sayDay(bitesAt)}. After that your projects still open, play and export, but cannot be changed.`,
      warning: true,
      needsActivation: false,
    };
  }

  return {
    writable: false,
    notice: `${ended} This copy is read-only. ${DESKTOP_LAPSE_PROMISE}`,
    warning: false,
    needsActivation: false,
  };
};

/** What the account page says about a subscription, in words. */
export const describeSubscription = (license: Pick<License, 'status' | 'expiresAt'>, now: Date): string => {
  const until = license.expiresAt ? new Date(license.expiresAt).toISOString().slice(0, 10) : null;
  if (!until) return 'Active. This licence does not expire.';
  if (licenseLive(license, now)) return `Active. Renews ${until}.`;
  return `Ended ${until}.`;
};
