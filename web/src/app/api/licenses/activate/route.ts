import { NextResponse } from 'next/server';
import { z } from 'zod';
import { platformSchema } from '@/domain';
import { activateDevice } from '@/lib/activation-service';
import { adminClient, currentUser } from '@/lib/supabase';
import { RULES, rateLimit } from '@/lib/rate-limit';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

const bodySchema = z.object({
  // Optional: a signed-in customer with one licence should not have to find a
  // serial to start, so without one the account's own licences are tried.
  serial: z.string().max(60).optional(),
  deviceFingerprint: z.string().min(8).max(200),
  deviceName: z.string().max(120).default(''),
  platform: platformSchema,
  appVersion: z.string().max(40).default(''),
});

/**
 * Activate this installation against a licence.
 *
 * Authenticated by the desktop app's bearer token (or a session cookie). A
 * serial alone is never enough: activation is scoped to licences on the
 * authenticated account, so a leaked serial cannot consume somebody else's
 * seats. The contract is documented in docs/api.md.
 */
export async function POST(request: Request): Promise<Response> {
  const limited = await rateLimit(request, RULES.activate);
  if (limited) return limited;

  const parsed = bodySchema.safeParse(await request.json().catch(() => null));
  if (!parsed.success) {
    return NextResponse.json({ error: 'A device is required' }, { status: 400 });
  }

  const userId = await resolveUserId(request);
  if (!userId) return NextResponse.json({ error: 'Sign in to activate VC Composer' }, { status: 401 });

  const result = await activateDevice({ userId, ...parsed.data });

  if (result.outcome.result === 'refused') {
    // 409: the request is well formed and the caller is who they say; there is
    // simply no seat (or no live licence), which the message explains.
    return NextResponse.json({ error: result.message, outcome: result.outcome }, { status: 409 });
  }

  return NextResponse.json({ activated: true, reason: result.outcome.reason, license: result.license });
}

const resolveUserId = async (request: Request): Promise<string | null> => {
  const header = request.headers.get('authorization');
  if (header?.toLowerCase().startsWith('bearer ')) {
    const { data } = await adminClient().auth.getUser(header.slice(7).trim());
    return data.user?.id ?? null;
  }
  const user = await currentUser();
  return user?.id ?? null;
};
