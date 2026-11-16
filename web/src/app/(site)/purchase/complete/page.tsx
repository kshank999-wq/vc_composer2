import type { Metadata } from 'next';
import Link from 'next/link';
import { parsePlatform } from '@/lib/fulfillment';
import { checkoutKind } from '@/lib/checkout-kind';
import { stripe } from '@/lib/stripe';
import { adminClient } from '@/lib/supabase';
import { DownloadButton } from '@/app/(site)/account/download-button';

export const metadata: Metadata = { title: 'Purchase complete' };
export const dynamic = 'force-dynamic';

/**
 * Immediate post-payment screen.
 *
 * The licence is issued by the webhook, which may land a second or two after
 * the redirect. This page therefore reads what exists and tells the customer
 * plainly when it is still on its way — it never fabricates a licence or
 * grants a download the server has not authorised.
 */
export default async function PurchaseCompletePage({
  searchParams,
}: {
  searchParams: { session_id?: string };
}) {
  const sessionId = searchParams.session_id;
  if (!sessionId) {
    return (
      <div className="hero">
        <h1>Purchase complete</h1>
        <p>
          Your licence is in your account. <Link href="/account">Go to downloads</Link>.
        </p>
      </div>
    );
  }

  // The session id comes from the URL, which anyone can type. A wrong or
  // expired one is not an error page: it is a customer who has already paid
  // and wants their downloads, which are in their account regardless. Another
  // product's session on the shared Stripe account is treated the same way.
  const found = await (async () => {
    try {
      return await stripe().checkout.sessions.retrieve(sessionId);
    } catch {
      return null;
    }
  })();
  const session = found && checkoutKind(found) === 'desktop' ? found : null;
  if (!session) {
    return (
      <div className="hero">
        <h1>Purchase complete</h1>
        <p>
          We could not find that checkout, but if your payment went through your licence is in your account.{' '}
          <Link href="/account">Go to downloads</Link>.
        </p>
      </div>
    );
  }
  const platform = parsePlatform(session.metadata?.['platform']);

  const { data: order } = await adminClient()
    .from('orders')
    .select('id')
    .eq('stripe_checkout_session_id', sessionId)
    .maybeSingle();

  const { data: licence } = order
    ? await adminClient().from('licenses').select('serial').eq('order_id', order.id).maybeSingle()
    : { data: null };

  return (
    <>
      <div className="hero">
        <h1>Thank you for subscribing to VC Composer</h1>
        <p>We have emailed your licence and a link to your downloads to {session.customer_details?.email ?? 'your address'}.</p>
      </div>

      <section>
        {licence ? (
          <>
            <h2>Your licence</h2>
            <p className="serial">{licence.serial}</p>
          </>
        ) : (
          <p className="notice">
            Your payment went through. The licence is being issued now — refresh this page in a moment, or open{' '}
            <Link href="/account">My account</Link>, where it will appear.
          </p>
        )}
      </section>

      {platform ? (
        <section>
          <h2>Download for {platform === 'windows' ? 'Windows 10 / 11' : 'macOS'}</h2>
          <DownloadButton platform={platform} />
        </section>
      ) : null}

      <section>
        <p className="lede">
          Sign in at any time to download the current Windows or macOS build, see your computers, or manage your
          subscription.
        </p>
        <Link href="/account" className="button secondary">
          Go to my account
        </Link>
      </section>
    </>
  );
}
