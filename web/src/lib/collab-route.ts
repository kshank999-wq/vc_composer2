import { NextResponse } from 'next/server';
import type { SupabaseClient } from '@supabase/supabase-js';
import type { Answer } from '@/lib/collab-service';
import { RULES, rateLimit, type RateLimitRule } from '@/lib/rate-limit';
import { requestUserId } from '@/lib/request-user';
import { adminClient } from '@/lib/supabase';

/**
 * The shape every collaboration route shares: rate-limited by caller,
 * signed in (the desktop's bearer token or the site's session), and an
 * `Answer` turned into JSON with its status.
 */
export const collabRoute = async (
  request: Request,
  handle: (db: SupabaseClient, userId: string) => Promise<Answer<unknown> | { ok: false; status: number; error: string; [k: string]: unknown }>,
  rule: RateLimitRule = RULES.collab,
): Promise<Response> => {
  const userId = await requestUserId(request);
  if (!userId) return NextResponse.json({ error: 'Sign in to use shared projects' }, { status: 401 });
  const limited = await rateLimit(request, rule, undefined, userId);
  if (limited) return limited;
  const answer = await handle(adminClient(), userId);
  if (answer.ok) return NextResponse.json(answer.value ?? { ok: true });
  const { ok: _ok, status, ...body } = answer;
  return NextResponse.json(body, { status });
};

export const badRequest = (error: string) => ({ ok: false as const, status: 400, error });
