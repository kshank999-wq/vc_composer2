import { z } from 'zod';
import { badRequest, collabRoute } from '@/lib/collab-route';
import { lockTrack, unlockTrack } from '@/lib/collab-service';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

const body = z.object({ trackId: z.string().min(1).max(80) });

/** Lock a part for writing (renewing your own lock). 409 says who holds it. */
export const POST = (request: Request, { params }: { params: { id: string } }) =>
  collabRoute(request, async (db, userId) => {
    const parsed = body.safeParse(await request.json().catch(() => null));
    if (!parsed.success) return badRequest('Name the track to lock');
    return lockTrack(db, params.id, userId, parsed.data.trackId);
  });

export const DELETE = (request: Request, { params }: { params: { id: string } }) =>
  collabRoute(request, async (db, userId) => {
    const parsed = body.safeParse(await request.json().catch(() => null));
    if (!parsed.success) return badRequest('Name the track to unlock');
    return unlockTrack(db, params.id, userId, parsed.data.trackId);
  });
