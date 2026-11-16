import Link from 'next/link';
import { currentAdmin } from '@/lib/admin';
import { COMPANY_LINKS, COMPANY_NAME, COMPANY_URL, SITE_NAME } from '@/lib/site';

/**
 * Whether the visitor is an administrator, for the one nav link only they
 * should see. The pages behind it check for themselves, so this decides
 * nothing about access. Never allowed to fail the layout: a Supabase hiccup
 * (or a deployment with no Supabase yet) costs an admin a link, not the site.
 */
const isAdminVisitor = async (): Promise<boolean> => {
  try {
    return (await currentAdmin()) !== null;
  } catch {
    return false;
  }
};

/** The mark beside the wordmark: a staff and a note, drawn inline. */
function Mark() {
  return (
    <svg className="wordmark-mark" viewBox="0 0 64 64" aria-hidden="true">
      <rect width="64" height="64" rx="12" fill="#262a31" />
      <g stroke="#4f9de8" strokeWidth="2.5" strokeLinecap="round">
        <path d="M12 20h40M12 28h40M12 36h40M12 44h40" />
      </g>
      <ellipse cx="28" cy="40" rx="7" ry="5" fill="#e8a33d" transform="rotate(-20 28 40)" />
      <path d="M34 38V14" stroke="#e8a33d" strokeWidth="3" strokeLinecap="round" />
    </svg>
  );
}

/** The site's header and footer. */
export async function SiteChrome({ children }: { children: React.ReactNode }) {
  const admin = await isAdminVisitor();

  return (
    <>
      <header className="site-header">
        <div className="shell">
          <nav aria-label="Main">
            <Link href="/" className="wordmark" aria-label={`${SITE_NAME} home`}>
              <Mark />
              <span>
                VC COMPOSER
              </span>
            </Link>
            <Link href="/#features">Features</Link>
            <Link href="/download">Subscribe &amp; download</Link>
            <Link href="/download#preview">Free preview</Link>
            <Link href="/collaboration">Collaboration</Link>
            <Link href="/account">My account</Link>
            {admin ? <Link href="/admin">Admin</Link> : null}
          </nav>
        </div>
      </header>
      <main className="shell">{children}</main>
      <footer className="site-footer">
        <div className="shell">
          <p>
            © {new Date().getFullYear()} {COMPANY_NAME}. {SITE_NAME} for Windows and macOS.
          </p>
          <p>
            <Link href="/privacy">Privacy</Link> · <Link href="/terms">Terms</Link> ·{' '}
            <a href={COMPANY_LINKS.support}>Support</a> · <a href={COMPANY_LINKS.privacy}>{COMPANY_NAME} privacy</a> ·{' '}
            <a href={COMPANY_LINKS.terms}>{COMPANY_NAME} terms</a>
          </p>
          <p>
            A <a href={COMPANY_URL}>{COMPANY_NAME}</a> product.
          </p>
        </div>
      </footer>
    </>
  );
}
