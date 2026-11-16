import type { Metadata } from 'next';
import Link from 'next/link';
import { DISCOUNT_PARAM, PLATFORM_LABEL, platformSchema, type Platform } from '@/domain';
import { fetchPlanPrices } from '@/lib/pricing';
import { adminClient, currentUserOrNull } from '@/lib/supabase';
import { DownloadButton } from '../account/download-button';
import { PlatformChoice } from './platform-choice';

export const metadata: Metadata = {
  title: 'Subscribe & download',
  description: 'Subscribe to VC Composer and download it for Windows or macOS, or try the free preview.',
};

// The price comes from Stripe on each render rather than being baked into the
// build, so changing it there changes it here.
export const dynamic = 'force-dynamic';

interface PreviewBuild {
  platform: Platform;
  version: string;
  minimumOsVersion: string;
}

/**
 * The live preview builds, for the free preview section. Empty where there are
 * none or Supabase is not configured: the section then says so rather than
 * failing the buying page with it.
 */
const livePreviews = async (): Promise<PreviewBuild[]> => {
  try {
    const { data } = await adminClient()
      .from('release_builds')
      .select('platform, version, minimum_os_version')
      .eq('channel', 'preview')
      .eq('active', true);
    return (data ?? []).flatMap((row) => {
      const platform = platformSchema.safeParse(row.platform);
      return platform.success
        ? [{ platform: platform.data, version: row.version as string, minimumOsVersion: (row.minimum_os_version as string) ?? '' }]
        : [];
    });
  } catch {
    return [];
  }
};

/**
 * Buying, and the free preview.
 *
 * The buyer picks a plan and the platform they are installing on first; the
 * choice is recorded with the order and the licence covers both platforms.
 *
 * The preview section is for anybody: sign in (an emailed link is enough, no
 * purchase) and download the current preview build for either platform.
 */
export default async function DownloadPage({
  searchParams,
}: {
  searchParams?: Record<string, string | string[] | undefined>;
}) {
  const [plans, user, previews] = await Promise.all([fetchPlanPrices(), currentUserOrNull(), livePreviews()]);
  // A code carried in from an advertisement. The control asks the shop what it
  // is worth; nothing is decided here.
  const asked = searchParams?.[DISCOUNT_PARAM];
  const code = (Array.isArray(asked) ? asked[0] : asked)?.trim() || null;
  const cancelled = searchParams?.['cancelled'] === '1';

  return (
    <>
      <div className="hero">
        <h1>Subscribe to VC Composer</h1>
        <p>
          Monthly or yearly, on Windows and macOS both. Choose a plan and the computer you are installing on first;
          your licence covers the other, so you can switch any time.
        </p>
      </div>

      {cancelled ? (
        <p className="notice" role="status">
          Checkout was cancelled and nothing was charged. Pick up where you left off below.
        </p>
      ) : null}

      <PlatformChoice advertisedCode={code} plans={plans} />

      <section id="preview" className="preview-panel">
        <h2>Free preview</h2>
        <p>
          Try VC Composer before you subscribe. The preview is free for anyone with an account: sign in with your
          email address, no card needed, and download it for Windows or macOS.
        </p>
        <p className="muted small">
          Previews are time-limited. After a preview&rsquo;s end date it stops saving, but it keeps opening and
          exporting your projects, so nothing you made is lost. A newer preview, or a subscription, carries on from
          there.
        </p>

        {!user ? (
          <div className="actions">
            <Link href={`/signin?next=${encodeURIComponent('/download#preview')}`} className="button warm">
              Sign in to download the preview
            </Link>
          </div>
        ) : previews.length === 0 ? (
          <p className="notice">There is no preview build available right now. Check back soon.</p>
        ) : (
          <div className="grid">
            {previews.map((build) => (
              <article key={build.platform} className="card">
                <h3>
                  {PLATFORM_LABEL[build.platform]}
                  <span className="channel-tag preview">Preview</span>
                </h3>
                <p className="lede">
                  Version {build.version}
                  {build.minimumOsVersion ? ` · requires ${build.minimumOsVersion} or later` : ''}
                </p>
                <DownloadButton platform={build.platform} channel="preview" label="Download the preview" />
              </article>
            ))}
          </div>
        )}
      </section>
    </>
  );
}
