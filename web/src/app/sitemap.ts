import type { MetadataRoute } from 'next';
import { env } from '@/lib/env';

/** The public pages, for search engines. */
export default function sitemap(): MetadataRoute.Sitemap {
  const at = (path: string) => new URL(path, env.siteUrl).toString();
  return [
    { url: at('/'), changeFrequency: 'weekly', priority: 1 },
    { url: at('/download'), changeFrequency: 'weekly', priority: 0.9 },
    { url: at('/collaboration'), changeFrequency: 'monthly', priority: 0.5 },
    { url: at('/privacy'), changeFrequency: 'yearly', priority: 0.3 },
    { url: at('/terms'), changeFrequency: 'yearly', priority: 0.3 },
  ];
}
