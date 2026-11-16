import type { Metadata } from 'next';
import Link from 'next/link';
import { DESKTOP_LAPSE_PROMISE } from '@/domain';
import { COMPANY_LINKS, COMPANY_NAME, SUPPORT_EMAIL } from '@/lib/site';

export const metadata: Metadata = {
  title: 'Terms of Use',
  description: 'What a VC Composer subscription gives you, the free preview, and what happens to your work if you stop.',
};

/** Typed rather than rendered from the build, so a redeploy does not claim the terms changed. */
const UPDATED = '16 November 2026';

export default function TermsPage() {
  return (
    <>
      <div className="hero hero-compact">
        <h1>Terms of Use</h1>
        <p>
          What you may do with VC Composer, what we undertake, and what happens to your work if you stop paying. Last
          updated {UPDATED}.
        </p>
      </div>

      <div className="prose">
        <h2>What this covers</h2>
        <p>
          These terms apply to VC Composer, the desktop application for Windows and macOS, and to this website.
          Using either means accepting them. How your information is handled is in{' '}
          <Link href="/privacy">the privacy policy</Link>. {COMPANY_NAME}&rsquo;s own{' '}
          <a href={COMPANY_LINKS.terms}>terms</a> apply alongside these.
        </p>

        <h2>Your music is yours</h2>
        <p>
          <strong>You own everything you compose.</strong> We claim no ownership of your projects, scores, recordings
          or mixes, and no licence to use them.
        </p>

        <h2>The subscription</h2>
        <p>
          A subscription gives you a licence to use VC Composer. It is yours personally, or your organisation&rsquo;s
          where an organisation bought it, and it covers a set number of computers. Your{' '}
          <Link href="/account">account page</Link> lists the computers it is on and lets you free a seat from one you
          no longer have.
        </p>
        <p>
          The plan and the price are shown at checkout before you pay. A subscription renews itself at the end of each
          period until you cancel it, which you can do at any time from your account; it then runs to the end of the
          period you have paid for.
        </p>
        <p>
          <strong>If a subscription ends, your work is not taken away.</strong> {DESKTOP_LAPSE_PROMISE} Subscribing again
          makes it editable straight away.
        </p>
        <p>
          Please do not share your licence key, rent the application out, or work round the activation. If a payment is
          refunded or charged back, the licence it paid for ends.
        </p>

        <h2>The free preview</h2>
        <p>
          Preview builds are free to anyone with an account, are offered as they are, and are time-limited. After a
          preview&rsquo;s end date it stops saving, but it keeps opening and exporting your projects. A preview may
          contain features that change or are removed before a stable release.
        </p>

        <h2>Collaboration</h2>
        <p>
          Collaboration is not part of the desktop subscription. When it is offered it will be a separate subscription
          with its own terms; see <Link href="/collaboration">Collaboration</Link>.
        </p>

        <h2>Plug-ins and libraries</h2>
        <p>
          VC Composer hosts instrument and effect plug-ins and sample libraries that you obtain and license from their
          makers. Their terms are between you and them, and a project package never bundles them.
        </p>

        <h2>What we do not promise</h2>
        <p>
          We work hard to keep VC Composer reliable, and the application saves crash-safely, but software has faults.
          Keep backups of work that matters. To the extent the law allows, our liability is limited to what you paid
          us in the twelve months before the claim.
        </p>

        <h2>Changes</h2>
        <p>If these terms change in a way that matters, the date at the top changes.</p>

        <h2>Asking us something</h2>
        <p>
          Write to <a href={`mailto:${SUPPORT_EMAIL}`}>{SUPPORT_EMAIL}</a>.
        </p>
      </div>
    </>
  );
}
