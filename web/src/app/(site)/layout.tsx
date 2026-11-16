import { SiteChrome } from '../site-chrome';

/**
 * Every page of the site wears the header and the footer. A route group
 * changes nothing about any address: `/download` is still `/download`.
 */
export default function SiteLayout({ children }: { children: React.ReactNode }) {
  return <SiteChrome>{children}</SiteChrome>;
}
