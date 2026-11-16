/**
 * Who this site is for, said once.
 *
 * The product identity lives here so the name, the company and the Stripe
 * product stamp cannot drift between pages, emails and the webhook. The site's
 * own address is not here: `env.siteUrl` (NEXT_PUBLIC_SITE_URL) decides it.
 */
export const SITE_NAME = 'VC Composer';
export const PRODUCT_SHORT_NAME = 'VC Composer';
export const COMPANY_NAME = 'VC Software';
export const COMPANY_URL = 'https://vc-software.com';

/**
 * The address customers write to. Shown on the privacy and terms pages and in
 * refusals that need a person. Change it here if support moves.
 */
export const SUPPORT_EMAIL = 'support@vc-software.com';

/** The company's own support, privacy and terms pages, linked from the footer. */
export const COMPANY_LINKS = {
  support: `${COMPANY_URL}/support`,
  privacy: `${COMPANY_URL}/privacy`,
  terms: `${COMPANY_URL}/terms`,
} as const;
