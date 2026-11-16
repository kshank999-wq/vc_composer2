import type { Metadata } from 'next';
import Link from 'next/link';
import {
  formatMoney,
  licenseGrantSchema,
  licenseKindName,
  licenseLive,
  licenseSeatsPhrase,
  PLATFORM_LABEL,
  platformSchema,
  type LicenseGrant,
  type LicenseStatus,
  type Platform,
} from '@/domain';
import { adminClient, currentUserOrNull } from '@/lib/supabase';
import { DownloadButton } from './download-button';
import { ResendLicense } from './resend-license';
import { ManageSubscription } from './manage-subscription';
import { Devices } from './devices';
import { SetPassword } from './set-password';
import { DeleteAccount } from './delete-account';

export const metadata: Metadata = { title: 'My account' };
export const dynamic = 'force-dynamic';

const formatDate = (value: string | null): string =>
  value ? new Date(value).toLocaleDateString(undefined, { year: 'numeric', month: 'long', day: 'numeric' }) : '—';

/** A row's grant, read as bought where it says something this build cannot name. */
const grantOf = (value: unknown): LicenseGrant => licenseGrantSchema.catch('purchase').parse(value);

interface BuildRow {
  platform: Platform;
  channel: 'stable' | 'preview';
  version: string;
  minimumOsVersion: string;
}

function BuildCard({ build }: { build: BuildRow }) {
  return (
    <article className="card">
      <h3>
        {PLATFORM_LABEL[build.platform]}
        <span className={`channel-tag ${build.channel}`}>{build.channel === 'preview' ? 'Preview' : 'Stable'}</span>
      </h3>
      <p className="lede">
        Version {build.version}
        {build.minimumOsVersion ? ` · requires ${build.minimumOsVersion} or later` : ''}
      </p>
      <DownloadButton
        platform={build.platform}
        channel={build.channel}
        label={build.channel === 'preview' ? 'Download the preview' : 'Download'}
      />
    </article>
  );
}

/**
 * My account: licences, downloads, devices, purchases, password.
 *
 * The customer can come back at any time and download the current build for
 * either platform, see what they bought, free a seat, and have their licence
 * sent again without contacting support. **Every signed-in account sees the
 * free preview downloads**; the stable downloads are listed for licence
 * holders, and the download route checks the licence again either way.
 */
export default async function AccountPage() {
  const user = await currentUserOrNull();
  if (!user) {
    return (
      <>
        <div className="hero">
          <h1>My account</h1>
          <p>Sign in to see your licence and downloads, or to download the free preview.</p>
        </div>
        <Link href="/signin" className="button">
          Sign in
        </Link>
      </>
    );
  }

  const client = adminClient();
  const [{ data: licenses }, { data: builds }, { data: orders }, { data: profile }] = await Promise.all([
    client
      .from('licenses')
      .select('serial, status, entitled_platforms, max_activations, created_at, expires_at, stripe_subscription_id, grant_kind, granted_to')
      .eq('user_id', user.id)
      .order('created_at', { ascending: false }),
    client
      .from('release_builds')
      .select('platform, channel, version, minimum_os_version')
      .in('channel', ['stable', 'preview'])
      .eq('active', true),
    client
      .from('orders')
      .select('id, status, amount_cents, currency, selected_platform, paid_at, created_at')
      .eq('user_id', user.id)
      .order('created_at', { ascending: false })
      .limit(20),
    client.from('profiles').select('is_admin, password_set_at').eq('id', user.id).maybeSingle(),
  ]);

  const now = new Date();
  const liveLicenses = (licenses ?? []).filter((license) =>
    licenseLive({ status: license.status as LicenseStatus, expiresAt: (license.expires_at as string | null) ?? null }, now),
  );
  const endedLicenses = (licenses ?? []).filter((license) => !liveLicenses.includes(license));
  const hasSubscription = (licenses ?? []).some((license) => license.stripe_subscription_id);

  const catalogue: BuildRow[] = (builds ?? []).flatMap((row) => {
    const platform = platformSchema.safeParse(row.platform);
    if (!platform.success || (row.channel !== 'stable' && row.channel !== 'preview')) return [];
    return [
      {
        platform: platform.data,
        channel: row.channel,
        version: row.version as string,
        minimumOsVersion: (row.minimum_os_version as string) ?? '',
      },
    ];
  });
  const stable = catalogue.filter((build) => build.channel === 'stable');
  const previews = catalogue.filter((build) => build.channel === 'preview');

  return (
    <>
      <div className="hero">
        <h1>My account</h1>
        <p>{user.email}</p>
      </div>

      <section>
        <h2>Licence</h2>
        {liveLicenses.length === 0 ? (
          <p className="lede">
            No active licence{endedLicenses.length > 0 ? ' (your previous one has ended)' : ''}.{' '}
            <Link href="/download">Subscribe to VC Composer 2</Link> to get one, or download the free preview below.
          </p>
        ) : (
          <div className="grid">
            {liveLicenses.map((license) => {
              const grant = grantOf(license.grant_kind);
              return (
                <article key={license.serial} className="card">
                  <h3>{licenseKindName(grant)}</h3>
                  <p className="serial">{license.serial}</p>
                  <p style={{ marginTop: 8 }}>
                    Covers {(license.entitled_platforms as Platform[]).map((one) => PLATFORM_LABEL[one]).join(' and ')} ·{' '}
                    {grant === 'master'
                      ? 'on any number of computers'
                      : `up to ${licenseSeatsPhrase({ grant, maxActivations: license.max_activations as number })}`}
                  </p>
                  {license.granted_to && grant === 'academic' ? (
                    <p className="muted small">For {license.granted_to}</p>
                  ) : null}
                  {grant === 'academic' && license.expires_at ? (
                    <p className="muted small">Until {formatDate(license.expires_at as string)}</p>
                  ) : null}
                  {license.stripe_subscription_id && license.expires_at ? (
                    <p className="muted small">Renews {formatDate(license.expires_at as string)}</p>
                  ) : null}
                </article>
              );
            })}
          </div>
        )}
        {liveLicenses.length > 0 || hasSubscription ? (
          <div className="actions" style={{ marginTop: 16, alignItems: 'flex-start' }}>
            {liveLicenses.length > 0 ? <ResendLicense /> : null}
            {hasSubscription ? <ManageSubscription /> : null}
          </div>
        ) : null}
      </section>

      <section>
        <h2>Downloads</h2>
        {liveLicenses.length === 0 ? null : stable.length === 0 ? (
          <p className="lede">No stable build has been published yet. Your licence is ready for when one is.</p>
        ) : (
          <div className="grid" style={{ marginBottom: 16 }}>
            {stable.map((build) => (
              <BuildCard key={`stable-${build.platform}`} build={build} />
            ))}
          </div>
        )}

        <h3 style={{ marginTop: 24 }}>Free preview</h3>
        <p className="muted small">
          Open to every account. Previews are time-limited: after a preview&rsquo;s end date it stops saving but keeps
          opening and exporting your projects.
        </p>
        {previews.length === 0 ? (
          <p className="lede">There is no preview build available right now.</p>
        ) : (
          <div className="grid">
            {previews.map((build) => (
              <BuildCard key={`preview-${build.platform}`} build={build} />
            ))}
          </div>
        )}
      </section>

      {liveLicenses.length > 0 ? (
        <section>
          <h2>Computers</h2>
          <Devices />
        </section>
      ) : null}

      {(orders ?? []).length > 0 ? (
        <section>
          <h2>Purchases</h2>
          <ul className="order-list">
            {(orders ?? []).map((order) => (
              <li key={order.id as string} className="card">
                <strong>{formatMoney(order.amount_cents as number, order.currency as string)}</strong>
                <p className="lede" style={{ margin: 0 }}>
                  {formatDate((order.paid_at as string | null) ?? (order.created_at as string))}
                  {order.selected_platform ? ` · chose ${order.selected_platform}` : ''}
                  {order.status !== 'paid' ? ` · ${order.status}` : ''}
                </p>
              </li>
            ))}
          </ul>
          <p className="muted small">Renewals and receipts are in Manage subscription.</p>
        </section>
      ) : null}

      <SetPassword setAt={(profile?.password_set_at as string | null | undefined) ?? null} />

      {profile?.is_admin ? (
        <section>
          <h2>Administration</h2>
          <p className="lede">
            <Link href="/admin">Dashboard</Link> · <Link href="/admin/releases">Release builds</Link> ·{' '}
            <Link href="/admin/support">Support console</Link>
          </p>
        </section>
      ) : null}

      {/* Last on the page on purpose: the one act here that cannot be undone. */}
      <DeleteAccount email={user.email ?? ''} />
    </>
  );
}
