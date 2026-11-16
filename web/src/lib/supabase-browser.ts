'use client';

import { createBrowserClient } from '@supabase/ssr';

/**
 * Browser client. Anon key only: it is bound by row level security, so it can
 * only ever read the signed-in visitor's own rows.
 */
export const browserClient = () =>
  createBrowserClient(process.env.NEXT_PUBLIC_SUPABASE_URL ?? '', process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY ?? '');
