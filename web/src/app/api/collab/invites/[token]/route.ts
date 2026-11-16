import { z } from 'zod';
import { badRequest, collabRoute } from '@/lib/collab-route';
import { acceptInvite } from '@/lib/collab-service';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/** Accept an invitation with the signed-in account. */
export const POST = (request: Request, { params }: { params: { token: string } }) =>
  collabRoute(request, async (db, userId) => {
    if (!z.string().uuid().safeParse(params.token).success) return badRequest('That invitation link is not valid');
    return acceptInvite(db, params.token, userId);
  });
