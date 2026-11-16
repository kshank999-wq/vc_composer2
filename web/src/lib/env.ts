/**
 * Environment access.
 *
 * Every value is read lazily and fails loudly at the point of use rather than
 * at import time: a missing Stripe key must break checkout, not the marketing
 * pages or a CI build that has no secrets configured.
 */

const required = (name: string): string => {
  const value = process.env[name];
  if (!value || value.length === 0) {
    throw new Error(`Missing required environment variable ${name}`);
  }
  return value;
};

const optional = (name: string, fallback: string): string => {
  const value = process.env[name];
  return value && value.length > 0 ? value : fallback;
};

export const DEFAULT_SITE_URL = 'https://vc-composer.com';

/**
 * The site's own origin, as an absolute URL with no trailing slash.
 *
 * Deliberately forgiving, unlike everything else here. The root layout reads it
 * at module scope (`metadataBase`), so a malformed value would throw during
 * `next build` and take the whole deployment down for a field whose only job
 * is to resolve relative Open Graph URLs.
 *
 * Anything that is not an absolute http(s) URL is refused rather than repaired:
 * prefixing a scheme would turn a pasted variable *name* into a URL that
 * parses, and a site that quietly links to the wrong host is worse than one
 * that warns and uses the default.
 */
const siteUrl = (): string => {
  const raw = process.env['NEXT_PUBLIC_SITE_URL']?.trim();
  if (!raw || raw.length === 0) return DEFAULT_SITE_URL;

  let parsed: URL;
  try {
    parsed = new URL(raw);
  } catch {
    console.warn(
      `NEXT_PUBLIC_SITE_URL is not a URL (${JSON.stringify(raw)}). ` +
        `Include the scheme, e.g. ${DEFAULT_SITE_URL}. Falling back to ${DEFAULT_SITE_URL}.`,
    );
    return DEFAULT_SITE_URL;
  }

  if (parsed.protocol !== 'https:' && parsed.protocol !== 'http:') {
    console.warn(
      `NEXT_PUBLIC_SITE_URL must be http or https, not ${parsed.protocol} ` +
        `(${JSON.stringify(raw)}). Falling back to ${DEFAULT_SITE_URL}.`,
    );
    return DEFAULT_SITE_URL;
  }

  // Callers append paths as `${env.siteUrl}/account`, so a trailing slash would
  // double up in Stripe redirects and every emailed link.
  return parsed.href.replace(/\/+$/, '');
};

export const env = {
  get supabaseUrl(): string {
    return required('NEXT_PUBLIC_SUPABASE_URL');
  },
  get supabaseAnonKey(): string {
    return required('NEXT_PUBLIC_SUPABASE_ANON_KEY');
  },
  /** Server only. Bypasses row level security: never import into a client component. */
  get supabaseServiceRoleKey(): string {
    return required('SUPABASE_SERVICE_ROLE_KEY');
  },
  get stripeSecretKey(): string {
    return required('STRIPE_SECRET_KEY');
  },
  get stripeWebhookSecret(): string {
    return required('STRIPE_WEBHOOK_SECRET');
  },
  /** The two recurring desktop plans. Prices are read from Stripe, never typed here. */
  get stripeMonthlyPriceId(): string {
    return required('STRIPE_PRICE_ID_MONTHLY');
  },
  get stripeYearlyPriceId(): string {
    return required('STRIPE_PRICE_ID_YEARLY');
  },
  /**
   * The collaboration plan's per-editor-seat prices (a separate Stripe
   * product). Empty until it is on sale: the collaboration checkout then
   * says so instead of failing.
   */
  get stripeCollabSeatMonthlyPriceId(): string {
    return optional('STRIPE_PRICE_ID_COLLAB_SEAT_MONTHLY', '');
  },
  get stripeCollabSeatYearlyPriceId(): string {
    return optional('STRIPE_PRICE_ID_COLLAB_SEAT_YEARLY', '');
  },
  get resendApiKey(): string {
    return required('RESEND_API_KEY');
  },
  get resendFrom(): string {
    return optional('RESEND_FROM_ADDRESS', 'VC Composer <noreply@vc-composer.com>');
  },
  /**
   * Whether Checkout asks Stripe Tax to work out sales tax and VAT. On unless
   * STRIPE_AUTOMATIC_TAX is "off" (or "false"/"0"): a Stripe sandbox cannot
   * hold the head-office address Stripe Tax needs, so testing turns it off.
   */
  get stripeAutomaticTax(): boolean {
    const v = optional('STRIPE_AUTOMATIC_TAX', 'on').trim().toLowerCase();
    return !['off', 'false', '0', 'no'].includes(v);
  },
  get releaseBucket(): string {
    return optional('RELEASE_BUCKET', 'releases');
  },
  get releaseDownloadTtlSeconds(): number {
    const parsed = Number.parseInt(optional('RELEASE_DOWNLOAD_TTL_SECONDS', '900'), 10);
    return Number.isFinite(parsed) && parsed > 0 ? parsed : 900;
  },
  get siteUrl(): string {
    return siteUrl();
  },
} as const;

/** Whether Supabase is configured at all, for pages that render without it. */
export const supabaseConfigured = (): boolean =>
  Boolean(process.env['NEXT_PUBLIC_SUPABASE_URL'] && process.env['NEXT_PUBLIC_SUPABASE_ANON_KEY']);
