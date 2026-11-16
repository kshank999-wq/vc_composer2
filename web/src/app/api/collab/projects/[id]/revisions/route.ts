import { pushSchema } from '@/domain';
import { badRequest, collabRoute } from '@/lib/collab-route';
import { pushRevision } from '@/lib/collab-service';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * The push: the uploaded file becomes the next revision if nobody pushed
 * since `base`; otherwise 409 with the head (pull, merge, push again).
 */
export const POST = (request: Request, { params }: { params: { id: string } }) =>
  collabRoute(request, async (db, userId) => {
    const parsed = pushSchema.safeParse(await request.json().catch(() => null));
    if (!parsed.success) return badRequest('That is not a revision this server takes');
    return pushRevision(db, params.id, userId, parsed.data);
  });
