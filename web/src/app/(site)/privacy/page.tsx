import type { Metadata } from 'next';
import Link from 'next/link';
import { COMPANY_LINKS, COMPANY_NAME, SUPPORT_EMAIL } from '@/lib/site';

export const metadata: Metadata = {
  title: 'Privacy',
  description: 'What the VC Composer website stores, who processes it, and how to delete your account.',
};

/**
 * The privacy policy for this site and the licensing the desktop app does
 * through it.
 *
 * **Every claim here is checkable against the code**: the list of processors
 * is the list of services in `lib/env.ts`, what is stored is the schema in
 * `supabase/migrations/`, and what the desktop app sends is the request body of
 * `/api/licenses/activate`. A policy that says more than the code does has to
 * be rewritten the first time anybody checks.
 *
 * The date is the last substantive revision and is typed here, so redeploying
 * does not silently claim the policy changed today.
 */
const UPDATED = '16 November 2026';

export default function PrivacyPage() {
  return (
    <>
      <div className="hero hero-compact">
        <h1>Privacy</h1>
        <p>What this site stores, who else touches it, and how to take it away. Last updated {UPDATED}.</p>
      </div>

      <div className="prose">
        <h2>What this covers</h2>
        <p>
          This website, where you subscribe to VC Composer, download it and manage your account, and the requests the
          VC Composer desktop application makes to it to activate a licence, check that licence, and look for updates.{' '}
          {COMPANY_NAME}&rsquo;s own <a href={COMPANY_LINKS.privacy}>privacy policy</a> covers the company as a whole.
        </p>

        <h2>Your music is yours</h2>
        <p>
          Your projects, scores, recordings and mixes stay on your own computer. This website does not receive them,
          store them, or have anywhere to put them. We do not read, sell or share your work, or use it to train any
          model.
        </p>

        <h2>What we store</h2>
        <ul>
          <li>
            <strong>Your account.</strong> Your email address, and a password if you set one. Passwords are stored
            hashed by our authentication provider; we never see one. We also record when a password was last set, so
            your account page can say whether you have one.
          </li>
          <li>
            <strong>Your subscription.</strong> What you bought, when, the amount and currency, which platform you chose
            first, and your licence key, with the Stripe identifiers that link them. <strong>We never see your card
            number.</strong> Payment details go straight to Stripe.
          </li>
          <li>
            <strong>Which computers you have activated,</strong> so a licence covers the number of computers it covers.
            The app sends an identifier derived on that computer, the name you give it, its platform and the app
            version. We do not collect hardware serial numbers.
          </li>
          <li>
            <strong>That an email was sent to you,</strong> and which template it was, so support can answer &ldquo;did
            I get the licence email&rdquo;. The message itself is not stored.
          </li>
          <li>
            <strong>A short-lived, hashed record of request counts</strong> per address, to stop abuse of sign-in,
            checkout and activation. The address itself is not stored, and old records are swept away within hours.
          </li>
        </ul>

        <h2>What we do not collect</h2>
        <p>
          No advertising identifiers. No analytics or tracking scripts: this site loads none, on any page. No
          third-party trackers or pixels. Nothing is sold or shared for advertising.
        </p>

        <h2>Who processes it</h2>
        <ul>
          <li>
            <strong>Supabase</strong>: the database, sign-in, and storage for the installers.
          </li>
          <li>
            <strong>Vercel</strong>: hosting for this website and its API.
          </li>
          <li>
            <strong>Stripe</strong>: payments, tax and the subscription billing portal.
          </li>
          <li>
            <strong>Resend</strong>: sending transactional email (your licence, a sign-in link).
          </li>
          <li>
            <strong>Cloudflare Turnstile</strong>: the check on the sign-in page that you are a person.
          </li>
        </ul>
        <p>Each acts on our instructions. There is no other party your data goes to.</p>

        <h2>Keeping it, and taking it away</h2>
        <p>
          We keep your account for as long as you have one. Order records are kept as long as tax and accounting law
          requires, which is the one thing that outlives a deletion.
        </p>
        <p>
          <strong>You can delete your account</strong> from <Link href="/account">My account</Link>. It removes your
          sign-in, your licence and the computers activated on it, immediately. The order record stays as a financial
          record with your name taken off it. The projects on your own computer are not touched.
        </p>
        <p>
          You may also ask for a copy of what we hold, or for a correction, by writing to the address below. Wherever
          you live, you are welcome to make the requests the GDPR and the CCPA describe.
        </p>

        <h2>Changes</h2>
        <p>If this policy changes in a way that matters, the date at the top changes.</p>

        <h2>Asking us something</h2>
        <p>
          Write to <a href={`mailto:${SUPPORT_EMAIL}`}>{SUPPORT_EMAIL}</a>, or use <a href={COMPANY_LINKS.support}>{COMPANY_NAME} support</a>.
        </p>
      </div>
    </>
  );
}
