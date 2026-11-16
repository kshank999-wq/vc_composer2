import Image from 'next/image';
import Link from 'next/link';
import logo from '../../public/brand/logo-horizontal.png';
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

/** The site's header and footer. */
export async function SiteChrome({ children }: { children: React.ReactNode }) {
  const admin = await isAdminVisitor();

  return (
    <>
      <header className="site-header">
        <div className="shell">
          <nav aria-label="Main">
            <Link href="/" className="wordmark" aria-label={`${SITE_NAME} home`}>
              <Image src={logo} alt="VC Composer" priority sizes="160px" />
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
          <Image src={logo} alt="VC Composer" className="footer-logo" sizes="130px" />
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
