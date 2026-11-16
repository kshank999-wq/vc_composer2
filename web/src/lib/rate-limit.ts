import { createHmac } from 'node:crypto';
import { NextResponse } from 'next/server';
import { adminClient } from './supabase';

/**
 * Per-address rate limiting for the routes anyone can call.
 *
 * What it is for: noise and cost. A script creating checkout sessions, an
 * activation loop, somebody guessing discount codes. What it is not for:
 * access control — entitlement is checked in every route regardless, and
 * nothing here grants or denies anything but a turn.
 *
 * That is why it fails open. If the database call errors, the request goes
 * through and a warning is logged. A limiter that could take purchases down
 * when it broke would be a worse risk than the one it guards against.
 *
 * Addresses are stored as a keyed hash, never raw, and rows live for two
 * windows at most. That is pseudonymisation, not anonymisation: with the salt
 * a row can be matched to an address again. It is enough for a table whose
 * contents are gone within the hour.
 */

export interface RateLimitRule {
  /** Names the route in the key so rules never share an allowance. */
  name: string;
  limit: number;
  windowSeconds: number;
}

export interface RateLimitVerdict {
  allowed: boolean;
  retryAfterSeconds: number;
}

export type Consume = (key: string, rule: RateLimitRule) => Promise<RateLimitVerdict>;

export const RULES = {
  /** Checkout sessions cost nothing to create, and nobody subscribes ten times in ten minutes. */
  checkout: { name: 'checkout', limit: 10, windowSeconds: 600 },
  /** Activation is account-scoped already; this stops a loop hammering auth. */
  activate: { name: 'activate', limit: 30, windowSeconds: 3600 },
  /**
   * Checking what a discount code is worth. A code is printed in
   * advertisements and so is public by design, but one nobody published is
   * worth finding by guessing, and thirty an hour is far more than anybody
   * types off a page and far too few to search by.
   */
  discount: { name: 'discount', limit: 30, windowSeconds: 3600 },
  /**
   * Emailed sign-in and reset links, by address. A person asks for a link a
   * few times at most, so ten an hour is room for mistakes and nothing more.
   */
  emailLink: { name: 'email-link', limit: 10, windowSeconds: 3600 },
} as const satisfies Record<string, RateLimitRule>;

/** The first hop in X-Forwarded-For is the client; Vercel sets it. */
export const clientAddress = (request: Request): string => {
  const forwarded = request.headers.get('x-forwarded-for');
  const first = forwarded?.split(',')[0]?.trim();
  return first || request.headers.get('x-real-ip')?.trim() || 'unknown';
};

/**
 * `subject` names who to count against when the address is the wrong unit:
 * a signed-in account, for a route where the cost belongs to the account and
 * not to whichever café it was called from. It is hashed like an address —
 * the table holds no user ids either.
 */
export const rateLimitKey = (rule: RateLimitRule, request: Request, subject?: string): string => {
  const salt = process.env['RATE_LIMIT_SALT'] || 'vc-composer-rate-limit';
  const who = subject ?? clientAddress(request);
  const digest = createHmac('sha256', salt).update(who).digest('hex').slice(0, 32);
  return `${rule.name}:${digest}`;
};

const consumeInDatabase: Consume = async (key, rule) => {
  const { data, error } = await adminClient().rpc('consume_rate_limit', {
    p_key: key,
    p_limit: rule.limit,
    p_window_seconds: rule.windowSeconds,
  });

  const row = (Array.isArray(data) ? data[0] : data) as
    | { allowed: boolean; retry_after_seconds: number }
    | null
    | undefined;

  if (error || !row) {
    console.warn(`rate limit unavailable for ${rule.name}; allowing the request`, error?.message ?? '');
    return { allowed: true, retryAfterSeconds: 0 };
  }

  return { allowed: row.allowed === true, retryAfterSeconds: Number(row.retry_after_seconds) || 0 };
};

/**
 * Returns a 429 response to send, or null when the request may proceed.
 *
 *   const limited = await rateLimit(request, RULES.checkout);
 *   if (limited) return limited;
 */
export const rateLimit = async (
  request: Request,
  rule: RateLimitRule,
  consume: Consume = consumeInDatabase,
  subject?: string,
): Promise<Response | null> => {
  let verdict: RateLimitVerdict;
  try {
    verdict = await consume(rateLimitKey(rule, request, subject), rule);
  } catch (cause) {
    console.warn(`rate limit threw for ${rule.name}; allowing the request`, cause);
    return null;
  }

  if (verdict.allowed) return null;

  const retryAfter = Math.max(1, Math.ceil(verdict.retryAfterSeconds));
  const from = subject ? 'on this account' : 'from this address';
  return NextResponse.json(
    { error: `Too many requests ${from}. Try again shortly.` },
    { status: 429, headers: { 'retry-after': String(retryAfter) } },
  );
};
