import type { Metadata } from 'next';
import { env } from '@/lib/env';
import { SITE_NAME } from '@/lib/site';
import './globals.css';

export const metadata: Metadata = {
  metadataBase: new URL(env.siteUrl),
  title: {
    default: `${SITE_NAME}: the notation-first DAW`,
    template: `%s · ${SITE_NAME}`,
  },
  description:
    'VC Composer is the notation-first DAW for film, TV and game composers: write on staves, hear your libraries, mix, score to picture and deliver. A new home for Finale users. Windows and macOS.',
  openGraph: {
    title: SITE_NAME,
    description: 'The notation-first DAW for film, TV and game composers. Windows and macOS.',
    url: env.siteUrl,
    siteName: SITE_NAME,
    type: 'website',
  },
};

/**
 * Every page renders per request.
 *
 * The content security policy carries a fresh nonce for each response (see
 * `middleware.ts`), and a statically prerendered page is HTML built before that
 * nonce existed: the browser would refuse every script on it, because
 * `strict-dynamic` makes `'self'` inert. Rendering per request keeps the nonce
 * in the HTML and the one in the header the same.
 */
export const dynamic = 'force-dynamic';

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
